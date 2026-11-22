#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <esp_crc.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>

namespace ProgressFile {

enum class ReadStatus : uint8_t { Ok, CorruptFallback, IoError, Missing };

struct Version {
  uint32_t generation = 0;
  std::optional<uint32_t> legacyCrc;
  std::optional<uint32_t> pendingCrc;
};

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
  ReadStatus status = ReadStatus::Missing;
};
inline constexpr Record UNREADABLE_RECORD{.header = {}, .data = {}, .status = ReadStatus::IoError};
inline constexpr Record CORRUPT_RECORD{.header = {}, .data = {}, .status = ReadStatus::CorruptFallback};

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
  const uint64_t fileSize = file.fileSize64();
  if (fileSize < sizeof(header)) return CORRUPT_RECORD;
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    return UNREADABLE_RECORD;
  }
  if (header.magic != MAGIC || header.generation == 0 || header.length == 0 || header.length > sizeof(record.data) ||
      fileSize != sizeof(header) + header.length) {
    return CORRUPT_RECORD;
  }
  if (file.read(record.data, header.length) != static_cast<int>(header.length)) {
    return UNREADABLE_RECORD;
  }
  if (recordCrc(header, record.data) != header.crc) return CORRUPT_RECORD;
  record.status = ReadStatus::Ok;
  return record;
}

inline ReadStatus readLegacy(const std::string& path, uint8_t* data, size_t maxLen, size_t& size) {
  size = 0;
  if (!Storage.exists(path.c_str())) return ReadStatus::Missing;
  HalFile file;
  if (!Storage.openFileForRead("PRG", path, file)) return ReadStatus::IoError;
  const uint64_t length = file.fileSize64();
  if (length == 0 || length > maxLen || length > INT32_MAX) return ReadStatus::CorruptFallback;
  if (file.read(data, static_cast<size_t>(length)) != static_cast<int>(length)) return ReadStatus::IoError;
  size = static_cast<size_t>(length);
  return ReadStatus::Ok;
}

inline bool writeRecord(const std::string& path, const Header& header, const uint8_t* data, bool createOnly) {
  HalFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | (createOnly ? O_EXCL : O_TRUNC));
  if (!file) return false;
  const bool written =
      file.write(&header, sizeof(header)) == sizeof(header) && file.write(data, header.length) == header.length;
  return file.close() && written;
}

}  // namespace detail

inline ReadStatus readAtomic(const std::string& cachePath, uint8_t* outData, size_t maxLen, size_t& size,
                             Version* version = nullptr) {
  size = 0;
  if (version) *version = {};
  if (!outData || maxLen == 0) return ReadStatus::IoError;
  std::lock_guard<std::mutex> lock(detail::progressMutex);
  const auto a = detail::readRecord(cachePath + "/progress_a.bin");
  const auto b = detail::readRecord(cachePath + "/progress_b.bin");
  const bool unreadable = a.status == ReadStatus::IoError || b.status == ReadStatus::IoError;
  const bool corrupt = a.status == ReadStatus::CorruptFallback || b.status == ReadStatus::CorruptFallback ||
                       a.header.length > maxLen || b.header.length > maxLen;
  const auto recordStatus = unreadable ? ReadStatus::IoError : corrupt ? ReadStatus::CorruptFallback : ReadStatus::Ok;
  const auto& newest = a.header.generation >= b.header.generation ? a : b;
  const auto& older = &newest == &a ? b : a;
  if (version) version->generation = newest.header.generation;
  for (const auto* record : {&newest, &older}) {
    const size_t length = record->header.length;
    if (length > 0 && length <= maxLen) {
      memcpy(outData, record->data, length);
      size = length;
      return recordStatus;
    }
  }

  const auto legacyStatus = detail::readLegacy(cachePath + "/progress.bin", outData, maxLen, size);
  if (unreadable || legacyStatus == ReadStatus::IoError) return ReadStatus::IoError;
  if (legacyStatus == ReadStatus::Ok && version) version->legacyCrc = esp_crc32_le(0, outData, size);
  return corrupt ? ReadStatus::CorruptFallback : legacyStatus;
}

inline size_t readAtomic(const std::string& cachePath, uint8_t* outData, size_t maxLen) {
  size_t size = 0;
  readAtomic(cachePath, outData, maxLen, size);
  return size;
}

inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len, Version* version = nullptr) {
  if (!data || len == 0 || len > detail::MAX_PROGRESS_BYTES) return false;
  std::lock_guard<std::mutex> lock(detail::progressMutex);
  const std::string pathA = cachePath + "/progress_a.bin";
  const std::string pathB = cachePath + "/progress_b.bin";
  const auto a = detail::readRecord(pathA);
  const auto b = detail::readRecord(pathB);
  if (a.status == ReadStatus::IoError || b.status == ReadStatus::IoError) {
    LOG_ERR("PRG", "Could not read progress slots in %s", cachePath.c_str());
    return false;
  }
  const bool aIsNewest = a.header.generation >= b.header.generation;
  const auto& latest = aIsNewest ? a : b;
  if (version) {
    if (version->pendingCrc && latest.header.generation == version->generation + 1 &&
        latest.header.crc == *version->pendingCrc) {
      version->generation = latest.header.generation;
      version->pendingCrc.reset();
    }
    if (latest.header.generation != version->generation) {
      LOG_ERR("PRG", "Saved progress changed in %s", cachePath.c_str());
      return false;
    }
    if (version->generation == 0) {
      uint8_t legacyData[detail::MAX_PROGRESS_BYTES];
      size_t size = 0;
      const auto status = detail::readLegacy(cachePath + "/progress.bin", legacyData, len, size);
      const auto crc = status == ReadStatus::Ok ? std::optional{esp_crc32_le(0, legacyData, size)} : std::nullopt;
      if (status == ReadStatus::IoError || crc != version->legacyCrc) {
        LOG_ERR("PRG", "Could not verify legacy progress in %s", cachePath.c_str());
        return false;
      }
    }
  }
  if (latest.header.length != len || memcmp(latest.data, data, len) != 0) {
    if (latest.header.generation == UINT32_MAX) return false;
    const auto& destination = aIsNewest ? pathB : pathA;
    const bool createOnly = (aIsNewest ? b : a).status == ReadStatus::Missing;
    detail::Header header{detail::MAGIC, latest.header.generation + 1, static_cast<uint32_t>(len), 0};
    header.crc = detail::recordCrc(header, data);
    if (!detail::writeRecord(destination, header, data, createOnly)) {
      LOG_ERR("PRG", "Failed to save progress in %s", destination.c_str());
      return false;
    }
    if (version) version->pendingCrc = header.crc;
    const auto saved = detail::readRecord(destination);
    if (saved.header.generation != header.generation || saved.header.length != len || memcmp(saved.data, data, len)) {
      LOG_ERR("PRG", "Failed to verify progress in %s", destination.c_str());
      return false;
    }
    if (version) {
      version->generation = saved.header.generation;
      version->pendingCrc.reset();
    }
  }
  return true;
}

}  // namespace ProgressFile
