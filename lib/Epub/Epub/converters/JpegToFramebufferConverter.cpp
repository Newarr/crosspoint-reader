#include "JpegToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <ImageBakeQuantizer.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "DecoderStorage.h"
#include "DirectPixelWriter.h"
#include "PixelCache.h"
#include "SampleRowClamp.h"

namespace {

// Context struct passed through JPEGDEC callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by setUserPointer()).
struct JpegContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Visible source dimensions after JPEGDEC's built-in scaling
  int scaledSrcWidth{0};
  int scaledSrcHeight{0};
  int cropLeft{0};
  int cropTop{0};

  // Final output dimensions
  int dstWidth{0};
  int dstHeight{0};

  // Fine scale in 16.16 fixed-point (ESP32-C3 has no FPU).
  // X and Y axes use separate scale factors: the aspect ratio of the output (dstWidth/dstHeight)
  // may differ from the source (srcWidth/srcHeight) due to integer rounding of displayHeight.
  int32_t fineScaleFPX{1 << 16};  // X: src -> dst column mapping
  int32_t invScaleFPX{1 << 16};   // X: dst -> src column mapping
  int32_t fineScaleFPY{1 << 16};  // Y: src -> dst row mapping
  int32_t invScaleFPY{1 << 16};   // Y: dst -> src row mapping

  PixelCache cache;
  bool caching{false};
  bool aborted{false};

  uint32_t lastYieldMs{0};  // throttle state for yieldDuringDecode()
};

constexpr int32_t JPEG_SOURCE_BUFFER_BYTES = 8 * 1024;

struct JpegSource {
  HalFile file;
  std::unique_ptr<uint8_t[]> buffer;
  int32_t size = 0;
  int32_t position = 0;
  int32_t bufferStart = 0;
  int32_t bufferedBytes = 0;
  bool readFailed = false;

  bool open(const std::string& path) {
    if (!Storage.openFileForRead("JPG", path, file)) return false;
    const size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > INT32_MAX) return false;
    size = static_cast<int32_t>(fileSize);
    return true;
  }

  void enableReadAhead() {
    if (ESP.getFreeHeap() >= JPEG_SOURCE_BUFFER_BYTES + 16 * 1024) {
      buffer = makeUniqueNoThrow<uint8_t[]>(JPEG_SOURCE_BUFFER_BYTES);
      bufferStart = position;
    }
  }
};

int32_t jpegRead(JPEGFILE* file, uint8_t* output, int32_t length) {
  auto& source = *static_cast<JpegSource*>(file->fHandle);
  if (length <= 0 || source.position >= source.size) return 0;
  length = std::min(length, source.size - source.position);
  int32_t copied = 0;
  if (!source.buffer) {
    copied = source.file.read(output, length);
    if (copied <= 0) {
      source.readFailed = true;
      return 0;
    }
    source.position += copied;
  } else {
    while (copied < length) {
      const int32_t offset = source.position - source.bufferStart;
      if (offset >= 0 && offset < source.bufferedBytes) {
        const int32_t count = std::min(source.bufferedBytes - offset, length - copied);
        memcpy(output + copied, source.buffer.get() + offset, count);
        copied += count;
        source.position += count;
        continue;
      }
      if (source.position != source.bufferStart + source.bufferedBytes && !source.file.seek(source.position)) {
        source.readFailed = true;
        break;
      }
      const int32_t count =
          source.file.read(source.buffer.get(), std::min(JPEG_SOURCE_BUFFER_BYTES, source.size - source.position));
      if (count <= 0) {
        source.readFailed = true;
        break;
      }
      source.bufferStart = source.position;
      source.bufferedBytes = count;
    }
  }
  file->iPos = source.position;
  return copied;
}

int32_t jpegSeek(JPEGFILE* file, int32_t position) {
  auto& source = *static_cast<JpegSource*>(file->fHandle);
  if (position < 0 || position > source.size || (!source.buffer && !source.file.seek(position))) {
    source.readFailed = true;
    return -1;
  }
  source.position = position;
  file->iPos = position;
  return position;
}

// Choose JPEGDEC's built-in scale factor for coarse downscaling.
// Returns the scale denominator (1, 2, 4, or 8) and sets jpegScaleOption.
int chooseJpegScale(float targetScale, int& jpegScaleOption) {
  if (targetScale <= 0.135f) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    return 8;
  }
  if (targetScale <= 0.27f) {
    jpegScaleOption = JPEG_SCALE_QUARTER;
    return 4;
  }
  if (targetScale <= 0.54f) {
    jpegScaleOption = JPEG_SCALE_HALF;
    return 2;
  }
  jpegScaleOption = 0;
  return 1;
}

