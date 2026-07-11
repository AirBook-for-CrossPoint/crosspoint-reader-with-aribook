// SD free-space probe for the AirBook Info characteristic.
//
// SdFat deliberately keeps FsVolume::cwv() (the current-working-volume
// static) private, and our SDCardManager (community-sdk submodule) doesn't
// expose its SdFat instance. Free space, however, can only be asked of the
// volume. Rather than patching the third-party submodule or the SdFat
// libdep, this one translation unit lifts the access specifiers before
// including SdFat.h.
//
// Why this is safe here: the only private thing we touch is the static
// accessor cwv() — a static has no object-layout implications, and the two
// methods we call on the returned volume (freeClusterCount, bytesPerCluster)
// are public API. Nothing from SdFat leaks out of this file.
//
// ponytail: access-specifier override on one static; the clean fix is a
// one-line `SdFat& fs()` accessor upstreamed to community-sdk's
// SDCardManager — switch to it if/when that lands.
#define private public
#define protected public
#include <SdFat.h>
#undef private
#undef protected

// Free kilobytes on the mounted SD volume, 0 if unknown. freeClusterCount()
// scans the FAT and can take a few hundred ms on large FAT32 cards — call
// this off the hot path (we do: BLE service start + Info reads only).
uint64_t crosspointSdFreeKB() {
  FsVolume* vol = FsVolume::cwv();
  if (!vol) return 0;
  const int32_t freeClusters = vol->freeClusterCount();
  if (freeClusters <= 0) return 0;
  return static_cast<uint64_t>(freeClusters) * vol->bytesPerCluster() / 1024;
}
