#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace LyraMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 84,
                                 .verticalSpacing = 16,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 40,
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 64,
                                 .menuSpacing = 8,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 56,
                                 .homeCoverHeight = 226,
                                 .homeCoverTileHeight = 242,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 16,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 6,
                                 .keyboardFillUnselected = false,
                                 .keyboardOutlineAllUnselected = false,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 1,
                                 .keyboardSecondaryLabelTopPadding = 0,
                                 .keyboardMinArrowHeadSize = 0,
                                 .popupTopOffsetRatio = 0.165f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 6,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class LyraTheme : public BaseTheme {
 public:
  enum class Variant { Standard, ThreeCovers, RoundedRaff, Carousel };

  explicit LyraTheme(Variant variant = Variant::Standard, const ThemeMetrics* metrics = &LyraMetrics::values,
                     const ThemeHomeRecentsSpec* homeRecents = nullptr,
                     const ThemeButtonMenuSpec* buttonMenu = nullptr)
      : variant_(variant), metrics_(metrics), homeRecents_(homeRecents), buttonMenu_(buttonMenu) {}

  // Component drawing methods
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  int getListPageItems(int contentHeight, bool hasSubtitle) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const;
  bool showsFileIcons() const override { return true; }

 private:
  Variant variant_;
  const ThemeMetrics* metrics_;
  const ThemeHomeRecentsSpec* homeRecents_;
  const ThemeButtonMenuSpec* buttonMenu_;
  const ThemeMetrics& metrics() const { return metrics_ ? *metrics_ : LyraMetrics::values; }
  void drawThreeCoverRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                             int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                             std::function<bool()> storeCoverBuffer) const;
  void drawCarouselRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                           std::function<bool()> storeCoverBuffer) const;
  void drawCoverStripRecents(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                             int selectorIndex, bool& coverRendered, bool& coverBufferStored) const;
};

namespace SdLyraMetrics {
constexpr ThemeMetrics threeCovers = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverTileHeight = 300;
  v.homeRecentBooksCount = 3;
  return v;
}();

constexpr ThemeMetrics carousel = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverHeight = 300;
  v.homeCoverTileHeight = 340;
  v.homeRecentBooksCount = 3;
  return v;
}();

constexpr ThemeMetrics roundedRaff = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.topPadding = 0;
  v.headerHeight = 45;
  v.listRowHeight = 42;
  v.listWithSubtitleRowHeight = 69;
  v.menuRowHeight = 42;
  v.menuSpacing = 6;
  v.homeTopPadding = 55;
  v.homeCoverHeight = 300;
  v.homeCoverTileHeight = 350;
  v.homeContinueReadingInMenu = true;
  v.homeMenuTopOffset = 20;
  v.keyboardKeyHeight = 30;
  v.keyboardKeySpacing = 10;
  v.keyboardBottomKeyHeight = 30;
  v.keyboardKeyCornerRadius = 10;
  v.keyboardFillUnselected = true;
  v.keyboardOutlineAllUnselected = true;
  v.popupCornerRadius = 18;
  v.popupTextBold = true;
  v.popupProgressDrawOutline = true;
  v.popupProgressClampPercent = true;
  return v;
}();
}  // namespace SdLyraMetrics
