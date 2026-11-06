#ifndef NVM3_DEFAULT_CONFIG_H
#define NVM3_DEFAULT_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>

// <h>NVM3 Default Instance Configuration

#ifndef NVM3_DEFAULT_CACHE_SIZE
// <o NVM3_DEFAULT_CACHE_SIZE> NVM3 Default Instance Cache Size
// <i> Number of NVM3 objects to cache. To reduce access times this number
// <i> should be equal to or higher than the number of NVM3 objects in the
// <i> default NVM3 instance.
// <i> Default: 200
#define NVM3_DEFAULT_CACHE_SIZE  200
#endif

#ifndef NVM3_DEFAULT_MAX_OBJECT_SIZE
// <o NVM3_DEFAULT_MAX_OBJECT_SIZE> NVM3 Default Instance Max Object Size
// <i> Max NVM3 object size that can be stored.
// <i> Must cover opendisplay config record: 16-byte header + OD_CONFIG_MAX_SIZE (2048).
// <i> Keep in sync with NVM3_MAX_OBJECT_SIZE in cmake_gcc/opendisplay-bg22.cmake.
/* 3136 = OD_CONFIG_MAX_SIZE 3120 + the 16-byte storage record header.
 *
 * SIZED AGAINST PAGE CAPACITY, NOT REGION SIZE. nvm3_open has two gates and the binding one
 * is `validNvmPageCnt * (pageSize - NVM3_PAGE_HEADER_SIZE) >= thrSize`, so the usable figure
 * for this 3-page/24576 B region is 24516 B, not 24576. thrSize(3136) = 22636, leaving 1880 B.
 * thrSize(4096) = 24556 clears the region check by 20 B and FAILS capacity by 40 -- that is
 * the configuration that stopped NVM3 opening on Firmware_Silabs, with the config reading back
 * as 0 bytes and every write NACKed. Raising this past 4064 additionally needs a 4th page.
 *
 * NVM3_SECURITY off and REPACK_HEADROOM 0 are part of the arithmetic: either one grows
 * thrSize, so re-derive the margin before enabling them. */
#define NVM3_DEFAULT_MAX_OBJECT_SIZE  3136
#endif

#ifndef NVM3_DEFAULT_REPACK_HEADROOM
// <o NVM3_DEFAULT_REPACK_HEADROOM> NVM3 Default Instance User Repack Headroom
// <i> Headroom determining how many bytes below the forced repack limit the user
// <i> repack limit should be placed. The default is 0, which means the user and
// <i> forced repack limits are equal.
// <i> Default: 0
#define NVM3_DEFAULT_REPACK_HEADROOM  0
#endif

#ifndef NVM3_DEFAULT_NVM_SIZE
// <o NVM3_DEFAULT_NVM_SIZE> NVM3 Default Instance Size
// <i> Size of the NVM3 storage region in flash. This size should be aligned with
// <i> the flash page size of the device.
// <i> EFR32BG22: FLASH_PAGE_SIZE 8192; 3 pages (24576) is enough for max-object 2112.
// <i> Keep size/base unchanged across upgrades — growing it invalidates NVM3 (needs chip erase).
#define NVM3_DEFAULT_NVM_SIZE  24576
#endif

// </h>

// <<< end of configuration section >>>

#endif // NVM3_DEFAULT_CONFIG_H

