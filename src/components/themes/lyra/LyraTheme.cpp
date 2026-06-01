#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int mainMenuColumns = 2;
int coverWidth = 0;

const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return Folder24Icon;
      case UIIcon::Text:
        return Text24Icon;
      case UIIcon::Image:
        return Image24Icon;
      case UIIcon::Book:
        return Book24Icon;
      case UIIcon::File:
        return File24Icon;
      default:
        return nullptr;
    }
  } else if (size == 32) {
    switch (icon) {
      case UIIcon::Folder:
        return FolderIcon;
      case UIIcon::Book:
        return BookIcon;
      case UIIcon::Recent:
        return RecentIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Library:
        return LibraryIcon;
      case UIIcon::Wifi:
        return WifiIcon;
      case UIIcon::Hotspot:
        return HotspotIcon;
      case UIIcon::Bookmark:
        return BookmarkIcon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}
}  // namespace

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - metrics().batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, metrics().batteryWidth, metrics().batteryHeight},
                   showBatteryPercentage);

  int maxTitleWidth = title != nullptr ? renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD) : 0;
  int maxSubtitleWidth =
      subtitle != nullptr ? renderer.getTextWidth(SMALL_FONT_ID, subtitle, EpdFontFamily::REGULAR) : 0;

  // Available space is the distance between the side paddings, and a with side padding between title and subtitle.
  const int availableSpace = rect.width - metrics().contentSidePadding * 3;

  if (maxTitleWidth + maxSubtitleWidth > availableSpace) {
    if ((maxTitleWidth > availableSpace / 2) && (maxSubtitleWidth > availableSpace / 2)) {
      // Both are wider then half the space, truncate both.
      maxTitleWidth = availableSpace / 2;
      maxSubtitleWidth = availableSpace / 2;
    } else {
      // Truncate the the longest one
      if (maxTitleWidth > maxSubtitleWidth) {
        maxTitleWidth = availableSpace - maxSubtitleWidth;
      } else {
        maxSubtitleWidth = availableSpace - maxTitleWidth;
      }
    }
  }

  if (title) {
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + metrics().contentSidePadding,
                      rect.y + metrics().batteryBarHeight + 3, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(SMALL_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - metrics().contentSidePadding - truncatedSubtitleWidth,
                      rect.y + 50, truncatedSubtitle.c_str(), true);
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + metrics().contentSidePadding;
  int rightSpace = metrics().contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - metrics().contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - metrics().contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  int currentX = rect.x + metrics().contentSidePadding;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        renderer.fillRoundedRect(currentX, rect.y + 1, textWidth + 2 * hPaddingInSelection, rect.height - 4,
                                 cornerRadius, Color::Black);
      } else {
        renderer.fillRectDither(currentX, rect.y, textWidth + 2 * hPaddingInSelection, rect.height - 3,
                                Color::LightGray);
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * hPaddingInSelection,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label, !(tab.selected && selected),
                      EpdFontFamily::REGULAR);

    currentX += textWidth + metrics().tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? metrics().listWithSubtitleRowHeight : metrics().listRowHeight;
  return contentHeight / rowHeight;
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? metrics().listWithSubtitleRowHeight : metrics().listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - metrics().scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - metrics().scrollBarWidth, scrollBarY, metrics().scrollBarWidth,
                      scrollBarHeight, true);
  }

  // Draw selection
  int contentWidth =
      rect.width -
      (totalPages > 1 ? (metrics().scrollBarWidth + metrics().scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(
        rect.x + metrics().contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
        contentWidth - metrics().contentSidePadding * 2, rowHeight, cornerRadius, Color::LightGray);
  }

  int textX = rect.x + metrics().contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - metrics().contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  int iconY = (rowSubtitle != nullptr) ? 16 : 10;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int rowTextWidth = textWidth;

    // Draw name
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
      rowTextWidth -= valueWidth;
    }

    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_10_FONT_ID, textX, itemY + 7, item.c_str(), true);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, item.c_str());
      const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
      for (int py = itemY + 7; py < itemY + 7 + lineH; py++)
        for (int px = textX; px < textX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, rect.x + metrics().contentSidePadding + hPaddingInSelection,
                          itemY + iconY, iconSize, iconSize);
      }
    }

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30, subtitle.c_str(), true);
    }

    // Draw value
    if (!valueText.empty()) {
      if (i == selectedIndex && highlightValue) {
        renderer.fillRoundedRect(
            rect.x + contentWidth - metrics().contentSidePadding - hPaddingInSelection - valueWidth, itemY,
            valueWidth + hPaddingInSelection, rowHeight, cornerRadius, Color::Black);
      }

      int valueY = itemY + 6;
      if (rowSubtitle != nullptr) {
        valueY = itemY + 16;
      }
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - metrics().contentSidePadding - valueWidth,
                        valueY, valueText.c_str(), !(i == selectedIndex && highlightValue));
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  const int buttonHeight = metrics().buttonHintsHeight;
  const int buttonY = metrics().buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // X3 has wider screen in portrait (528 vs 480), use more spacing
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    } else {
      // Draw the filled background and border for a SMALL-sized button
      renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, cornerRadius,
                               Color::White);
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true,
                               true, false, false, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  const int buttonWidth = metrics().sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  if (homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::CoverStrip) {
    drawCoverStripRecents(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored);
    bufferRestored = false;
    return;
  }
  if (homeRecents_ != nullptr && homeRecents_->type == ThemeHomeRecentsType::None) {
    coverBufferStored = false;
    coverRendered = false;
    bufferRestored = false;
    return;
  }

  if (variant_ == Variant::ThreeCovers) {
    drawThreeCoverRecents(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                          storeCoverBuffer);
    return;
  }
  if (variant_ == Variant::Carousel) {
    drawCarouselRecents(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored, storeCoverBuffer);
    return;
  }
  if (variant_ == Variant::RoundedRaff) {
    drawCarouselRecents(renderer, rect, recentBooks, selectorIndex, coverRendered, coverBufferStored, storeCoverBuffer);
    return;
  }

  const int tileWidth = rect.width - 2 * metrics().contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = metrics().homeCoverHeight * 0.6;
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = metrics().contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, metrics().homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                metrics().homeCoverHeight);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                        metrics().homeCoverHeight, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + hPaddingInSelection,
                          tileY + hPaddingInSelection + (metrics().homeCoverHeight / 3), coverWidth,
                          2 * metrics().homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = metrics().contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - metrics().verticalSpacing - coverWidth;

    if (bookSelected) {
      // Draw selection box
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                              metrics().homeCoverHeight, Color::LightGray);
      renderer.fillRectDither(tileX + hPaddingInSelection + coverWidth, tileY + hPaddingInSelection,
                              tileWidth - hPaddingInSelection - coverWidth, metrics().homeCoverHeight,
                              Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + metrics().homeCoverHeight + hPaddingInSelection, tileWidth,
                               hPaddingInSelection, cornerRadius, false, false, true, true, Color::LightGray);
    }

    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 3, EpdFontFamily::BOLD);

    auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
    const int authorHeight = book.author.empty() ? 0 : (renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2);
    const int totalBlockHeight = titleBlockHeight + authorHeight;
    int titleY = tileY + tileHeight / 2 - totalBlockHeight / 2;
    const int textX = tileX + hPaddingInSelection + coverWidth + metrics().verticalSpacing;
    for (const auto& line : titleLines) {
      renderer.drawText(UI_12_FONT_ID, textX, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }
    if (!book.author.empty()) {
      titleY += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, titleY, author.c_str(), true);
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void LyraTheme::drawThreeCoverRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                      std::function<bool()> storeCoverBuffer) const {
  const auto& m = metrics();
  const int tileWidth = (rect.width - 2 * m.contentSidePadding) / 3;
  const int tileY = rect.y;

  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  if (!coverRendered) {
    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), m.homeRecentBooksCount);
         i++) {
      const int tileX = m.contentSidePadding + tileWidth * i;
      bool hasCover = !recentBooks[i].coverBmpPath.empty();

      if (hasCover) {
        const std::string coverBmpPath =
            UITheme::getCoverThumbPath(recentBooks[i].coverBmpPath, m.homeCoverHeight);
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            const float coverHeight = static_cast<float>(bitmap.getHeight());
            const float coverWidthF = static_cast<float>(bitmap.getWidth());
            const float ratio = coverWidthF / coverHeight;
            const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                    static_cast<float>(m.homeCoverHeight);
            const float cropX = 1.0f - (tileRatio / ratio);
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                tileWidth - 2 * hPaddingInSelection, m.homeCoverHeight,
                                cropX);
          } else {
            hasCover = false;
          }
          file.close();
        } else {
          hasCover = false;
        }
      }

      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                        tileWidth - 2 * hPaddingInSelection, m.homeCoverHeight, true);

      if (!hasCover) {
        renderer.fillRect(tileX + hPaddingInSelection,
                          tileY + hPaddingInSelection + (m.homeCoverHeight / 3),
                          tileWidth - 2 * hPaddingInSelection, 2 * m.homeCoverHeight / 3,
                          true);
        renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
      }
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), m.homeRecentBooksCount);
       i++) {
    const int tileX = m.contentSidePadding + tileWidth * i;
    const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;
    auto titleLines = renderer.wrappedText(SMALL_FONT_ID, recentBooks[i].title.c_str(), maxLineWidth, 3);
    const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int titleBoxHeight = static_cast<int>(titleLines.size()) * titleLineHeight + hPaddingInSelection + 5;

    if (selectorIndex == i) {
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                              m.homeCoverHeight, Color::LightGray);
      renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                              hPaddingInSelection, m.homeCoverHeight, Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + m.homeCoverHeight + hPaddingInSelection,
                               tileWidth, titleBoxHeight, cornerRadius, false, false, true, true, Color::LightGray);
    }

    int currentY = tileY + m.homeCoverHeight + hPaddingInSelection + 5;
    for (const auto& line : titleLines) {
      renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
      currentY += titleLineHeight;
    }
  }
}

