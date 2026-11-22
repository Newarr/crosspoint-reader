#include "Page.h"

#include "converters/ImageToFramebufferDecoder.h"

bool PageImage::cacheImage(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  return imageBlock->cacheImage(renderer, xPos + xOffset, yPos + yOffset);
}

bool Page::hasImagesNeedingFramebufferLoan() const {
  return std::any_of(elements.begin(), elements.end(), [](const auto& element) {
    return element->getTag() == TAG_PageImage &&
           static_cast<const PageImage&>(*element).getImageBlock().needsFramebufferLoanToDecode();
  });
}

bool Page::cacheImagesNeedingDecode(GfxRenderer& renderer, int, const int xOffset, const int yOffset) const {
  bool cachedAll = true;
  for (const auto& element : elements) {
    if (ImageToFramebufferDecoder::isAborted()) return false;
    if (element->getTag() != TAG_PageImage) continue;
    const auto& image = static_cast<const PageImage&>(*element);
    if (image.getImageBlock().needsFramebufferLoanToDecode() && !image.cacheImage(renderer, xOffset, yOffset)) {
      cachedAll = false;
    }
  }
  return cachedAll;
}
