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

#include <algorithm>
#include <cctype>
#include <string_view>

namespace html5 {

namespace {

constexpr const char* VOID_ELEMENTS[] = {"img",  "br",  "hr",    "input", "meta",   "link",  "area",
                                         "base", "col", "embed", "param", "source", "track", "wbr"};
constexpr size_t VOID_ELEMENT_COUNT = sizeof(VOID_ELEMENTS) / sizeof(VOID_ELEMENTS[0]);
constexpr size_t MAX_TAG_NAME_LENGTH = 8;
constexpr size_t MAX_ATTR_NAME_LENGTH = 32;
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
  InClosingTagRest,
  Declaration,
  Comment,
  Cdata,
  Instruction
};

char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

bool isVoidElement(const char* name, size_t len) {
  for (size_t i = 0; i < VOID_ELEMENT_COUNT; i++) {
    const char* ve = VOID_ELEMENTS[i];
    size_t veLen = 0;
    while (ve[veLen] != '\0') veLen++;
    if (len == veLen) {
      bool match = true;
      for (size_t j = 0; j < len && match; j++) {
        if (toLowerAscii(name[j]) != ve[j]) match = false;
      }
      if (match) return true;
    }
  }
  return false;
}

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
  if (!Storage.openFileForRead("H5N", inputPath, inFile) || !Storage.openFileForWrite("H5N", outputPath, outFile)) {
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
  char beforePrevChar = 0;
  size_t declarationPrefixLength = 0;
  bool commentPrefix = false;
  bool cdataPrefix = false;
  bool inSubset = false;

  char attrNameBuf[MAX_ATTR_NAME_LENGTH + 1] = {0};
  size_t attrNameLen = 0;
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

  auto flushBareAttr = [&]() -> bool {
    for (size_t j = 0; j < attrNameLen; j++) {
      if (!writeChar(attrNameBuf[j])) return false;
    }
    if (!writeChar('=')) return false;
    if (!writeChar('"')) return false;
    if (!writeChar('"')) return false;
    attrNameLen = 0;
    inAttrName = false;
    afterAttrName = false;
    return true;
  };

  auto flushAttrNameRaw = [&]() -> bool {
    for (size_t j = 0; j < attrNameLen; j++) {
      if (!writeChar(attrNameBuf[j])) return false;
    }
    attrNameLen = 0;
    inAttrName = false;
    afterAttrName = false;
    return true;
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
      char c = static_cast<char>(readBuffer[i]);

      switch (state) {
        case State::Normal:
          if (c == '<') {
            state = State::InTagStart;
            tagNameLen = 0;
            isCurrentTagVoid = false;
          } else {
            if (!writeChar(c)) goto error;
          }
          break;

        case State::InTagStart:
          if (c == '/') {
            state = State::InClosingTagName;
            tagNameLen = 0;
            closingTagWsLen = 0;
          } else if (c == '!' || c == '?') {
            state = c == '!' ? State::Declaration : State::Instruction;
            declarationPrefixLength = 0;
            commentPrefix = cdataPrefix = true;
            inSubset = false;
            quoteChar = 0;
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
            isCurrentTagVoid = isVoidElement(tagName, tagNameLen);

            if (c == '>') {
              if (isCurrentTagVoid && prevChar != '/') {
                if (!writeChar(' ')) goto error;
                if (!writeChar('/')) goto error;
              }
              if (!writeChar(c)) goto error;
              state = State::Normal;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
              state = State::InTagAttrs;
              inAttrName = false;
              attrNameLen = 0;
              if (!writeChar(c)) goto error;
            } else if (c == '/') {
              if (!writeChar(c)) goto error;
              state = State::InTagAttrs;
              inAttrName = false;
              attrNameLen = 0;
            } else {
              if (!writeChar(c)) goto error;
              state = State::Normal;
            }
          }
          break;

        case State::InTagAttrs:
          if (c == '"' || c == '\'') {
            if (inAttrName) {
              if (!flushBareAttr()) goto error;
            }
            state = State::InQuote;
            quoteChar = c;
            if (!writeChar(c)) goto error;
          } else if (c == '=') {
            if (!flushAttrNameRaw() || !writeChar(c)) goto error;
            state = State::BeforeValue;
          } else if (c == '>') {
            if (inAttrName) {
              if (!flushBareAttr()) goto error;
            }
            if (!closeCurrentTag()) goto error;
            state = State::Normal;
          } else if (c == '<') {
            if (inAttrName) {
              if (!flushBareAttr()) goto error;
            }
            if (!closeCurrentTag()) goto error;
            state = State::InTagStart;
            tagNameLen = 0;
            isCurrentTagVoid = false;
          } else if (isAttrNameStart(c) && (!inAttrName || afterAttrName)) {
            if (afterAttrName && (!flushBareAttr() || !writeChar(' '))) goto error;
            if (prevChar == '"' || prevChar == '\'') {
              if (!writeChar(' ')) goto error;
            }
            inAttrName = true;
            attrNameLen = 0;
            attrNameBuf[attrNameLen++] = c;
          } else if (inAttrName && isAttrNameChar(c)) {
            if (attrNameLen == MAX_ATTR_NAME_LENGTH) {
              if (!writeChar(attrNameBuf[0])) goto error;
              std::move(attrNameBuf + 1, attrNameBuf + attrNameLen, attrNameBuf);
              --attrNameLen;
            }
            attrNameBuf[attrNameLen++] = c;
          } else if (c == '/') {
            if (inAttrName && !flushBareAttr()) goto error;
            if (!writeChar(c)) goto error;
          } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (inAttrName) {
              afterAttrName = true;
            } else if (!writeChar(c)) {
              goto error;
            }
          } else {
            if (inAttrName) {
              if (!flushAttrNameRaw()) goto error;
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
            if (!writeChar('"') || !writeChar(c)) goto error;
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
          } else if (!writeChar(c)) {
            goto error;
          }
          break;

        case State::InQuote:
          if (c == quoteChar) {
            state = State::InTagAttrs;
            inAttrName = false;
            attrNameLen = 0;
            if (!writeChar(c)) goto error;
          } else if (c == '<') {
            if (!writeChar('&')) goto error;
            if (!writeChar('l')) goto error;
            if (!writeChar('t')) goto error;
            if (!writeChar(';')) goto error;
          } else {
            if (!writeChar(c)) goto error;
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
            if (isVoidElement(tagName, tagNameLen)) {
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

        case State::Declaration:
          if (!writeChar(c)) goto error;
          if (commentPrefix || cdataPrefix) {
            commentPrefix = commentPrefix && c == "--"[declarationPrefixLength];
            cdataPrefix = cdataPrefix && c == "[CDATA["[declarationPrefixLength];
            ++declarationPrefixLength;
            if (commentPrefix && declarationPrefixLength == 2) {
              state = State::Comment;
            } else if (cdataPrefix && declarationPrefixLength == 7) {
              state = State::Cdata;
            }
            if (commentPrefix || cdataPrefix) break;
          }
          if (quoteChar) {
            if (c == quoteChar) quoteChar = 0;
          } else if (c == '\'' || c == '"') {
            quoteChar = c;
          } else if (c == '[' || c == ']') {
            inSubset = c == '[';
          } else if (c == '>' && !inSubset) {
            state = State::Normal;
          }
          break;

        case State::Comment:
        case State::Cdata:
        case State::Instruction: {
          if (!writeChar(c)) goto error;
          const char delimiter = state == State::Comment ? '-' : state == State::Cdata ? ']' : '?';
          if (c == '>' && prevChar == delimiter && (state == State::Instruction || beforePrevChar == delimiter)) {
            state = State::Normal;
          }
          break;
        }
      }

      beforePrevChar = prevChar;
      prevChar = c;
    }
  }

  if (state == State::InTagAttrs || state == State::InTagName) {
    if (inAttrName) {
      if (!flushBareAttr()) goto error;
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
