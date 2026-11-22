#include "ChapterXPathResolver.h"

#include <Epub/VisibleTextUtils.h>
#include <Epub/parsers/ChapterXmlEntities.h>
#include <Epub/parsers/DataUriStripper.h>
#include <HalStorage.h>
#include <Html5Normalizer.h>
#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

// Mirrors crengine's IsEmptySpace(): only these four count as empty space.
bool isWhitespaceOnly(const XML_Char* data, const int len) {
  for (int i = 0; i < len; i++) {
    const char c = data[i];
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
      return false;
    }
  }
  return true;
}

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path, const int textNodeIndex,
                                const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

class ChapterXmlParser : public Print {
 public:
  ~ChapterXmlParser() override { destroyXmlParser(parser); }

  bool ok() const { return parser && error == XML_ERROR_NONE; }
  bool needsNormalization() const { return error != XML_ERROR_NONE && error != XML_ERROR_NO_MEMORY; }

  bool finish() {
    if (ok() && !finished) recordStatus(XML_Parse(parser, "", 0, XML_TRUE));
    return ok();
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!ok() || finished || size == 0) return size;
    auto* xml = static_cast<char*>(XML_GetBuffer(parser, static_cast<int>(size)));
    if (!xml) {
      error = XML_ERROR_NO_MEMORY;
      return size;
    }
    int written = 0;
    for (size_t i = 0; i < size; ++i) {
      const char c = static_cast<char>(buffer[i]);
      if (filter.keep(c)) xml[written++] = c;
    }
    recordStatus(XML_ParseBuffer(parser, written, XML_FALSE));
    return size;
  }

 protected:
  XML_Parser parser = XML_ParserCreate(nullptr);
  bool matched = false;
  int depth = 0;

  std::string closeElement(const XML_Char* rawName) {
    --depth;
    if (depth != 0 && matched) return {};
    auto name = stripPrefix(rawName);
    if (depth == 0 && name == "html") htmlEnded = true;
    return name;
  }

  template <XML_CharacterDataHandler Consume>
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* data, int len) {
    const auto text = chapterXmlEntityText({data, static_cast<size_t>(len)});
    if (!text.empty()) Consume(userData, text.data(), static_cast<int>(text.size()));
  }

 private:
  XML_Error error = XML_ERROR_NONE;
  ImageDataUriFilter filter;
  bool htmlEnded = false;
  bool finished = false;

  void recordStatus(XML_Status status) {
    if (status != XML_STATUS_ERROR || finished) return;
    const auto parseError = XML_GetErrorCode(parser);
    if (htmlEnded && parseError != XML_ERROR_NO_MEMORY) {
      finished = true;
      return;
    }
    error = parseError;
    LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
  }
};

class ParagraphTextCounter final : public ChapterXmlParser {
 public:
  ParagraphTextCounter() {
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
    XML_SetDefaultHandlerExpand(parser, &defaultHandlerExpand<&ParagraphTextCounter::characterData>);
  }

  // Visible codepoints up to the end of the last non-whitespace run. Trailing formatting
  // whitespace is excluded so a 100% target still resolves inside real text.
  size_t totalVisibleChars() const { return lastTextEndChars; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = closeElement(rawName);

    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
    if (!isWhitespaceOnly(data, len)) {
      // Trailing whitespace inside this run is single-byte, so bytes equal codepoints here.
      int trailing = 0;
      while (trailing < len && isWhitespaceOnly(data + len - 1 - trailing, 1)) {
        trailing++;
      }
      lastTextEndChars = visibleChars - static_cast<size_t>(trailing);
    }
  }

 private:
  bool insideBody = false;
  int bodyDepth = -1;
  uint16_t nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t lastTextEndChars = 0;
};

