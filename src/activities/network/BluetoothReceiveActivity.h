#pragma once

#include "activities/Activity.h"
#include "network/BluetoothFileReceiver.h"

class BluetoothReceiveActivity final : public Activity {
  BluetoothFileReceiver receiver;
  unsigned long lastAnimationAt = 0;
  unsigned long lastStatusPollAt = 0;
  uint8_t animationFrame = 0;
  BluetoothFileReceiver::StatusSnapshot lastSnapshot;

  void renderStatus(const BluetoothFileReceiver::StatusSnapshot& status) const;
  void drawBluetoothMark(int centerX, int centerY, uint8_t frame) const;
  void drawProgress(int x, int y, int width, int progress) const;

 public:
  explicit BluetoothReceiveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BluetoothReceive", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
};
