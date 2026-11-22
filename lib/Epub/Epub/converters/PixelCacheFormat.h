#pragma once

#include <cstdint>

constexpr uint16_t PXC_MAGIC = 0x5850;
constexpr uint8_t PXC_VERSION = 1;

constexpr uint8_t PXC_V1_SCALING = 1;

struct PixelCacheHeader {
  uint16_t magic;
  uint8_t version;
  uint8_t legacyScaling;
  uint16_t width;
  uint16_t height;

  bool isValid() const {
    return magic == PXC_MAGIC && version == PXC_VERSION && legacyScaling == PXC_V1_SCALING && width != 0 && height != 0;
  }
};

static_assert(sizeof(PixelCacheHeader) == 8);
