#include "SdCardThemeRegistry.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"

namespace {
constexpr int THEME_SCHEMA_VERSION = 1;

void applyMetricOverrides(JsonObjectConst obj, ThemeMetrics& metrics) {
  if (obj.isNull()) return;
#define APPLY_INT_FIELD(name) metrics.name = obj[#name] | metrics.name
#define APPLY_BOOL_FIELD(name) metrics.name = obj[#name] | metrics.name
  APPLY_INT_FIELD(batteryWidth);
  APPLY_INT_FIELD(batteryHeight);
  APPLY_INT_FIELD(topPadding);
  APPLY_INT_FIELD(batteryBarHeight);
  APPLY_INT_FIELD(headerHeight);
  APPLY_INT_FIELD(verticalSpacing);
  APPLY_INT_FIELD(contentSidePadding);
  APPLY_INT_FIELD(listRowHeight);
  APPLY_INT_FIELD(listWithSubtitleRowHeight);
  APPLY_INT_FIELD(menuRowHeight);
  APPLY_INT_FIELD(menuSpacing);
  APPLY_INT_FIELD(tabSpacing);
  APPLY_INT_FIELD(tabBarHeight);
  APPLY_INT_FIELD(scrollBarWidth);
  APPLY_INT_FIELD(scrollBarRightOffset);
  APPLY_INT_FIELD(homeTopPadding);
  APPLY_INT_FIELD(homeCoverHeight);
  APPLY_INT_FIELD(homeCoverTileHeight);
  APPLY_INT_FIELD(homeRecentBooksCount);
  APPLY_BOOL_FIELD(homeContinueReadingInMenu);
  APPLY_INT_FIELD(homeMenuTopOffset);
  APPLY_INT_FIELD(buttonHintsHeight);
  APPLY_INT_FIELD(sideButtonHintsWidth);
  APPLY_INT_FIELD(progressBarHeight);
  APPLY_INT_FIELD(progressBarMarginTop);
  APPLY_INT_FIELD(statusBarHorizontalMargin);
  APPLY_INT_FIELD(statusBarVerticalMargin);
  APPLY_INT_FIELD(keyboardKeyWidth);
  APPLY_INT_FIELD(keyboardKeyHeight);
  APPLY_INT_FIELD(keyboardKeySpacing);
  APPLY_INT_FIELD(keyboardBottomKeyHeight);
  APPLY_INT_FIELD(keyboardBottomKeySpacing);
  APPLY_BOOL_FIELD(keyboardBottomAligned);
  APPLY_BOOL_FIELD(keyboardCenteredText);
  APPLY_INT_FIELD(keyboardVerticalOffset);
  APPLY_INT_FIELD(keyboardTextFieldWidthPercent);
  APPLY_INT_FIELD(keyboardWidthPercent);
  APPLY_INT_FIELD(keyboardKeyCornerRadius);
  APPLY_BOOL_FIELD(keyboardFillUnselected);
  APPLY_BOOL_FIELD(keyboardOutlineAllUnselected);
  APPLY_BOOL_FIELD(keyboardDrawSpecialOutlineWhenUnselected);
  APPLY_INT_FIELD(keyboardSecondaryLabelRightPadding);
  APPLY_INT_FIELD(keyboardSecondaryLabelTopPadding);
  APPLY_INT_FIELD(keyboardMinArrowHeadSize);
  metrics.popupTopOffsetRatio = obj["popupTopOffsetRatio"] | metrics.popupTopOffsetRatio;
  APPLY_INT_FIELD(popupMarginX);
  APPLY_INT_FIELD(popupMarginY);
  APPLY_INT_FIELD(popupFrameThickness);
  APPLY_INT_FIELD(popupCornerRadius);
  APPLY_BOOL_FIELD(popupTextBold);
  APPLY_BOOL_FIELD(popupTextInverted);
  APPLY_INT_FIELD(popupTextBaselineOffsetY);
  APPLY_INT_FIELD(popupProgressBarHeight);
  APPLY_BOOL_FIELD(popupProgressDrawOutline);
  APPLY_BOOL_FIELD(popupProgressClampPercent);
  APPLY_BOOL_FIELD(popupProgressFillInverted);
  APPLY_BOOL_FIELD(popupProgressOutlineInverted);
  APPLY_INT_FIELD(textFieldHorizontalPadding);
  APPLY_INT_FIELD(textFieldNormalThickness);
  APPLY_INT_FIELD(textFieldCursorThickness);
  APPLY_INT_FIELD(textFieldLineEndOffset);
#undef APPLY_BOOL_FIELD
#undef APPLY_INT_FIELD
}

ThemeSlotX parseSlotX(const char* value) {
  if (value == nullptr) return ThemeSlotX::Center;
  if (strcmp(value, "padding") == 0) return ThemeSlotX::Padding;
  if (strcmp(value, "right-padding") == 0) return ThemeSlotX::RightPadding;
  return ThemeSlotX::Center;
}

ThemeSlotY parseSlotY(const char* value) {
  if (value == nullptr) return ThemeSlotY::Top;
  if (strcmp(value, "center") == 0 || strcmp(value, "centerY") == 0) return ThemeSlotY::Center;
  return ThemeSlotY::Top;
}

ThemeBookRef parseBookRef(const char* value) {
  if (value == nullptr) return ThemeBookRef::Selected;
  if (strcmp(value, "previous") == 0) return ThemeBookRef::Previous;
  if (strcmp(value, "next") == 0) return ThemeBookRef::Next;
  if (strcmp(value, "index") == 0) return ThemeBookRef::Index;
  return ThemeBookRef::Selected;
}

void parseTitleSpec(JsonObjectConst obj, ThemeTitleSpec& title) {
  if (obj.isNull()) return;
  title.enabled = obj["enabled"] | true;
  title.fontId = obj["fontId"] | title.fontId;
  const char* font = obj["font"] | nullptr;
  if (font != nullptr) {
    if (strcmp(font, "ui10") == 0) {
      title.fontId = UI_10_FONT_ID;
    } else if (strcmp(font, "small") == 0) {
      title.fontId = SMALL_FONT_ID;
    } else {
      title.fontId = UI_12_FONT_ID;
    }
  } else if (title.fontId == 10) {
    title.fontId = UI_10_FONT_ID;
  } else if (title.fontId == 12) {
    title.fontId = UI_12_FONT_ID;
  }
  title.bold = obj["bold"] | title.bold;
  title.maxLines = obj["maxLines"] | title.maxLines;
  title.offsetY = obj["offsetY"] | title.offsetY;

  const char* style = obj["style"] | nullptr;
  if (style != nullptr) {
    title.bold = strcmp(style, "bold") == 0;
  }
}

void parseCoverSlot(JsonObjectConst obj, ThemeCoverSlotSpec& slot) {
  if (obj.isNull()) return;
  slot.book = parseBookRef(obj["book"] | nullptr);
  slot.bookIndex = obj["bookIndex"] | slot.bookIndex;
  slot.x = parseSlotX(obj["x"] | nullptr);
  slot.y = parseSlotY(obj["y"] | nullptr);
  slot.height = obj["height"] | slot.height;
  slot.widthPercent = obj["widthPercent"] | slot.widthPercent;
  slot.xOffset = obj["xOffset"] | slot.xOffset;
  slot.yOffset = obj["yOffset"] | slot.yOffset;
  slot.selected = obj["selected"] | slot.selected;
  parseTitleSpec(obj["title"].as<JsonObjectConst>(), slot.title);
}

void parseHomeRecentsSpec(JsonObjectConst obj, ThemeHomeRecentsSpec& spec) {
  if (obj.isNull()) return;
  const char* type = obj["type"] | nullptr;
  if (type != nullptr && strcmp(type, "cover-strip") == 0) {
    spec.type = ThemeHomeRecentsType::CoverStrip;
  }
  spec.maxBooks = obj["maxBooks"] | spec.maxBooks;
  spec.wrap = obj["wrap"] | spec.wrap;
  spec.selectionLineWidth = obj["selectionLineWidth"] | spec.selectionLineWidth;
  spec.selectionCornerRadius = obj["selectionCornerRadius"] | spec.selectionCornerRadius;

  JsonArrayConst slots = obj["slots"].as<JsonArrayConst>();
  if (!slots.isNull()) {
    spec.slots.clear();
    for (JsonObjectConst slotObj : slots) {
      if (spec.slots.size() >= 5) break;
      ThemeCoverSlotSpec slot;
      parseCoverSlot(slotObj, slot);
      spec.slots.push_back(slot);
    }
  }
}

ThemeMetrics defaultMetricsFor(SdThemeLayout layout) {
  ThemeMetrics metrics = LyraMetrics::values;
  switch (layout) {
    case SdThemeLayout::ThreeCovers:
      metrics.homeCoverTileHeight = 300;
      metrics.homeRecentBooksCount = 3;
      break;
    case SdThemeLayout::Carousel:
      metrics.homeCoverHeight = 300;
      metrics.homeCoverTileHeight = 340;
      metrics.homeRecentBooksCount = 3;
      break;
    case SdThemeLayout::RoundedRaff:
      metrics.topPadding = 0;
      metrics.headerHeight = 45;
      metrics.listRowHeight = 42;
      metrics.listWithSubtitleRowHeight = 69;
      metrics.menuRowHeight = 42;
      metrics.menuSpacing = 6;
      metrics.homeTopPadding = 55;
      metrics.homeCoverHeight = 300;
      metrics.homeCoverTileHeight = 350;
      metrics.homeContinueReadingInMenu = true;
      metrics.homeMenuTopOffset = 20;
      metrics.keyboardKeyHeight = 30;
      metrics.keyboardKeySpacing = 10;
      metrics.keyboardBottomKeyHeight = 30;
      metrics.keyboardKeyCornerRadius = 10;
      metrics.keyboardFillUnselected = true;
      metrics.keyboardOutlineAllUnselected = true;
      metrics.popupCornerRadius = 18;
      metrics.popupTextBold = true;
      metrics.popupProgressDrawOutline = true;
      metrics.popupProgressClampPercent = true;
      break;
    case SdThemeLayout::Lyra:
    default:
      break;
  }
  return metrics;
}
}  // namespace

