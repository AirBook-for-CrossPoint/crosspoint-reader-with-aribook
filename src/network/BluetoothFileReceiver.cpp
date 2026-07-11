#include "BluetoothFileReceiver.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <esp_random.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "network/FirmwareFlasher.h"
#include "util/BookCacheUtils.h"

// SdFreeSpace.cpp — kept prototype-only here so this TU never sees SdFat.
uint64_t crosspointSdFreeKB();

namespace {
constexpr size_t MAX_FILE_NAME_LEN = 96;
constexpr size_t MAX_UPLOAD_SIZE = 200UL * 1024UL * 1024UL;

class ReceiverLock {
 public:
  explicit ReceiverLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  ~ReceiverLock() {
    if (mutex_) xSemaphoreGive(mutex_);
  }

 private:
  SemaphoreHandle_t mutex_;
};

bool isSupportedBookFile(const std::string& filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasBmpExtension(filename);
}

std::string baseNameOnly(const std::string& raw) {
  const auto slash = raw.find_last_of("/\\");
  if (slash == std::string::npos) return raw;
  return raw.substr(slash + 1);
}

std::string trimFilename(const std::string& raw) {
  const std::string base = baseNameOnly(raw);
  const auto first = std::find_if_not(base.begin(), base.end(), [](unsigned char c) { return std::isspace(c); });
  const auto last = std::find_if_not(base.rbegin(), base.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (first >= last) return "book.epub";
  return std::string(first, last);
}

std::string uniquePathFor(const std::string& dir, const std::string& filename) {
  std::string path = dir + "/" + filename;
  if (!Storage.exists(path.c_str())) return path;

  const auto dot = filename.find_last_of('.');
  const std::string stem = dot == std::string::npos ? filename : filename.substr(0, dot);
  const std::string ext = dot == std::string::npos ? "" : filename.substr(dot);
  for (int i = 1; i < 1000; i++) {
    path = dir + "/" + stem + " (" + std::to_string(i) + ")" + ext;
    if (!Storage.exists(path.c_str())) return path;
  }
  return dir + "/" + stem + "-new" + ext;
}

bool isValidUuid(const std::string& s) {
  if (s.length() != 36) return false;
  for (size_t i = 0; i < 36; i++) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

// Generate a v4 UUID for "foreign" books on the SD that we receive without
// an iOS-assigned UUID. Uppercase to match the Swift UUID().uuidString
// canonical format.
std::string generateUuidV4() {
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char buf[37];
  snprintf(buf, sizeof(buf),
           "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return std::string(buf);
}
}  // namespace

class BluetoothFileReceiver::ServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(BluetoothFileReceiver& owner) : owner_(owner) {}

  void onConnect(NimBLEServer*, NimBLEConnInfo&) override { owner_.onClientConnected(); }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo&, int) override {
    owner_.onClientDisconnected();
    if (server) server->startAdvertising();
  }

 private:
  BluetoothFileReceiver& owner_;
};

class BluetoothFileReceiver::ControlCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit ControlCallbacks(BluetoothFileReceiver& owner) : owner_(owner) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const std::string value = characteristic->getValue();
    owner_.onControlWrite(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }

 private:
  BluetoothFileReceiver& owner_;
};

class BluetoothFileReceiver::DataCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit DataCallbacks(BluetoothFileReceiver& owner) : owner_(owner) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const std::string value = characteristic->getValue();
    owner_.onDataWrite(reinterpret_cast<const uint8_t*>(value.data()), value.size());
  }

 private:
  BluetoothFileReceiver& owner_;
};

// Refreshes the Info characteristic value on every iOS read. We don't
// pre-cache the payload — used_kb / books need to reflect what's
// currently on the SD card, not what was there at boot.
class BluetoothFileReceiver::InfoCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit InfoCallbacks(BluetoothFileReceiver& owner) : owner_(owner) {}

  void onRead(NimBLECharacteristic*, NimBLEConnInfo&) override {
    owner_.refreshInfoPayload();
  }

 private:
  BluetoothFileReceiver& owner_;
};

BluetoothFileReceiver::BluetoothFileReceiver() {
  mutex_ = xSemaphoreCreateMutex();
  assert(mutex_ != nullptr);
}

BluetoothFileReceiver::~BluetoothFileReceiver() {
  stop();
  if (mutex_) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
}

bool BluetoothFileReceiver::begin() {
  stop();

  {
    ReceiverLock lock(mutex_);
    state_ = State::Starting;
    active_ = true;
    connected_ = false;
    syncMode_ = false;
    syncProtoVersion_ = 0;
    pendingUuid_.clear();
    uuidMapLoaded_ = false;
    uuidToFile_.clear();
    fileToUuid_.clear();
    otaMode_ = false;
    otaOpen_ = false;
    otaSha256Hex_.clear();
    otaFlashRequested_ = false;
    browseReading_ = false;
    browsePath_.clear();
    browseBytesSent_ = 0;
    browseBytesTotal_ = 0;
    browseNextProgressMark_ = 0;
    fileName_.clear();
    filePath_.clear();
    lastCompleteName_.clear();
    error_.clear();
    bytesReceived_ = 0;
    bytesExpected_ = 0;
  }

  LOG_DBG("BLE", "Starting CrossPoint AirBook receiver");

  if (!NimBLEDevice::init(DEVICE_NAME)) {
    failUpload("Failed to start Bluetooth");
    return false;
  }
  NimBLEDevice::setMTU(517);

  serverCallbacks_ = std::make_unique<ServerCallbacks>(*this);
  controlCallbacks_ = std::make_unique<ControlCallbacks>(*this);
  dataCallbacks_ = std::make_unique<DataCallbacks>(*this);
  infoCallbacks_ = std::make_unique<InfoCallbacks>(*this);

  server_ = NimBLEDevice::createServer();
  if (!server_) {
    failUpload("Failed to create Bluetooth server");
    return false;
  }
  server_->setCallbacks(serverCallbacks_.get());
  server_->advertiseOnDisconnect(true);

  service_ = server_->createService(SERVICE_UUID);
  if (!service_) {
    failUpload("Failed to create Bluetooth service");
    return false;
  }

  auto* control = service_->createCharacteristic(CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  auto* data = service_->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  statusCharacteristic_ = service_->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  auto* info = service_->createCharacteristic(INFO_UUID, NIMBLE_PROPERTY::READ);
  fileOutCharacteristic_ = service_->createCharacteristic(FILE_OUT_UUID, NIMBLE_PROPERTY::NOTIFY);
  if (!control || !data || !statusCharacteristic_ || !info || !fileOutCharacteristic_) {
    failUpload("Failed to create Bluetooth characteristics");
    return false;
  }

  control->setCallbacks(controlCallbacks_.get());
  data->setCallbacks(dataCallbacks_.get());
  info->setCallbacks(infoCallbacks_.get());
  infoCharacteristic_ = info;
  statusCharacteristic_->setValue("WAITING");

  // Seed the Info characteristic with a non-stat payload so a client
  // that reads BEFORE notifying gets at least fw/proto/caps. The
  // InfoCallbacks::onRead callback recomputes the full payload (with
  // used_kb / books) on every subsequent read.
  refreshInfoPayload();

  service_->start();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setName(DEVICE_NAME);
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->start();

  {
    ReceiverLock lock(mutex_);
    state_ = State::Waiting;
  }
  LOG_DBG("BLE", "Advertising as %s", DEVICE_NAME);
  return true;
}

void BluetoothFileReceiver::stop() {
  bool wasActive = false;
  {
    ReceiverLock lock(mutex_);
    wasActive = active_;
    active_ = false;
    if (uploadOpen_) {
      closeUpload(true);
    }
    state_ = State::Off;
    connected_ = false;
  }

  if (wasActive || NimBLEDevice::isInitialized()) {
    LOG_DBG("BLE", "Stopping Bluetooth receiver");
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);
  }

  {
    ReceiverLock lock(mutex_);
    server_ = nullptr;
    service_ = nullptr;
    statusCharacteristic_ = nullptr;
    fileOutCharacteristic_ = nullptr;
    infoCharacteristic_ = nullptr;
  }
  dataCallbacks_.reset();
  controlCallbacks_.reset();
  serverCallbacks_.reset();
  infoCallbacks_.reset();
}

