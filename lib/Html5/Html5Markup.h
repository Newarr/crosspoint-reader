#pragma once

#include <cstdint>
#include <string_view>

namespace html5 {

inline char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

inline bool isVoidElement(std::string_view name) {
  static constexpr std::string_view elements[] = {"img",  "br",  "hr",    "input", "meta",   "link",  "area",
                                           "base", "col", "embed", "param", "source", "track", "wbr"};
  for (const auto element : elements) {
    if (name.size() != element.size()) continue;
    size_t i = 0;
    while (i < name.size() && lowerAscii(name[i]) == element[i]) ++i;
    if (i == name.size()) return true;
  }
  return false;
}

class OpaqueMarkup {
 public:
  bool start(char marker) {
    if (marker == '!') {
      kind = Kind::Bang;
    } else if (marker == '?') {
      kind = Kind::Instruction;
    } else {
      return false;
    }
    prefixLength = 0;
    terminatorLength = 0;
    quote = 0;
    inSubset = false;
    return true;
  }

  bool active() const { return kind != Kind::None; }

  void consume(char c) {
    switch (kind) {
      case Kind::Bang:
        if (c == '-') {
          kind = Kind::CommentPrefix;
          return;
        }
        if (c == '[') {
          kind = Kind::CdataPrefix;
          inSubset = true;
          return;
        }
        break;
      case Kind::CommentPrefix:
        if (c == '-') {
          kind = Kind::Comment;
          return;
        }
        break;
      case Kind::CdataPrefix:
        if (c == "CDATA["[prefixLength]) {
          if (++prefixLength == 6) {
            kind = Kind::Cdata;
            inSubset = false;
          }
          return;
        }
        break;
      case Kind::Comment:
        consumeTerminator(c, '-', 2);
        return;
      case Kind::Cdata:
        consumeTerminator(c, ']', 2);
        return;
      case Kind::Instruction:
        consumeTerminator(c, '?', 1);
        return;
      case Kind::SubsetTagOpen:
        if (c == '!') {
          kind = Kind::Bang;
          return;
        }
        if (c == '?') {
          kind = Kind::Instruction;
          return;
        }
        break;
      case Kind::Declaration:
        break;
      case Kind::None:
        return;
    }

    kind = Kind::Declaration;
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == '<' && inSubset) {
      kind = Kind::SubsetTagOpen;
    } else if (c == '[' || c == ']') {
      inSubset = c == '[';
    } else if (c == '>' && !inSubset) {
      kind = Kind::None;
    }
  }

 private:
  enum class Kind : uint8_t {
    None,
    Bang,
    CommentPrefix,
    CdataPrefix,
    Comment,
    Cdata,
    Instruction,
    SubsetTagOpen,
    Declaration
  };
  Kind kind = Kind::None;
  uint8_t prefixLength = 0;
  uint8_t terminatorLength = 0;
  char quote = 0;
  bool inSubset = false;

  void consumeTerminator(char c, char marker, uint8_t count) {
    if (c == marker) {
      if (terminatorLength < count) ++terminatorLength;
    } else {
      if (c == '>' && terminatorLength == count) {
        kind = inSubset ? Kind::Declaration : Kind::None;
      }
      terminatorLength = 0;
    }
  }
};

}  // namespace html5
