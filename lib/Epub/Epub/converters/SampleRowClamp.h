#pragma once

#include <algorithm>
#include <cstdint>

inline constexpr uint32_t SAMPLE_ONE = 1u << 16;
inline constexpr uint32_t SAMPLE_MASK = SAMPLE_ONE - 1;

inline constexpr uint32_t samplePosition(int destination, uint32_t step) {
  const uint32_t offset = step > SAMPLE_ONE ? (step - SAMPLE_ONE) / 2 : 0;
  return static_cast<uint32_t>(destination) * step + offset;
}

struct SamplePair {
  int first;
  int second;
  uint32_t fraction;
};

inline constexpr SamplePair samplePair(uint32_t source, int origin, int extent) {
  const int index = static_cast<int>(source >> 16) - origin;
  return {std::clamp(index, 0, extent - 1), std::clamp(index + 1, 0, extent - 1), source & SAMPLE_MASK};
}

inline constexpr uint32_t interpolateSample(uint32_t first, uint32_t second, uint32_t fraction) {
  return (first * (SAMPLE_ONE - fraction) + second * fraction + SAMPLE_ONE / 2) >> 16;
}