BluetoothFileReceiver::StatusSnapshot BluetoothFileReceiver::getStatus() const {
  ReceiverLock lock(mutex_);
  return StatusSnapshot{.state = state_,
                        .connected = connected_,
                        .fileName = fileName_,
                        .lastCompleteName = lastCompleteName_,
                        .error = error_,
                        .bytesReceived = bytesReceived_,
                        .bytesExpected = bytesExpected_};
}

void BluetoothFileReceiver::onClientConnected() {
  ReceiverLock lock(mutex_);
  if (!active_) return;
  connected_ = true;
  if (state_ == State::Waiting) state_ = State::Connected;
  notifyStatusLocked("CONNECTED");
  LOG_DBG("BLE", "Client connected");
}

void BluetoothFileReceiver::onClientDisconnected() {
  ReceiverLock lock(mutex_);
  if (!active_) return;
  connected_ = false;
  if (uploadOpen_) closeUpload(true);
  // Abort a partial OTA: a half-streamed firmware image is useless and we
  // must not let it carry over into a future session. Flash never starts
  // until OTA_END arrives, so just drop the staging file.
  if (otaOpen_ && !otaFlashRequested_) {
    closeOtaFile(true);
    otaMode_ = false;
  }
  // Drop a half-finished browse read — iOS will retry after reconnecting.
  if (browseReading_) {
    browseFile_.close();
    browseReading_ = false;
    browsePath_.clear();
  }
  if (state_ != State::Complete && state_ != State::Error &&
      state_ != State::OtaVerifying && state_ != State::OtaFlashing &&
      state_ != State::OtaRebooting) {
    state_ = State::Waiting;
  }
  notifyStatusLocked("WAITING");
  LOG_DBG("BLE", "Client disconnected");
}

