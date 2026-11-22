#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

class DataUriStripper {
 public:
  static constexpr size_t BUFFER_SIZE = 1024;

  size_t strip(char* buffer, size_t length) {
    size_t written = 0;
    for (size_t i = 0; i < length; ++i) {
      const char c = buffer[i];
      if (keep(c)) {
        buffer[written++] = c;
      } else {
        recordRemoval(xmlEnd + written);
      }
    }
    xmlEnd += written;
    return written;
  }

  std::optional<size_t> rawOffset(size_t xmlOffset) const {
    if (removedBytes == 0 || xmlOffset < firstRemoval) return xmlOffset;
    for (size_t i = 0; i < removalCount; ++i) {
      const auto& removal = removals[(nextRemoval + MAX_REMOVALS - 1 - i) % MAX_REMOVALS];
      if (removal.xml <= xmlOffset) return xmlOffset + removal.bytes;
    }
    return std::nullopt;
  }

 private:
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

  enum class State : uint8_t { Text, TagOpen, TagName, Attribute, Quote, Declaration, Comment, Cdata, Instruction };
  State state = State::Text;
  char quote = 0;
  uint8_t attributeLength = 0;
  bool sourceAttribute = false;
  bool afterEquals = false;
  bool afterAttributeName = false;
  uint8_t dataPrefixLength = 0;
  char previous = 0;
  char beforePrevious = 0;
  uint8_t declarationPrefixLength = 0;
  bool commentPrefix = false;
  bool cdataPrefix = false;
  bool inSubset = false;

  static bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
  static char lower(const char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

  void resetAttribute() {
    attributeLength = 0;
    sourceAttribute = false;
    afterEquals = false;
    afterAttributeName = false;
  }

  bool keep(const char c) {
    switch (state) {
      case State::Text:
        if (c == '<') state = State::TagOpen;
        break;
      case State::TagOpen:
        if (c == '!') {
          state = State::Declaration;
          declarationPrefixLength = 0;
          commentPrefix = cdataPrefix = true;
          inSubset = false;
        } else if (c == '?') {
          state = State::Instruction;
          previous = 0;
        } else {
          state = c == '>' ? State::Text : State::TagName;
        }
        break;
      case State::TagName:
        if (c == '>') {
          state = State::Text;
        } else if (isSpace(c)) {
          state = State::Attribute;
          resetAttribute();
        }
        break;
      case State::Attribute:
        if (c == '>') {
          state = State::Text;
        } else if (c == '"' || c == '\'') {
          state = State::Quote;
          quote = c;
          dataPrefixLength = sourceAttribute && afterEquals ? 0 : 6;
        } else if (c == '=') {
          sourceAttribute = sourceAttribute && attributeLength == 3;
          afterEquals = true;
          attributeLength = 0;
        } else if (isSpace(c)) {
          afterAttributeName = !afterEquals;
        } else {
          if (attributeLength == 0 || afterAttributeName) {
            sourceAttribute = !afterEquals;
            attributeLength = 0;
            afterEquals = false;
            afterAttributeName = false;
          }
          if (attributeLength < 3) {
            sourceAttribute = sourceAttribute && lower(c) == "src"[attributeLength];
            ++attributeLength;
          } else {
            sourceAttribute = false;
          }
        }
        break;
      case State::Quote:
        if (c == quote) {
          quote = 0;
          state = State::Attribute;
          resetAttribute();
        } else if (dataPrefixLength == 5) {
          return false;
        } else if (dataPrefixLength < 5) {
          dataPrefixLength = lower(c) == "data:"[dataPrefixLength] ? dataPrefixLength + 1 : 6;
        }
        break;
      case State::Declaration:
        if (commentPrefix || cdataPrefix) {
          commentPrefix = commentPrefix && c == "--"[declarationPrefixLength];
          cdataPrefix = cdataPrefix && c == "[CDATA["[declarationPrefixLength];
          ++declarationPrefixLength;
          if (commentPrefix && declarationPrefixLength == 2) {
            state = State::Comment;
            previous = beforePrevious = 0;
          } else if (cdataPrefix && declarationPrefixLength == 7) {
            state = State::Cdata;
            previous = beforePrevious = 0;
          }
          if (commentPrefix || cdataPrefix) break;
        }
        if (quote) {
          if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
          quote = c;
        } else if (c == '[' || c == ']') {
          inSubset = c == '[';
        } else if (c == '>' && !inSubset) {
          state = State::Text;
        }
        break;
      case State::Comment:
      case State::Cdata:
      case State::Instruction: {
        const char delimiter = state == State::Comment ? '-' : state == State::Cdata ? ']' : '?';
        if (c == '>' && previous == delimiter && (state == State::Instruction || beforePrevious == delimiter)) {
          state = State::Text;
        }
        beforePrevious = previous;
        previous = c;
        break;
      }
    }
    return true;
  }
};
