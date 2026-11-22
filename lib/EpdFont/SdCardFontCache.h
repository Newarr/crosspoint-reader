#pragma once

#include <cstddef>
#include <cstdint>

namespace SdCardFontCache {

struct ReadHandle {
  uint32_t generation = 0;
  size_t size = 0;
};

#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
ReadHandle prepare(const char* path, size_t fileSize);
bool readAt(ReadHandle handle, size_t offset, void* data, size_t length);
#else
inline ReadHandle prepare(const char*, size_t) { return {}; }
inline bool readAt(ReadHandle, size_t, void*, size_t) { return false; }
#endif

}  // namespace SdCardFontCache