class XPathParagraphResolver final : public ChapterXmlParser {
 public:
  explicit XPathParagraphResolver(const int targetParagraph) : targetParagraph(targetParagraph) {
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathParagraphResolver::startElement, &XPathParagraphResolver::endElement);
  }

  const std::string& getXPath() const { return xpath; }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    if (matched) {
      ++depth;
      return;
    }
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    // Count both <p> and <li> as paragraph-like positions, matching how the section
    // layout tracks them (xpathParagraphIndex and xpathListItemIndex). This ensures
    // KOReader progress in list items maps to the correct XPath.
    if (name == "p") {
      paragraphCount++;
    } else if (name == "li") {
      paragraphCount++;
    }
    if (paragraphCount == targetParagraph) {
      xpath = buildParagraphXPath(spineIndex, path, 0, 0);
      matched = true;
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = closeElement(rawName);

    if (matched || !insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  const int targetParagraph;
  bool insideBody = false;
  int bodyDepth = -1;
  int paragraphCount = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

class XPathProgressResolver final : public ChapterXmlParser {
 public:
  enum class BoundaryMode { Exclusive, Inclusive };

  explicit XPathProgressResolver(const size_t targetVisibleChar,
                                 const BoundaryMode boundaryMode = BoundaryMode::Exclusive)
      : targetVisibleChar(targetVisibleChar), boundaryMode(boundaryMode) {
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
    XML_SetDefaultHandlerExpand(parser, &defaultHandlerExpand<&XPathProgressResolver::characterData>);
    XML_SetCommentHandler(parser, &XPathProgressResolver::comment);
    XML_SetProcessingInstructionHandler(parser, &XPathProgressResolver::processingInstruction);
    XML_SetCdataSectionHandler(parser, &XPathProgressResolver::startCdataSection,
                               &XPathProgressResolver::endCdataSection);
  }

  const std::string& getXPath() const { return xpath; }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onCharacterData(data, len);
  }

  static void XMLCALL comment(void* userData, const XML_Char*) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL processingInstruction(void* userData, const XML_Char*, const XML_Char*) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL startCdataSection(void* userData) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL endCdataSection(void* userData) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  void onStartElement(const XML_Char* rawName) {
    if (matched) {
      ++depth;
      return;
    }
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
        textNodeIndexStack.push_back(0);
      }
      depth++;
      return;
    }

    pendingTextNode = true;
    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeIndexStack.push_back(0);

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = closeElement(rawName);

    if (matched || !insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }

    pendingTextNode = true;
    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.pop_back();
    }
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  // Visible-offset counting mirrors ChapterHtmlSlimParser::characterData (every body
  // codepoint outside non-visible elements), so offsets recorded by the page LUT line up
  // with what is counted here regardless of which elements hold the text.
  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0 || matched) {
      return;
    }

    const size_t codepointCount = countUtf8Codepoints(data, len);
    if (codepointCount == 0) {
      return;
    }

    const bool whitespaceOnly = isWhitespaceOnly(data, len);

    // Start a new text node on first non-empty content after any structural boundary.
    // Only counting non-empty nodes matches KOReader's text()[N] indexing behavior,
    // which skips empty text nodes created by bare <a id="anchor"/> anchors. crengine
    // also drops a whitespace-only run that is the first child of a block element, but
    // "block" there comes from the computed style (display, white-space), which this
    // resolver does not see, so such a run is counted here like any other.
    if (pendingTextNode) {
      if (!textNodeIndexStack.empty()) {
        textNodeIndexStack.back()++;
      }
      textNodeStartChars = visibleChars;
      pendingTextNode = false;
    }

    // Whitespace-only runs are never used as anchors: a target that falls inside one is
    // carried forward and resolves at the start of the next text run instead.
    const size_t nextVisibleChars = visibleChars + codepointCount;
    const bool targetReached = boundaryMode == BoundaryMode::Inclusive ? targetVisibleChar <= nextVisibleChars
                                                                       : targetVisibleChar < nextVisibleChars;
    if (targetReached && !whitespaceOnly) {
      const size_t delta = targetVisibleChar > visibleChars ? targetVisibleChar - visibleChars : 0;
      const int texNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
      const size_t charOff = visibleChars - textNodeStartChars + delta;
      xpath = buildParagraphXPath(spineIndex, path, texNode, charOff);
      matched = true;
      return;
    }

    visibleChars = nextVisibleChars;
  }

  void onMarkupBoundary() {
    if (!insideBody || nonVisibleDepth > 0 || matched) {
      return;
    }

    pendingTextNode = true;
  }

  const size_t targetVisibleChar;
  const BoundaryMode boundaryMode;
  bool insideBody = false;
  bool pendingTextNode = true;
  int bodyDepth = -1;
  uint16_t nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t textNodeStartChars = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};