void BluetoothFileReceiver::onControlWrite(const uint8_t* data, const size_t length) {
  std::string message(reinterpret_cast<const char*>(data), length);
  LOG_DBG("BLE", "Control: %s", message.c_str());

  if (message == "SYNC_START") {
    ReceiverLock lock(mutex_);
    if (!active_) return;
    syncMode_ = true;
    syncProtoVersion_ = 1;
    notifyStatusLocked("SYNC_READY");
    return;
  }

  if (message == "SYNC_START_V2") {
    ReceiverLock lock(mutex_);
    if (!active_) return;
    syncMode_ = true;
    syncProtoVersion_ = 2;
    ensureUuidMapLoaded();
    notifyStatusLocked("SYNC_READY_V2");
    return;
  }

  if (message == "LIST") {
    handleListCommand();
    return;
  }

  if (message == "LIST_V2") {
    handleListV2Command();
    return;
  }

  if (message.rfind("DELETE:", 0) == 0) {
    handleDeleteCommand(message.substr(7));
    return;
  }

  if (message.rfind("DELETE_ENTRY:", 0) == 0) {
    handleDeleteByUuid(message.substr(13), "DELETED_V2:");
    return;
  }

  if (message.rfind("DELETE_FILE:", 0) == 0) {
    handleDeleteByUuid(message.substr(12), "FILE_REMOVED:");
    return;
  }

  if (message == "SYNC_END") {
    ReceiverLock lock(mutex_);
    if (!active_) return;
    syncMode_ = false;
    syncProtoVersion_ = 0;
    if (uploadOpen_) closeUpload(true);
    state_ = State::Complete;
    lastCompleteName_.clear();
    notifyStatusLocked("SYNC_DONE");
    return;
  }

  if (message.rfind("START:", 0) == 0) {
    const auto secondColon = message.find(':', 6);
    if (secondColon == std::string::npos) {
      failUpload("Invalid START message");
      return;
    }

    const std::string filename = message.substr(6, secondColon - 6);
    const std::string sizeToken = message.substr(secondColon + 1);
    if (sizeToken.empty() || !std::all_of(sizeToken.begin(), sizeToken.end(), [](unsigned char c) {
          return std::isdigit(c);
        })) {
      failUpload("Invalid file size");
      return;
    }

    {
      ReceiverLock lock(mutex_);
      pendingUuid_.clear();  // V1 START → DONE without UUID
    }
    startUpload(filename, strtoul(sizeToken.c_str(), nullptr, 10));
    return;
  }

  if (message.rfind("START_V2:", 0) == 0) {
    // START_V2:<uuid>:<size>:<filename>  (filename may contain colons)
    const size_t firstColon = 8;  // start_idx of ':' after "START_V2"
    const auto secondColon = message.find(':', firstColon + 1);
    const auto thirdColon = secondColon == std::string::npos ?
                             std::string::npos : message.find(':', secondColon + 1);
    if (secondColon == std::string::npos || thirdColon == std::string::npos) {
      ReceiverLock lock(mutex_);
      const std::string err = "ERROR_V2::Invalid START_V2 message";
      notifyStatusLocked(err.c_str());
      return;
    }
    const std::string uuid = message.substr(firstColon + 1, secondColon - firstColon - 1);
    const std::string sizeToken = message.substr(secondColon + 1, thirdColon - secondColon - 1);
    const std::string filename = message.substr(thirdColon + 1);

    if (!isValidUuid(uuid)) {
      ReceiverLock lock(mutex_);
      const std::string err = "ERROR_V2::Invalid UUID";
      notifyStatusLocked(err.c_str());
      return;
    }
    if (sizeToken.empty() || !std::all_of(sizeToken.begin(), sizeToken.end(),
                                          [](unsigned char c) { return std::isdigit(c); })) {
      ReceiverLock lock(mutex_);
      const std::string err = "ERROR_V2:" + uuid + ":Invalid file size";
      notifyStatusLocked(err.c_str());
      return;
    }

    handleStartV2(uuid, strtoul(sizeToken.c_str(), nullptr, 10), filename);
    return;
  }

  if (message == "CANCEL") {
    {
      ReceiverLock lock(mutex_);
      if (otaOpen_) {
        // Cancel an in-flight OTA: just drop the staging file. We don't
        // touch otadata because the partition write only happens in
        // OTA_END's flash step, which we haven't reached.
        closeOtaFile(true);
        otaMode_ = false;
        otaFlashRequested_ = false;
        state_ = connected_ ? State::Connected : State::Waiting;
        notifyStatusLocked("CANCELLED");
        return;
      }
    }
    cancelUpload("Transfer cancelled");
    return;
  }

  if (message == "END") {
    ReceiverLock lock(mutex_);
    if (uploadOpen_ && bytesReceived_ == bytesExpected_) {
      completeUpload();
    } else {
      failUploadLocked("Transfer incomplete");
    }
    return;
  }

  if (message.rfind("OTA_START:", 0) == 0) {
    // OTA_START:<bytes>:<sha256_hex_lowercase>
    const auto firstColon = message.find(':');
    const auto secondColon = message.find(':', firstColon + 1);
    if (secondColon == std::string::npos) {
      ReceiverLock lock(mutex_);
      failOtaLocked("Invalid OTA_START");
      return;
    }
    const std::string sizeToken = message.substr(firstColon + 1, secondColon - firstColon - 1);
    const std::string sha256Hex = message.substr(secondColon + 1);
    if (sizeToken.empty() || !std::all_of(sizeToken.begin(), sizeToken.end(),
                                          [](unsigned char c) { return std::isdigit(c); })) {
      ReceiverLock lock(mutex_);
      failOtaLocked("Invalid OTA size");
      return;
    }
    if (sha256Hex.length() != 64) {
      // sha256 hex is exactly 64 chars; reject anything else upfront.
      ReceiverLock lock(mutex_);
      failOtaLocked("Invalid SHA-256");
      return;
    }
    startOta(strtoul(sizeToken.c_str(), nullptr, 10), sha256Hex);
    return;
  }

  if (message == "OTA_END") {
    ReceiverLock lock(mutex_);
    if (!otaOpen_) {
      failOtaLocked("No OTA in progress");
      return;
    }
    if (bytesReceived_ != bytesExpected_) {
      failOtaLocked("Firmware transfer incomplete");
      return;
    }
    closeOtaFile(false);
    state_ = State::OtaVerifying;
    otaFlashRequested_ = true;
    notifyStatusLocked("OTA_VERIFYING");
    // The activity loop picks up otaFlashRequested_ and calls
    // performOtaFlash() on the main thread — we deliberately don't run
    // validation/flash inside this BLE callback so the radio stack stays
    // responsive while we still send the OTA_VERIFYING / OTA_FLASHING /
    // OTA_REBOOTING notifications.
    return;
  }

  if (message.rfind("BROWSE_LS:", 0) == 0) {
    handleBrowseLs(message.substr(10));
    return;
  }

  if (message.rfind("BROWSE_READ:", 0) == 0) {
    handleBrowseRead(message.substr(12));
    return;
  }

  if (message == "BROWSE_CANCEL") {
    cancelBrowseRead("Cancelled");
    return;
  }

  failUpload("Unknown control message");
}

void BluetoothFileReceiver::onDataWrite(const uint8_t* data, const size_t length) {
  ReceiverLock lock(mutex_);
  if (!active_) {
    notifyStatusLocked("ERROR:No active transfer");
    return;
  }
  if (otaOpen_) {
    handleOtaDataWriteLocked(data, length);
    return;
  }
  if (!uploadOpen_) {
    notifyStatusLocked("ERROR:No active transfer");
    return;
  }

  if (bytesReceived_ + length > bytesExpected_) {
    closeUpload(true);
    state_ = State::Error;
    error_ = "Transfer overflow";
    notifyStatusLocked("ERROR:Transfer overflow");
    return;
  }

  const size_t written = uploadFile_.write(data, length);
  if (written != length) {
    closeUpload(true);
    state_ = State::Error;
    error_ = "SD card write failed";
    notifyStatusLocked("ERROR:SD card write failed");
    return;
  }

  bytesReceived_ += written;
  if (bytesExpected_ > 0 && bytesReceived_ >= bytesExpected_) {
    completeUpload();
    return;
  }

  if (bytesExpected_ > 0) {
    char progress[48];
    snprintf(progress, sizeof(progress), "PROGRESS:%u:%u", static_cast<unsigned>(bytesReceived_),
             static_cast<unsigned>(bytesExpected_));
    notifyStatusLocked(progress);
  }
}

void BluetoothFileReceiver::handleOtaDataWriteLocked(const uint8_t* data, const size_t length) {
  if (bytesReceived_ + length > bytesExpected_) {
    failOtaLocked("Firmware transfer overflow");
    return;
  }
  const size_t written = otaFile_.write(data, length);
  if (written != length) {
    failOtaLocked("SD card write failed");
    return;
  }
  bytesReceived_ += written;

  // Throttle the notify to ~1% steps. The iOS app drives the progress bar from
  // these and on a 2-3 MB image, a notify per chunk floods the GATT queue and
  // slows the actual transfer.
  if (bytesExpected_ > 0) {
    static constexpr size_t kNotifyEveryBytes = 32 * 1024;
    if ((bytesReceived_ % kNotifyEveryBytes) < length || bytesReceived_ == bytesExpected_) {
      char progress[64];
      snprintf(progress, sizeof(progress), "OTA_PROGRESS:%u:%u",
               static_cast<unsigned>(bytesReceived_),
               static_cast<unsigned>(bytesExpected_));
      notifyStatusLocked(progress);
    }
  }
}

