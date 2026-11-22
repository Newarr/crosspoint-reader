#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "../../../Html5/Html5Markup.h"

class ImageDataUriFilter {
 public:
  bool keep(char c) {
    if (opaque.active()) {
      opaque.consume(c);
      return true;
    }
    switch (state) {
      case State::Text:
        if (c == '<') state = State::TagOpen;
        break;
      case State::TagOpen:
        if (opaque.start(c) || c == '>') {
          state = State::Text;
        } else if (c == '/') {
          state = State::ClosingTag;
        } else {
          nameLength = 0;
          appendName(c);
          state = State::TagName;
        }
        break;
      case State::TagName:
        if (c == '>') {
          state = State::Text;
        } else if (isSpace(c) || c == '/') {
          const auto tag = currentName();
          imageTag = tag == "img" || tag == "image";
          state = State::BetweenAttributes;
        } else if (c == ':') {
          nameLength = 0;
        } else {
          appendName(c);
        }
        break;
      case State::ClosingTag:
        if (c == '>') state = State::Text;
        break;
      case State::BetweenAttributes:
      case State::AfterAttributeName:
        if (c == '>') {
          state = State::Text;
        } else if (c == '=' && state == State::AfterAttributeName) {
          beginValue();
        } else if (!isSpace(c) && c != '/') {
          beginAttribute(c);
        }
        break;
      case State::AttributeName:
        if (c == '=') {
          beginValue();
        } else if (c == '>') {
          state = State::Text;
        } else if (isSpace(c)) {
          state = State::AfterAttributeName;
        } else {
          appendName(c);
        }
        break;
      case State::BeforeValue:
        if (c == '"' || c == '\'') {
          quote = c;
          state = State::QuotedValue;
        } else if (c == '>') {
          state = State::Text;
        } else if (!isSpace(c)) {
          state = State::UnquotedValue;
          return keepValue(c);
        }
        break;
      case State::UnquotedValue:
        if (c == '>') {
          state = State::Text;
        } else if (isSpace(c)) {
          state = State::BetweenAttributes;
        } else {
          return keepValue(c);
        }
        break;
      case State::QuotedValue:
        if (c == quote) {
          state = State::BetweenAttributes;
        } else {
          return keepValue(c);
        }
        break;
    }
    return true;
  }

 private:
  enum class State : uint8_t {
    Text,
    TagOpen,
    TagName,
    ClosingTag,
    BetweenAttributes,
    AttributeName,
    AfterAttributeName,
    BeforeValue,
    UnquotedValue,
    QuotedValue
  };
  State state = State::Text;
  html5::OpaqueMarkup opaque;
  char name[10] = {};
  uint8_t nameLength = 0;
  char quote = 0;
  bool imageTag = false;
  bool sourceAttribute = false;
  uint8_t dataPrefixLength = 0;

  static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  void appendName(char c) {
    if (nameLength < sizeof(name))
      name[nameLength++] = html5::lowerAscii(c);
    else
      nameLength = sizeof(name) + 1;
  }

  std::string_view currentName() const {
    return nameLength <= sizeof(name) ? std::string_view(name, nameLength) : std::string_view{};
  }

  void beginAttribute(char c) {
    nameLength = 0;
    appendName(c);
    state = State::AttributeName;
  }

  void beginValue() {
    const auto attribute = currentName();
    sourceAttribute = imageTag && (attribute == "src" || attribute == "href" || attribute == "xlink:href");
    dataPrefixLength = 0;
    state = State::BeforeValue;
  }

  bool keepValue(char c) {
    if (!sourceAttribute) return true;
    if (dataPrefixLength == 5) return false;
    if (html5::lowerAscii(c) == "data:"[dataPrefixLength])
      ++dataPrefixLength;
    else
      sourceAttribute = false;
    return true;
  }
};

class DataUriStripper {
 public:
  static constexpr size_t BUFFER_SIZE = 1024;

  size_t strip(char* buffer, size_t length) {
    previousChunk = currentChunk;
    currentChunk = {filter, xmlEnd, xmlEnd + removedBytes};
    size_t written = 0;
    for (size_t i = 0; i < length; ++i) {
      const char c = buffer[i];
      if (filter.keep(c)) {
        buffer[written++] = c;
      } else {
        recordRemoval(xmlEnd + written);
      }
    }
    xmlEnd += written;
    return written;
  }

  std::optional<size_t> rawOffset(size_t xmlOffset, HalFile& file) const {
    if (removedBytes == 0 || xmlOffset < firstRemoval) return xmlOffset;
    for (size_t i = 0; i < removalCount; ++i) {
      const auto& removal = removals[(nextRemoval + MAX_REMOVALS - 1 - i) % MAX_REMOVALS];
      if (removal.xml <= xmlOffset) return xmlOffset + removal.bytes;
    }

    ChunkStart start;
    if (xmlOffset >= currentChunk.xml) {
      start = currentChunk;
    } else if (xmlOffset >= previousChunk.xml) {
      start = previousChunk;
    }
    if (!file.seek(start.raw)) return std::nullopt;
    char buffer[128];
    size_t raw = start.raw;
    size_t xml = start.xml;
    for (;;) {
      const int length = file.read(buffer, sizeof(buffer));
      if (length <= 0) return std::nullopt;
      for (int i = 0; i < length; ++i, ++raw) {
        if (start.filter.keep(buffer[i]) && xml++ == xmlOffset) return raw;
      }
    }
  }

 private:
  struct ChunkStart {
    ImageDataUriFilter filter;
    size_t xml = 0;
    size_t raw = 0;
  };
  ImageDataUriFilter filter;
  ChunkStart currentChunk;
  ChunkStart previousChunk;

  struct OffsetAdjustment {
    size_t xml;
    size_t bytes;
  };
  static constexpr size_t MAX_REMOVALS = 32;
  OffsetAdjustment removals[MAX_REMOVALS] = {};
  size_t removalCount = 0;
  size_t nextRemoval = 0;
  size_t removedBytes = 0;
  size_t firstRemoval = 0;
  size_t xmlEnd = 0;

  void recordRemoval(size_t xmlOffset) {
    if (removedBytes == 0) firstRemoval = xmlOffset;
    ++removedBytes;
    if (removalCount) {
      auto& latest = removals[(nextRemoval + MAX_REMOVALS - 1) % MAX_REMOVALS];
      if (latest.xml == xmlOffset) {
        latest.bytes = removedBytes;
        return;
      }
    }
    removals[nextRemoval] = {xmlOffset, removedBytes};
    nextRemoval = (nextRemoval + 1) % MAX_REMOVALS;
    if (removalCount < MAX_REMOVALS) ++removalCount;
  }
};
