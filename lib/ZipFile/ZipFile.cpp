#include "ZipFile.h"

#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

struct ZipInflateCtx {
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;
// Central directory header excluding the filename, extra field, and comment.
constexpr size_t CENTRAL_HEADER_BYTES = 46;
constexpr uint32_t CENTRAL_HEADER_SIGNATURE = 0x02014b50;
constexpr uint32_t END_OF_DIRECTORY_SIGNATURE = 0x06054b50;
constexpr uint32_t ZIP64_END_OF_DIRECTORY_SIGNATURE = 0x06064b50;
constexpr uint32_t DIGITAL_SIGNATURE = 0x05054b50;

uint16_t readLe16(const uint8_t* bytes) {
  uint16_t value;
  // memcpy to avoid unaligned access issues
  memcpy(&value, bytes, sizeof(value));
  return value;
}

uint32_t readLe32(const uint8_t* bytes) {
  uint32_t value;
  memcpy(&value, bytes, sizeof(value));
  return value;
}

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

size_t zipFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipInflateCtx*>(vctx);
  if (ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int result = ctx->file->read(ctx->readBuf, toRead);
  // HalFile::read() returns a negative int on error. Treat it as end-of-stream
  // rather than letting the negative-to-size_t conversion underflow fileRemaining
  // and report a huge bytesRead, which would have the inflate library read past
  // the end of readBuf.
  if (result < 0) {
    LOG_ERR("ZIP", "Failed to read compressed data: %d", result);
    return 0;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}
}  // namespace

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (loadZipDetails() != StreamResult::Ok) return false;

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;  // End of list

    FileStatSlim fileStat = {};

    file.seekCur(6);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);

    if (nameLen < sizeof(itemName)) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else {
      // Skip over oversized entry names to avoid writing past fixed buffer.
      file.seekCur(nameLen);
    }

    // Skip the rest of this entry (extra field + comment)
    file.seekCur(m + k);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

ZipFile::StreamResult ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return StreamResult::Ok;
    }
    return StreamResult::NotFound;
  }

  if (lastLookupValid && lastLookupName == filename) {
    *fileStat = lastLookup;
    return StreamResult::Ok;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return StreamResult::IoError;

  const auto details = loadZipDetails();
  if (details != StreamResult::Ok) return details;

  // Phase 1: Try scanning from cursor position first
  const uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  uint32_t entryStart = startPos;
  bool wrapped = false;

  const size_t targetLen = strlen(filename);
  uint8_t header[CENTRAL_HEADER_BYTES];
  char itemName[256];

  while (true) {
    // stop if we've wrapped and reached our start position
    if (wrapped && entryStart >= startPos) break;

    if (!file.seek(entryStart)) return StreamResult::IoError;
    const int headerRead = file.read(header, sizeof(header));
    if (headerRead < static_cast<int>(sizeof(uint32_t))) return StreamResult::IoError;
    const uint32_t signature = readLe32(header);
    if (signature == END_OF_DIRECTORY_SIGNATURE || signature == ZIP64_END_OF_DIRECTORY_SIGNATURE ||
        signature == DIGITAL_SIGNATURE) {
      if (wrapped || startPos == zipDetails.centralDirOffset) break;
      entryStart = zipDetails.centralDirOffset;
      wrapped = true;
      continue;
    }
    if (signature != CENTRAL_HEADER_SIGNATURE || headerRead != static_cast<int>(sizeof(header))) {
      return StreamResult::IoError;
    }

    const uint16_t nameLen = readLe16(header + 28);
    const uint16_t extraLen = readLe16(header + 30);
    const uint16_t commentLen = readLe16(header + 32);
    // skip name, extra field + comment
    const size_t entrySize = sizeof(header) + nameLen + extraLen + commentLen;
    if (entrySize > UINT32_MAX - entryStart) return StreamResult::IoError;
    const uint32_t nextEntry = entryStart + entrySize;

    // Skip names that are too long or cannot match by length
    if (nameLen == targetLen && nameLen < sizeof(itemName)) {
      const int nameRead = file.read(itemName, nameLen);
      if (nameRead != static_cast<int>(nameLen)) {
        LOG_ERR("ZIP", "Failed to read entry name for %s: expected %u bytes, got %d", filename,
                static_cast<unsigned int>(nameLen), nameRead);
        return StreamResult::IoError;
      }
      if (memcmp(itemName, filename, nameLen) == 0) {
        // Found it! Update cursor to next entry
        fileStat->method = readLe16(header + 10);
        fileStat->compressedSize = readLe32(header + 20);
        fileStat->uncompressedSize = readLe32(header + 24);
        fileStat->localHeaderOffset = readLe32(header + 42);
        lastCentralDirPos = nextEntry;
        lastCentralDirPosValid = true;
        lastLookupName.assign(filename, nameLen);
        lastLookup = *fileStat;
        lastLookupValid = true;
        return StreamResult::Ok;
      }
    }

    entryStart = nextEntry;
  }

  return StreamResult::NotFound;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  if (!file.seek(fileOffset)) return -1;
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

ZipFile::StreamResult ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return StreamResult::Ok;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return StreamResult::IoError;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return StreamResult::IoError;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = makeUniqueNoThrow<uint8_t[]>(scanRange);
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return StreamResult::OutOfMemory;
  }

  if (!file.seek(fileSize - scanRange) || file.read(buffer.get(), scanRange) != scanRange) return StreamResult::IoError;

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    if (readLe32(buffer.get() + i) == END_OF_DIRECTORY_SIGNATURE) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    return StreamResult::IoError;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  const auto* eocd = buffer.get() + foundOffset;
  const auto directorySize = readLe32(eocd + 12);
  const auto directoryOffset = readLe32(eocd + 16);
  const size_t eocdOffset = fileSize - scanRange + foundOffset;
  if (directoryOffset > eocdOffset || directorySize > eocdOffset - directoryOffset) return StreamResult::IoError;
  zipDetails.totalEntries = readLe16(eocd + 10);
  zipDetails.centralDirOffset = directoryOffset;
  zipDetails.isSet = true;

  return StreamResult::Ok;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  // the archive may change before the next open
  lastLookupValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (loadFileStatSlim(filename, &fileStat) != StreamResult::Ok) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (loadZipDetails() != StreamResult::Ok) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (loadFileStatSlim(filename, &fileStat) != StreamResult::Ok) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(1024));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      free(data);
      return nullptr;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = 1024;

    // One-shot mode: `data` holds the entire output, so back-references
    // resolve inside it and no 32KB window is allocated.
    InflateStream inflate;
    if (!inflate.init(false)) {
      LOG_ERR("ZIP", "Failed to init inflate stream for %s", filename);
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    inflate.setFill(zipFillCallback, &ctx);

    if (!inflate.read(data, inflatedDataSize)) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    free(fileReadBuffer);

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool allowEarlyStop) {
  return streamFile(filename, out, chunkSize, allowEarlyStop) == StreamResult::Ok;
}

