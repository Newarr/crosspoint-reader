#pragma once

#include <string_view>

#include "Epub/htmlEntities.h"

inline std::string_view chapterXmlEntityText(std::string_view token) {
  if (token.size() < 3 || token.front() != '&' || token.back() != ';') return {};
  const char* decoded = lookupHtmlEntity(token.data(), token.size());
  return decoded ? std::string_view(decoded) : token;
}