void LyraTheme::drawCoverStripRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered,
                                      bool& coverBufferStored) const {
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const ThemeHomeRecentsSpec& spec = *homeRecents_;
  if (spec.slots.empty()) {
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  const int selected = selectorIndex >= 0 && selectorIndex < bookCount ? selectorIndex : 0;
  const auto& m = metrics();

  auto resolveBookIndex = [&](const ThemeCoverSlotSpec& slot) {
    switch (slot.book) {
      case ThemeBookRef::Previous:
        if (bookCount <= 1) return -1;
        return spec.wrap ? (selected + bookCount - 1) % bookCount : selected - 1;
      case ThemeBookRef::Next:
        if (bookCount <= 1) return -1;
        return spec.wrap ? (selected + 1) % bookCount : selected + 1;
      case ThemeBookRef::Index:
        return slot.bookIndex >= 0 && slot.bookIndex < bookCount ? slot.bookIndex : -1;
      case ThemeBookRef::Selected:
      default:
        return selected;
    }
  };

  auto drawCover = [&](int bookIndex, int x, int y, int w, int h, bool selectedCover) {
    bool hasCover = bookIndex >= 0 && bookIndex < bookCount && !recentBooks[bookIndex].coverBmpPath.empty();
    if (hasCover) {
      auto drawThumb = [&](int thumbHeight) {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath, thumbHeight);
        HalFile file;
        if (!Storage.openFileForRead("HOME", coverBmpPath, file)) return false;
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, x, y, w, h);
          return true;
        } else {
          return false;
        }
      };

      bool drawn = drawThumb(h);
      if (!drawn && h != m.homeCoverHeight) {
        drawn = drawThumb(m.homeCoverHeight);
      }
      if (!drawn) {
        hasCover = false;
      }
    }

    if (!hasCover) {
      renderer.drawRect(x, y, w, h, true);
      renderer.fillRect(x, y + h / 3, w, 2 * h / 3, true);
      renderer.drawIcon(CoverIcon, x + std::max(4, (w - 32) / 2), y + 16, 32, 32);
    }

    renderer.drawRect(x, y, w, h, true);
    if (selectedCover) {
      for (int i = 0; i < std::max(1, spec.selectionLineWidth); ++i) {
        renderer.drawRoundedRect(x - 6 - i, y - 6 - i, w + 12 + 2 * i, h + 12 + 2 * i,
                                 std::max(1, spec.selectionLineWidth), spec.selectionCornerRadius, true);
      }
    }
  };

  for (const auto& slot : spec.slots) {
    const int bookIndex = resolveBookIndex(slot);
    if (bookIndex < 0 || bookIndex >= bookCount) continue;

    const int h = std::min(slot.height, rect.height);
    const int w = std::max(1, h * std::max(1, slot.widthPercent) / 100);
    int x = rect.x + (rect.width - w) / 2;
    if (slot.x == ThemeSlotX::Padding) {
      x = rect.x + m.contentSidePadding;
    } else if (slot.x == ThemeSlotX::RightPadding) {
      x = rect.x + rect.width - m.contentSidePadding - w;
    }
    x += slot.xOffset;

    int y = rect.y;
    if (slot.y == ThemeSlotY::Center) {
      y = rect.y + (rect.height - h) / 2;
    }
    y += slot.yOffset;

    const bool selectedCover = slot.selected && (slot.book != ThemeBookRef::Index || bookIndex == selected);
    drawCover(bookIndex, x, y, w, h, selectedCover);

    if (slot.title.enabled) {
      const int maxWidth = std::max(40, rect.width - 2 * m.contentSidePadding);
      const auto style = slot.title.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const auto titleLines =
          renderer.wrappedText(slot.title.fontId, recentBooks[bookIndex].title.c_str(), maxWidth, slot.title.maxLines,
                               style);
      int titleY = y + h + slot.title.offsetY;
      for (const auto& line : titleLines) {
        const int textWidth = renderer.getTextWidth(slot.title.fontId, line.c_str(), style);
        renderer.drawText(slot.title.fontId, rect.x + (rect.width - textWidth) / 2, titleY, line.c_str(), true, style);
        titleY += renderer.getLineHeight(slot.title.fontId);
      }
    }
  }

  coverBufferStored = false;
  coverRendered = false;
}

