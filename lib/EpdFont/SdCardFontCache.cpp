#include "SdCardFontCache.h"

#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
#include <HalOtaSlot.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <mutex>

#include "SdCardFontCacheFormat.h"

namespace SdCardFontCache {
namespace {
using namespace sd_card_font_cache_format;

static_assert(HEADER_AREA_SIZE == HalOtaSlot::ERASE_SIZE);
constexpr size_t COPY_BYTES = HalOtaSlot::ERASE_SIZE;
constexpr size_t ERASE_BLOCK_BYTES = 64 * 1024;

std::mutex cacheMutex;
uint32_t generation = 1;

bool readHeader(const HalOtaSlot& slot, Header& header) {
  return slot.size() > HEADER_AREA_SIZE && slot.read(0, &header, sizeof(header)) &&
         isHeaderValid(header, slot.size() - HEADER_AREA_SIZE);
}

bool checksum(const HalOtaSlot& slot, HalFile* source, uint8_t* buffer, const size_t size, uint32_t& crc) {
  crc = 0;
  for (size_t offset = 0; offset < size; offset += COPY_BYTES) {
    const size_t length = std::min(COPY_BYTES, size - offset);
    if (source) {
      if (source->read(buffer, length) != static_cast<int>(length)) return false;
    } else if (!slot.read(HEADER_AREA_SIZE + offset, buffer, length)) {
      return false;
    }
    crc = esp_rom_crc32_le(crc, buffer, length);
  }
  return true;
}

bool copyToFlash(const HalOtaSlot& slot, HalFile& source, uint8_t* buffer, Header& header) {
  if (!source.seekSet(0) || !slot.erase(0, HEADER_AREA_SIZE)) return false;

  header.payloadCrc = 0;
  for (size_t offset = 0; offset < header.payloadSize;) {
    const size_t remaining = header.payloadSize - offset;
    const size_t toBlockEnd = ERASE_BLOCK_BYTES - (HEADER_AREA_SIZE + offset) % ERASE_BLOCK_BYTES;
    const size_t eraseBytes = std::min((remaining + COPY_BYTES - 1) / COPY_BYTES * COPY_BYTES, toBlockEnd);
    if (!slot.erase(HEADER_AREA_SIZE + offset, eraseBytes)) return false;

    const size_t end = offset + std::min(remaining, eraseBytes);
    while (offset < end) {
      const size_t length = std::min(COPY_BYTES, end - offset);
      if (source.read(buffer, length) != static_cast<int>(length) ||
          !slot.write(HEADER_AREA_SIZE + offset, buffer, length)) {
        return false;
      }
      header.payloadCrc = esp_rom_crc32_le(header.payloadCrc, buffer, length);
      offset += length;
    }
  }

  uint32_t flashCrc;
  if (!checksum(slot, nullptr, buffer, header.payloadSize, flashCrc) || flashCrc != header.payloadCrc) return false;
  header.headerCrc = headerCrc(header);
  return slot.write(0, &header, sizeof(header));
}

}  // namespace

ReadHandle prepare(const char* path, const size_t fileSize) {
  std::lock_guard lock(cacheMutex);
  const HalOtaSlot slot = HalOtaSlot::inactive();
  if (!slot.safeForScratchWrite() || slot.size() <= HEADER_AREA_SIZE || fileSize == 0 ||
      fileSize > slot.size() - HEADER_AREA_SIZE || strlen(path) >= sizeof(Header::sourcePath)) {
    return {};
  }

  HalFile source;
  if (!Storage.openFileForRead("SDFCACHE", path, source) || source.fileSize() != fileSize) return {};
  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BYTES);
  if (!buffer) return {};

  Header header{};
  if (readHeader(slot, header) && header.payloadSize == fileSize && strcmp(header.sourcePath, path) == 0) {
    uint32_t sourceCrc;
    uint32_t flashCrc;
    if (!checksum(slot, &source, buffer.get(), fileSize, sourceCrc)) return {};
    if (sourceCrc == header.payloadCrc && checksum(slot, nullptr, buffer.get(), fileSize, flashCrc) &&
        flashCrc == header.payloadCrc) {
      return {generation, fileSize};
    }
  }

  if (++generation == 0) ++generation;
  header = {};
  memcpy(header.magic, MAGIC, sizeof(MAGIC));
  header.version = VERSION;
  header.headerSize = sizeof(header);
  header.payloadSize = fileSize;
  memcpy(header.sourcePath, path, strlen(path) + 1);
  if (!copyToFlash(slot, source, buffer.get(), header) || !readHeader(slot, header)) {
    LOG_ERR("SDFCACHE", "Failed to cache %s", path);
    return {};
  }
  LOG_DBG("SDFCACHE", "Cached %s (%zu bytes)", path, fileSize);
  return {generation, fileSize};
}

bool isCurrent(const ReadHandle handle) {
  std::lock_guard lock(cacheMutex);
  return handle.generation != 0 && handle.generation == generation;
}

bool readAt(const ReadHandle handle, const size_t offset, void* data, const size_t length) {
  std::lock_guard lock(cacheMutex);
  if (handle.generation == 0 || handle.generation != generation || !containsPayloadRange(handle.size, offset, length)) {
    return false;
  }
  static const HalOtaSlot slot = HalOtaSlot::inactive();
  uint8_t magic[sizeof(MAGIC)];
  const auto hasCacheHeader = [&] {
    return slot.read(0, magic, sizeof(magic)) && memcmp(magic, MAGIC, sizeof(magic)) == 0;
  };
  return hasCacheHeader() && slot.read(HEADER_AREA_SIZE + offset, data, length) && hasCacheHeader();
}

}  // namespace SdCardFontCache
#endif
