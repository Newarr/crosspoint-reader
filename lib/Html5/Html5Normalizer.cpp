/*
MIT License

Copyright (c) 2026 Pavel Liashkov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "Html5Normalizer.h"

#include <BuildScratch.h>
#include <HalStorage.h>
#include <Memory.h>

#include <cctype>
#include <string_view>

#include "Html5Markup.h"

namespace html5 {

namespace {

constexpr size_t MAX_TAG_NAME_LENGTH = 8;
constexpr size_t BUFFER_SIZE = 1024;

enum class State : uint8_t {
  Normal,
  InTagStart,
  InTagName,
  InTagAttrs,
  BeforeValue,
  InUnquotedValue,
  InQuote,
  InClosingTagName,
  InClosingTagRest
};

enum class ReferenceStatus : uint8_t { Pending, Valid, Invalid };

class EntityReference {
 public:
  ReferenceStatus consume(char c) {
    switch (state) {
      case State::Start:
        if (c == '#') {
          state = State::NumericStart;
          return ReferenceStatus::Pending;
        }
        if (!isNameStart(c)) return ReferenceStatus::Invalid;
        state = State::Named;
        return ReferenceStatus::Pending;
      case State::Named:
        if (c == ';') return ReferenceStatus::Valid;
        return isNameStart(c) || isDigit(c) || c == '.' || c == '-' ? ReferenceStatus::Pending
                                                                    : ReferenceStatus::Invalid;
      case State::NumericStart:
        if (c == 'x') {
          state = State::Hex;
          return ReferenceStatus::Pending;
        }
        state = State::Decimal;
        break;
      case State::Decimal:
      case State::Hex:
        break;
    }

    if (c == ';') return haveDigit ? ReferenceStatus::Valid : ReferenceStatus::Invalid;
    const char lower = lowerAscii(c);
    if (!isDigit(c) && !(state == State::Hex && lower >= 'a' && lower <= 'f')) return ReferenceStatus::Invalid;
    haveDigit = true;
    return ReferenceStatus::Pending;
  }

 private:
  enum class State : uint8_t { Start, Named, NumericStart, Decimal, Hex };
  State state = State::Start;
  bool haveDigit = false;

  static bool isDigit(char c) { return c >= '0' && c <= '9'; }
  static bool isNameStart(char c) {
    const char lower = lowerAscii(c);
    return (lower >= 'a' && lower <= 'z') || c == '_' || c == ':' || static_cast<uint8_t>(c) >= 0x80;
  }
};

bool isAttrNameStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }

bool isAttrNameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == ':' || c == '.' || c == '_';
}

}  // namespace

NormalizeResult normalizeHtmlForXml(const std::string& inputPath, const std::string& outputPath,
                                    bool (*shouldAbort)()) {
  if (shouldAbort && shouldAbort()) return NormalizeResult::Aborted;

  auto* scratch = buildscratch::claim(2 * BUFFER_SIZE);
  const ScopedCleanup releaseScratch{[scratch] {
    if (scratch) buildscratch::release(scratch);
  }};
  auto owned = scratch ? nullptr : makeUniqueNoThrow<uint8_t[]>(2 * BUFFER_SIZE);
  auto* readBuffer = scratch ? scratch : owned.get();
  if (!readBuffer) return NormalizeResult::OutOfMemory;
  auto* writeBuffer = readBuffer + BUFFER_SIZE;

  HalFile inFile, outFile;
  if (!Storage.openFileForRead("H5N", inputPath, inFile)) return NormalizeResult::Error;
  if (!Storage.openFileForWrite("H5N", outputPath, outFile)) {
    if (outFile.isOpen()) outFile.close();
    Storage.remove(outputPath.c_str());
    return NormalizeResult::Error;
  }

  State state = State::Normal;
  char tagName[MAX_TAG_NAME_LENGTH + 1] = {0};
  size_t tagNameLen = 0;
  char closingTagWhitespace[8] = {0};
  size_t closingTagWsLen = 0;
  bool isCurrentTagVoid = false;
  char quoteChar = 0;
  char prevChar = 0;
  OpaqueMarkup opaque;

  bool inAttrName = false;
  bool afterAttrName = false;

  NormalizeResult result = NormalizeResult::Error;
  const auto aborted = [&]() {
    if (!shouldAbort || !shouldAbort()) return false;
    result = NormalizeResult::Aborted;
    return true;
  };
  size_t writePos = 0;
  size_t remaining = inFile.size();

  auto flushWrite = [&]() -> bool {
    if (writePos > 0) {
      if (outFile.write(writeBuffer, writePos) != writePos) {
        return false;
      }
      writePos = 0;
    }
    return true;
  };

  auto writeChar = [&](char c) -> bool {
    writeBuffer[writePos++] = static_cast<uint8_t>(c);
    if (writePos >= BUFFER_SIZE) {
      return flushWrite();
    }
    return true;
  };

  const auto writeText = [&](std::string_view text) {
    for (const char c : text) {
      if (!writeChar(c)) return false;
    }
    return true;
  };

  const auto writeValueChar = [&](char c, const uint8_t* next, const uint8_t* end) {
    if (c != '&') return writeChar(c);
    EntityReference reference;
    auto status = ReferenceStatus::Pending;
    while (next != end && status == ReferenceStatus::Pending) {
      status = reference.consume(static_cast<char>(*next++));
    }
    if (status == ReferenceStatus::Pending && remaining > 0) {
      if (!flushWrite()) return false;
      const auto resume = inFile.position();
      size_t left = remaining;
      while (left > 0 && status == ReferenceStatus::Pending) {
        if (aborted()) return false;
        const size_t wanted = left < BUFFER_SIZE ? left : BUFFER_SIZE;
        const int read = inFile.read(writeBuffer, wanted);
        if (read <= 0 || static_cast<size_t>(read) > wanted) return false;
        left -= static_cast<size_t>(read);
        for (int i = 0; i < read && status == ReferenceStatus::Pending; ++i) {
          status = reference.consume(static_cast<char>(writeBuffer[i]));
        }
      }
      if (!inFile.seek(resume)) return false;
    }
    return status == ReferenceStatus::Valid ? writeChar('&') : writeText("&amp;");
  };

  const auto finishAttr = [&](bool hasValue) {
    inAttrName = false;
    afterAttrName = false;
    return hasValue || writeText("=\"\"");
  };

  auto closeCurrentTag = [&]() -> bool {
    if (isCurrentTagVoid && prevChar != '/') {
      if (!writeChar(' ')) return false;
      if (!writeChar('/')) return false;
    }
    return writeChar('>');
  };

  while (remaining > 0) {
    if (aborted()) goto error;
    const int bytesRead = inFile.read(readBuffer, BUFFER_SIZE);
    if (bytesRead <= 0 || static_cast<size_t>(bytesRead) > remaining) goto error;
    remaining -= static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; i++) {
      const char c = static_cast<char>(readBuffer[i]);
      if (opaque.active()) {
        opaque.consume(c);
        if (!writeChar(c)) goto error;
        continue;
      }
      const auto writeValue = [&] { return writeValueChar(c, readBuffer + i + 1, readBuffer + bytesRead); };

      switch (state) {
        case State::Normal:
          if (c == '<') {
            state = State::InTagStart;
            tagNameLen = 0;
            isCurrentTagVoid = false;
          } else if (!writeValue()) {
            goto error;
          }
          break;

        case State::InTagStart:
          if (c == '/') {
            state = State::InClosingTagName;
            tagNameLen = 0;
            closingTagWsLen = 0;
          } else if (opaque.start(c)) {
            state = State::Normal;
            if (!writeChar('<') || !writeChar(c)) goto error;
          } else if (std::isalpha(static_cast<unsigned char>(c))) {
            state = State::InTagName;
            tagName[0] = c;
            tagNameLen = 1;
            if (!writeChar('<')) goto error;
            if (!writeChar(c)) goto error;
          } else {
            state = State::Normal;
            if (!writeChar('<')) goto error;
            if (!writeChar(c)) goto error;
          }
          break;

        case State::InTagName:
          if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == ':') {
            if (tagNameLen < MAX_TAG_NAME_LENGTH) {
              tagName[tagNameLen++] = c;
            } else {
              tagNameLen = MAX_TAG_NAME_LENGTH + 1;
            }
            if (!writeChar(c)) goto error;
          } else {
            isCurrentTagVoid = isVoidElement({tagName, tagNameLen});

            if (c == '>') {
              if (!closeCurrentTag()) goto error;
              state = State::Normal;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
              state = State::InTagAttrs;
              inAttrName = false;
              if (!writeChar(c)) goto error;
            } else if (c == '/') {
              if (!writeChar(c)) goto error;
              state = State::InTagAttrs;
              inAttrName = false;
            } else {
              if (!writeChar(c)) goto error;
              state = State::Normal;
            }
          }
          break;

        case State::InTagAttrs:
          if (c == '"' || c == '\'') {
            if (inAttrName) {
              if (!finishAttr(false)) goto error;
            }
            state = State::InQuote;
            quoteChar = c;
            if (!writeChar(c)) goto error;
          } else if (c == '=') {
            if (!finishAttr(true) || !writeChar(c)) goto error;
            state = State::BeforeValue;
          } else if (c == '>') {
            if (inAttrName) {
              if (!finishAttr(false)) goto error;
            }
            if (!closeCurrentTag()) goto error;
            state = State::Normal;
          } else if (c == '<') {
            if (inAttrName) {
              if (!finishAttr(false)) goto error;
            }
            if (!closeCurrentTag()) goto error;
            state = State::InTagStart;
            tagNameLen = 0;
            isCurrentTagVoid = false;
          } else if (isAttrNameStart(c) && (!inAttrName || afterAttrName)) {
            if (afterAttrName && (!finishAttr(false) || !writeChar(' '))) goto error;
            if (prevChar == '"' || prevChar == '\'') {
              if (!writeChar(' ')) goto error;
            }
            inAttrName = true;
            if (!writeChar(c)) goto error;
          } else if (inAttrName && isAttrNameChar(c)) {
            if (!writeChar(c)) goto error;
          } else if (c == '/') {
            if (inAttrName && !finishAttr(false)) goto error;
            if (!writeChar(c)) goto error;
          } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (inAttrName) {
              afterAttrName = true;
            } else if (!writeChar(c)) {
              goto error;
            }
          } else {
            if (inAttrName) {
              if (!finishAttr(true)) goto error;
            }
            if (!writeChar(c)) goto error;
          }
          break;

        case State::BeforeValue:
          if (std::isspace(static_cast<unsigned char>(c))) {
            if (!writeChar(c)) goto error;
          } else if (c == '"' || c == '\'') {
            if (!writeChar(c)) goto error;
            quoteChar = c;
            state = State::InQuote;
          } else if (c == '>' || c == '<') {
            if (!writeChar('"') || !writeChar('"') || !closeCurrentTag()) goto error;
            state = c == '>' ? State::Normal : State::InTagStart;
          } else {
            if (!writeChar('"') || !writeValue()) goto error;
            state = State::InUnquotedValue;
          }
          break;

        case State::InUnquotedValue:
          if (c == '>' || c == '<') {
            if (!writeChar('"')) goto error;
            if (isCurrentTagVoid && !writeChar('/')) goto error;
            if (!writeChar('>')) goto error;
            state = c == '>' ? State::Normal : State::InTagStart;
          } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!writeChar('"') || !writeChar(c)) goto error;
            state = State::InTagAttrs;
          } else if (c == '"') {
            for (char ch : std::string_view("&quot;")) {
              if (!writeChar(ch)) goto error;
            }
          } else if (!writeValue()) {
            goto error;
          }
          break;

        case State::InQuote:
          if (c == quoteChar) {
            state = State::InTagAttrs;
            inAttrName = false;
            if (!writeChar(c)) goto error;
          } else if (c == '<') {
            if (!writeChar('&')) goto error;
            if (!writeChar('l')) goto error;
            if (!writeChar('t')) goto error;
            if (!writeChar(';')) goto error;
          } else if (!writeValue()) {
            goto error;
          }
          break;

        case State::InClosingTagName:
          if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == ':') {
            if (tagNameLen < MAX_TAG_NAME_LENGTH) {
              tagName[tagNameLen++] = c;
            } else {
              if (!writeChar('<')) goto error;
              if (!writeChar('/')) goto error;
              for (size_t j = 0; j < tagNameLen; j++) {
                if (!writeChar(tagName[j])) goto error;
              }
              if (!writeChar(c)) goto error;
              state = State::InClosingTagRest;
            }
          } else if (c == '>') {
            tagName[tagNameLen] = '\0';
            if (isVoidElement({tagName, tagNameLen})) {
              // Skip closing tag for void elements
            } else {
              if (!writeChar('<')) goto error;
              if (!writeChar('/')) goto error;
              for (size_t j = 0; j < tagNameLen; j++) {
                if (!writeChar(tagName[j])) goto error;
              }
              for (size_t j = 0; j < closingTagWsLen; j++) {
                if (!writeChar(closingTagWhitespace[j])) goto error;
              }
              if (!writeChar('>')) goto error;
            }
            state = State::Normal;
          } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (closingTagWsLen < sizeof(closingTagWhitespace)) {
              closingTagWhitespace[closingTagWsLen++] = c;
            }
          } else {
            if (!writeChar('<')) goto error;
            if (!writeChar('/')) goto error;
            for (size_t j = 0; j < tagNameLen; j++) {
              if (!writeChar(tagName[j])) goto error;
            }
            if (!writeChar(c)) goto error;
            state = State::Normal;
          }
          break;

        case State::InClosingTagRest:
          if (!writeChar(c)) goto error;
          if (c == '>') state = State::Normal;
          break;
      }

      prevChar = c;
    }
  }

  if (state == State::InTagAttrs || state == State::InTagName) {
    if (inAttrName) {
      if (!finishAttr(false)) goto error;
    }
    if (!closeCurrentTag()) goto error;
  } else if (state == State::BeforeValue || state == State::InUnquotedValue || state == State::InQuote) {
    if (state == State::BeforeValue && !writeChar('"')) goto error;
    if (!writeChar(state == State::InQuote ? quoteChar : '"')) goto error;
    if (!closeCurrentTag()) goto error;
  } else if (state == State::InTagStart) {
    if (!writeChar('<')) goto error;
  } else if (state == State::InClosingTagName) {
    if (!writeChar('<')) goto error;
    if (!writeChar('/')) goto error;
    for (size_t j = 0; j < tagNameLen; j++) {
      if (!writeChar(tagName[j])) goto error;
    }
    for (size_t j = 0; j < closingTagWsLen; j++) {
      if (!writeChar(closingTagWhitespace[j])) goto error;
    }
  }

  if (aborted() || !flushWrite() || !outFile.close()) goto error;
  return NormalizeResult::Ok;

error:
  if (outFile.isOpen()) outFile.close();
  Storage.remove(outputPath.c_str());
  return result;
}

}  // namespace html5