void BluetoothFileReceiver::startOta(const size_t expectedSize, const std::string& sha256Hex) {
  ReceiverLock lock(mutex_);
  if (!active_) return;

  if (uploadOpen_ || otaOpen_) {
    failOtaLocked("Another transfer is running");
    return;
  }
  if (expectedSize < 64UL * 1024UL) {
    failOtaLocked("Firmware too small");
    return;
  }
  // 5 MB hard cap as defence-in-depth; the OTA partition is ~6.25 MB but
  // the bootloader needs some headroom and our own builds never exceed
  // ~3 MB. The flasher does a stricter check against the actual partition.
  if (expectedSize > 5UL * 1024UL * 1024UL) {
    failOtaLocked("Firmware too large");
    return;
  }

  if (!Storage.exists(TARGET_DIR) && !Storage.mkdir(TARGET_DIR)) {
    failOtaLocked("Failed to create AirBook folder");
    return;
  }

  // Drop a stale staging file from a previous aborted OTA, if any.
  Storage.remove(OTA_STAGING_PATH);

  if (!Storage.openFileForWrite("OTA", OTA_STAGING_PATH, otaFile_)) {
    failOtaLocked("Failed to open staging file");
    return;
  }

  otaOpen_ = true;
  otaMode_ = true;
  state_ = State::OtaReceiving;
  bytesExpected_ = expectedSize;
  bytesReceived_ = 0;
  otaSha256Hex_ = sha256Hex;
  otaFlashRequested_ = false;
  error_.clear();
  notifyStatusLocked("OTA_READY");
  LOG_INF("BLE", "OTA starting: %u bytes", static_cast<unsigned>(expectedSize));
}

void BluetoothFileReceiver::cancelOta(const char* reason) {
  ReceiverLock lock(mutex_);
  if (otaOpen_) {
    closeOtaFile(true);
    otaMode_ = false;
    otaFlashRequested_ = false;
    state_ = connected_ ? State::Connected : State::Waiting;
    error_ = reason;
    notifyStatusLocked("CANCELLED");
  }
}

void BluetoothFileReceiver::failOtaLocked(const char* message) {
  if (otaOpen_) closeOtaFile(true);
  otaMode_ = false;
  otaFlashRequested_ = false;
  state_ = State::Error;
  error_ = message;
  const std::string status = std::string("OTA_ERROR:") + message;
  notifyStatusLocked(status.c_str());
  LOG_ERR("BLE", "OTA failed: %s", message);
}

void BluetoothFileReceiver::closeOtaFile(const bool removePartial) {
  otaFile_.close();
  otaOpen_ = false;
  if (removePartial) {
    Storage.remove(OTA_STAGING_PATH);
  }
}

bool BluetoothFileReceiver::isOtaFlashPending() const {
  ReceiverLock lock(mutex_);
  return otaFlashRequested_;
}

void BluetoothFileReceiver::performOtaFlash() {
  // Claim the work atomically so the activity's poll-driven loop can't
  // accidentally call us twice.
  {
    ReceiverLock lock(mutex_);
    if (!otaFlashRequested_) return;
    otaFlashRequested_ = false;
  }

  LOG_INF("OTA", "Validating staged firmware at %s", OTA_STAGING_PATH);
  // Pre-validate before any partition write so we never corrupt otadata
  // with a half-streamed image. The flasher also validates internally;
  // doing it here lets us surface OTA_ERROR to iOS before going dark.
  const auto vr = firmware_flash::validateImageFile(OTA_STAGING_PATH, 0);
  if (vr != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "validation failed: %s", firmware_flash::resultName(vr));
    ReceiverLock lock(mutex_);
    Storage.remove(OTA_STAGING_PATH);
    failOtaLocked("Image validation failed");
    return;
  }

  {
    ReceiverLock lock(mutex_);
    state_ = State::OtaFlashing;
    notifyStatusLocked("OTA_FLASHING");
  }

  // We deliberately don't pump OTA_PROGRESS notifications during flash —
  // the partition write blocks the radio task in short bursts and the
  // single OTA_FLASHING state is enough for iOS to keep an indeterminate
  // spinner running. The whole flash runs in well under a minute.
  const auto fr = firmware_flash::flashFromSdPath(OTA_STAGING_PATH, nullptr, nullptr,
                                                  /*alreadyValidated=*/true);
  if (fr != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "flash failed: %s", firmware_flash::resultName(fr));
    ReceiverLock lock(mutex_);
    Storage.remove(OTA_STAGING_PATH);
    failOtaLocked("Flash write failed");
    return;
  }

  // Clean up the staging file. Even if remove fails the bootloader will
  // happily ignore a stray .firmware-incoming.bin — it's only a hint, the
  // real firmware now lives in the OTA partition.
  Storage.remove(OTA_STAGING_PATH);

  {
    ReceiverLock lock(mutex_);
    state_ = State::OtaRebooting;
    notifyStatusLocked("OTA_REBOOTING");
  }

  // Give the BLE notify a beat to actually reach the phone before we drop
  // the link by rebooting. 1.5 s is what SdFirmwareUpdateActivity uses.
  delay(1500);
  LOG_INF("OTA", "OTA complete — restarting");
  ESP.restart();
}

void BluetoothFileReceiver::startUpload(const std::string& rawFileName, const size_t expectedSize) {
  ReceiverLock lock(mutex_);
  if (!active_) return;

  if (uploadOpen_) {
    failUploadLocked("Transfer already running");
    return;
  }

  if (expectedSize > MAX_UPLOAD_SIZE) {
    failUploadLocked("File too large");
    return;
  }

  filePath_ = reserveTargetPath(rawFileName);
  fileName_ = filePath_.substr(filePath_.find_last_of('/') + 1);
  if (!isSupportedBookFile(fileName_)) {
    failUploadLocked("Unsupported file type");
    return;
  }

  if (!Storage.exists(TARGET_DIR) && !Storage.mkdir(TARGET_DIR)) {
    failUploadLocked("Failed to create AirBook folder");
    return;
  }

  if (!Storage.openFileForWrite("BLE", filePath_, uploadFile_)) {
    failUploadLocked("Failed to create file");
    return;
  }

  uploadOpen_ = true;
  state_ = State::Receiving;
  bytesExpected_ = expectedSize;
  bytesReceived_ = 0;
  error_.clear();
  notifyStatusLocked("READY");
  LOG_DBG("BLE", "Receiving %s (%u bytes)", filePath_.c_str(), static_cast<unsigned>(expectedSize));

  if (expectedSize == 0) {
    completeUpload();
  }
}

