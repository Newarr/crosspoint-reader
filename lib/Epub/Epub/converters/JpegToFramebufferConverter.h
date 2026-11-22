#pragma once

#include <stdint.h>

#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool decoderFitsHeap();
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;
  bool decodeWithCacheResult(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config,
                             bool& cacheWritten);

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }
};