// Fixed-point 16.16 arithmetic avoids software float emulation on ESP32-C3 (no FPU).
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;

int jpegDrawCallback(JPEGDRAW* pDraw) {
  JpegContext* ctx = reinterpret_cast<JpegContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer) return 0;
  if (ImageToFramebufferDecoder::isAborted()) {
    ctx->aborted = true;
    return 0;
  }

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);
  if (ImageToFramebufferDecoder::isAborted()) {
    ctx->aborted = true;
    return 0;
  }

  // In EIGHT_BIT_GRAYSCALE mode, pPixels contains 8-bit grayscale values
  // Buffer is densely packed: stride = pDraw->iWidth, valid columns = pDraw->iWidthUsed
  uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;

  if (!pixels || stride <= 0 || blockH <= 0 || validW <= 0 || validW > stride) return 1;

  const bool useDithering = ctx->config->useDithering;
  const bool cacheOnly = ctx->config->cacheOnly;
  bool caching = ctx->caching;
  const int32_t fineScaleFPX = ctx->fineScaleFPX;
  const int32_t invScaleFPX = ctx->invScaleFPX;
  const int32_t fineScaleFPY = ctx->fineScaleFPY;
  const int32_t invScaleFPY = ctx->invScaleFPY;
  const int cfgX = ctx->config->x;
  const int cfgY = ctx->config->y;
  const int blockX = pDraw->x - ctx->cropLeft;
  const int blockY = pDraw->y - ctx->cropTop;

  // Determine destination pixel range covered by this source block
  const int srcYEnd = blockY + blockH;
  const int srcXEnd = blockX + validW;

  int dstYStart = (int)((int64_t)blockY * fineScaleFPY >> FP_SHIFT);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)((int64_t)srcYEnd * fineScaleFPY >> FP_SHIFT);
  int dstXStart = (int)((int64_t)blockX * fineScaleFPX >> FP_SHIFT);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)((int64_t)srcXEnd * fineScaleFPX >> FP_SHIFT);

  // Pre-clamp destination ranges to screen bounds (eliminates per-pixel screen checks)
  int clampYMax = ctx->dstHeight;
  if (ctx->screenHeight - cfgY < clampYMax) clampYMax = ctx->screenHeight - cfgY;
  dstYStart = std::max(dstYStart, std::max(0, -cfgY));
  if (dstYEnd > clampYMax) dstYEnd = clampYMax;

  int clampXMax = ctx->dstWidth;
  if (ctx->screenWidth - cfgX < clampXMax) clampXMax = ctx->screenWidth - cfgX;
  dstXStart = std::max(dstXStart, std::max(0, -cfgX));
  if (dstXEnd > clampXMax) dstXEnd = clampXMax;

  if (dstYStart >= dstYEnd || dstXStart >= dstXEnd) return 1;

  // Pre-compute orientation and render-mode state once per callback invocation
  DirectPixelWriter pw{};
  if (!cacheOnly) pw.init(*ctx->renderer);

  // The cache streams to disk one MCU-row band at a time. Flushing rows below
  // this block (raster order guarantees they are final) repositions the band;
  // cacheOriginY then maps screen rows to the band-local buffer rows. If a flush
  // write fails, stop caching for the rest of this decode (and let finalize drop
  // the partial file) rather than writing past the band buffer.
  DirectCacheWriter cw;
  int cacheOriginY = 0;
  if (caching) {
    if (!ctx->cache.advanceTo(dstYStart)) {
      caching = false;
      ctx->caching = false;
      if (cacheOnly) return 0;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
      cacheOriginY = ctx->config->y + ctx->cache.bandStart;
    }
  }

  const bool unscaled = fineScaleFPX == FP_ONE && fineScaleFPY == FP_ONE;
  for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
    const int outY = cfgY + dstY;
    if (!cacheOnly) pw.beginRow(outY);
    if (caching) cw.beginRow(outY, cacheOriginY);
    const auto drawRow = [&](const auto& grayAt) {
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const uint8_t gray = grayAt(dstX);
        const uint8_t level = useDithering ? imagebake::quantizeGray2bit(dstX, dstY, gray) : gray / 85;
        const int outX = cfgX + dstX;
        if (!cacheOnly) pw.writePixel(outX, level);
        if (caching) cw.writePixel(outX, level);
      }
    };

    if (unscaled) {
      const uint8_t* row = pixels + (dstY - blockY) * stride;
      drawRow([&](int dstX) { return row[dstX - blockX]; });
      continue;
    }

    const SamplePair rows = samplePair(samplePosition(dstY, invScaleFPY), blockY, blockH);
    const uint8_t* row0 = pixels + rows.first * stride;
    const uint8_t* row1 = pixels + rows.second * stride;
    drawRow([&](int dstX) {
      const SamplePair cols = samplePair(samplePosition(dstX, invScaleFPX), blockX, validW);
      const auto top = interpolateSample(row0[cols.first], row0[cols.second], cols.fraction);
      const auto bottom = interpolateSample(row1[cols.first], row1[cols.second], cols.fraction);
      return static_cast<uint8_t>(interpolateSample(top, bottom, rows.fraction));
    });
  }

  return 1;
}

}  // namespace

