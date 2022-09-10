/*
 *
 * Copyright (C) 2022 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop and adapted from igt-gpu-tools from Intel Corporation.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/extract_processinfo_fdinfo.h"
#include "nvtop/time.h"

#include <assert.h>
#include <libudev.h>
#include <stdio.h>
#include <string.h>
#include <uthash.h>

#define HASH_FIND_CLIENT(head, key_ptr, out_ptr) HASH_FIND(hh, head, key_ptr, sizeof(unsigned), out_ptr)
#define HASH_ADD_CLIENT(head, in_ptr) HASH_ADD(hh, head, client_id, sizeof(unsigned), in_ptr)

#define SET_INTEL_CACHE(cachePtr, field, value) SET_VALUE(cachePtr, field, value, intel_cache_)
#define RESET_INTEL_CACHE(cachePtr, field) INVALIDATE_VALUE(cachePtr, field, intel_cache_)
#define INTEL_CACHE_FIELD_VALID(cachePtr, field) VALUE_IS_VALID(cachePtr, field, intel_cache_)

enum intel_process_info_cache_valid {
  intel_cache_engine_render_valid = 0,
  intel_cache_engine_copy_valid,
  intel_cache_engine_video_valid,
  intel_cache_engine_video_enhance_valid,
  intel_cache_process_info_cache_valid_count
};

struct __attribute__((__packed__)) unique_cache_id {
  unsigned client_id;
  pid_t pid;
};

struct intel_process_info_cache {
  struct unique_cache_id client_id;
  uint64_t engine_render;
  uint64_t engine_copy;
  uint64_t engine_video;
  uint64_t engine_video_enhance;
  nvtop_time last_measurement_tstamp;
  unsigned char valid[(intel_cache_process_info_cache_valid_count + CHAR_BIT - 1) / CHAR_BIT];
  UT_hash_handle hh;
};

struct gpu_info_intel {
  struct gpu_info base;

  struct udev_device *card_device;
  struct udev_device *card_parent;
  char pdev[PDEV_LEN];
  struct intel_process_info_cache *last_update_process_cache, *current_update_process_cache; // Cached processes info
};

static bool gpuinfo_intel_init(void);
static void gpuinfo_intel_shutdown(void);
static const char *gpuinfo_intel_last_error_string(void);
static bool gpuinfo_intel_get_device_handles(struct list_head *devices, unsigned *count, ssize_t *mask);
static void gpuinfo_intel_populate_static_info(struct gpu_info *_gpu_info);
static void gpuinfo_intel_refresh_dynamic_info(struct gpu_info *_gpu_info);
static void gpuinfo_intel_get_running_processes(struct gpu_info *_gpu_info);

struct gpu_vendor gpu_vendor_intel = {
    .init = gpuinfo_intel_init,
    .shutdown = gpuinfo_intel_shutdown,
    .last_error_string = gpuinfo_intel_last_error_string,
    .get_device_handles = gpuinfo_intel_get_device_handles,
    .populate_static_info = gpuinfo_intel_populate_static_info,
    .refresh_dynamic_info = gpuinfo_intel_refresh_dynamic_info,
    .refresh_running_processes = gpuinfo_intel_get_running_processes,
};

unsigned intel_gpu_count;
static struct gpu_info_intel *gpu_infos;

#define STRINGIFY(x) STRINGIFY_HELPER_(x)
#define STRINGIFY_HELPER_(x) #x

#define VENDOR_INTEL 0x8086
#define VENDOR_INTEL_STR STRINGIFY(VENDOR_INTEL)
// The integrated Intel GPU is always this device
// Discrete GPU are others
#define INTEGRATED_I915_GPU_PCI_ID "0000:00:02.0"

__attribute__((constructor)) static void init_extract_gpuinfo_intel(void) { register_gpu_vendor(&gpu_vendor_intel); }

bool gpuinfo_intel_init(void) { return true; }
void gpuinfo_intel_shutdown(void) {
  for (unsigned i = 0; i < intel_gpu_count; ++i) {
    struct gpu_info_intel *current = &gpu_infos[i];
    udev_device_unref(current->card_device);
    udev_device_unref(current->card_parent);
  }
}

const char *gpuinfo_intel_last_error_string(void) { return "Err"; }

static const char drm_intel_render[] = "drm-engine-render";
static const char drm_intel_copy[] = "drm-engine-copy";
static const char drm_intel_video[] = "drm-engine-video";
static const char drm_intel_video_enhance[] = "drm-engine-video-enhance";

static bool parse_drm_fdinfo_intel(struct gpu_info *info, FILE *fdinfo_file, struct gpu_process *process_info) {
  struct gpu_info_intel *gpu_info = container_of(info, struct gpu_info_intel, base);
  static char *line = NULL;
  static size_t line_buf_size = 0;
  ssize_t count = 0;

  bool client_id_set = false;
  unsigned cid;
  nvtop_time current_time;
  nvtop_get_current_time(&current_time);

  while ((count = getline(&line, &line_buf_size, fdinfo_file)) != -1) {
    char *key, *val;
    // Get rid of the newline if present
    if (line[count - 1] == '\n') {
      line[--count] = '\0';
    }

    if (!extract_drm_fdinfo_key_value(line, &key, &val))
      continue;

    if (!strcmp(key, drm_pdev)) {
      if (strcmp(val, gpu_info->pdev)) {
        return false;
      }
    } else if (!strcmp(key, drm_client_id)) {
      char *endptr;
      cid = strtoul(val, &endptr, 10);
      if (*endptr)
        continue;
      client_id_set = true;
    } else {
      bool is_render = !strcmp(key, drm_intel_render);
      bool is_copy = !strcmp(key, drm_intel_copy);
      bool is_video = !strcmp(key, drm_intel_video);
      bool is_video_enhance = !strcmp(key, drm_intel_video_enhance);

      if (is_render || is_copy || is_video || is_video_enhance) {
        char *endptr;
        uint64_t time_spent = strtoull(val, &endptr, 10);
        if (endptr == val || strcmp(endptr, " ns"))
          continue;
        if (is_render) {
          SET_GPUINFO_PROCESS(process_info, gfx_engine_used, time_spent);
        }
        if (is_copy) {
          // TODO: what is copy?
          (void)time_spent;
        }
        if (is_video) {
          // TODO: is this truly decode?
          SET_GPUINFO_PROCESS(process_info, enc_engine_used, time_spent);
        }
        if (is_video_enhance) {
          // TODO: is this truly decode?
          SET_GPUINFO_PROCESS(process_info, dec_engine_used, time_spent);
        }
      }
    }
  }
  if (!client_id_set)
    return false;

  struct intel_process_info_cache *cache_entry;
  struct unique_cache_id ucid = {.client_id = cid, .pid = process_info->pid};
  HASH_FIND_CLIENT(gpu_info->last_update_process_cache, &ucid, cache_entry);
  if (cache_entry) {
    uint64_t time_elapsed = nvtop_difftime_u64(cache_entry->last_measurement_tstamp, current_time);
    HASH_DEL(gpu_info->last_update_process_cache, cache_entry);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, gfx_engine_used) &&
        INTEL_CACHE_FIELD_VALID(cache_entry, engine_render) &&
        // In some rare occasions, the gfx engine usage reported by the driver is lowering (might be a driver bug)
        process_info->gfx_engine_used >= cache_entry->engine_render &&
        process_info->gfx_engine_used - cache_entry->engine_render <= time_elapsed) {
      SET_GPUINFO_PROCESS(
          process_info, gpu_usage,
          busy_usage_from_time_usage_round(process_info->gfx_engine_used, cache_entry->engine_render, time_elapsed));
    }
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, dec_engine_used) &&
        INTEL_CACHE_FIELD_VALID(cache_entry, engine_video) &&
        process_info->dec_engine_used >= cache_entry->engine_video &&
        process_info->dec_engine_used - cache_entry->engine_video <= time_elapsed) {
      SET_GPUINFO_PROCESS(
          process_info, decode_usage,
          busy_usage_from_time_usage_round(process_info->dec_engine_used, cache_entry->engine_video, time_elapsed));
    }
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, enc_engine_used) &&
        INTEL_CACHE_FIELD_VALID(cache_entry, engine_video_enhance) &&
        process_info->enc_engine_used >= cache_entry->engine_video_enhance &&
        process_info->enc_engine_used - cache_entry->engine_video_enhance <= time_elapsed) {
      SET_GPUINFO_PROCESS(process_info, encode_usage,
                          busy_usage_from_time_usage_round(process_info->enc_engine_used,
                                                           cache_entry->engine_video_enhance, time_elapsed));
    }
  } else {
    cache_entry = calloc(1, sizeof(*cache_entry));
    if (!cache_entry)
      goto parse_fdinfo_exit;
    cache_entry->client_id.client_id = cid;
    cache_entry->client_id.pid = process_info->pid;
  }

#ifndef NDEBUG
  // We should only process one fdinfo entry per client id per update
  struct intel_process_info_cache *cache_entry_check;
  HASH_FIND_CLIENT(gpu_info->current_update_process_cache, &cid, cache_entry_check);
  assert(!cache_entry_check && "We should not be processing a client id twice per update");
#endif

  RESET_ALL(cache_entry->valid);
  if (GPUINFO_PROCESS_FIELD_VALID(process_info, gfx_engine_used))
    SET_INTEL_CACHE(cache_entry, engine_render, process_info->gfx_engine_used);
  if (GPUINFO_PROCESS_FIELD_VALID(process_info, dec_engine_used))
    SET_INTEL_CACHE(cache_entry, engine_video, process_info->dec_engine_used);
  if (GPUINFO_PROCESS_FIELD_VALID(process_info, enc_engine_used))
    SET_INTEL_CACHE(cache_entry, engine_video_enhance, process_info->enc_engine_used);

  cache_entry->last_measurement_tstamp = current_time;
  HASH_ADD_CLIENT(gpu_info->current_update_process_cache, cache_entry);

parse_fdinfo_exit:
  return true;
}

static void add_intel_cards(struct udev_device *dev, struct list_head *devices, unsigned *count, ssize_t *mask) {
  struct udev_device *parent = udev_device_get_parent(dev);
  // Consider enabled Intel cards using the i915 driver
  if (!strcmp(udev_device_get_sysattr_value(parent, "vendor"), VENDOR_INTEL_STR) &&
      !strcmp(udev_device_get_driver(parent), "i915") &&
      !strcmp(udev_device_get_sysattr_value(parent, "enable"), "1")) {
    struct gpu_info_intel *thisGPU = &gpu_infos[intel_gpu_count++];
    thisGPU->card_device = udev_device_ref(dev);
    thisGPU->card_parent = udev_device_ref(parent);
    const char *pdev_val = udev_device_get_property_value(thisGPU->card_parent, "PCI_SLOT_NAME");
    assert(pdev_val != NULL && "Could not retrieve device PCI slot name");
    strncpy(thisGPU->pdev, pdev_val, PDEV_LEN);
    list_add_tail(&thisGPU->base.list, devices);
    // Register a fdinfo callback for this GPU
    processinfo_register_fdinfo_callback(parse_drm_fdinfo_intel, &thisGPU->base);
    (*count)++;
  }
  // TODO mask support
  (void)mask;
}

bool gpuinfo_intel_get_device_handles(struct list_head *devices_list, unsigned *count, ssize_t *mask) {
  struct udev *udev;
  struct udev_enumerate *enumerate;
  struct udev_list_entry *devices, *dev_list_entry;
  int ret;
  *count = 0;

  udev = udev_new();
  assert(udev);

  enumerate = udev_enumerate_new(udev);
  assert(enumerate);

  ret = udev_enumerate_add_match_subsystem(enumerate, "drm");
  assert(!ret);

  ret = udev_enumerate_add_match_property(enumerate, "DEVNAME", "/dev/dri/*");
  assert(!ret);

  ret = udev_enumerate_scan_devices(enumerate);
  assert(!ret);

  devices = udev_enumerate_get_list_entry(enumerate);
  if (!devices)
    return false;

  unsigned num_devices = 0;
  udev_list_entry_foreach(dev_list_entry, devices) { num_devices++; }
  gpu_infos = reallocarray(gpu_infos, num_devices, sizeof(*gpu_infos));
  if (!gpu_infos)
    return false;

  udev_list_entry_foreach(dev_list_entry, devices) {
    const char *path;
    struct udev_device *udev_dev;

    path = udev_list_entry_get_name(dev_list_entry);
    udev_dev = udev_device_new_from_syspath(udev, path);
    if (strstr(udev_device_get_devnode(udev_dev), "/dev/dri/card")) {
      add_intel_cards(udev_dev, devices_list, count, mask);
    }

    udev_device_unref(udev_dev);
  }
  udev_enumerate_unref(enumerate);
  udev_unref(udev);
  return true;
}

void gpuinfo_intel_populate_static_info(struct gpu_info *_gpu_info) {
  struct gpu_info_intel *gpu_info = container_of(_gpu_info, struct gpu_info_intel, base);
  struct gpuinfo_static_info *static_info = &gpu_info->base.static_info;
  const char *dev_name = udev_device_get_property_value(gpu_info->card_parent, "ID_MODEL_FROM_DATABASE");
  if (dev_name) {
    snprintf(static_info->device_name, sizeof(static_info->device_name), "%s", dev_name);
    SET_VALID(gpuinfo_device_name_valid, static_info->valid);
  }
}

void gpuinfo_intel_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  struct gpu_info_intel *gpu_info = container_of(_gpu_info, struct gpu_info_intel, base);
  struct gpuinfo_dynamic_info *dynamic_info = &gpu_info->base.dynamic_info;

  // GPU clock
  const char *gt_cur_freq = udev_device_get_sysattr_value(gpu_info->card_device, "gt_gt_cur_freq_mhz");
  if (gt_cur_freq) {
    unsigned val = strtoul(gt_cur_freq, NULL, 10);
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, val);
  }
  const char *gt_max_freq = udev_device_get_sysattr_value(gpu_info->card_device, "gt_max_freq_mhz");
  if (gt_max_freq) {
    unsigned val = strtoul(gt_max_freq, NULL, 10);
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, val);
  }

  // Mem clock
  // TODO: the attribute mem_cur_freq_mhz and mem_max_freq_mhz are speculative (not present on integrated graphics)
  const char *mem_cur_freq = udev_device_get_sysattr_value(gpu_info->card_device, "mem_cur_freq_mhz");
  if (mem_cur_freq) {
    unsigned val = strtoul(mem_cur_freq, NULL, 10);
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, val);
  }
  const char *mem_max_freq = udev_device_get_sysattr_value(gpu_info->card_device, "mem_max_freq_mhz");
  if (mem_max_freq) {
    unsigned val = strtoul(mem_max_freq, NULL, 10);
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, val);
  }

  // TODO: find how to extract global utilization
  // gpu util will be computed as the sum of all the processes utilization for now

  // TODO: Unknown attribute names to retrieve memory, pcie, fan, temperature, power info for discrete cards
}

static void swap_process_cache_for_next_update(struct gpu_info_intel *gpu_info) {
  // Free old cache data and set the cache for the next update
  if (gpu_info->last_update_process_cache) {
    struct intel_process_info_cache *cache_entry, *tmp;
    HASH_ITER(hh, gpu_info->last_update_process_cache, cache_entry, tmp) {
      HASH_DEL(gpu_info->last_update_process_cache, cache_entry);
      free(cache_entry);
    }
  }
  gpu_info->last_update_process_cache = gpu_info->current_update_process_cache;
  gpu_info->current_update_process_cache = NULL;
}

void gpuinfo_intel_get_running_processes(struct gpu_info *_gpu_info) {
  // For Intel, we register a fdinfo callback that will fill the gpu_process datastructure of the gpu_info structure
  // for us. This avoids going through /proc multiple times per update for multiple GPUs.
  struct gpu_info_intel *gpu_info = container_of(_gpu_info, struct gpu_info_intel, base);
  swap_process_cache_for_next_update(gpu_info);
}
