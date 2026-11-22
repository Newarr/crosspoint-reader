#pragma once

#include "BlueNoise64.h"

namespace imagebake {

inline uint8_t blueNoiseAt(int x, int y) noexcept {
  return ttfimg::kBlueNoise64[static_cast<unsigned>(y) & 63u][static_cast<unsigned>(x) & 63u];
}

inline uint8_t quantizeGray2bit(int x, int y, uint8_t gray) noexcept {
  return ttfimg::blueNoiseQuantize(gray, blueNoiseAt(x, y));
}

inline uint8_t quantizeGray1bit(int x, int y, uint8_t gray) noexcept {
  const int threshold = 128 + (static_cast<int>(blueNoiseAt(x, y)) - 128) / 2;
  return gray >= threshold;
}

}  // namespace imagebake
