# Versioning

This fork ships its own releases on top of upstream [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader). The tag scheme keeps both lineages legible at a glance.

## Format

```
v<upstream-version>-airbook.<N>
```

- **`<upstream-version>`** — the upstream tag this build is merged on top of, with no leading `v` (so the `.` separators stay obvious). Always matches one of the upstream's released tags.
- **`<N>`** — our fork-only patch counter. Starts at **1** for the first AirBook release on a given upstream version. Increments per release within the same upstream cycle. **Resets to 1** when we merge a new upstream version.

The `[crosspoint] version` field in `platformio.ini` holds the same string. It propagates through the build into the `CROSSPOINT_VERSION` macro, and from there into the BLE Info characteristic that the iOS app reads to decide whether an OTA update is available.

## Examples

| Tag                     | Means                                                  |
|-------------------------|--------------------------------------------------------|
| `v1.3.0-airbook.1`      | First AirBook release on top of upstream `1.3.0`       |
| `v1.3.0-airbook.2`      | Second AirBook-only patch (e.g. UI fix), still on 1.3.0|
| `v1.4.0-airbook.1`      | First AirBook release after we sync to upstream `1.4.0`|

## When to bump what

- **Fixed a bug in AirBook code only / changed iOS-facing protocol / UI rearrangement** → bump `<N>` by one. Same `<upstream-version>`.
- **Upstream just released a new tag and we merged it** → set `<upstream-version>` to the new upstream tag, reset `<N>` to `1`. The release watcher workflow drafts this for you — fill in the AirBook section, publish.
- **Hotfix on an older upstream** → continue the `<N>` series for that upstream version (`v1.3.0-airbook.4` even if `v1.4.0-airbook.2` already exists). Don't be afraid of out-of-order publishing; releases are sorted by date on the GitHub Releases page.

## Why this scheme

It puts the upstream lineage front and centre — anyone reading `v1.3.0-airbook.2` immediately knows the build includes everything in upstream `1.3.0` plus our extras, with no semver acrobatics to figure out. The `-airbook.N` suffix is semver pre-release syntax, but the iOS app only checks string equality (any difference = "update available"), so the strict pre-release ordering rule that says `1.3.0-airbook.1 < 1.3.0` doesn't bite us — we never compare across the fork boundary.

## What not to do

- **Don't reuse a tag.** Once published, treat it as immutable. If you need to repair a release, increment `<N>` and republish.
- **Don't skip `<N>`.** A jump from `airbook.1` to `airbook.5` reads as four missing patches and makes git history hunting harder. Increment one at a time.
- **Don't add suffixes after `<N>`** (e.g. `-airbook.2-fix`). If a build is broken, pull it and ship `airbook.3` instead. Keep the scheme flat.
- **Don't change `<upstream-version>` without actually merging that upstream tag.** The base version on the device must be a real merge marker, not a label.
