#include "BluetoothFileReceiver.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "network/FirmwareFlasher.h"
#include "util/BookCacheUtils.h"

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
    otaMode_ = false;
    otaOpen_ = false;
    otaSha256Hex_.clear();
    otaFlashRequested_ = false;
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
  if (!control || !data || !statusCharacteristic_ || !info) {
    failUpload("Failed to create Bluetooth characteristics");
    return false;
  }

  control->setCallbacks(controlCallbacks_.get());
  data->setCallbacks(dataCallbacks_.get());
  statusCharacteristic_->setValue("WAITING");

  // Static identity payload exposed on the Info characteristic. Newline-
  // separated key=value lines so the iOS app can parse it with a one-pass
  // split; unknown keys are ignored for forward compatibility.
  {
    std::string payload;
    payload.reserve(96);
    payload += "fw=";
    payload += CROSSPOINT_VERSION;
    payload += "\nproto=1\ncaps=book,sync,ota\n";
    info->setValue(payload);
  }

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
  }
  dataCallbacks_.reset();
  controlCallbacks_.reset();
  serverCallbacks_.reset();
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
    notifyStatusLocked("SYNC_READY");
    return;
  }

  if (message == "LIST") {
    handleListCommand();
    return;
  }

  if (message.rfind("DELETE:", 0) == 0) {
    handleDeleteCommand(message.substr(7));
    return;
  }

  if (message == "SYNC_END") {
    ReceiverLock lock(mutex_);
    if (!active_) return;
    syncMode_ = false;
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

    startUpload(filename, strtoul(sizeToken.c_str(), nullptr, 10));
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
  notifyStatusLocked("DONE");
}

void BluetoothFileReceiver::failUpload(const char* error) {
  ReceiverLock lock(mutex_);
  failUploadLocked(error);
}

void BluetoothFileReceiver::failUploadLocked(const char* error) {
  if (uploadOpen_) closeUpload(true);
  state_ = State::Error;
  error_ = error;
  const std::string status = std::string("ERROR:") + error;
  notifyStatusLocked(status.c_str());
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
