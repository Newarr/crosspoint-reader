#pragma once

#include <string>

#include "Epub.h"

namespace serialization {

inline void writeString(HalFile&, const std::string&) {}
inline bool tryWriteString(HalFile&, const std::string&) { return true; }
inline void readString(HalFile&, std::string& out) { out.clear(); }
inline bool tryReadString(HalFile&, std::string& out, size_t = 0) {
  out.clear();
  return false;
}

}  // namespace serialization