void BluetoothFileReceiver::cancelUpload(const char* reason) {
  ReceiverLock lock(mutex_);
  if (uploadOpen_) closeUpload(true);
  state_ = connected_ ? State::Connected : State::Waiting;
  error_ = reason;
  notifyStatusLocked("CANCELLED");
}

void BluetoothFileReceiver::completeUpload() {
  if (!uploadOpen_) return;

  uploadFile_.flush();
  uploadFile_.close();
  uploadOpen_ = false;
  lastCompleteName_ = fileName_;
  clearBookCache(filePath_.c_str());
  LOG_DBG("BLE", "Received %s (%u bytes)", filePath_.c_str(), static_cast<unsigned>(bytesReceived_));

  if (syncMode_) {
    // In sync mode stay Connected so the iOS can issue the next START/DELETE
    state_ = State::Connected;
  } else {
    state_ = State::Complete;
  }

  // V2 upload: rewrite the UUID↔filename map so subsequent LIST_V2 / DELETE
  // commands resolve to the right file, then reply DONE_V2:<uuid>. V1 path
  // keeps the bare "DONE" reply for backwards compatibility.
  if (!pendingUuid_.empty()) {
    // If the iOS app re-uploads with the same UUID after the device-side
    // filename was changed (e.g. dedupe suffix), the map should point at the
    // newly-written file, not whatever it used to point at.
    mapRemoveByUuid(pendingUuid_);
    mapRemoveByFilename(fileName_);
    mapPut(pendingUuid_, fileName_);
    saveUuidMap();
    const std::string msg = std::string("DONE_V2:") + pendingUuid_;
    notifyStatusLocked(msg.c_str());
    pendingUuid_.clear();
  } else {
    notifyStatusLocked("DONE");
  }
}

void BluetoothFileReceiver::failUpload(const char* error) {
  ReceiverLock lock(mutex_);
  failUploadLocked(error);
}

void BluetoothFileReceiver::failUploadLocked(const char* error) {
  if (uploadOpen_) closeUpload(true);
  state_ = State::Error;
  error_ = error;
  // V2 callers want a per-uuid ERROR_V2:<uuid>:<msg> so the iOS sync can
  // skip just this book and continue. V1 keeps the bare ERROR: form.
  if (!pendingUuid_.empty()) {
    const std::string status = std::string("ERROR_V2:") + pendingUuid_ + ":" + error;
    notifyStatusLocked(status.c_str());
    pendingUuid_.clear();
  } else {
    const std::string status = std::string("ERROR:") + error;
    notifyStatusLocked(status.c_str());
  }
  LOG_ERR("BLE", "%s", error);
}

void BluetoothFileReceiver::closeUpload(const bool removePartial) {
  uploadFile_.close();
  uploadOpen_ = false;
  if (removePartial && !filePath_.empty()) {
    Storage.remove(filePath_.c_str());
  }
}

std::string BluetoothFileReceiver::reserveTargetPath(const std::string& rawFileName) const {
  std::string trimmed = trimFilename(rawFileName);
  char sanitized[MAX_FILE_NAME_LEN];
  FsHelpers::sanitizePathComponentForFat32(trimmed.c_str(), sanitized, sizeof(sanitized));
  if (sanitized[0] == '\0') {
    strncpy(sanitized, "book.epub", sizeof(sanitized));
    sanitized[sizeof(sanitized) - 1] = '\0';
  }
  return uniquePathFor(TARGET_DIR, sanitized);
}

void BluetoothFileReceiver::handleListCommand() {
  // Collect file entries outside the mutex to avoid blocking BLE callbacks during I/O
  std::vector<std::pair<std::string, size_t>> entries;

  if (Storage.exists(TARGET_DIR)) {
    auto root = Storage.open(TARGET_DIR);
    if (root && root.isDirectory()) {
      root.rewindDirectory();
      char nameBuf[MAX_FILE_NAME_LEN + 1];
      for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
        if (file.isDirectory()) continue;
        file.getName(nameBuf, sizeof(nameBuf));
        const std::string name(nameBuf);
        if (isSupportedBookFile(name)) {
          entries.push_back({name, static_cast<size_t>(file.fileSize())});
        }
      }
    }
  }

  ReceiverLock lock(mutex_);
  if (!active_) return;

  char notification[MAX_FILE_NAME_LEN + 32];
  for (const auto& entry : entries) {
    snprintf(notification, sizeof(notification), "FILE:%s:%u",
             entry.first.c_str(), static_cast<unsigned>(entry.second));
    notifyStatusLocked(notification);
  }
  notifyStatusLocked("FILES_END");
}

void BluetoothFileReceiver::handleDeleteCommand(const std::string& filename) {
  // Reject path traversal and non-book files
  if (filename.empty() ||
      filename.find('/') != std::string::npos ||
      filename.find('\\') != std::string::npos ||
      filename == "." || filename == "..") {
    ReceiverLock lock(mutex_);
    notifyStatusLocked("ERROR:Invalid filename");
    return;
  }
  if (!isSupportedBookFile(filename)) {
    ReceiverLock lock(mutex_);
    notifyStatusLocked("ERROR:Not a book file");
    return;
  }

  const std::string path = std::string(TARGET_DIR) + "/" + filename;
  if (Storage.exists(path.c_str())) {
    clearBookCache(path.c_str());
    if (!Storage.remove(path.c_str())) {
      ReceiverLock lock(mutex_);
      notifyStatusLocked("ERROR:Delete failed");
      return;
    }
  }
  // Idempotent: succeed even if file was already absent

  ReceiverLock lock(mutex_);
  char msg[MAX_FILE_NAME_LEN + 10];
  snprintf(msg, sizeof(msg), "DELETED:%s", filename.c_str());
  notifyStatusLocked(msg);
}

void BluetoothFileReceiver::notifyStatusLocked(const char* status) {
  if (!statusCharacteristic_) return;
  statusCharacteristic_->setValue(status);
  statusCharacteristic_->notify();
}

// MARK: - V2 sync protocol

void BluetoothFileReceiver::ensureUuidMapLoaded() {
  if (uuidMapLoaded_) return;
  loadUuidMap();
  uuidMapLoaded_ = true;
}

