#pragma once

#include <stdint.h>

#include <string>

#include "ImageToFramebufferDecoder.h"
#include "PixelCacheFormat.h"

class JpegToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool decoderFitsHeap();
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;
  // The sleep-cover reader consumes a temporary width/height prefix.
  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config,
                           bool& cacheWritten);

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "JPEG"; }

 private:
  bool decode(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config, PixelCacheLayout layout,
              bool& cacheWritten);
};
