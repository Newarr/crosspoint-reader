#pragma once

#include <Print.h>

#include <cstdint>
#include <cstdio>
#include <string>

class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() override { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool open(const char* path, const char* mode) {
    close();
    file_ = std::fopen(path, mode);
    return file_ != nullptr;
  }
  size_t read(void* buffer, size_t count) { return file_ ? std::fread(buffer, 1, count, file_) : 0; }
  size_t write(uint8_t byte) override { return file_ ? std::fwrite(&byte, 1, 1, file_) : 0; }
  size_t write(const uint8_t* buffer, size_t count) override {
    return file_ ? std::fwrite(buffer, 1, count, file_) : 0;
  }
  size_t write(const void* buffer, size_t count) { return write(static_cast<const uint8_t*>(buffer), count); }
  bool seek(size_t pos) { return file_ && std::fseek(file_, static_cast<long>(pos), SEEK_SET) == 0; }
  bool flush() { return file_ && std::fflush(file_) == 0; }
  size_t position() const { return file_ ? static_cast<size_t>(std::ftell(file_)) : 0; }
  size_t size() const {
    if (!file_) return 0;
    const long offset = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    const long end = std::ftell(file_);
    std::fseek(file_, offset, SEEK_SET);
    return end > 0 ? static_cast<size_t>(end) : 0;
  }
  int available() const { return file_ ? static_cast<int>(size() - position()) : 0; }
  bool isOpen() const { return file_ != nullptr; }
  bool close() {
    if (!file_) return false;
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    return ok;
  }
  explicit operator bool() const { return file_ != nullptr; }

 private:
  std::FILE* file_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "rb"); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "wb"); }
  bool readFileToStream(const char* path, Print& out) {
    HalFile file;
    if (!file.open(path, "rb")) return false;
    uint8_t buf[1024];
    for (;;) {
      const size_t n = file.read(buf, sizeof(buf));
      if (n == 0) break;
      if (out.write(buf, n) != n) return false;
    }
    return true;
  }
  bool remove(const char* path) { return std::remove(path) == 0; }
  bool remove(const std::string& path) { return remove(path.c_str()); }
};

#define Storage HalStorage::getInstance()