const char* SdCardThemeRegistry::activeDeviceId() { return gpio.deviceIsX3() ? "x3" : "x4"; }

bool SdCardThemeRegistry::isSafeId(const char* value) {
  if (value == nullptr || value[0] == '\0') return false;
  if (strstr(value, "..") != nullptr || strchr(value, '/') != nullptr || strchr(value, '\\') != nullptr) return false;
  for (const char* p = value; *p != '\0'; ++p) {
    const char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') return false;
  }
  return true;
}

SdThemeRendererHint SdCardThemeRegistry::rendererHintFor(const char* id, const char* componentModule) {
  if (componentModule != nullptr && strcmp(componentModule, "carousel") == 0) return SdThemeRendererHint::Carousel;
  if (id != nullptr) {
    if (strcmp(id, "classic") == 0 || strcmp(id, "Classic") == 0) return SdThemeRendererHint::Lyra;
    if (strcmp(id, "lyra-3-covers") == 0 || strcmp(id, "Lyra 3 Covers") == 0 || strcmp(id, "LYRA_3_COVERS") == 0) {
      return SdThemeRendererHint::Lyra;
    }
    if (strcmp(id, "roundedraff") == 0 || strcmp(id, "RoundedRaff") == 0) return SdThemeRendererHint::Lyra;
  }
  return SdThemeRendererHint::Lyra;
}