enum class ChapterParseResult { Ok, SyntaxError, Error };

std::mutex normalizationMutex;

class ChapterXmlInput {
 public:
  ChapterXmlInput(const Epub& epub, const std::string& href) : epub(epub), href(href) {}
  ~ChapterXmlInput() {
    if (!normalizedPath.empty()) Storage.remove(normalizedPath.c_str());
  }

  ChapterParseResult parse(ChapterXmlParser& parser) const {
    if (!parser.ok()) return ChapterParseResult::Error;
    const bool read = normalizedPath.empty() ? epub.readItemContentsToStream(href, parser, 1024)
                                             : Storage.readFileToStream(normalizedPath.c_str(), parser);
    if (!read) return ChapterParseResult::Error;
    if (parser.finish()) return ChapterParseResult::Ok;
    return parser.needsNormalization() ? ChapterParseResult::SyntaxError : ChapterParseResult::Error;
  }

  bool normalize() {
    lock.lock();
    const auto rawPath = epub.getCachePath() + "/.koreader-chapter.html";
    normalizedPath = epub.getCachePath() + "/.koreader-chapter.xml";
    HalFile raw;
    if (!Storage.openFileForWrite("KOX", rawPath, raw)) {
      Storage.remove(rawPath.c_str());
      return false;
    }
    const bool streamed = epub.readItemContentsToStream(href, raw, 1024);
    const bool closed = raw.close();
    const bool normalized =
        streamed && closed && html5::normalizeHtmlForXml(rawPath, normalizedPath) == html5::NormalizeResult::Ok;
    Storage.remove(rawPath.c_str());
    return normalized;
  }

 private:
  const Epub& epub;
  const std::string& href;
  std::string normalizedPath;
  std::unique_lock<std::mutex> lock{normalizationMutex, std::defer_lock};
};

template <typename Resolve>
auto parseChapter(ChapterXmlInput& input, const Resolve& resolve) {
  auto [result, value] = resolve();
  if (result == ChapterParseResult::SyntaxError && input.normalize()) {
    auto [repairedResult, repairedValue] = resolve();
    if (repairedResult == ChapterParseResult::Ok) return repairedValue;
  }
  return result == ChapterParseResult::Ok ? value : decltype(value){};
}

}  // namespace

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  ChapterXmlInput input(*epub, href);
  return parseChapter(input, [&] {
    XPathParagraphResolver resolver(paragraphIndex);
    resolver.spineIndex = spineIndex;
    const auto result = input.parse(resolver);
    return std::pair{result, resolver.getXPath()};
  });
}

std::string ChapterXPathResolver::findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                                const uint32_t visibleTextOffset) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  ChapterXmlInput input(*epub, href);
  return parseChapter(input, [&] {
    XPathProgressResolver resolver(visibleTextOffset);
    resolver.spineIndex = spineIndex;
    const auto result = input.parse(resolver);
    return std::pair{result, resolver.getXPath()};
  });
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  ChapterXmlInput input(*epub, href);
  const auto totalVisibleChars = parseChapter(input, [&] {
    ParagraphTextCounter counter;
    const auto result = input.parse(counter);
    return std::pair{result, counter.totalVisibleChars()};
  });
  if (totalVisibleChars == 0) return "";

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  XPathProgressResolver resolver(targetVisibleChar, XPathProgressResolver::BoundaryMode::Inclusive);
  resolver.spineIndex = spineIndex;
  return input.parse(resolver) == ChapterParseResult::Ok ? resolver.getXPath() : "";
}