void BluetoothFileReceiver::loadUuidMap() {
  uuidToFile_.clear();
  fileToUuid_.clear();
  if (!Storage.exists(UUID_MAP_PATH)) return;

  HalFile f;
  if (!Storage.openFileForRead("BLE", UUID_MAP_PATH, f) || !f) {
    LOG_ERR("BLE", "loadUuidMap: open failed");
    return;
  }

  // Plain text, one "<uuid>=<filename>\n" per entry. Stream a fixed-size
  // line buffer so a corrupted file can't blow the heap.
  constexpr size_t kBufSize = 192;
  char line[kBufSize];
  size_t pos = 0;
  while (f.available()) {
    const int b = f.read();
    if (b < 0) break;
    const char c = static_cast<char>(b);
    if (c == '\n' || pos >= kBufSize - 1) {
      line[pos] = '\0';
      const std::string entry(line);
      const auto eq = entry.find('=');
      if (eq != std::string::npos && eq > 0 && eq + 1 < entry.size()) {
        const std::string uuid = entry.substr(0, eq);
        const std::string filename = entry.substr(eq + 1);
        if (isValidUuid(uuid) && !filename.empty()) {
          // Don't trust the on-disk map blindly — if the file went missing
          // (user pulled the SD, deleted manually) we drop the entry on
          // next save. For load, we still keep it: a later LIST_V2 will
          // skip missing files.
          uuidToFile_[uuid] = filename;
          fileToUuid_[filename] = uuid;
        }
      }
      pos = 0;
    } else if (c != '\r') {
      line[pos++] = c;
    }
  }
  f.close();
  LOG_DBG("BLE", "loaded %u UUID mappings", static_cast<unsigned>(uuidToFile_.size()));
}

void BluetoothFileReceiver::saveUuidMap() const {
  // Make sure /AirBook exists — saveUuidMap may run before any upload.
  if (!Storage.exists(TARGET_DIR) && !Storage.mkdir(TARGET_DIR)) {
    LOG_ERR("BLE", "saveUuidMap: mkdir failed");
    return;
  }
  // Write to a tmp path then rename so a power loss can't truncate the
  // existing map.
  const std::string tmpPath = std::string(UUID_MAP_PATH) + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite("BLE", tmpPath, f) || !f) {
    LOG_ERR("BLE", "saveUuidMap: open failed");
    return;
  }
  for (const auto& kv : uuidToFile_) {
    f.write(reinterpret_cast<const uint8_t*>(kv.first.data()), kv.first.size());
    f.write(reinterpret_cast<const uint8_t*>("="), 1);
    f.write(reinterpret_cast<const uint8_t*>(kv.second.data()), kv.second.size());
    f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  }
  f.flush();
  f.close();
  Storage.remove(UUID_MAP_PATH);
  Storage.rename(tmpPath.c_str(), UUID_MAP_PATH);
}

void BluetoothFileReceiver::mapPut(const std::string& uuid, const std::string& filename) {
  uuidToFile_[uuid] = filename;
  fileToUuid_[filename] = uuid;
}

void BluetoothFileReceiver::mapRemoveByUuid(const std::string& uuid) {
  const auto it = uuidToFile_.find(uuid);
  if (it == uuidToFile_.end()) return;
  fileToUuid_.erase(it->second);
  uuidToFile_.erase(it);
}

void BluetoothFileReceiver::mapRemoveByFilename(const std::string& filename) {
  const auto it = fileToUuid_.find(filename);
  if (it == fileToUuid_.end()) return;
  uuidToFile_.erase(it->second);
  fileToUuid_.erase(it);
}

std::string BluetoothFileReceiver::mapFilenameForUuid(const std::string& uuid) const {
  const auto it = uuidToFile_.find(uuid);
  return it == uuidToFile_.end() ? std::string() : it->second;
}

std::string BluetoothFileReceiver::mapUuidForFilename(const std::string& filename) {
  const auto it = fileToUuid_.find(filename);
  if (it != fileToUuid_.end()) return it->second;
  // Foreign book — assign a fresh UUID so iOS can refer to it.
  const std::string uuid = generateUuidV4();
  mapPut(uuid, filename);
  return uuid;
}

void BluetoothFileReceiver::handleListV2Command() {
  // Enumerate /AirBook outside the mutex so the BLE callback thread isn't
  // blocked during SD reads.
  std::vector<std::pair<std::string, size_t>> entries;
  if (Storage.exists(TARGET_DIR)) {
    auto root = Storage.open(TARGET_DIR);
    if (root && root.isDirectory()) {
      root.rewindDirectory();
      char nameBuf[MAX_FILE_NAME_LEN + 1];
      for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
        if (file.isDirectory()) continue;
        file.getName(nameBuf, sizeof(nameBuf));
        const std::string name(nameBuf);
        if (!name.empty() && name[0] == '.') continue;  // skip .firmware-incoming.bin, .book-uuids
        if (isSupportedBookFile(name)) {
          entries.push_back({name, static_cast<size_t>(file.fileSize())});
        }
      }
    }
  }

  bool mapDirty = false;
  {
    ReceiverLock lock(mutex_);
    if (!active_) return;
    ensureUuidMapLoaded();

    // Drop map entries whose files vanished from the SD card. This is the
    // path that converges the on-disk map after a manual delete.
    std::vector<std::string> uuidsToDrop;
    for (const auto& kv : uuidToFile_) {
      bool found = false;
      for (const auto& e : entries) {
        if (e.first == kv.second) { found = true; break; }
      }
      if (!found) uuidsToDrop.push_back(kv.first);
    }
    if (!uuidsToDrop.empty()) {
      for (const auto& u : uuidsToDrop) mapRemoveByUuid(u);
      mapDirty = true;
    }

    char buf[64 + MAX_FILE_NAME_LEN];
    for (const auto& entry : entries) {
      // Look up / mint a UUID for the file. mapUuidForFilename returns the
      // existing UUID if iOS sent the file via START_V2 earlier; otherwise
      // it generates a v4 UUID so foreign files still show up in iOS's
      // device LIST as "foreign" entries.
      const bool hadUuid = fileToUuid_.count(entry.first) != 0;
      const std::string uuid = mapUuidForFilename(entry.first);
      if (!hadUuid) mapDirty = true;

      // FILE_V2:<uuid>:<has_file>:<size>:<filename>
      // has_file is always 1 — we don't track entry-only state on the
      // device; if the file is enumerated, it's present.
      snprintf(buf, sizeof(buf), "FILE_V2:%s:1:%u:%s", uuid.c_str(),
               static_cast<unsigned>(entry.second), entry.first.c_str());
      notifyStatusLocked(buf);
    }

    if (mapDirty) saveUuidMap();
    notifyStatusLocked("FILES_END");
  }
}

