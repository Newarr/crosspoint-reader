#include "PngToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <ImageBakeQuantizer.h>
#include <Logging.h>
#include <Memory.h>
#include <PNGdec.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "DecoderStorage.h"
#include "DirectPixelWriter.h"
#include "PixelCache.h"
#include "SampleRowClamp.h"

namespace {

constexpr size_t MAX_GRAY_LINE_BUFFER_BYTES = PNG_MAX_BUFFERED_PIXELS / 2;

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  PNG* decoder{nullptr};
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int cropLeft{0};
  int cropTop{0};
  int visibleWidth{0};
  int visibleHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates
  PixelCache cache;
  bool caching{false};
  bool aborted{false};

  uint8_t* grayLineBuffer{nullptr};
  uint8_t* alphaLineBuffer{nullptr};
  uint8_t* previousGrayLine{nullptr};
  uint32_t stepX{SAMPLE_ONE};
  uint32_t stepY{SAMPLE_ONE};
  uint32_t lastYieldMs{0};  // throttle state for yieldDuringDecode()
};

// File I/O callbacks use pFile->fHandle to access the HalFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  auto file = makeUniqueNoThrow<HalFile>();
  if (!file || !Storage.openFileForRead("PNG", filename, *file)) return nullptr;
  const size_t fileSize = file->size();
  if (fileSize == 0 || fileSize > INT32_MAX) return nullptr;
  *size = static_cast<int32_t>(fileSize);
  return file.release();
}

void pngCloseWithHandle(void* handle) { delete static_cast<HalFile*>(handle); }

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f || len <= 0) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f || pos < 0 || !f->seek(pos)) return -1;
  return pos;
}

int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int packedRowBytes(int srcWidth, int bitsPerSample) { return (srcWidth * bitsPerSample + 7) / 8; }

int requiredPngInternalBufferBytes(int srcWidth, int pixelType, int bitsPerSample) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  if ((pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED) && bitsPerSample < 8) {
    pitch = packedRowBytes(srcWidth, bitsPerSample);
  }
  return ((pitch + 1) * 2) + 32;
}

bool isSupportedBitDepth(int pixelType, int bitsPerSample) {
  if (bitsPerSample == 8) return true;
  if (bitsPerSample != 1 && bitsPerSample != 2 && bitsPerSample != 4) return false;
  return pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED;
}

uint8_t readPackedSample(const uint8_t* pixels, int x, int bitsPerSample) {
  if (bitsPerSample == 8) return pixels[x];

  const int bitOffset = x * bitsPerSample;
  const int shift = 8 - bitsPerSample - (bitOffset & 7);
  const uint8_t mask = (1U << bitsPerSample) - 1;
  return (pixels[bitOffset >> 3] >> shift) & mask;
}

uint8_t expandSampleToByte(uint8_t sample, int bitsPerSample) {
  if (bitsPerSample == 8) return sample;
  const uint8_t maxSample = (1U << bitsPerSample) - 1;
  return static_cast<uint8_t>((sample * 255U) / maxSample);
}

