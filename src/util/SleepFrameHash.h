#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t sleepFrameAdler32(const uint8_t* data, size_t len) {
  constexpr uint32_t MOD = 65521;
  constexpr size_t MAX_CHUNK = 5552;
  uint32_t a = 1;
  uint32_t b = 0;
  while (len > 0) {
    const size_t chunk = len < MAX_CHUNK ? len : MAX_CHUNK;
    for (size_t i = 0; i < chunk; ++i) {
      a += data[i];
      b += a;
    }
    a %= MOD;
    b %= MOD;
    len -= chunk;
    data += chunk;
  }
  return (b << 16) | a;
}