void BluetoothFileReceiver::handleStartV2(const std::string& uuid, const size_t size,
                                          const std::string& filename) {
  // Dedup before the upload opens. Without this, iOS's UUID-only planner
  // happily re-uploads a book the device already has — uniquePathFor would
  // assign `book (1).epub` and we'd ship a duplicate on disk.
  //   (a) UUID already mapped → erase the old file so the same path is
  //       reused on overwrite.
  //   (b) UUID is new but the sanitized filename matches a file already on
  //       the SD (foreign import indexed under a different UUID) → re-tag
  //       and reply DONE_V2 immediately, no bytes transferred.
  std::string adoptDoneMsg;
  {
    ReceiverLock lock(mutex_);
    if (!active_) return;
    ensureUuidMapLoaded();

    const std::string existingForUuid = mapFilenameForUuid(uuid);
    if (!existingForUuid.empty()) {
      const std::string oldPath = std::string(TARGET_DIR) + "/" + existingForUuid;
      Storage.remove(oldPath.c_str());
    } else {
      char sanitized[MAX_FILE_NAME_LEN];
      FsHelpers::sanitizePathComponentForFat32(
          trimFilename(filename).c_str(), sanitized, sizeof(sanitized));
      const std::string sanitizedStr(sanitized);
      const auto it = fileToUuid_.find(sanitizedStr);
      if (it != fileToUuid_.end()) {
        mapRemoveByFilename(sanitizedStr);
        mapPut(uuid, sanitizedStr);
        saveUuidMap();
        adoptDoneMsg = std::string("DONE_V2:") + uuid;
      }
    }

    if (!adoptDoneMsg.empty()) {
      syncMode_ = true;
      notifyStatusLocked(adoptDoneMsg.c_str());
      return;
    }

    pendingUuid_ = uuid;
  }
  // Reuse the V1 startUpload pipeline — it does the path-safety and
  // file-open work. The pending UUID picked up above flips completeUpload
  // to send DONE_V2:<uuid>.
  startUpload(filename, size);
}

void BluetoothFileReceiver::handleDeleteByUuid(const std::string& uuid,
                                               const char* successPrefix) {
  ReceiverLock lock(mutex_);
  if (!active_) return;
  ensureUuidMapLoaded();

  if (!isValidUuid(uuid)) {
    const std::string err = "ERROR_V2::Invalid UUID";
    notifyStatusLocked(err.c_str());
    return;
  }

  const std::string filename = mapFilenameForUuid(uuid);
  if (!filename.empty()) {
    const std::string path = std::string(TARGET_DIR) + "/" + filename;
    if (Storage.exists(path.c_str())) {
      clearBookCache(path.c_str());
      Storage.remove(path.c_str());
    }
    mapRemoveByUuid(uuid);
    saveUuidMap();
  }
  // Idempotent: succeed even if the book wasn't on the device — iOS expects
  // the ack so the sync can advance to the next op.
  const std::string msg = std::string(successPrefix) + uuid;
  notifyStatusLocked(msg.c_str());
}

// MARK: - Browse-read (device → iOS file extraction)

std::string BluetoothFileReceiver::resolveBrowsePath(const std::string& rawPath) const {
  // Empty path = TARGET_DIR root, valid for BROWSE_LS only. Caller
  // decides whether empty is acceptable.
  if (rawPath.empty()) return std::string(TARGET_DIR);

  // Reject absolute paths, Windows separators, and lone-dot-segments.
  // Path components are split on '/' and individually validated.
  if (rawPath[0] == '/' ||
      rawPath.find('\\') != std::string::npos) {
    return std::string();
  }

  size_t cursor = 0;
  while (cursor < rawPath.size()) {
    const auto slash = rawPath.find('/', cursor);
    const std::string segment = rawPath.substr(
        cursor, slash == std::string::npos ? std::string::npos : slash - cursor);
    if (segment.empty() || segment == "." || segment == ".." ||
        (segment[0] == '.' && segment.size() > 1)) {
      // No empty segments (// or trailing /), no traversal, no hidden
      // dotfiles. The leading-dot guard protects /AirBook/.book-uuids
      // and the OTA staging file from showing up over BLE.
      return std::string();
    }
    if (slash == std::string::npos) break;
    cursor = slash + 1;
  }
  return std::string(TARGET_DIR) + "/" + rawPath;
}

void BluetoothFileReceiver::handleBrowseLs(const std::string& rawPath) {
  std::string full;
  {
    ReceiverLock lock(mutex_);
    if (!active_) return;
    full = resolveBrowsePath(rawPath);
    if (full.empty()) {
      notifyStatusLocked("BROWSE_ERROR:Invalid path");
      return;
    }
  }

  // Enumerate outside the mutex so SD I/O doesn't stall BLE callbacks.
  struct Entry { std::string name; size_t size; bool isDir; };
  std::vector<Entry> entries;
  if (Storage.exists(full.c_str())) {
    auto dir = Storage.open(full.c_str());
    if (dir && dir.isDirectory()) {
      dir.rewindDirectory();
      char nameBuf[MAX_FILE_NAME_LEN + 1];
      for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
        f.getName(nameBuf, sizeof(nameBuf));
        const std::string name(nameBuf);
        if (name.empty() || name[0] == '.') continue;  // hidden / config
        entries.push_back({name, static_cast<size_t>(f.fileSize()), f.isDirectory()});
      }
    } else {
      ReceiverLock lock(mutex_);
      notifyStatusLocked("BROWSE_ERROR:Not a directory");
      return;
    }
  } else {
    ReceiverLock lock(mutex_);
    notifyStatusLocked("BROWSE_ERROR:Path not found");
    return;
  }

  ReceiverLock lock(mutex_);
  if (!active_) return;
  char buf[64 + MAX_FILE_NAME_LEN];
  for (const auto& e : entries) {
    snprintf(buf, sizeof(buf), "BROWSE_ENTRY:%d:%u:%s",
             e.isDir ? 1 : 0, static_cast<unsigned>(e.size), e.name.c_str());
    notifyStatusLocked(buf);
  }
  notifyStatusLocked("BROWSE_LS_END");
}