uint8_t alphaThreshold4x4(const int x, const int y) {
  static constexpr uint8_t BAYER_4X4[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
  return BAYER_4X4[((y & 0x03) << 2) | (x & 0x03)];
}

// Convert an entire source line to grayscale and optionally retain alpha.
// Low-bit-depth grayscale/indexed scanlines are packed most-significant sample first.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(const uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bitsPerSample,
                       uint8_t* palette, int hasAlpha, uint32_t transparentColor, uint8_t* alphaLine) {
  const auto store = [grayLine, alphaLine](const int x, const uint8_t gray, const uint8_t alpha) {
    grayLine[x] = alphaLine ? gray : static_cast<uint8_t>((gray * alpha + 255u * (255u - alpha)) / 255u);
    if (alphaLine) alphaLine[x] = alpha;
  };

  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      if (bitsPerSample == 8 && !hasAlpha && !alphaLine) {
        memcpy(grayLine, pPixels, width);
      } else {
        for (int x = 0; x < width; x++) {
          const uint8_t sample = readPackedSample(pPixels, x, bitsPerSample);
          const uint8_t gray = expandSampleToByte(sample, bitsPerSample);
          store(x, gray, hasAlpha && sample == static_cast<uint8_t>(transparentColor) ? 0 : 255);
        }
      }
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 3];
        const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        const uint32_t color = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
        store(x, gray, hasAlpha && color == transparentColor ? 0 : 255);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        for (int x = 0; x < width; x++) {
          const uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
          const uint8_t* p = &palette[idx * 3];
          const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          store(x, gray, hasAlpha ? palette[768 + idx] : 255);
        }
      } else {
        for (int x = 0; x < width; x++) {
          store(x, expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample), 255);
        }
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        store(x, pPixels[x * 2], pPixels[x * 2 + 1]);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 4];
        const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        store(x, gray, p[3]);
      }
      break;

    default:
      memset(grayLine, 128, width);
      if (alphaLine) memset(alphaLine, 255, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;
  if (ImageToFramebufferDecoder::isAborted()) {
    ctx->aborted = true;
    return 0;
  }

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);
  if (ImageToFramebufferDecoder::isAborted()) {
    ctx->aborted = true;
    return 0;
  }

  const int srcY = pDraw->y - ctx->cropTop;
  if (srcY < 0 || srcY >= ctx->visibleHeight) return 1;

  const bool bilinear = ctx->previousGrayLine != nullptr;
  const bool cacheOnly = ctx->config->cacheOnly;
  const int firstDstY = ctx->lastDstY + 1;
  int endDstY = firstDstY;
  if (bilinear) {
    while (endDstY < ctx->dstHeight &&
           samplePair(samplePosition(endDstY, ctx->stepY), 0, ctx->visibleHeight).second <= srcY) {
      ++endDstY;
    }
    if (firstDstY == endDstY &&
        (firstDstY == ctx->dstHeight ||
         samplePair(samplePosition(firstDstY, ctx->stepY), 0, ctx->visibleHeight).first > srcY)) {
      return 1;
    }
  } else {
    endDstY = (srcY * ctx->dstHeight) / ctx->visibleHeight + 1;
    if (ctx->dstHeight > ctx->visibleHeight) endDstY = ((srcY + 1) * ctx->dstHeight) / ctx->visibleHeight;
    if (firstDstY >= endDstY) return 1;
  }

  const uint32_t transparentColor = ctx->decoder->getTransparentColor();
  convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, ctx->srcWidth, pDraw->iPixelType, pDraw->iBpp, pDraw->pPalette,
                    pDraw->iHasAlpha, transparentColor, ctx->alphaLineBuffer);

  DirectPixelWriter pw{};
  if (!cacheOnly) pw.init(*ctx->renderer);

  for (int dstY = firstDstY; dstY < endDstY; dstY++) {
    ctx->lastDstY = dstY;
    const int outY = ctx->config->y + dstY;
    if (!cacheOnly && (outY < 0 || outY >= ctx->screenHeight)) continue;
    if (!cacheOnly) pw.beginRow(outY);

    DirectCacheWriter cw;
    if (ctx->caching) {
      if (!ctx->cache.advanceTo(dstY)) {
        ctx->caching = false;
        if (cacheOnly) return 0;
      } else {
        cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
        cw.beginRow(outY, ctx->config->y + ctx->cache.bandStart);
      }
    }

    const auto drawSample = [&](int dstX, uint8_t gray) {
      const int outX = ctx->config->x + dstX;
      const uint8_t level = ctx->config->useDithering ? imagebake::quantizeGray2bit(dstX, dstY, gray) : gray >> 6;
      if (!cacheOnly) pw.writePixel(outX, level, ctx->alphaLineBuffer != nullptr);
      if (ctx->caching) cw.writePixel(outX, level);
    };

    const int startX = cacheOnly ? 0 : std::max(0, -ctx->config->x);
    const int endX = cacheOnly ? ctx->dstWidth : std::min(ctx->dstWidth, ctx->screenWidth - ctx->config->x);
    const uint8_t* current = ctx->grayLineBuffer + ctx->cropLeft;
    if (bilinear) {
      const SamplePair rows = samplePair(samplePosition(dstY, ctx->stepY), 0, ctx->visibleHeight);
      const uint8_t* above = rows.first == srcY ? current : ctx->previousGrayLine + ctx->cropLeft;
      for (int dstX = startX; dstX < endX; dstX++) {
        const SamplePair cols = samplePair(samplePosition(dstX, ctx->stepX), 0, ctx->visibleWidth);
        const auto top = interpolateSample(above[cols.first], above[cols.second], cols.fraction);
        const auto bottom = interpolateSample(current[cols.first], current[cols.second], cols.fraction);
        drawSample(dstX, static_cast<uint8_t>(interpolateSample(top, bottom, rows.fraction)));
      }
    } else {
      for (int dstX = startX; dstX < endX; dstX++) {
        const int srcX = (dstX * ctx->visibleWidth) / ctx->dstWidth;
        if (ctx->alphaLineBuffer) {
          const uint8_t alpha = ctx->alphaLineBuffer[ctx->cropLeft + srcX];
          if (alpha < 8 || alpha <= alphaThreshold4x4(ctx->config->x + dstX, outY)) continue;
        }
        drawSample(dstX, current[srcX]);
      }
    }
  }
  if (bilinear) std::swap(ctx->grayLineBuffer, ctx->previousGrayLine);

  return 1;
}

}  // namespace

