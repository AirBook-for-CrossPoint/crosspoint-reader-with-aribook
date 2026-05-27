#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DeviceProfile.h"
#include "Logging.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;
  const int thumbHeight = coverHeight * DeviceProfiles::current().coverThumbScale;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(thumbHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(thumbHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  const auto touchPoint = mappedInput.getTouchPoint();
  if (touchPoint.valid) {
    lastHandledTouchAt = touchPoint.timestamp;
    ignoreTouchUntilRelease = true;
  }
  resetHomeTouchTracking();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (ignoreTouchUntilRelease) {
    if (mappedInput.isTouchPressed()) {
      return;
    }
    ignoreTouchUntilRelease = false;
    resetHomeTouchTracking();
    return;
  }

  const auto touchPoint = homeTouchPoint();
  if (mappedInput.isTouchPressed() && touchPoint.valid && isFreshHomeTouch(touchPoint.timestamp)) {
    if (homeTouchDownAt == 0) {
      homeTouchDownAt = touchPoint.timestamp;
      homeTouchDownSelector = touchedHomeSelectorIndex(touchPoint);
      LOG_DBG("HOME", "touch down selector=%d x=%d y=%d hits=%u", homeTouchDownSelector, touchPoint.x, touchPoint.y,
              static_cast<unsigned>(homeHitRects.size()));
      if (homeTouchDownSelector >= 0 && selectorIndex != homeTouchDownSelector) {
        selectorIndex = homeTouchDownSelector;
        requestUpdate(true);
      }
    }
    return;
  }

  if (homeTouchDownAt != 0 && mappedInput.wasTouchReleased()) {
    const unsigned long heldMs = millis() - homeTouchDownAt;
    const int releaseSelector = touchPoint.valid ? touchedHomeSelectorIndex(touchPoint) : homeTouchDownSelector;
    if (heldMs < 700 && homeTouchDownSelector >= 0 && releaseSelector == homeTouchDownSelector &&
        homeTouchDownAt != lastHandledTouchAt) {
      selectorIndex = homeTouchDownSelector;
      LOG_DBG("HOME", "touch tap selector=%d held=%lu", selectorIndex, heldMs);
      lastHandledTouchAt = homeTouchDownAt;
      resetHomeTouchTracking();
      activateSelectedItem();
      return;
    }
    LOG_DBG("HOME", "touch ignored selector=%d release=%d held=%lu", homeTouchDownSelector, releaseSelector, heldMs);
    lastHandledTouchAt = homeTouchDownAt;
    resetHomeTouchTracking();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelectedItem();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  homeHitRects.clear();
  homeHitSelectors.clear();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  if (!metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    homeHitRects.push_back(Rect{coverRectX, coverRectY, coverRectW, coverRectH});
    homeHitSelectors.push_back(0);
  }

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  const Rect menuRect = homeMenuRect();
  const int menuTileWidth = menuRect.width - metrics.contentSidePadding * 2;
  for (int row = 0; row < visibleHomeMenuItemCount(); ++row) {
    homeHitRects.push_back(Rect{menuRect.x + metrics.contentSidePadding,
                                menuRect.y + row * (metrics.menuRowHeight + metrics.menuSpacing),
                                menuTileWidth,
                                metrics.menuRowHeight});
    homeHitSelectors.push_back(homeMenuRowToSelectorIndex(row));
  }

  GUI.drawButtonMenu(
      renderer, menuRect,
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

#if !defined(CROSSPOINT_BOARD_MURPHY_M3) && !defined(BOARD_MURPHY_M3)
  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::activateSelectedItem() {
  if (selectorIndex < recentBooks.size()) {
    onSelectBook(recentBooks[selectorIndex].path);
    return;
  }

  const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
  switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
    case HomeMenuItem::FILE_BROWSER:
      onFileBrowserOpen();
      break;
    case HomeMenuItem::RECENTS:
      onRecentsOpen();
      break;
    case HomeMenuItem::OPDS_BROWSER:
      onOpdsBrowserOpen();
      break;
    case HomeMenuItem::FILE_TRANSFER:
      onFileTransferOpen();
      break;
    case HomeMenuItem::SETTINGS_MENU:
      onSettingsOpen();
      break;
    default:
      break;
  }
}

Rect HomeActivity::homeMenuRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  return Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
              pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                            metrics.homeMenuTopOffset
#if !defined(CROSSPOINT_BOARD_MURPHY_M3) && !defined(BOARD_MURPHY_M3)
                            + metrics.buttonHintsHeight
#endif
                            )};
}

int HomeActivity::visibleHomeMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (hasOpdsServers) {
    count++;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count++;
  }
  return count;
}

int HomeActivity::homeMenuRowToSelectorIndex(const int rowIndex) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    if (rowIndex == 0) {
      return 0;
    }
    return static_cast<int>(recentBooks.size()) + rowIndex - 1;
  }
  return static_cast<int>(recentBooks.size()) + rowIndex;
}

InputManager::TouchPoint HomeActivity::homeTouchPoint() const {
  return mappedInput.getTouchPoint();
}

int HomeActivity::touchedHomeSelectorIndex(const InputManager::TouchPoint& touchPoint) const {
  if (!touchPoint.valid || !isFreshHomeTouch(touchPoint.timestamp)) {
    return -1;
  }

  constexpr int hitSlop = 2;
  for (size_t i = 0; i < homeHitRects.size() && i < homeHitSelectors.size(); ++i) {
    const Rect& rect = homeHitRects[i];
    if (touchPoint.x >= rect.x - hitSlop && touchPoint.x < rect.x + rect.width + hitSlop &&
        touchPoint.y >= rect.y - hitSlop && touchPoint.y < rect.y + rect.height + hitSlop) {
      return homeHitSelectors[i];
    }
  }

  LOG_DBG("HOME", "touch miss x=%d y=%d hits=%u", touchPoint.x, touchPoint.y,
          static_cast<unsigned>(homeHitRects.size()));
  return -1;
}

bool HomeActivity::isFreshHomeTouch(const unsigned long timestamp) const {
  return timestamp != lastHandledTouchAt && millis() - timestamp < 1000;
}

void HomeActivity::resetHomeTouchTracking() {
  homeTouchDownAt = 0;
  homeTouchDownSelector = -1;
}