void BluetoothFileReceiver::handleBrowseRead(const std::string& rawPath) {
  ReceiverLock lock(mutex_);
  if (!active_) return;

  if (browseReading_) {
    // Don't queue — fail the new request and keep the active one
    // running. iOS can cancel first if it wants to switch files.
    notifyStatusLocked("BROWSE_ERROR:Read already in progress");
    return;
  }

  const std::string full = resolveBrowsePath(rawPath);
  if (full.empty() || rawPath.empty()) {
    notifyStatusLocked("BROWSE_ERROR:Invalid path");
    return;
  }

  if (!Storage.exists(full.c_str())) {
    notifyStatusLocked("BROWSE_ERROR:File not found");
    return;
  }

  if (!Storage.openFileForRead("BROWSE", full, browseFile_) || !browseFile_) {
    notifyStatusLocked("BROWSE_ERROR:Could not open file");
    return;
  }
  // Reject directories: BROWSE_READ is files-only. BROWSE_LS for dirs.
  // SdFat doesn't separate open(file vs dir); detect via the HalFile
  // wrapper if it exposes isDirectory — for safety we check fileSize
  // staying >0 isn't enough, but Storage.open in dir mode would
  // succeed differently. Pragmatic: rely on the iOS UI to only send
  // BROWSE_READ for file entries it saw via BROWSE_LS.

  browsePath_ = full;
  browseBytesSent_ = 0;
  browseBytesTotal_ = static_cast<size_t>(browseFile_.fileSize());
  browseNextProgressMark_ = 0;
  browseReading_ = true;

  char buf[64];
  snprintf(buf, sizeof(buf), "BROWSE_READ_READY:%u",
           static_cast<unsigned>(browseBytesTotal_));
  notifyStatusLocked(buf);
  LOG_DBG("BLE", "Browse read start: %s (%u bytes)", full.c_str(),
          static_cast<unsigned>(browseBytesTotal_));
}

void BluetoothFileReceiver::refreshInfoPayload() {
  // Enumerate /AirBook to compute used_kb + books. Skips hidden dotfiles
  // (uuid map, OTA staging) so the count reflects user-visible books.
  size_t usedBytes = 0;
  int bookCount = 0;
  if (Storage.exists(TARGET_DIR)) {
    auto dir = Storage.open(TARGET_DIR);
    if (dir && dir.isDirectory()) {
      dir.rewindDirectory();
      char nameBuf[MAX_FILE_NAME_LEN + 1];
      for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;
        f.getName(nameBuf, sizeof(nameBuf));
        const std::string name(nameBuf);
        if (name.empty() || name[0] == '.') continue;
        if (!isSupportedBookFile(name)) continue;
        usedBytes += static_cast<size_t>(f.fileSize());
        bookCount++;
      }
    }
  }

  // SD free space for the iOS upload-plan pre-check. See SdFreeSpace.cpp
  // for why this lives in its own translation unit.
  const uint64_t freeKB = crosspointSdFreeKB();

  char payload[256];
  snprintf(payload, sizeof(payload),
           "fw=%s\nproto=2\ncaps=book,sync,ota,browse\nused_kb=%u\nbooks=%d\nfree_kb=%llu\n",
           CROSSPOINT_VERSION,
           static_cast<unsigned>(usedBytes / 1024),
           bookCount,
           static_cast<unsigned long long>(freeKB));
  if (infoCharacteristic_) {
    infoCharacteristic_->setValue(payload);
  }
}

void BluetoothFileReceiver::cancelBrowseRead(const char* reason) {
  ReceiverLock lock(mutex_);
  if (!browseReading_) return;
  browseFile_.close();
  browseReading_ = false;
  browsePath_.clear();
  if (reason && reason[0]) {
    const std::string msg = std::string("BROWSE_ERROR:") + reason;
    notifyStatusLocked(msg.c_str());
  }
}

void BluetoothFileReceiver::finishBrowseRead() {
  // mutex already held by caller (pumpBrowseRead under lock)
  browseFile_.close();
  browseReading_ = false;
  notifyStatusLocked("BROWSE_READ_DONE");
  LOG_DBG("BLE", "Browse read done: %s (%u bytes)", browsePath_.c_str(),
          static_cast<unsigned>(browseBytesSent_));
  browsePath_.clear();
}

bool BluetoothFileReceiver::isBrowseReadActive() const {
  ReceiverLock lock(mutex_);
  return browseReading_;
}

void BluetoothFileReceiver::pumpBrowseRead() {
  ReceiverLock lock(mutex_);
  if (!browseReading_ || !fileOutCharacteristic_) return;

  // 224 bytes per chunk: comfortably under the negotiated 517 MTU,
  // matches upstream tooling's known-good buffer size, and stays small
  // enough that an unrelated notify on the STATUS characteristic
  // (BROWSE_READ_PROGRESS) can be slotted in without backpressure.
  constexpr size_t CHUNK = 224;
  uint8_t buf[CHUNK];
  const size_t remaining = browseBytesTotal_ - browseBytesSent_;
  const size_t want = remaining < CHUNK ? remaining : CHUNK;
  if (want == 0) {
    finishBrowseRead();
    return;
  }

  const int read = browseFile_.read(buf, want);
  if (read <= 0) {
    // Mid-file read failure — abort and let iOS retry.
    browseFile_.close();
    browseReading_ = false;
    browsePath_.clear();
    notifyStatusLocked("BROWSE_ERROR:Read error");
    return;
  }

  // Match the rest of the file's notify pattern (setValue + notify()).
  // NimBLE queues internally; sustained throughput on BLE 5 1M PHY is
  // ~30 KB/s, which is below the rate the activity-loop pump can call
  // us at (≤50 Hz × 224 B = 11 KB/s), so we don't outrun the radio.
  fileOutCharacteristic_->setValue(buf, static_cast<size_t>(read));
  fileOutCharacteristic_->notify();
  browseBytesSent_ += static_cast<size_t>(read);

  // Throttled status progress (one notify per 64 KB or completion). The
  // FILE_OUT data path drives the actual transfer; this just keeps the
  // iOS progress bar moving.
  constexpr size_t PROGRESS_STEP = 64 * 1024;
  if (browseBytesSent_ >= browseNextProgressMark_ || browseBytesSent_ == browseBytesTotal_) {
    char pbuf[64];
    snprintf(pbuf, sizeof(pbuf), "BROWSE_READ_PROGRESS:%u:%u",
             static_cast<unsigned>(browseBytesSent_),
             static_cast<unsigned>(browseBytesTotal_));
    notifyStatusLocked(pbuf);
    browseNextProgressMark_ = browseBytesSent_ + PROGRESS_STEP;
  }

  if (browseBytesSent_ >= browseBytesTotal_) {
    finishBrowseRead();
  }
}
