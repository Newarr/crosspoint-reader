#pragma once

#include <BuildScratch.h>

#include <cstddef>
#include <memory>
#include <new>

template <typename Decoder>
class DecoderStorage {
 public:
  static bool fitsHeap(size_t heapReserve = 16 * 1024, size_t contiguousReserve = 0) {
    return ESP.getFreeHeap() >= sizeof(Decoder) + heapReserve &&
           ESP.getMaxAllocHeap() >= sizeof(Decoder) + contiguousReserve;
  }

  explicit DecoderStorage(bool allowScratch) {
    if (allowScratch) {
      size_t available = 0;
      scratch = buildscratch::claim(sizeof(Decoder) + alignof(Decoder) - 1, &available);
      if (scratch) {
        void* aligned = scratch;
        std::align(alignof(Decoder), sizeof(Decoder), aligned, available);
        decoder = new (aligned) Decoder();
        return;
      }
    }
    if (fitsHeap()) decoder = new (std::nothrow) Decoder();
  }

  ~DecoderStorage() {
    if (scratch) {
      decoder->~Decoder();
      buildscratch::release(scratch);
    } else {
      delete decoder;
    }
  }

  DecoderStorage(const DecoderStorage&) = delete;
  DecoderStorage& operator=(const DecoderStorage&) = delete;

  Decoder* get() const { return decoder; }

 private:
  uint8_t* scratch = nullptr;
  Decoder* decoder = nullptr;
};
