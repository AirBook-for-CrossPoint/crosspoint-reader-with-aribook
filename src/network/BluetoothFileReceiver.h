#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

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

  /// True while a BROWSE_READ is mid-stream. The activity polls this and
  /// calls pumpBrowseRead() per loop tick to keep chunks flowing without
  /// blocking the BLE callback thread.
  bool isBrowseReadActive() const;

  /// Push the next chunk of the active browse read over the FILE_OUT
  /// characteristic. No-op if no read is active. Driven by the activity.
  void pumpBrowseRead();

  static constexpr const char* DEVICE_NAME = "CrossPoint AirBook";
  static constexpr const char* TARGET_DIR = "/AirBook";
  /// Where the incoming firmware .bin is staged before validation+flash.
  /// Inside TARGET_DIR but leading-dot named so it doesn't show up in the
  /// AirBook iOS library listings, and is cleaned on next OTA start.
  static constexpr const char* OTA_STAGING_PATH = "/AirBook/.firmware-incoming.bin";
  /// UUID↔filename map for the V2 sync protocol. Plain text, one
  /// `<uuid>=<filename>` line per entry. Leading-dot so it doesn't show up
  /// in the iOS app's library listings.
  static constexpr const char* UUID_MAP_PATH = "/AirBook/.book-uuids";

  // GATT protocol used by the iOS companion app:
  //
  // Single-receive mode (V1, used by the Discover/import-one flow):
  // - control write: "START:<filename>:<byte_count>", "END", or "CANCEL"
  // - data write:    raw file bytes, chunked by the client according to negotiated MTU
  // - status notify: "WAITING", "CONNECTED", "READY", "PROGRESS:<done>:<total>",
  //                  "DONE", "CANCELLED", or "ERROR:<message>"
  //
  // Sync mode V1 (legacy, kept for back-compat):
  // - control write: "SYNC_START", "LIST", "DELETE:<filename>", "SYNC_END"
  // - status notify: "SYNC_READY", "FILE:<filename>:<size>", "FILES_END",
  //                  "DELETED:<filename>", "SYNC_DONE"
  //
  // Sync mode V2 (used by the iOS SyncManager — books indexed by UUID):
  // - control write: "SYNC_START_V2", "LIST_V2",
  //                  "START_V2:<uuid>:<size>:<filename>",
  //                  "DELETE_ENTRY:<uuid>", "DELETE_FILE:<uuid>", "SYNC_END"
  // - status notify: "SYNC_READY_V2",
  //                  "FILE_V2:<uuid>:<has_file 0|1>:<size>:<filename>", "FILES_END",
  //                  "DONE_V2:<uuid>", "DELETED_V2:<uuid>", "FILE_REMOVED:<uuid>",
  //                  "ERROR_V2:<uuid>:<message>", "SYNC_DONE"
  //                  plus READY/PROGRESS:<done>:<total> during each file transfer.
  // The device persists the UUID↔filename map at /AirBook/.book-uuids so
  // the same book keeps the same UUID across syncs.
  //
  // OTA mode (orthogonal to file modes — can be triggered standalone):
  // - control write: "OTA_START:<byte_count>:<sha256_hex>", "OTA_END", "CANCEL"
  // - data write:    raw firmware .bin bytes, chunked
  // - status notify: "OTA_READY", "OTA_PROGRESS:<done>:<total>",
  //                  "OTA_VERIFYING", "OTA_FLASHING", "OTA_REBOOTING",
  //                  "OTA_ERROR:<message>"
  //
  // Browse-read (device → iOS file extraction):
  // - control write: "BROWSE_READ:<filename>" (scoped to /AirBook only —
  //                  no traversal, no leading slash, no `..`)
  //                  "BROWSE_CANCEL"
  // - status notify: "BROWSE_READ_READY:<size>",
  //                  "BROWSE_READ_PROGRESS:<sent>:<total>",
  //                  "BROWSE_READ_DONE", "BROWSE_ERROR:<message>"
  // - file_out notify: raw file bytes streamed from the device. iOS
  //                    subscribes on the FILE_OUT characteristic and
  //                    concatenates chunks until BROWSE_READ_DONE.
  //
  // Info characteristic (read): plain text, newline-separated key=value lines.
  //   fw=<version>
  //   proto=2
  //   caps=book,sync,ota,browse
  static constexpr const char* SERVICE_UUID  = "8b45f100-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* CONTROL_UUID  = "8b45f101-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* DATA_UUID     = "8b45f102-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* STATUS_UUID   = "8b45f103-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* INFO_UUID     = "8b45f104-9128-4d4f-9a4f-7a0dc1b26b01";
  static constexpr const char* FILE_OUT_UUID = "8b45f105-9128-4d4f-9a4f-7a0dc1b26b01";

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
  NimBLECharacteristic* fileOutCharacteristic_ = nullptr;

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
  /// Sync wire protocol the current session negotiated. 1 for the
  /// legacy SYNC_START / LIST / DELETE flow; 2 once SYNC_START_V2 lands.
  uint8_t syncProtoVersion_ = 0;

  // V2: UUID assigned by iOS for the current upload (empty for V1 START).
  // completeUpload() picks this up to decide between DONE and DONE_V2:<uuid>.
  std::string pendingUuid_;

  // V2 persistent UUID↔filename map. Loaded lazily on the first V2 command,
  // saved after each mutation. Both directions are kept in memory so list/
  // resolve operations stay O(1).
  bool uuidMapLoaded_ = false;
  std::unordered_map<std::string, std::string> uuidToFile_;
  std::unordered_map<std::string, std::string> fileToUuid_;

  // OTA state
  bool otaMode_ = false;
  bool otaOpen_ = false;
  HalFile otaFile_;
  std::string otaSha256Hex_;
  bool otaFlashRequested_ = false;

  // Browse-read state. While `browseReading_` is true the activity loop
  // pumps chunks from `browseFile_` over the FILE_OUT notify
  // characteristic on the main task — keeps the BLE stack responsive
  // (no blocking inside callbacks) and lets the user CANCEL mid-stream.
  bool browseReading_ = false;
  HalFile browseFile_;
  std::string browsePath_;
  size_t browseBytesSent_ = 0;
  size_t browseBytesTotal_ = 0;
  size_t browseNextProgressMark_ = 0;

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

  // V2 sync helpers
  void ensureUuidMapLoaded();
  void loadUuidMap();
  void saveUuidMap() const;
  void mapPut(const std::string& uuid, const std::string& filename);
  void mapRemoveByUuid(const std::string& uuid);
  void mapRemoveByFilename(const std::string& filename);
  std::string mapFilenameForUuid(const std::string& uuid) const;
  std::string mapUuidForFilename(const std::string& filename);  // creates if missing
  void handleListV2Command();
  void handleStartV2(const std::string& uuid, size_t size, const std::string& filename);
  void handleDeleteByUuid(const std::string& uuid, const char* successPrefix);

  // OTA helpers
  void startOta(size_t expectedSize, const std::string& sha256Hex);
  void cancelOta(const char* reason);
  void failOtaLocked(const char* message);
  void closeOtaFile(bool removePartial);
  void handleOtaDataWriteLocked(const uint8_t* data, size_t length);

  // Browse-read helpers
  void handleBrowseRead(const std::string& rawFilename);
  void cancelBrowseRead(const char* reason);
  void finishBrowseRead();
};
