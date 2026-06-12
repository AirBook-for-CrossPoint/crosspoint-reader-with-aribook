# CrossPoint Reader + AirBook

Fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) that adds support for **[AirBook for CrossPoint](https://github.com/Yoddikko/Airbook-for-CrossPoint)**, the iOS companion app. Send ebooks wirelessly from iPhone/iPad to your e-reader over Bluetooth Low Energy.

[![Upstream sync](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fapi.github.com%2Frepos%2FYoddikko%2Fcrosspoint-reader%2Fcompare%2Fcrosspoint-reader%3Amaster...master&label=commits%20behind%20upstream&query=%24.behind_by&color=orange)](https://github.com/crosspoint-reader/crosspoint-reader/compare/master...Yoddikko:crosspoint-reader:master)

## What this fork adds

Two new entries in the Wi‑Fi / Transfer menu:

- **Bluetooth Receive** — receive a single book from the AirBook iOS app via BLE
- **Sync with AirBook** — bidirectional library sync between device and iOS app

Received books land in `/AirBook` on the SD card. The device advertises as `CrossPoint AirBook` (BLE service `8b45f100-9128-4d4f-9a4f-7a0dc1b26b01`). The iOS app discovers the device, sends files in chunks over the data characteristic, and confirms completion through status notifications. Both sides follow the same GATT protocol — the iOS app source is the reference client implementation.

## Install

Only the **main branch** is built and released. Grab the latest `firmware.bin` from [Releases](https://github.com/Yoddikko/crosspoint-reader/releases), then flash:

- **Web flasher** — [crosspoint-airbook-tools](https://github.com/Yoddikko/crosspoint-airbook-tools) provides a dedicated web installer (deploy your own or use the upstream one at [crosspointreader.com](https://crosspointreader.com/#flash-tools) with "Custom .bin")
- **esptool** — command line:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 firmware.bin
```

## Staying in sync with upstream

This fork tracks upstream. Run the check script to see how far behind (or ahead) we are:

```bash
./scripts/check-upstream.sh
```

To pull in upstream changes:

```bash
git fetch upstream
git merge upstream/master
```

## Documentation

For everything else — reader engine, supported formats, wireless workflows, customization, i18n, contributing, firmware internals, SD‑card caching — see the **[upstream CrossPoint repository](https://github.com/crosspoint-reader/crosspoint-reader)**. This README only documents what the fork adds.

---

CrossPoint Reader is open-source e-reader firmware — community-built, fully hackable, free forever. Not affiliated with Xteink or any device manufacturer.
