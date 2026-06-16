#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class NimBLECharacteristic;
class NimBLEServer;
class NimBLEService;

class BluetoothFileReceiver {
 public:
  enum class State {
    Off,
    Starting,
    Waiting,
    Connected,
    Receiving,
    Complete,
    Error,
    OtaReceiving,
    OtaVerifying,
    OtaFlashing,
    OtaRebooting,
  };

  struct StatusSnapshot {
    State state = State::Off;
    bool connected = false;
    std::string fileName;
    std::string lastCompleteName;
    std::string error;
    size_t bytesReceived = 0;
    size_t bytesExpected = 0;
  };

  BluetoothFileReceiver();
  ~BluetoothFileReceiver();

  bool begin();
  void stop();
  StatusSnapshot getStatus() const;

  /// Returns true once OTA_END was received and validation has started — the
  /// activity polls this to drive the flash+reboot on the main loop, off the
  /// BLE callback thread.
  bool isOtaFlashPending() const;

  /// Run validation + flash + restart for the staged firmware image. Called
  /// by the sync activity from its main loop. Blocks for the entire flash
  /// (~30 s). On success it calls ESP.restart() and never returns.
  void performOtaFlash();

  static constexpr const char* DEVICE_NAME = "CrossPoint AirBook";
  static constexpr const char* TARGET_DIR = "/AirBook";
  /// Where the incoming firmware .bin is staged before validation+flash.
  /// Inside TARGET_DIR but leading-dot named so it doesn't show up in the
  /// AirBook iOS library listings, and is cleaned on next OTA start.
  static constexpr const char* OTA_STAGING_PATH = "/AirBook/.firmware-incoming.bin";

  // GATT protocol used by the iOS companion app:
  //
  // Single-receive mode:
  // - control write: "START:<filename>:<byte_count>", "END", or "CANCEL"
  // - data write:    raw file bytes, chunked by the client according to negotiated MTU
  // - status notify: "WAITING", "CONNECTED", "READY", "PROGRESS:<done>:<total>", "DONE", "CANCELLED", or "ERROR:<message>"
  //
  // Sync mode (triggered by SYNC_START):
  // - control write: "SYNC_START", "LIST", "DELETE:<filename>", "SYNC_END"
  //                  plus the standard START/data/CANCEL within each file transfer
  // - status notify: "SYNC_READY", "FILE:<filename>:<size>", "FILES_END",
  //                  "DELETED:<filename>", "SYNC_DONE" (on SYNC_END)
  //                  plus standard READY/PROGRESS/DONE per file (DONE returns to Connected, not Complete)
  //
  // OTA mode (orthogonal to file modes — can be triggered standalone):
  // - control write: "OTA_START:<byte_count>:<sha256_hex>", "OTA_END", "CANCEL"
  // - data write:    raw firmware .bin bytes, chunked
  // - status notify: "OTA_READY", "OTA_PROGRESS:<done>:<total>",
  //                  "OTA_VERIFYING", "OTA_FLASHING", "OTA_REBOOTING",
  //                  "OTA_ERROR:<message>"
  //
  // Info characteristic (read): plain text, newline-separated key=value lines.
  //   fw=<version>
  //   proto=1
  //   caps=book,sync,ota
  static constexpr const char* SERVICE_UUID = "8b45f100-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* CONTROL_UUID = "8b45f101-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* DATA_UUID = "8b45f102-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* STATUS_UUID = "8b45f103-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* INFO_UUID = "8b45f104-9128-4d4f-9a4f-7a0dc1b26b01";

 private:
  class ServerCallbacks;
  class ControlCallbacks;
  class DataCallbacks;

  mutable SemaphoreHandle_t mutex_ = nullptr;
  std::unique_ptr<ServerCallbacks> serverCallbacks_;
  std::unique_ptr<ControlCallbacks> controlCallbacks_;
  std::unique_ptr<DataCallbacks> dataCallbacks_;

  NimBLEServer* server_ = nullptr;
  NimBLEService* service_ = nullptr;
  NimBLECharacteristic* statusCharacteristic_ = nullptr;

  State state_ = State::Off;
  bool active_ = false;
  bool connected_ = false;
  bool uploadOpen_ = false;
  HalFile uploadFile_;
  std::string fileName_;
  std::string filePath_;
  std::string lastCompleteName_;
  std::string error_;
  size_t bytesReceived_ = 0;
  size_t bytesExpected_ = 0;

  bool syncMode_ = false;

  // OTA state
  bool otaMode_ = false;
  bool otaOpen_ = false;
  HalFile otaFile_;
  std::string otaSha256Hex_;
  bool otaFlashRequested_ = false;

  void onClientConnected();
  void onClientDisconnected();
  void onControlWrite(const uint8_t* data, size_t length);
  void onDataWrite(const uint8_t* data, size_t length);

  void startUpload(const std::string& rawFileName, size_t expectedSize);
  void cancelUpload(const char* reason);
  void completeUpload();
  void failUpload(const char* error);
  void failUploadLocked(const char* error);
  void closeUpload(bool removePartial);
  void handleListCommand();
  void handleDeleteCommand(const std::string& filename);
  std::string reserveTargetPath(const std::string& rawFileName) const;
  void notifyStatusLocked(const char* status);

  // OTA helpers
  void startOta(size_t expectedSize, const std::string& sha256Hex);
  void cancelOta(const char* reason);
  void failOtaLocked(const char* message);
  void closeOtaFile(bool removePartial);
  void handleOtaDataWriteLocked(const uint8_t* data, size_t length);
};
