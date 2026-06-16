# CrossPoint Reader + AirBook

Fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) that adds BLE support for the [AirBook for CrossPoint](https://github.com/AirBook-for-CrossPoint/Airbook-iOS-for-CrossPoint) iOS companion app.

[![Upstream sync](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fapi.github.com%2Frepos%2FAirBook-for-CrossPoint%2Fcrosspoint-reader-with-aribook%2Fcompare%2Fcrosspoint-reader%3Amaster...master&label=commits%20behind%20upstream&query=%24.behind_by&color=orange)](https://github.com/crosspoint-reader/crosspoint-reader/compare/master...AirBook-for-CrossPoint:crosspoint-reader-with-aribook:master)

## What this fork adds

One unified entry in the Wi‑Fi / Transfer menu:

- **AirBook sync** — receive books and firmware updates from the AirBook iOS app over BLE

Received books land in `/AirBook` on the SD card. The device advertises as `CrossPoint AirBook`. Firmware updates run on the same BLE session — see release notes from `v1.3.0-airbook.1` onward.

## Install

**[crosspoint-airbook-tools](https://airbook-for-crosspoint.github.io/crosspoint-airbook-tools/)** — web flasher.

After the first install, future updates can also be applied wirelessly from the AirBook iOS app — no SD card swap needed.

## Documentation

Everything else (reader engine, formats, customization, contributing) lives in the [upstream repository](https://github.com/crosspoint-reader/crosspoint-reader). This README only covers what the fork adds.

---

Not affiliated with Xteink or any device manufacturer.
