#pragma once

#include <functional>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT };

/**
 * NetworkModeSelectionActivity presents the user with a choice between
 * the wifi-based transfer modes. AirBook sync used to live here but is
 * now a first-class entry on the home menu — File Transfer is the
 * wifi/Calibre/hotspot gate only.
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NetworkModeSelection", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  void onModeSelected(NetworkMode mode);
  void onCancel();
};