bool JpegToFramebufferConverter::decoderFitsHeap() { return DecoderStorage<JPEGDEC>::fitsHeap(); }

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  DecoderStorage<JPEGDEC> storage(false);
  JPEGDEC* jpeg = storage.get();
  JpegSource source;
  if (!jpeg || !source.open(imagePath)) return false;

  if (jpeg->open(&source, source.size, nullptr, jpegRead, jpegSeek, nullptr) != 1 || source.readFailed) {
    LOG_ERR("JPG", "Failed to open JPEG for dimensions (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    return false;
  }

  const int width = jpeg->getWidth();
  const int height = jpeg->getHeight();
  if (!validateAndStoreDimensions(width, height, out, "JPEG")) return false;
  LOG_DBG("JPG", "Image dimensions: %dx%d", width, height);

  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  bool cacheWritten;
  return decodeWithCacheResult(imagePath, renderer, config, cacheWritten);
}

bool JpegToFramebufferConverter::decodeWithCacheResult(const std::string& imagePath, GfxRenderer& renderer,
                                                       const RenderConfig& config, bool& cacheWritten) {
  cacheWritten = false;
  LOG_DBG("JPG", "Decoding JPEG: %s", imagePath.c_str());

  if (config.cacheOnly && config.cachePath.empty()) return false;
  DecoderStorage<JPEGDEC> storage(config.cacheOnly && !renderer.hasFrameBuffer());
  JPEGDEC* jpeg = storage.get();
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder");
    return false;
  }

  JpegSource source;
  if (!source.open(imagePath)) return false;

  JpegContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  if (!config.cacheOnly) {
    ctx.screenWidth = renderer.getScreenWidth();
    ctx.screenHeight = renderer.getScreenHeight();
  }

  int rc = jpeg->open(&source, source.size, nullptr, jpegRead, jpegSeek, jpegDrawCallback);
  if (rc != 1 || source.readFailed) {
    LOG_ERR("JPG", "Failed to open JPEG (err=%d): %s", jpeg->getLastError(), imagePath.c_str());
    return false;
  }

  ImageDimensions sourceDimensions;
  if (!validateAndStoreDimensions(jpeg->getWidth(), jpeg->getHeight(), sourceDimensions, "JPEG")) return false;
  const int srcWidth = sourceDimensions.width;
  const int srcHeight = sourceDimensions.height;
  const float cropX = std::clamp(config.sourceCropX, 0.0f, 0.99f);
  const float cropY = std::clamp(config.sourceCropY, 0.0f, 0.99f);
  const int visibleWidth = srcWidth - 2 * static_cast<int>(srcWidth * cropX / 2);
  const int visibleHeight = srcHeight - 2 * static_cast<int>(srcHeight * cropY / 2);

  bool isProgressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
  if (isProgressive) {
    LOG_INF("JPG", "Progressive JPEG detected - decoding DC coefficients only (lower quality)");
  }

  // Calculate overall target scale
  float targetScale;
  int destWidth, destHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    targetScale = (float)destWidth / visibleWidth;
  } else {
    float scaleX =
        (config.maxWidth > 0 && visibleWidth > config.maxWidth) ? (float)config.maxWidth / visibleWidth : 1.0f;
    float scaleY =
        (config.maxHeight > 0 && visibleHeight > config.maxHeight) ? (float)config.maxHeight / visibleHeight : 1.0f;
    targetScale = (scaleX < scaleY) ? scaleX : scaleY;
    if (targetScale > 1.0f) targetScale = 1.0f;

    destWidth = (int)(visibleWidth * targetScale);
    destHeight = (int)(visibleHeight * targetScale);
  }

  // Choose JPEGDEC built-in scaling for coarse downscaling.
  // Progressive JPEGs: JPEGDEC forces JPEG_SCALE_EIGHTH internally (DC-only
  // decode produces 1/8 resolution). We must match this to avoid the if/else
  // priority chain in DecodeJPEG selecting a different scale.
  int jpegScaleOption;
  int jpegScaleDenom;
  if (isProgressive) {
    jpegScaleOption = JPEG_SCALE_EIGHTH;
    jpegScaleDenom = 8;
  } else {
    jpegScaleDenom = chooseJpegScale(targetScale, jpegScaleOption);
  }

  if (destWidth <= 0 || destHeight <= 0 || destWidth > INT16_MAX || destHeight > INT16_MAX) {
    LOG_ERR("JPG", "Degenerate output dimensions %dx%d for %s, skipping render", destWidth, destHeight,
            imagePath.c_str());
    return false;
  }

  ctx.scaledSrcWidth = (srcWidth + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.scaledSrcHeight = (srcHeight + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.cropLeft = static_cast<int>(ctx.scaledSrcWidth * cropX / 2);
  ctx.cropTop = static_cast<int>(ctx.scaledSrcHeight * cropY / 2);
  ctx.scaledSrcWidth -= 2 * ctx.cropLeft;
  ctx.scaledSrcHeight -= 2 * ctx.cropTop;
  ctx.dstWidth = destWidth;
  ctx.dstHeight = destHeight;
  if (config.cacheOnly) {
    ctx.screenWidth = config.x + destWidth;
    ctx.screenHeight = config.y + destHeight;
  }
  ctx.fineScaleFPX = (int32_t)((int64_t)destWidth * FP_ONE / ctx.scaledSrcWidth);
  ctx.invScaleFPX = (int32_t)((int64_t)ctx.scaledSrcWidth * FP_ONE / destWidth);
  ctx.fineScaleFPY = (int32_t)((int64_t)destHeight * FP_ONE / ctx.scaledSrcHeight);
  ctx.invScaleFPY = (int32_t)((int64_t)ctx.scaledSrcHeight * FP_ONE / destHeight);

  LOG_DBG("JPG", "JPEG %dx%d -> %dx%d (scale %.2f, jpegScale 1/%d, fineScale %.2f)%s", srcWidth, srcHeight, destWidth,
          destHeight, targetScale, jpegScaleDenom, (float)destWidth / ctx.scaledSrcWidth,
          isProgressive ? " [progressive]" : "");

  // Set pixel type to 8-bit grayscale (must be after open())
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  // Start streaming the pixel cache to disk. The band only needs to hold the
  // tallest single decode block: a JPEGDEC MCU cell is at most 16 scaled-source
  // rows tall, which our fine scale maps to this many output rows.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    const int maxBlockDstRows = (int)(((int64_t)16 * ctx.fineScaleFPY) >> FP_SHIFT) + 2;
    if (!ctx.cache.begin(config.cachePath, destWidth, destHeight, config.x, config.y, maxBlockDstRows)) {
      if (config.cacheOnly) return false;
      LOG_ERR("JPG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    } else {
      source.enableReadAhead();
    }
  }

  unsigned long decodeStart = millis();
  ctx.lastYieldMs = decodeStart;
  rc = jpeg->decode(0, 0, jpegScaleOption);
  unsigned long decodeTime = millis() - decodeStart;

  // JPEGDEC returns 1 even when jpegDrawCallback returns 0 to stop early, so
  // check the latched ctx.aborted flag before finalizing the streamed cache.
  if (ctx.aborted || ImageToFramebufferDecoder::isAborted() || source.readFailed || rc != 1) {
    if (ctx.aborted || ImageToFramebufferDecoder::isAborted()) {
      LOG_DBG("JPG", "JPEG decode aborted after %lu ms", decodeTime);
    } else {
      LOG_ERR("JPG", "Decode failed (rc=%d, lastError=%d)", rc, jpeg->getLastError());
    }
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("JPG", "JPEG decoding complete - render time: %lu ms", decodeTime);

  // Finalize the streamed cache file. Note: a flush failure mid-decode clears
  // ctx.caching (the partial file is dropped), so re-read the flag here.
  if (ctx.caching) {
    cacheWritten = ctx.cache.finalize();
  }

  return !config.cacheOnly || cacheWritten;
}
