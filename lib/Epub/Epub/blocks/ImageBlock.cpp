#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/PixelCacheFormat.h"

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void* ImageBlock::extractCtx = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* ctx, ExtractFn fn) {
  extractCtx = ctx;
  extractFn = fn;
}

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  const size_t slash = imagePath.rfind('/');
  if (dotPos != std::string::npos && (slash == std::string::npos || dotPos > slash)) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  PixelCacheHeader header;
  if (cacheFile.read(&header, sizeof(header)) != sizeof(header) || !header.isValid()) {
    return false;
  }

  cachedWidth = header.width;
  cachedHeight = header.height;
  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = sizeof(header) + bytesPerRow * cachedHeight;
  return cacheFile.size() == expectedSize;
}

// Suppress repeated failures across the BW/grayscale passes of one page render.
// Clear before the next page render so transient memory/storage failures retry.
constexpr size_t MAX_RENDER_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_RENDER_IMAGE_FAILURES];
size_t failedImageCount = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisRender(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_RENDER_IMAGE_FAILURES || imageFailedThisRender(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
constexpr size_t PXC_MAX_CHUNKS = 6;  // 96 KB: a full-screen 2bpp image
constexpr size_t PXC_HEAP_RESERVE = 16 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary; they are reassembled into a stack
// buffer. (screenWidth + 3) / 4 caps at 200 B for an 800px panel.
constexpr int PXC_MAX_BYTES_PER_ROW = 208;

std::unique_ptr<uint8_t[]> pxcChunks[PXC_MAX_CHUNKS];
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;
size_t pxcSlotBytes = 0;

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
  pxcSlotBytes = 0;
}

const uint8_t* pxcRowPtr(size_t rowStart, int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

bool loadPxcSlot(uint64_t cacheHash, HalFile& cacheFile, uint16_t cachedWidth, uint16_t cachedHeight, int bytesPerRow) {
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW) return true;

  size_t remaining = static_cast<size_t>(bytesPerRow) * cachedHeight;
  for (size_t i = 0; i < PXC_MAX_CHUNKS && remaining != 0; i++) {
    if (ImageToFramebufferDecoder::isAborted()) {
      releasePxcSlot();
      return false;
    }
    const size_t want = std::min(remaining, PXC_CHUNK_SIZE);
    if (ESP.getFreeHeap() < want + PXC_HEAP_RESERVE || ESP.getMaxAllocHeap() < want + PXC_MAX_ALLOC_RESERVE) break;
    pxcChunks[i] = makeUniqueNoThrow<uint8_t[]>(want);
    if (!pxcChunks[i]) break;
    if (cacheFile.read(pxcChunks[i].get(), want) != static_cast<int>(want)) {
      releasePxcSlot();
      return true;
    }
    pxcSlotBytes += want;
    remaining -= want;
  }
  if (pxcSlotBytes != 0) {
    pxcSlotHash = cacheHash;
    pxcSlotWidth = cachedWidth;
    pxcSlotHeight = cachedHeight;
  }
  return true;
}

struct DrawByteTables {
  bool bw[256]{};
  bool lsb[256]{};
  bool msb[256]{};
};

constexpr DrawByteTables buildDrawByteTables() {
  DrawByteTables tables;
  for (int packed = 0; packed < 256; ++packed) {
    tables.bw[packed] = packed != 255;
    for (int shift = 0; shift < 8; shift += 2) {
      const uint8_t level = (packed >> shift) & 3;
      tables.lsb[packed] |= grayPlanePixel(level, false, false).write;
      tables.msb[packed] |= grayPlanePixel(level, true, false).write;
    }
  }
  return tables;
}

constexpr DrawByteTables DRAW_BYTE_TABLES = buildDrawByteTables();

const bool* drawByteTable(const DirectPixelWriter& writer) {
  if (writer.mode == GfxRenderer::BW) return DRAW_BYTE_TABLES.bw;
  if (writer.absolute) return nullptr;
  if (writer.mode == GfxRenderer::GRAYSCALE_LSB) return DRAW_BYTE_TABLES.lsb;
  if (writer.mode == GfxRenderer::GRAYSCALE_MSB) return DRAW_BYTE_TABLES.msb;
  return nullptr;
}

void drawPxcRow(DirectPixelWriter& writer, const bool* drawByte, const uint8_t* data, int x, int y, int width) {
  writer.beginRow(y);
  int colStart, colEnd;
  writer.bandColRange(x, width, colStart, colEnd);
  for (int col = colStart; col < colEnd;) {
    const uint8_t packed = data[col >> 2];
    const int end = std::min((col | 3) + 1, colEnd);
    if (!drawByte || drawByte[packed]) {
      for (; col < end; ++col) {
        writer.writePixel(x + col, (packed >> (6 - (col & 3) * 2)) & 3);
      }
    }
    col = end;
  }
}

enum class CacheRenderResult { Unavailable, Complete, Aborted };

CacheRenderResult renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                                  int expectedHeight) {
  const uint64_t cacheHash = imagePathHash(cachePath);
  HalFile cacheFile;
  uint16_t cachedWidth = pxcSlotWidth;
  uint16_t cachedHeight = pxcSlotHeight;
  if (pxcSlotWidth == 0 || pxcSlotHash != cacheHash) {
    if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) return CacheRenderResult::Unavailable;
    if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
      LOG_DBG("IMG", "Invalid image cache: %s", cachePath.c_str());
      return CacheRenderResult::Unavailable;
    }
    if (pxcSlotWidth == 0 && !loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, (cachedWidth + 3) / 4)) {
      return CacheRenderResult::Aborted;
    }
  }

  const int bytesPerRow = (cachedWidth + 3) / 4;
  const int rowsInRam = pxcSlotWidth != 0 && pxcSlotHash == cacheHash ? pxcSlotBytes / bytesPerRow : 0;
  DirectPixelWriter writer;
  writer.init(renderer);
  const bool* drawByte = drawByteTable(writer);
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  for (int row = 0; row < rowsInRam; ++row) {
    if ((row & 31) == 0 && ImageToFramebufferDecoder::isAborted()) return CacheRenderResult::Aborted;
    const uint8_t* data = pxcRowPtr(static_cast<size_t>(row) * bytesPerRow, bytesPerRow, tempRow);
    drawPxcRow(writer, drawByte, data, x, y + row, cachedWidth);
  }
  if (rowsInRam == cachedHeight) return CacheRenderResult::Complete;
  if (ImageToFramebufferDecoder::isAborted()) return CacheRenderResult::Aborted;

  if (!cacheFile.isOpen() &&
      (!Storage.openFileForRead("IMG", cachePath, cacheFile) ||
       !readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight) ||
       cachedWidth != pxcSlotWidth || cachedHeight != pxcSlotHeight)) {
    releasePxcSlot();
    return CacheRenderResult::Unavailable;
  }
  if (!cacheFile.seek(sizeof(PixelCacheHeader) + static_cast<size_t>(rowsInRam) * bytesPerRow))
    return CacheRenderResult::Unavailable;

  int rowsPerRead = std::clamp(4096 / bytesPerRow, 1, static_cast<int>(cachedHeight) - rowsInRam);
  auto buffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow);
  uint8_t* readBuffer = buffer.get();
  if (!readBuffer) {
    rowsPerRead = 1;
    if (bytesPerRow <= PXC_MAX_BYTES_PER_ROW) {
      readBuffer = tempRow;
    } else {
      buffer = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
      if (!buffer) return CacheRenderResult::Unavailable;
      readBuffer = buffer.get();
    }
  }

  for (int row = rowsInRam; row < cachedHeight;) {
    if (ImageToFramebufferDecoder::isAborted()) return CacheRenderResult::Aborted;
    const int count = std::min(rowsPerRead, cachedHeight - row);
    const size_t bytes = static_cast<size_t>(count) * bytesPerRow;
    if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
      LOG_ERR("IMG", "Cache read error at row %d", row);
      if (rowsInRam != 0) releasePxcSlot();
      return CacheRenderResult::Unavailable;
    }
    for (int i = 0; i < count; ++i, ++row) {
      if ((row & 31) == 0 && ImageToFramebufferDecoder::isAborted()) return CacheRenderResult::Aborted;
      drawPxcRow(writer, drawByte, readBuffer + static_cast<size_t>(i) * bytesPerRow, x, y + row, cachedWidth);
    }
  }
  return CacheRenderResult::Complete;
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisRender(imagePath) && !hasValidCache(); }