SdThemeLayout SdCardThemeRegistry::layoutFor(const char* id, const char* componentModule, const char* layout) {
  if (componentModule != nullptr && strcmp(componentModule, "carousel") == 0) return SdThemeLayout::Carousel;
  if (layout != nullptr) {
    if (strcmp(layout, "three-covers") == 0) return SdThemeLayout::ThreeCovers;
    if (strcmp(layout, "roundedraff") == 0) return SdThemeLayout::RoundedRaff;
    if (strcmp(layout, "carousel") == 0) return SdThemeLayout::Carousel;
  }
  if (id != nullptr) {
    if (strcmp(id, "lyra-3-covers") == 0 || strcmp(id, "LYRA_3_COVERS") == 0) return SdThemeLayout::ThreeCovers;
    if (strcmp(id, "roundedraff") == 0 || strcmp(id, "RoundedRaff") == 0) return SdThemeLayout::RoundedRaff;
    if (strcmp(id, "carousel") == 0) return SdThemeLayout::Carousel;
  }
  return SdThemeLayout::Lyra;
}

bool SdCardThemeRegistry::parseThemeJson(const char* themeDirPath, SdCardThemeInfo& out) {
  char jsonPath[180];
  snprintf(jsonPath, sizeof(jsonPath), "%s/theme.json", themeDirPath);

  HalFile file;
  if (!Storage.openFileForRead("THREG", jsonPath, file)) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_ERR("THREG", "Theme JSON parse error in %s: %s", jsonPath, err.c_str());
    return false;
  }

  const int schema = doc["schema"] | 0;
  if (schema != THEME_SCHEMA_VERSION) {
    LOG_ERR("THREG", "Unsupported theme schema %d in %s", schema, jsonPath);
    return false;
  }

  const char* id = doc["id"] | "";
  const char* name = doc["name"] | id;
  if (!isSafeId(id) || !isSafeId(name)) {
    LOG_ERR("THREG", "Invalid theme id/name in %s", jsonPath);
    return false;
  }

  const char* deviceId = activeDeviceId();
  JsonObject deviceObj = doc["devices"][deviceId].as<JsonObject>();

  const char* inherits = deviceObj["inherits"] | doc["inherits"] | "lyra";
  const char* homeRecentsModule =
      deviceObj["components"]["homeRecents"]["module"] | doc["components"]["homeRecents"]["module"] | nullptr;
  const char* homeRecentsLayout =
      deviceObj["components"]["homeRecents"]["layout"] | doc["components"]["homeRecents"]["layout"] | nullptr;

  out.id = id;
  out.name = name;
  out.path = themeDirPath;
  out.inherits = inherits;
  out.deviceId = deviceId;
  out.layout = layoutFor(id, homeRecentsModule, homeRecentsLayout);
  out.metrics = defaultMetricsFor(out.layout);
  parseHomeRecentsSpec(doc["components"]["homeRecents"].as<JsonObjectConst>(), out.homeRecents);
  parseHomeRecentsSpec(deviceObj["components"]["homeRecents"].as<JsonObjectConst>(), out.homeRecents);
  if (out.homeRecents.type == ThemeHomeRecentsType::CoverStrip) {
    out.metrics.homeRecentBooksCount = std::max(1, out.homeRecents.maxBooks);
  }
  applyMetricOverrides(doc["metrics"].as<JsonObjectConst>(), out.metrics);
  applyMetricOverrides(deviceObj["metrics"].as<JsonObjectConst>(), out.metrics);
  out.constraints.screenWidth = deviceObj["constraints"]["screenWidth"] | doc["constraints"]["screenWidth"] | 0;
  out.constraints.screenHeight = deviceObj["constraints"]["screenHeight"] | doc["constraints"]["screenHeight"] | 0;
  out.constraints.frontButtons = deviceObj["constraints"]["frontButtons"] | doc["constraints"]["frontButtons"] | 0;
  out.constraints.sideButtons =
      (deviceObj["constraints"]["sideButtons"] | doc["constraints"]["sideButtons"] | "");
  out.rendererHint = rendererHintFor(id, homeRecentsModule);
  return true;
}

