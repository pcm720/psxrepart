#include "init.h"
#include <debug.h>
#include <hdd-ioctl.h>
#include <kernel.h>
#include <libhdd.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <fileio.h>

#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif

// LBA sector calculation:
// 1GiB = 0x200000 sectors (1024 * 1024 * 1024 / 512 bytes per sector)
// 1GB  = 0x1dcd65 sectors (1000 * 1000 * 1000 / 512 bytes per sector)
// Sony uses GB in xosdmain code (40GB = 0x4a817c8, not 0x5000000).
#define SECTORS_PER_GB 0x1dcd65
#define MAX_LBA28 0xFFFFFFF
#define MIN_LBA28 0x400000
#define MAX_GB 137
#define MIN_GB 2

static const uint32_t defaultZoneSize = 0x2000;
static const uint32_t xcontentsZoneSize = 0x100000;
static const uint32_t xdataZoneSize = defaultZoneSize;

int targetSizeSelectLoop(uint32_t *targetLBA);
int clearHDD(char *mountpoint);
int copyBootflag();

void printHeader(uint32_t targetLBA, int showHelperText) {
  scr_clear();
  scr_printf("\n\n\tPSX repartitioning utility %s\n\tby pcm720\n\n", GIT_VERSION);

  if (targetLBA != 0) {
    scr_printf("\tWill resize the PS2 area to %ld GB\n\n", targetLBA / SECTORS_PER_GB);
    if (showHelperText)
      scr_printf("\tUp/Down/Left/Right to adjust the size\n"
                 "\tCross/Circle to start the repartitioning\n"
                 "\tSTART to exit\n");
  }
}

int main(int argc, char *argv[]) {
  init_scr();
  scr_setCursor(0);
  printHeader(0, 0);

  uint32_t targetLBA = MAX_LBA28;
  uint32_t targetGB = 137;
  if (argc > 1 && sscanf(argv[1], "-size=%lu", &targetGB))
    targetLBA = targetGB * SECTORS_PER_GB;
  else {
    if (loadPadModules())
      goto fail;
    if (targetSizeSelectLoop(&targetLBA) < 0) {
      return 0;
    }
  }

  if (targetLBA < MIN_LBA28)
    targetLBA = MIN_LBA28;
  else if (targetLBA > MAX_LBA28)
    targetLBA = MAX_LBA28;

  printHeader(targetLBA, 0);

  scr_printf("\tInitializing modules\n");
  if (initModules() != 0) {
    scr_printf("\n\tERROR: Failed to init modules\n");
    goto fail;
  }
  fileXioInit();

  scr_printf("\n\tStarting...\n\n");
  scr_printf("\t1. Setting max LBA28\n");
  int res = fileXioDevctl("dvr_hdd0:", HDIOC_PRESETMAXLBA28, NULL, 0x0, NULL, 0x0);
  if (res < 0) {
    scr_printf("\n\tERROR: HDIOC_PRESETMAXLBA28 failed: %d\n", res);
    goto fail;
  }
  res = fileXioDevctl("dvr_hdd0:", HDIOC_SETMAXLBA28, &targetLBA, 0x4, NULL, 0x0);
  if (res < 0) {
    scr_printf("\n\tERROR: HDIOC_SETMAXLBA28 failed: %d\n", res);
    goto fail;
  }
  if (initModules() != 0) {
    scr_printf("\n\tERROR: Failed to init modules\n");
    goto fail;
  }
  fileXioInit();

  scr_printf("\t2. Formatting hdd0:\n");
  res = clearHDD("hdd0:");
  if (res < 0) {
    goto fail;
  }
  scr_printf("\t3. Formatting dvr_hdd0:\n");
  res = fileXioFormat("dvr_hdd0:", NULL, NULL, 0);
  if (res < 0) {
    scr_printf("\n\tERROR: failed to format dvr_hdd0: %d\n", res);
    goto fail;
  }
  res = fileXioFormat("dvr_pfs:", "dvr_hdd0:__xcontents", &xcontentsZoneSize, sizeof(xcontentsZoneSize));
  if (res < 0) {
    scr_printf("\n\tERROR: failed to format dvr_hdd0:__xcontents %d\n", res);
    goto fail;
  }
  res = fileXioFormat("dvr_pfs:", "dvr_hdd0:__xdata", &xdataZoneSize, sizeof(xdataZoneSize));
  if (res < 0) {
    scr_printf("\n\tERROR: failed to format dvr_hdd0:__xdata %d\n", res);
    goto fail;
  }

  scr_printf("\t4. Finalizing\n");
  res = fileXioDevctl("dvr_hdd0:", HDIOC_POSTSETMAXLBA28, NULL, 0x0, NULL, 0x0);
  if (res < 0) {
    scr_printf("\n\tERROR: HDIOC_POSTSETMAXLBA28 failed: %d\n", res);
    goto fail;
  }

  if (copyBootflag())
    goto fail;

  scr_printf("\n\tSuccess. The system will reboot in 5 seconds.\n");
fail:
  sleep(5);
  return 0;
}

