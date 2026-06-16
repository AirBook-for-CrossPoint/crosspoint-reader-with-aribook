#include "BluetoothSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long ANIMATION_INTERVAL_MS = 1500;
constexpr unsigned long STATUS_POLL_INTERVAL_MS = 500;

int progressPercent(const BluetoothFileReceiver::StatusSnapshot& status) {
  if (status.bytesExpected == 0) return 0;
  return static_cast<int>((status.bytesReceived * 100UL) / status.bytesExpected);
}

bool isOtaState(BluetoothFileReceiver::State s) {
  return s == BluetoothFileReceiver::State::OtaReceiving ||
         s == BluetoothFileReceiver::State::OtaVerifying ||
         s == BluetoothFileReceiver::State::OtaFlashing ||
         s == BluetoothFileReceiver::State::OtaRebooting;
}
}  // namespace

void BluetoothSyncActivity::onEnter() {
  Activity::onEnter();
  animationFrame = 0;
  lastAnimationAt = millis();
  lastStatusPollAt = 0;
  receiver.begin();
  lastSnapshot = receiver.getStatus();
  requestUpdate();
}

void BluetoothSyncActivity::onExit() {
  receiver.stop();
  Activity::onExit();
}

void BluetoothSyncActivity::loop() {
  // Drain a pending OTA flash from the main loop. We do this before
  // listening for the back button so the user can't accidentally cancel
  // the activity mid-OTA. performOtaFlash() either fails (with the
  // receiver back in Error state) or calls ESP.restart() and never returns.
  if (receiver.isOtaFlashPending()) {
    receiver.performOtaFlash();
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // Don't let the user back out while OTA is mid-flight. The radio is
    // sending notifications, the SD has a staging .bin in progress, and
    // teardown here would corrupt both. The OTA finishes (success or
    // failure) before we honour the press.
    const auto current = receiver.getStatus().state;
    if (!isOtaState(current)) {
      finish();
      return;
    }
  }

  const unsigned long now = millis();
  bool repaint = false;

  if (now - lastStatusPollAt >= STATUS_POLL_INTERVAL_MS) {
    lastStatusPollAt = now;
    const auto snapshot = receiver.getStatus();
    if (snapshot.state != lastSnapshot.state || snapshot.connected != lastSnapshot.connected ||
        snapshot.bytesReceived != lastSnapshot.bytesReceived ||
        snapshot.bytesExpected != lastSnapshot.bytesExpected ||
        snapshot.fileName != lastSnapshot.fileName ||
        snapshot.lastCompleteName != lastSnapshot.lastCompleteName ||
        snapshot.error != lastSnapshot.error) {
      lastSnapshot = snapshot;
      repaint = true;
    }
  }

  if (now - lastAnimationAt >= ANIMATION_INTERVAL_MS) {
    lastAnimationAt = now;
    animationFrame = (animationFrame + 1) % 4;
    repaint = true;
  }

  if (repaint) requestUpdate();
}

void BluetoothSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_SYNC_AIRBOOK), tr(STR_CROSSPOINT_AIRBOOK));

  const auto status = receiver.getStatus();
  const int iconY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 4;
  drawBluetoothMark(pageWidth / 2, iconY + 74, animationFrame);
  renderStatus(status);

  // Firmware version badge in the bottom corner — small enough to stay out
  // of the way but visible during an AirBook session so the user can
  // confirm what's running before/after an OTA.
  {
    const int pageHeight = renderer.getScreenHeight();
    const int versionY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
    std::string versionLine = std::string(tr(STR_AIRBOOK_FW_PREFIX)) + CROSSPOINT_VERSION;
    renderer.drawCenteredText(SMALL_FONT_ID, versionY, versionLine.c_str());
  }

  // While OTA is mid-flight we hide the EXIT hint so the user doesn't
  // think they can safely back out — see the input handler in loop().
  const bool hideExit = isOtaState(status.state);
  const auto labels = mappedInput.mapLabels(hideExit ? "" : tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSyncActivity::renderStatus(const BluetoothFileReceiver::StatusSnapshot& status) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int textTop = metrics.topPadding + metrics.headerHeight + 180;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textWidth = pageWidth - metrics.contentSidePadding * 2;

  std::string title;
  std::string detail;

  switch (status.state) {
    case BluetoothFileReceiver::State::Starting:
      title = tr(STR_BLUETOOTH_STARTING);
      detail = tr(STR_KEEP_SCREEN_OPEN);
      break;
    case BluetoothFileReceiver::State::Waiting:
      title = tr(STR_WAITING_FOR_AIRBOOK);
      detail = tr(STR_OPEN_SYNC_HINT);
      break;
    case BluetoothFileReceiver::State::Connected:
      title = tr(STR_SYNC_SYNCING);
      detail = status.lastCompleteName.empty() ? "" : status.lastCompleteName;
      break;
    case BluetoothFileReceiver::State::Receiving:
      title = tr(STR_BLUETOOTH_RECEIVING);
      detail = status.fileName;
      break;
    case BluetoothFileReceiver::State::Complete:
      title = tr(STR_SYNC_COMPLETE);
      detail = "";
      break;
    case BluetoothFileReceiver::State::OtaReceiving:
      title = tr(STR_AIRBOOK_OTA_RECEIVING);
      detail = tr(STR_KEEP_SCREEN_OPEN);
      break;
    case BluetoothFileReceiver::State::OtaVerifying:
      title = tr(STR_AIRBOOK_OTA_VERIFYING);
      detail = "";
      break;
    case BluetoothFileReceiver::State::OtaFlashing:
      title = tr(STR_AIRBOOK_OTA_FLASHING);
      detail = tr(STR_KEEP_SCREEN_OPEN);
      break;
    case BluetoothFileReceiver::State::OtaRebooting:
      title = tr(STR_AIRBOOK_OTA_REBOOTING);
      detail = "";
      break;
    case BluetoothFileReceiver::State::Error:
      // OTA errors land here with status.error populated by failOtaLocked.
      // The bare title doesn't disambiguate between book vs firmware
      // failure, but the detail line (e.g. "Image validation failed")
      // makes it obvious in practice.
      title = tr(STR_BLUETOOTH_ERROR);
      detail = status.error;
      break;
    case BluetoothFileReceiver::State::Off:
      title = tr(STR_BLUETOOTH_OFF);
      detail = "";
      break;
  }

  renderer.drawCenteredText(UI_12_FONT_ID, textTop, title.c_str(), true, EpdFontFamily::BOLD);

  auto lines = renderer.wrappedText(UI_10_FONT_ID, detail.c_str(), textWidth, 2);
  int y = textTop + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += lineHeight;
  }

  // Show a progress bar for both book reception and OTA reception. The
  // verifying/flashing/rebooting OTA states deliberately don't show one —
  // they're not byte-progress driven and an indeterminate spinner here
  // would just lie. The header title is the indicator instead.
  const bool isReceiving = status.state == BluetoothFileReceiver::State::Receiving ||
                           status.state == BluetoothFileReceiver::State::OtaReceiving;
  if (isReceiving && status.bytesExpected > 0) {
    const int progress = progressPercent(status);
    const int barWidth = pageWidth - metrics.contentSidePadding * 3;
    const int barX = (pageWidth - barWidth) / 2;
    const int barY = std::min(pageHeight - metrics.buttonHintsHeight - 110, y + metrics.verticalSpacing * 3);
    drawProgress(barX, barY, barWidth, progress);

    char progressText[32];
    snprintf(progressText, sizeof(progressText), "%d%%", progress);
    renderer.drawCenteredText(UI_10_FONT_ID, barY + 18, progressText);
  }

  // Footer hint: where books land. Skipped during OTA so the eye goes to
  // the OTA state instead of an unrelated path string.
  if (!isOtaState(status.state)) {
    const int targetY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 4 - lineHeight;
    renderer.drawCenteredText(SMALL_FONT_ID, targetY, tr(STR_AIRBOOK_SAVE_HINT));
  }
}

void BluetoothSyncActivity::drawBluetoothMark(const int centerX, const int centerY,
                                               const uint8_t frame) const {
  constexpr int radius = 56;
  constexpr int stemTop = 38;
  constexpr int stemBottom = 38;
  constexpr int wing = 28;
  constexpr int pulseGap = 10;

  const int pulse = 1 + static_cast<int>(frame);
  for (int i = 0; i < pulse; i++) {
    const int r = radius + i * pulseGap;
    renderer.drawArc(r, centerX, centerY, 1, 1, 1, true);
    renderer.drawArc(r, centerX, centerY, -1, 1, 1, true);
    renderer.drawArc(r, centerX, centerY, 1, -1, 1, true);
    renderer.drawArc(r, centerX, centerY, -1, -1, 1, true);
  }

  renderer.drawLine(centerX, centerY - stemTop, centerX, centerY + stemBottom, 3, true);
  renderer.drawLine(centerX, centerY - stemTop, centerX + wing, centerY - 12, 3, true);
  renderer.drawLine(centerX + wing, centerY - 12, centerX - wing / 2, centerY + 12, 3, true);
  renderer.drawLine(centerX - wing / 2, centerY - 12, centerX + wing, centerY + 12, 3, true);
  renderer.drawLine(centerX + wing, centerY + 12, centerX, centerY + stemBottom, 3, true);
}

void BluetoothSyncActivity::drawProgress(const int x, const int y, const int width,
                                          const int progress) const {
  constexpr int height = 12;
  renderer.drawRect(x, y, width, height, 1, true);
  const int fillWidth = std::clamp((width - 4) * progress / 100, 0, width - 4);
  if (fillWidth > 0) {
    renderer.fillRect(x + 2, y + 2, fillWidth, height - 4, true);
  }
}
