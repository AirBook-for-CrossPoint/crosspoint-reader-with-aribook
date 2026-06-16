# CrossPoint Reader + AirBook

Fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) that adds BLE support for the [AirBook for CrossPoint](https://github.com/Yoddikko/Airbook-for-CrossPoint) iOS companion app.

[![Upstream sync](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fapi.github.com%2Frepos%2FYoddikko%2Fcrosspoint-reader%2Fcompare%2Fcrosspoint-reader%3Amaster...master&label=commits%20behind%20upstream&query=%24.behind_by&color=orange)](https://github.com/crosspoint-reader/crosspoint-reader/compare/master...Yoddikko:crosspoint-reader:master)

## What this fork adds

Two new entries in the Wi‑Fi / Transfer menu:

- **Bluetooth Receive** — receive a book from AirBook via BLE
- **Sync with AirBook** — bidirectional library sync

Received books land in `/AirBook` on the SD card. The device advertises as `CrossPoint AirBook`.

## Install

**[crosspoint-airbook-tools](https://Yoddikko.github.io/crosspoint-airbook-tools/)** — web flasher.

## Documentation

Everything else (reader engine, formats, customization, contributing) lives in the [upstream repository](https://github.com/crosspoint-reader/crosspoint-reader). This README only covers what the fork adds.

---

Not affiliated with Xteink or any device manufacturer.
