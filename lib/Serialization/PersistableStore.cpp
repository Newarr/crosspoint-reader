#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

std::mutex jsonFileMutex;

class JsonFileWriter {
  HalFile& file;
  uint8_t buffer[512];
  size_t pending = 0;
  bool failed = false;

 public:
  explicit JsonFileWriter(HalFile& file) : file(file) {}

  size_t write(uint8_t byte) { return write(&byte, 1); }

  size_t write(const uint8_t* data, size_t size) {
    size_t accepted = 0;
    while (!failed && accepted < size) {
      const size_t count = std::min(size - accepted, sizeof(buffer) - pending);
      memcpy(buffer + pending, data + accepted, count);
      pending += count;
      accepted += count;
      if (pending == sizeof(buffer)) flush();
    }
    return accepted;
  }

  bool flush() {
    if (pending && !failed) failed = file.write(buffer, pending) != pending;
    pending = 0;
    return !failed;
  }
};

class JsonFileReader {
  HalFile& file;
  const size_t length;
  uint8_t buffer[512];
  size_t readFromFile = 0;
  size_t position = 0;
  size_t available = 0;
  bool failed = false;

 public:
  explicit JsonFileReader(HalFile& file) : file(file), length(file.size()) {}

  int read() {
    if (position == available) {
      if (failed || readFromFile == length) return -1;
      const size_t count = std::min(length - readFromFile, sizeof(buffer));
      if (file.read(buffer, count) != static_cast<int>(count)) {
        failed = true;
        return -1;
      }
      readFromFile += count;
      available = count;
      position = 0;
    }
    return buffer[position++];
  }

  size_t readBytes(char* output, size_t count) {
    size_t readCount = 0;
    for (; readCount < count; ++readCount) {
      const int byte = read();
      if (byte < 0) break;
      output[readCount] = static_cast<char>(byte);
    }
    return readCount;
  }

  bool atEndOfDocument() {
    for (int byte = read(); byte >= 0; byte = read()) {
      if (byte != ' ' && byte != '\r' && byte != '\n' && byte != '\t') return false;
    }
    return !failed;
  }

  bool hasReadError() const { return failed; }
};

enum class JsonReadResult { Valid, Invalid, Unreadable };

JsonReadResult readJsonFile(const char* path, JsonDocument& doc) {
  HalFile file;
  if (!Storage.openFileForRead("PERSIST", path, file)) return JsonReadResult::Unreadable;
  JsonFileReader reader(file);
  const auto error = deserializeJson(doc, reader);
  if (!error && reader.atEndOfDocument()) return JsonReadResult::Valid;
  LOG_ERR("PERSIST", "JSON read failed for %s: %s", path, error ? error.c_str() : "trailing data");
  return reader.hasReadError() || error == DeserializationError::NoMemory ? JsonReadResult::Unreadable
                                                                          : JsonReadResult::Invalid;
}

}  // namespace

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  std::lock_guard<std::mutex> lock(jsonFileMutex);
  if (doc.overflowed()) {
    LOG_ERR("PERSIST", "Out of memory writing %s", path);
    return false;
  }

  Storage.mkdir("/.crosspoint");
  const std::string temporary = std::string(path) + ".tmp";
  if (!Storage.exists(path) && Storage.exists(temporary.c_str())) {
    JsonDocument pending;
    const auto result = readJsonFile(temporary.c_str(), pending);
    if (result == JsonReadResult::Unreadable ||
        (result == JsonReadResult::Valid && !Storage.rename(temporary.c_str(), path))) {
      LOG_ERR("PERSIST", "Could not preserve pending file for %s", path);
      return false;
    }
  }

  HalFile file;
  if (!Storage.openFileForWrite("PERSIST", temporary, file)) return false;
  JsonFileWriter writer(file);
  const size_t written = serializeJson(doc, writer);
  const bool flushed = writer.flush();
  const bool closed = file.close();
  if (!flushed || !closed || written != measureJson(doc)) {
    Storage.remove(temporary.c_str());
    LOG_ERR("PERSIST", "Incomplete JSON write for %s", path);
    return false;
  }
  if (!Storage.replaceFile(temporary.c_str(), path)) {
    LOG_ERR("PERSIST", "Could not install %s", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::recoverTempFile(const char* path, JsonDocument& doc) {
  const std::string temporary = std::string(path) + ".tmp";
  if (!Storage.exists(temporary.c_str()) || readJsonFile(temporary.c_str(), doc) != JsonReadResult::Valid) return false;
  if (!Storage.replaceFile(temporary.c_str(), path)) {
    LOG_ERR("PERSIST", "Could not restore %s from temp", path);
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  std::lock_guard<std::mutex> lock(jsonFileMutex);
  if (Storage.exists(path)) {
    const auto result = readJsonFile(path, doc);
    if (result == JsonReadResult::Valid) return true;
    if (result == JsonReadResult::Unreadable) return false;
  }
  doc.clear();
  return recoverTempFile(path, doc);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
