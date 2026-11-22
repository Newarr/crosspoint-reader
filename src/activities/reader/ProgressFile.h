#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <esp_crc.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

namespace ProgressFile {
namespace detail {

inline std::mutex progressMutex;
inline constexpr uint32_t MAGIC = 0x31475250;
inline constexpr size_t MAX_PROGRESS_BYTES = 32;

struct Header {
  uint32_t magic;
  uint32_t generation;
  uint32_t length;
  uint32_t crc;
};
static_assert(sizeof(Header) == 16);

struct Record {
  Header header;
  uint8_t data[MAX_PROGRESS_BYTES];
  bool unreadable;
};
inline constexpr Record UNREADABLE_RECORD{.header = {}, .data = {}, .unreadable = true};

inline uint32_t recordCrc(const Header& header, const uint8_t* data) {
  const auto crc = esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&header), offsetof(Header, crc));
  return esp_crc32_le(crc, data, header.length);
}

inline Record readRecord(const std::string& path) {
  if (!Storage.exists(path.c_str())) return {};
  HalFile file;
  Record record{};
  auto& header = record.header;
  if (!Storage.openFileForRead("PRG", path, file)) return UNREADABLE_RECORD;
  const size_t fileSize = file.size();
  if (fileSize < sizeof(header)) return {};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    return UNREADABLE_RECORD;
  }
  if (header.magic != MAGIC || header.generation == 0 || header.length == 0 || header.length > sizeof(record.data) ||
      fileSize != sizeof(header) + header.length) {
    return {};
  }
  if (file.read(record.data, header.length) != static_cast<int>(header.length)) {
    return UNREADABLE_RECORD;
  }
  if (recordCrc(header, record.data) != header.crc) return {};
  return record;
}

inline bool writeFile(const std::string& path, const uint8_t* data, size_t len, const Header* header = nullptr) {
  HalFile file;
  if (!Storage.openFileForWrite("PRG", path, file)) return false;
  const bool written =
      (!header || file.write(header, sizeof(*header)) == sizeof(*header)) && file.write(data, len) == len;
  return file.close() && written;
}

}  // namespace detail

inline size_t readAtomic(const std::string& cachePath, uint8_t* outData, size_t maxLen) {
  if (!outData || maxLen == 0) return 0;
  std::lock_guard<std::mutex> lock(detail::progressMutex);
  const auto a = detail::readRecord(cachePath + "/progress_a.bin");
  const auto b = detail::readRecord(cachePath + "/progress_b.bin");
  const auto& newest = a.header.generation >= b.header.generation ? a : b;
  const auto& older = &newest == &a ? b : a;
  for (const auto* record : {&newest, &older}) {
    const size_t length = record->header.length;
    if (length > 0 && length <= maxLen) {
      memcpy(outData, record->data, length);
      return length;
    }
  }

  HalFile legacy;
  const std::string path = cachePath + "/progress.bin";
  if (!Storage.exists(path.c_str()) || !Storage.openFileForRead("PRG", path, legacy)) return 0;
  const size_t length = legacy.size();
  if (length == 0 || length > maxLen || length > INT32_MAX) return 0;
  return legacy.read(outData, length) == static_cast<int>(length) ? length : 0;
}

inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  if (!data || len == 0 || len > detail::MAX_PROGRESS_BYTES) return false;
  std::lock_guard<std::mutex> lock(detail::progressMutex);
  const std::string pathA = cachePath + "/progress_a.bin";
  const std::string pathB = cachePath + "/progress_b.bin";
  const auto a = detail::readRecord(pathA);
  const auto b = detail::readRecord(pathB);
  if (a.unreadable || b.unreadable) {
    LOG_ERR("PRG", "Could not read progress slots in %s", cachePath.c_str());
    return false;
  }
  const bool aIsNewest = a.header.generation >= b.header.generation;
  const auto& latest = aIsNewest ? a : b;
  if (latest.header.length != len || memcmp(latest.data, data, len) != 0) {
    if (latest.header.generation == UINT32_MAX) return false;
    const auto& destination = aIsNewest ? pathB : pathA;
    detail::Header header{detail::MAGIC, latest.header.generation + 1, static_cast<uint32_t>(len), 0};
    header.crc = detail::recordCrc(header, data);
    if (!detail::writeFile(destination, data, len, &header)) {
      LOG_ERR("PRG", "Failed to save progress in %s", destination.c_str());
      return false;
    }
    const auto saved = detail::readRecord(destination);
    if (saved.header.generation != header.generation || saved.header.length != len || memcmp(saved.data, data, len)) {
      LOG_ERR("PRG", "Failed to verify progress in %s", destination.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace ProgressFile