// Main target GB selector loop
int targetSizeSelectLoop(uint32_t *targetLBA) {
  static char pad1Buf[256] __attribute__((aligned(64)));
  static char pad2Buf[256] __attribute__((aligned(64)));
  uint32_t paddata = 0;
  struct padButtonStatus buttons;
  int t = 0;

  int res = padInit(0);
  if (res != 1) {
    scr_printf("\n\tERROR: failed to init libpad: %d\n", res);
    sleep(5);
    return t;
  }

  int retries = 10;
  while (retries--) {
    res = padPortOpen(0, 0, pad1Buf) | padPortOpen(1, 0, pad2Buf);
    if (res != 0)
      goto ready;
  }
  padEnd();
  return t;

ready:
  for (int i = 0; i < 2; i++) {
    retries = 5;
    while ((res = padGetState(i, 0))) {
      switch (res) {
      case PAD_STATE_DISCONN:
      case PAD_STATE_STABLE:
      case PAD_STATE_FINDCTP1:
        goto next;
      case PAD_STATE_ERROR:
        // Some PSX consoles have issues initializing the pad instantly
        // Spamming padGetState without any delay results in padRead always returning 0
        if (retries == 0)
          goto next;
        retries--;
        sleep(1);
      default:
        continue;
      }
    }
  next:
  }

  uint32_t targetGB = *targetLBA / SECTORS_PER_GB;
  printHeader(targetGB * SECTORS_PER_GB, 1);
  struct timespec tv = {0};
  tv.tv_nsec = 10 * 16 * 1000000;
  while (1) {
    for (int i = 0; i < 2; i++) {
      if ((res = padRead(i, 0, &buttons)) != 0) {
        paddata = (0xffff ^ buttons.btns) & ~paddata;
        if (paddata & PAD_START) {
          t = -1;
          goto exit;
        }
        if (paddata & (PAD_CIRCLE | PAD_CROSS)) {
          t = 0;
          *targetLBA = (targetGB >= 137) ? MAX_LBA28 : (targetGB * SECTORS_PER_GB) & MAX_LBA28;
          goto exit;
        }
        if (paddata & PAD_LEFT) {
          targetGB = (--targetGB < MIN_GB) ? MIN_GB : targetGB;
          printHeader(targetGB * SECTORS_PER_GB, 1);
          nanosleep(&tv, NULL);
        }
        if (paddata & PAD_RIGHT) {
          targetGB = (++targetGB > MAX_GB) ? MAX_GB : targetGB;
          printHeader(targetGB * SECTORS_PER_GB, 1);
          nanosleep(&tv, NULL);
        }
        if (paddata & PAD_UP) {
          targetGB = ((targetGB - 10) < MIN_GB || (targetGB - 10) > MAX_GB) ? MIN_GB : targetGB - 10;
          printHeader(targetGB * SECTORS_PER_GB, 1);
          nanosleep(&tv, NULL);
        }
        if (paddata & PAD_DOWN) {
          targetGB = ((targetGB + 10) > MAX_GB) ? MAX_GB : (targetGB + 10);
          printHeader(targetGB * SECTORS_PER_GB, 1);
          nanosleep(&tv, NULL);
        }
      }
    }
  }

exit:
  padPortClose(0, 0);
  padPortClose(1, 0);
  padEnd();
  return t;
}