bool PngToFramebufferConverter::decoderFitsHeap() {
#ifdef PNGDEC_CALLER_ROW_BUFFER
  constexpr size_t cacheBandHeadroom = 4 * 1024;
  constexpr size_t rowHeadroom = PNG_MAX_BUFFERED_PIXELS + MAX_GRAY_LINE_BUFFER_BYTES + cacheBandHeadroom;
  constexpr size_t allocatorSlack = 64;
  return DecoderStorage<PNG>::fitsHeap(rowHeadroom, PNG_MAX_BUFFERED_PIXELS + allocatorSlack);
#else
  return DecoderStorage<PNG>::fitsHeap();
#endif
}

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  DecoderStorage<PNG> storage(false);
  PNG* png = storage.get();
  if (!png) return false;

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     nullptr);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};

  if (rc != 0) {
    LOG_ERR("PNG", "Failed to open PNG for dimensions: %d", rc);
    return false;
  }

  return validateAndStoreDimensions(png->getWidth(), png->getHeight(), out, "PNG");
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s", imagePath.c_str());

  if (config.cacheOnly && (config.cachePath.empty() || config.preserveAlpha)) return false;
  DecoderStorage<PNG> storage(config.cacheOnly && !renderer.hasFrameBuffer());
  PNG* png = storage.get();
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    return false;
  }

  PngContext ctx;
  ctx.decoder = png;
  ctx.renderer = &renderer;
  ctx.config = &config;
  if (!config.cacheOnly) {
    ctx.screenWidth = renderer.getScreenWidth();
    ctx.screenHeight = renderer.getScreenHeight();
  }

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     pngDrawCallback);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};
  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG: %d", rc);
    return false;
  }

  ImageDimensions sourceDimensions;
  if (!validateAndStoreDimensions(png->getWidth(), png->getHeight(), sourceDimensions, "PNG")) return false;

  // Calculate output dimensions
  ctx.srcWidth = sourceDimensions.width;
  ctx.srcHeight = sourceDimensions.height;
  ctx.cropLeft = static_cast<int>(ctx.srcWidth * std::clamp(config.sourceCropX, 0.0f, 0.99f) / 2.0f);
  ctx.cropTop = static_cast<int>(ctx.srcHeight * std::clamp(config.sourceCropY, 0.0f, 0.99f) / 2.0f);
  ctx.visibleWidth = ctx.srcWidth - 2 * ctx.cropLeft;
  ctx.visibleHeight = ctx.srcHeight - 2 * ctx.cropTop;
  if (ctx.visibleWidth <= 0 || ctx.visibleHeight <= 0) {
    LOG_ERR("PNG", "PNG crop leaves no visible pixels");
    return false;
  }

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.visibleWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.visibleWidth;
    float scaleY = (float)config.maxHeight / ctx.visibleHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.visibleWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.visibleHeight * ctx.scale);
  }
  if (ctx.dstWidth <= 0 || ctx.dstHeight <= 0 || ctx.dstWidth > INT16_MAX || ctx.dstHeight > INT16_MAX) return false;
  ctx.stepX = (static_cast<uint32_t>(ctx.visibleWidth) << 16) / ctx.dstWidth;
  ctx.stepY = (static_cast<uint32_t>(ctx.visibleHeight) << 16) / ctx.dstHeight;

  const int pixelType = png->getPixelType();
  const int bitsPerSample = png->getBpp();
  LOG_DBG("PNG", "PNG %dx%d (visible %dx%d) -> %dx%d (scale %.2f), type: %d, bpp: %d", ctx.srcWidth, ctx.srcHeight,
          ctx.visibleWidth, ctx.visibleHeight, ctx.dstWidth, ctx.dstHeight, ctx.scale, pixelType, bitsPerSample);

  if (!isSupportedBitDepth(pixelType, bitsPerSample)) {
    warnUnsupportedFeature(
        "bit depth (" + std::to_string(bitsPerSample) + "bpp) for pixel type " + std::to_string(pixelType), imagePath);
    return false;
  }

  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType, bitsPerSample);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR("PNG", "PNG row exceeds buffer limit: need %d bytes, max %d", requiredInternal, PNG_MAX_BUFFERED_PIXELS);
    return false;
  }
