/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "install/wipe_data.h"

#include <string.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <functional>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <fs_mgr/roots.h>
#include <libdm/dm.h>

#include "install/snapshot_utils.h"
#include "otautil/dirutil.h"
#include "recovery_ui/ui.h"
#include "recovery_utils/logging.h"
#include "recovery_utils/roots.h"

constexpr const char* CACHE_ROOT = "/cache";
constexpr const char* DATA_ROOT = "/data";
constexpr const char* METADATA_ROOT = "/metadata";

static bool EraseVolume(const char* volume, RecoveryUI* ui, std::string fs) {
  bool is_cache = (strcmp(volume, CACHE_ROOT) == 0);
  bool is_data = (strcmp(volume, DATA_ROOT) == 0);

  std::vector<saved_log_file> log_files;
  if (is_cache) {
    // If we're reformatting /cache, we load any past logs (i.e. "/cache/recovery/last_*") and the
    // current log ("/cache/recovery/log") into memory, so we can restore them after the reformat.
    log_files = ReadLogFilesToMemory();
  }

  ui->Print("Formatting %s to %s...\n", volume, fs.c_str());

  Volume* vol = volume_for_mount_point(volume);
  if (vol->fs_mgr_flags.logical) {
    android::dm::DeviceMapper& dm = android::dm::DeviceMapper::Instance();

    map_logical_partitions();
    // map_logical_partitions is non-blocking, so check for some limited time
    // if it succeeded
    for (int i = 0; i < 500; i++) {
      if (vol->blk_device[0] == '/' ||
          dm.GetState(vol->blk_device) == android::dm::DmDeviceState::ACTIVE)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (vol->blk_device[0] != '/' && !dm.GetDmDevicePathByName(vol->blk_device, &vol->blk_device)) {
      PLOG(ERROR) << "Failed to find dm device path for " << vol->blk_device;
      return false;
    }

    int fd;
    for (int i = 0; i < 500; i++) {
      fd = open(vol->blk_device.c_str(), O_RDWR);
      if (fd >= 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (fd < 0) {
      PLOG(ERROR) << "Failed to open " << vol->blk_device;
      return false;
    }

    int val = 0;
    if (ioctl(fd, BLKROSET, &val) != 0) {
      PLOG(ERROR) << "Failed to set " << vol->blk_device << " rw";
      close(fd);
      return false;
    }

    close(fd);
  }

  if (ensure_volume_unmounted(vol->blk_device) == -1) {
    PLOG(ERROR) << "Failed to unmount volume!";
    return false;
  }

  int result;
  if (is_data) {
    result = format_volume(volume, "", fs);
  } else {
    result = format_volume(volume);
  }

  if (is_cache) {
    RestoreLogFilesAfterFormat(log_files);
  }

  return (result == 0);
}

static bool EraseVolume(const char* volume, RecoveryUI* ui) {
  return EraseVolume(volume, ui, volume_for_mount_point(volume)->fs_type);
}

bool WipeCache(RecoveryUI* ui, const std::function<bool()>& confirm_func) {
  bool has_cache = volume_for_mount_point("/cache") != nullptr;
  if (!has_cache) {
    ui->Print("No /cache partition found.\n");
    return false;
  }

  if (confirm_func && !confirm_func()) {
    return false;
  }

  ui->Print("\n-- Wiping cache...\n");
  ui->SetBackground(RecoveryUI::ERASING);
  ui->SetProgressType(RecoveryUI::INDETERMINATE);

  bool success = EraseVolume("/cache", ui);
  ui->Print("Cache wipe %s.\n", success ? "complete" : "failed");
  return success;
}

bool WipeData(Device* device, std::string fs) {
  RecoveryUI* ui = device->GetUI();
  ui->Print("\n-- Wiping data...\n");
  ui->SetBackground(RecoveryUI::ERASING);
  ui->SetProgressType(RecoveryUI::INDETERMINATE);

  if (!FinishPendingSnapshotMerges(device)) {
    ui->Print("Unable to check update status or complete merge, cannot wipe partitions.\n");
    return false;
  }

  bool success = device->PreWipeData();
  if (success) {
    success &= EraseVolume(DATA_ROOT, ui, fs);
    bool has_cache = volume_for_mount_point("/cache") != nullptr;
    if (has_cache) {
      success &= EraseVolume(CACHE_ROOT, ui);
    }
    if (volume_for_mount_point(METADATA_ROOT) != nullptr) {
      success &= EraseVolume(METADATA_ROOT, ui);
    }
  }
  if (success) {
    success &= device->PostWipeData();
  }
  ui->Print("Data wipe %s.\n", success ? "complete" : "failed");
  return success;
}

bool WipeData(Device* device) {
  return WipeData(device, volume_for_mount_point("/data")->fs_type);
}

bool WipeSystem(RecoveryUI* ui, const std::function<bool()>& confirm_func) {
  if (confirm_func && !confirm_func()) {
    return false;
  }

  ui->Print("\n-- Wiping system...\n");
  bool success = EraseVolume(android::fs_mgr::GetSystemRoot().c_str(), ui);
  ui->Print("System wipe %s.\n", success ? "complete" : "failed");
  return success;
}
