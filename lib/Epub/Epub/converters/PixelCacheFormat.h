#pragma once

#include <cstdint>

constexpr uint16_t PXC_MAGIC = 0x5850;
constexpr uint8_t PXC_VERSION = 1;

enum class PixelCacheLayout { PackedRows, Versioned };

struct PixelCacheHeader {
  uint16_t magic;
  uint8_t version;
  uint8_t bilinearRequested;
  uint16_t width;
  uint16_t height;
};

static_assert(sizeof(PixelCacheHeader) == 8);