#ifdef PNGDEC_CALLER_ROW_BUFFER
  auto decoderRows = makeUniqueNoThrow<uint8_t[]>(requiredInternal);
  if (!decoderRows) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder rows (%d bytes)", requiredInternal);
    return false;
  }
  png->setRowBuffer(decoderRows.get(), requiredInternal);
#endif

  // The converter expands each source row to 8-bit grayscale before dithering,
  // so this scratch buffer is sized by source pixels even when PNGdec reads a
  // packed 1/2/4-bit row internally.
  const size_t grayBufSize = static_cast<size_t>(ctx.srcWidth);
  if (grayBufSize > MAX_GRAY_LINE_BUFFER_BYTES) {
    LOG_ERR("PNG", "Expanded gray row too wide: need %u bytes for width=%d, max=%u", static_cast<unsigned>(grayBufSize),
            ctx.srcWidth, static_cast<unsigned>(MAX_GRAY_LINE_BUFFER_BYTES));
    return false;
  }

  // tRNS chunks are parsed during decode(), so hasAlpha() is not reliable yet
  // for color-key and indexed transparency. Reserve the alpha line whenever
  // the caller requests preservation; opaque pixels simply store alpha 255.
  const bool retainAlpha = config.preserveAlpha;
  const size_t lineBufferBytes = grayBufSize * (retainAlpha ? 2u : 1u);
  auto lineBuffers = makeUniqueNoThrow<uint8_t[]>(lineBufferBytes);
  if (!lineBuffers) {
    LOG_ERR("PNG", "Failed to allocate PNG line buffers (%u bytes)", static_cast<unsigned>(lineBufferBytes));
    return false;
  }
  ctx.grayLineBuffer = lineBuffers.get();
  ctx.alphaLineBuffer = retainAlpha ? ctx.grayLineBuffer + grayBufSize : nullptr;

  // Stream the pixel cache to disk. PNGdec delivers source scanlines top to
  // bottom and we emit at most one (downscaled) output row per callback, so the
  // band only needs a single row. Streaming keeps the working set tiny, so
  // unlike the old full-image buffer it neither competes with the ~44KB decoder
  // nor forces larger images to skip caching - which previously meant a full
  // re-decode on every one of an image page's ~14 render passes.
  ctx.caching = !config.preserveAlpha && !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      if (config.cacheOnly) return false;
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  std::unique_ptr<uint8_t[]> previousLine;
  if (!retainAlpha && (ctx.stepX > SAMPLE_ONE || ctx.stepY > SAMPLE_ONE)) {
    previousLine = makeUniqueNoThrow<uint8_t[]>(grayBufSize);
    ctx.previousGrayLine = previousLine.get();
  }

  unsigned long decodeStart = millis();
  ctx.lastYieldMs = decodeStart;
  rc = png->decode(&ctx, 0);
  unsigned long decodeTime = millis() - decodeStart;

  ctx.grayLineBuffer = nullptr;
  ctx.alphaLineBuffer = nullptr;

  if (ctx.aborted || ImageToFramebufferDecoder::isAborted() || rc != PNG_SUCCESS || ctx.lastDstY != ctx.dstHeight - 1) {
    if (ctx.aborted || ImageToFramebufferDecoder::isAborted()) {
      LOG_DBG("PNG", "PNG decode aborted after %lu ms", decodeTime);
    } else {
      LOG_ERR("PNG", "Decode failed: %d", rc);
    }
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", decodeTime);

  // Finalize the streamed cache (caching may have been cleared on a flush error).
  const bool cacheWritten = ctx.caching && ctx.cache.finalize();
  return !config.cacheOnly || cacheWritten;
}