bool ImageBlock::needsFramebufferLoanToDecode() const {
  const bool extractionNeedsLoan = !srcPath.empty() && extractFn && !Storage.exists(imagePath.c_str());
  return (extractionNeedsLoan || ImageDecoderFactory::decoderNeedsMoreHeap(imagePath)) && needsDecode();
}

bool ImageBlock::cacheImage(GfxRenderer& renderer, int x, int y) {
  if (ImageToFramebufferDecoder::isAborted()) return false;
  if (hasValidCache()) return true;
  if (width <= 0 || height <= 0 || x < 0 || y < 0 || x > renderer.getScreenWidth() - width ||
      y > renderer.getScreenHeight() - height)
    return false;
  return decodeImage(renderer, x, y, true);
}

void ImageBlock::clearRenderFailures() { failedImageCount = 0; }

void ImageBlock::releaseRenderCache() { releasePxcSlot(); }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;
  if (ImageToFramebufferDecoder::isAborted()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (width <= 0 || height <= 0 || x < 0 || y < 0 || x > screenWidth - width || y > screenHeight - height) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (imageFailedThisRender(imagePath)) {
    renderPlaceholder(renderer, x, y);
    return;
  }

  if (renderer.imagesDitheredInBw() && renderer.getRenderMode() != GfxRenderer::BW) return;

  // Try to render from cache first
  const auto cached = renderFromCache(renderer, getCachePath(imagePath), x, y, width, height);
  if (cached == CacheRenderResult::Aborted) return;
  if (cached == CacheRenderResult::Complete) {
    if (!ImageToFramebufferDecoder::isAborted()) {
      renderer.preserveImagePolarity(x, y, width, height);
    }
    return;  // Successfully rendered from cache
  }
  if (ImageToFramebufferDecoder::isAborted()) return;

  if (!decodeImage(renderer, x, y, false)) {
    if (ImageToFramebufferDecoder::isAborted()) return;
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  renderer.preserveImagePolarity(x, y, width, height);
}

bool ImageBlock::decodeImage(GfxRenderer& renderer, int x, int y, bool cacheOnly) {
  releasePxcSlot();
  if (!srcPath.empty() && extractFn && !Storage.exists(imagePath.c_str()) &&
      !extractFn(extractCtx, srcPath.c_str(), imagePath.c_str())) {
    if (!ImageToFramebufferDecoder::isAborted()) LOG_ERR("IMG", "Lazy extraction failed: %s", srcPath.c_str());
    return false;
  }
  if (ImageToFramebufferDecoder::isAborted()) return false;

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) return false;

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useExactDimensions = true;
  config.cacheOnly = cacheOnly;
  config.cachePath = getCachePath(imagePath);
  return decoder->decodeToFramebuffer(imagePath, renderer, config);
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  std::string src;
  serialization::readString(file, path);
  serialization::readString(file, src);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, src, w, h));
}