ZipFile::StreamResult ZipFile::streamFile(const char* filename, Print& out, const size_t chunkSize,
                                          const bool allowEarlyStop) {
  if (!chunkSize) return StreamResult::IoError;
  const ScopedOpenClose zip{*this};
  if (!zip) return StreamResult::IoError;

  FileStatSlim stat{};
  const auto lookup = loadFileStatSlim(filename, &stat);
  if (lookup != StreamResult::Ok) return lookup;
  const long fileOffset = getDataOffset(stat);
  if (fileOffset < 0 || !file.seek(fileOffset)) return StreamResult::IoError;
  if (stat.method != ZIP_METHOD_STORED && stat.method != ZIP_METHOD_DEFLATED) {
    LOG_ERR("ZIP", "Unsupported compression method");
    return StreamResult::IoError;
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(chunkSize);
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate ZIP stream buffer");
    return StreamResult::OutOfMemory;
  }

  const auto shortWrite = [&]() {
    if (allowEarlyStop) return StreamResult::Ok;
    LOG_ERR("ZIP", "Failed to write all output bytes to stream");
    return StreamResult::IoError;
  };

  if (stat.method == ZIP_METHOD_STORED) {
    size_t remaining = stat.uncompressedSize;
    while (remaining > 0) {
      const int result = file.read(buffer.get(), std::min(remaining, chunkSize));
      if (result <= 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        return StreamResult::IoError;
      }
      const auto count = static_cast<size_t>(result);
      if (out.write(buffer.get(), count) != count) return shortWrite();
      remaining -= count;
    }
    return StreamResult::Ok;
  }

  auto readBuffer = makeUniqueNoThrow<uint8_t[]>(chunkSize);
  if (!readBuffer) {
    LOG_ERR("ZIP", "Failed to allocate ZIP input buffer");
    return StreamResult::OutOfMemory;
  }
  ZipInflateCtx ctx{&file, stat.compressedSize, readBuffer.get(), chunkSize};
  InflateStream inflate;
  if (!inflate.init(true)) {
    LOG_ERR("ZIP", "Failed to init inflate stream for %s", filename);
    return StreamResult::OutOfMemory;
  }
  inflate.setFill(zipFillCallback, &ctx);

  size_t remaining = stat.uncompressedSize;
  for (;;) {
    size_t produced = 0;
    const InflateStream::Status status = inflate.readAtMost(buffer.get(), chunkSize, &produced);
    if (produced > remaining) {
      LOG_ERR("ZIP", "Decompressed size exceeds expected size");
      return StreamResult::IoError;
    }
    if (produced > 0 && out.write(buffer.get(), produced) != produced) return shortWrite();
    remaining -= produced;
    if (status == InflateStream::Status::Done) {
      if (remaining != 0) {
        LOG_ERR("ZIP", "Decompressed size mismatch (%zu bytes missing)", remaining);
        return StreamResult::IoError;
      }
      return StreamResult::Ok;
    }
    if (status == InflateStream::Status::Error) {
      LOG_ERR("ZIP", "Decompression failed");
      return StreamResult::IoError;
    }
  }
}