void LyraTheme::drawCarouselRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    std::function<bool()> storeCoverBuffer) const {
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int bookCount = static_cast<int>(recentBooks.size());
  const int selected = selectorIndex >= 0 && selectorIndex < bookCount ? selectorIndex : 0;
  const auto& m = metrics();
  const int centerCoverHeight = m.homeCoverHeight;
  const int centerCoverWidth = static_cast<int>(centerCoverHeight * 0.62f);
  const int sideCoverHeight = centerCoverHeight * 3 / 4;
  const int sideCoverWidth = static_cast<int>(sideCoverHeight * 0.62f);
  const int centerX = rect.x + (rect.width - centerCoverWidth) / 2;
  const int centerY = rect.y + hPaddingInSelection;
  const int sideY = centerY + (centerCoverHeight - sideCoverHeight) / 2;

  auto drawCover = [&](int bookIndex, int x, int y, int w, int h, bool selectedCover) {
    bool hasCover = bookIndex >= 0 && bookIndex < bookCount && !recentBooks[bookIndex].coverBmpPath.empty();
    if (hasCover) {
      const std::string coverBmpPath = UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath, h);
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, x, y, w, h);
        } else {
          hasCover = false;
        }
        file.close();
      } else {
        hasCover = false;
      }
    }
    if (!hasCover) {
      renderer.drawRect(x, y, w, h, true);
      renderer.fillRect(x, y + h / 3, w, 2 * h / 3, true);
      renderer.drawIcon(CoverIcon, x + 16, y + 16, 32, 32);
    }
    renderer.drawRect(x, y, w, h, true);
    if (selectedCover) {
      renderer.drawRoundedRect(x - 6, y - 6, w + 12, h + 12, 3, cornerRadius, true);
    }
  };

  if (bookCount > 1) {
    const int prev = (selected + bookCount - 1) % bookCount;
    drawCover(prev, rect.x + m.contentSidePadding, sideY, sideCoverWidth, sideCoverHeight, false);
  }
  if (bookCount > 2) {
    const int next = (selected + 1) % bookCount;
    drawCover(next, rect.x + rect.width - m.contentSidePadding - sideCoverWidth, sideY, sideCoverWidth,
              sideCoverHeight, false);
  }
  drawCover(selected, centerX, centerY, centerCoverWidth, centerCoverHeight, true);

  coverBufferStored = false;
  coverRendered = false;

  const auto titleLines =
      renderer.wrappedText(UI_12_FONT_ID, recentBooks[selected].title.c_str(), rect.width - 80, 2, EpdFontFamily::BOLD);
  int titleY = centerY + centerCoverHeight + 12;
  for (const auto& line : titleLines) {
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - textWidth) / 2, titleY, line.c_str(), true,
                      EpdFontFamily::BOLD);
    titleY += renderer.getLineHeight(UI_12_FONT_ID);
  }
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonMenu_ != nullptr && buttonMenu_->enabled) {
    const auto& spec = *buttonMenu_;
    const auto& m = metrics();
    const int panelWidth = spec.panelWidth > 0 ? std::min(spec.panelWidth, rect.width) : rect.width;
    const int panelX = rect.x + (rect.width - panelWidth) / 2;
    const int panelHeight = buttonCount * m.menuRowHeight + std::max(0, buttonCount - 1) * m.menuSpacing;

    if (spec.drawPanel) {
      renderer.drawRoundedRect(panelX, rect.y, panelWidth, panelHeight, 1, spec.panelCornerRadius, true);
    }

    for (int i = 0; i < buttonCount; ++i) {
      Rect tileRect = Rect{panelX + spec.selectionInset,
                           rect.y + i * (m.menuRowHeight + m.menuSpacing),
                           panelWidth - spec.selectionInset * 2, m.menuRowHeight};
      const bool selected = selectedIndex == i;

      if (selected) {
        if (spec.selectionStyle == ThemeMenuSelectionStyle::Outline) {
          renderer.drawRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, 1,
                                   spec.selectionCornerRadius, true);
        } else if (spec.selectionStyle == ThemeMenuSelectionStyle::Triangle) {
          constexpr int triangleWidth = 12;
          constexpr int triangleHeight = 18;
          const int triangleX = panelX + spec.selectionInset;
          const int triangleCenterY = tileRect.y + tileRect.height / 2;
          const int triangleXPoints[3] = {triangleX, triangleX, triangleX + triangleWidth};
          const int triangleYPoints[3] = {triangleCenterY - triangleHeight / 2,
                                          triangleCenterY + triangleHeight / 2, triangleCenterY};
          renderer.fillPolygon(triangleXPoints, triangleYPoints, 3, true);
        } else {
          renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height,
                                   spec.selectionCornerRadius, Color::LightGray);
        }
      }

      std::string labelStr = buttonLabel(i);
      const char* label = labelStr.c_str();
      const auto style = spec.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int lineHeight = renderer.getLineHeight(spec.fontId);
      const int textY = tileRect.y + (tileRect.height - lineHeight) / 2;
      int textX = tileRect.x + 16;

      if (spec.showIcons && rowIcon != nullptr) {
        UIIcon icon = rowIcon(i);
        const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
        if (iconBitmap != nullptr) {
          renderer.drawIcon(iconBitmap, textX, textY + 3, mainMenuIconSize, mainMenuIconSize);
          textX += mainMenuIconSize + hPaddingInSelection + 2;
        }
      }

      if (spec.centeredText) {
        const int textWidth = renderer.getTextWidth(spec.fontId, label, style);
        textX = tileRect.x + (tileRect.width - textWidth) / 2;
      }
      renderer.drawText(spec.fontId, textX, textY, label, true, style);
    }
    return;
  }

  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - metrics().contentSidePadding * 2;
    Rect tileRect = Rect{rect.x + metrics().contentSidePadding,
                         rect.y + i * (metrics().menuRowHeight + metrics().menuSpacing), tileWidth,
                         metrics().menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (metrics().menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, textX, textY + 3, mainMenuIconSize, mainMenuIconSize);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}