void SdCardThemeRegistry::scanRoot(const char* rootPath, std::vector<SdCardThemeInfo>& out) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("THREG", "Themes directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("THREG", "Themes path is not a directory: %s", rootPath);
    return;
  }

  char nameBuffer[128];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
    if (!isSafeId(nameBuffer)) continue;

    char themeDirPath[180];
    snprintf(themeDirPath, sizeof(themeDirPath), "%s/%s", rootPath, nameBuffer);

    SdCardThemeInfo info;
    if (!parseThemeJson(themeDirPath, info)) continue;

    bool exists = false;
    for (const auto& theme : out) {
      if (theme.id == info.id) {
        exists = true;
        break;
      }
    }
    if (exists) continue;

    LOG_DBG("THREG", "Found theme: %s (%s)", info.name.c_str(), info.path.c_str());
    out.push_back(std::move(info));
  }
}

bool SdCardThemeRegistry::discover() {
  themes_.clear();
  themes_.reserve(MAX_SD_THEMES);

  scanRoot(THEMES_DIR_HIDDEN, themes_);
  scanRoot(THEMES_DIR_VISIBLE, themes_);

  std::sort(themes_.begin(), themes_.end(),
            [](const SdCardThemeInfo& a, const SdCardThemeInfo& b) { return a.name < b.name; });

  if (static_cast<int>(themes_.size()) > MAX_SD_THEMES) {
    themes_.resize(MAX_SD_THEMES);
  }

  LOG_DBG("THREG", "Discovery complete: %d themes", static_cast<int>(themes_.size()));
  return !themes_.empty();
}

const SdCardThemeInfo* SdCardThemeRegistry::findTheme(const std::string& id) const {
  auto it = std::find_if(themes_.begin(), themes_.end(), [&](const SdCardThemeInfo& theme) {
    return theme.id == id || theme.name == id;
  });
  return it == themes_.end() ? nullptr : &*it;
}

const char* SdCardThemeRegistry::findThemeRoot(const char* themeId) {
  if (!isSafeId(themeId)) return nullptr;
  char path[180];
  snprintf(path, sizeof(path), "%s/%s", THEMES_DIR_HIDDEN, themeId);
  if (Storage.exists(path)) return THEMES_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", THEMES_DIR_VISIBLE, themeId);
  if (Storage.exists(path)) return THEMES_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardThemeRegistry::defaultWriteRoot() {
  const bool hiddenExists = Storage.exists(THEMES_DIR_HIDDEN);
  const bool visibleExists = Storage.exists(THEMES_DIR_VISIBLE);
  if (hiddenExists) return THEMES_DIR_HIDDEN;
  if (visibleExists) return THEMES_DIR_VISIBLE;
  return THEMES_DIR_HIDDEN;
}