// Removes all non-system partitions from the drive and recreates __common
int clearHDD(char *mountpoint) {
  char nameBuf[40] = {0};
  strcat(nameBuf, mountpoint);

  int fd = fileXioDopen(nameBuf);
  if (fd < 0) {
    scr_printf("\n\tERROR: failed to open %s: %d\n", nameBuf, fd);
    return fd;
  }

  iox_dirent_t dirent;
  int commonFound = 0;
  int res = 0;
  int dir = 0;
  // Remove all partitions
  while ((dir = fileXioDread(fd, &dirent)) > 0) {
    if ((dirent.stat.attr & ATTR_SUB_PARTITION) || (dirent.stat.mode == FS_TYPE_EMPTY))
      continue;

    if (!strncmp(dirent.name, "__common", 8)) {
      commonFound = 1;
      continue;
    }

    if (commonFound) {
      strcat(nameBuf, dirent.name);
      res = fileXioRemove(nameBuf);
      if (res < 0) {
        scr_printf("\n\tERROR: failed to remove %s: %d\n", nameBuf, res);
        return res;
      }
      nameBuf[5] = '\0';
    }
  }
  fileXioClose(fd);

  nameBuf[5] = '\0';
  strcat(nameBuf, "__common");
  res = fileXioFormat("pfs:", nameBuf, &defaultZoneSize, sizeof(defaultZoneSize));
  if (res < 0) {
    scr_printf("\n\tERROR: failed to format %s: %d\n", nameBuf, res);
    return res;
  }
  return 0;
}

// Contains 'contents = 1' flag to force XMB into copying system files to the DVR partition
const uint8_t psx2Bootflag[512] = {
    0x4F, 0xDB, 0x6B, 0xBB, 0xE7, 0x59, 0x78, 0x09, 0x2E, 0xF3, 0xDF, 0xEC, 0xE5, 0xE7, 0xFD, 0xF5, 0xFA, 0xC0, 0x67, 0x24, 0x42, 0x4F, 0x4F, 0x54,
    0x4D, 0x4F, 0x44, 0x45, 0x3D, 0x4E, 0x6F, 0x72, 0x6D, 0x61, 0x6C, 0x00, 0x63, 0x6F, 0x6E, 0x74, 0x65, 0x6E, 0x74, 0x73, 0x3D, 0x31, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Copies PSX2 bootflag to XFROM to restore the contents
int copyBootflag() {
  char romver[15];
  int fd = fileXioOpen("rom0:ROMVER", FIO_O_RDONLY);
  if (fd < 0) {
    scr_printf("\n\tERROR: failed to get ROMVER: %d\n", fd);
    return -1;
  }
  fileXioRead(fd, romver, 14);
  romver[14] = '\0';
  fileXioClose(fd);

  if (strncmp(romver, "0210J", 5))
    return 0;

  fileXioMkdir("xfrom0:/BIEXEC-SYSTEM", 0777);
  fd = fileXioOpen("xfrom0:/BIEXEC-SYSTEM/bootflag.txt", FIO_O_RDWR | FIO_O_CREAT | FIO_O_TRUNC);
  if (fd < 0) {
    scr_printf("\n\tERROR: failed to create xfrom:/BIEXEC-SYSTEM/bootflag.txt: %d\n", fd);
    return -1;
  }
  int res = fileXioWrite(fd, psx2Bootflag, sizeof(psx2Bootflag));
  if (res != sizeof(psx2Bootflag)) {
    fileXioClose(fd);
    scr_printf("\n\tERROR: failed to write xfrom:/BIEXEC-SYSTEM/bootflag.txt: %d/%d\n", res, sizeof(psx2Bootflag));
    return -1;
  }
  fileXioClose(fd);
  return 0;
}
