/*
 * Copyright (C) 2018 The Android Open Source Project
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
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

/*
* Changes from Qualcomm Innovation Center are provided under the following license:
*
* Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted (subject to the limitations in the
* disclaimer below) provided that the following conditions are met:
*
*    * Redistributions of source code must retain the above copyright
*      notice, this list of conditions and the following disclaimer.
*
*    * Redistributions in binary form must reproduce the above
*      copyright notice, this list of conditions and the following
*      disclaimer in the documentation and/or other materials provided
*      with the distribution.
*
*    * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
*      contributors may be used to endorse or promote products derived
*      from this software without specific prior written permission.
*
* NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
* GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
* HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
* ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
* GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
* IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
* OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <fcntl.h>
#include <log/log.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <display/drm/msm_drm_pp.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "histogram_collector.h"
#include "ringbuffer.h"

constexpr static auto implementation_defined_max_frame_ringbuffer = 300;

// NEON optimization for ARM
#ifdef __ARM_NEON
#include <arm_neon.h>

static std::array<uint64_t, 8> rebucketTo8Buckets(
    std::array<uint64_t, HIST_V_SIZE> const &frame) {
  std::array<uint64_t, 8> bins;
  bins.fill(0);
  
  // NEON-optimized for 256 entries (32 per bucket)
  for (size_t bucket = 0; bucket < 8; bucket++) {
    uint64x2_t sum_low = vdupq_n_u64(0);
    uint64x2_t sum_high = vdupq_n_u64(0);
    
    size_t start = bucket * 32;
    for (size_t i = 0; i < 32; i += 4) {
      uint64x2_t a = vld1q_u64(&frame.data[start + i]);
      uint64x2_t b = vld1q_u64(&frame.data[start + i + 2]);
      sum_low = vaddq_u64(sum_low, a);
      sum_high = vaddq_u64(sum_high, b);
    }
    
    // Sum all elements
    uint64_t total = vaddvq_u64(sum_low) + vaddvq_u64(sum_high);
    bins[bucket] = total;
  }
  return bins;
}

#else
// Fallback scalar implementation
static constexpr size_t numBuckets = 8;
static_assert((HIST_V_SIZE % numBuckets) == 0,
              "histogram cannot be rebucketed to smaller number of buckets");
static constexpr int bucket_compression = HIST_V_SIZE / numBuckets;

static std::array<uint64_t, 8> rebucketTo8Buckets(
    std::array<uint64_t, HIST_V_SIZE> const &frame) {
  std::array<uint64_t, 8> bins;
  bins.fill(0);
  for (auto i = 0u; i < HIST_V_SIZE; i++)
    bins[i / bucket_compression] += frame[i];
  return bins;
}
#endif

histogram::HistogramCollector::HistogramCollector()
    : histogram(histogram::Ringbuffer::create(implementation_defined_max_frame_ringbuffer,
                                              std::make_unique<histogram::DefaultTimeKeeper>())) {
  last_sample_time_ = std::chrono::steady_clock::now();
}

histogram::HistogramCollector::~HistogramCollector() {
  stop();
}

std::string histogram::HistogramCollector::Dump() const {
  uint64_t num_frames = 0;
  std::array<uint64_t, HIST_V_SIZE> all_sample_buckets;
  std::tie(num_frames, all_sample_buckets) = histogram->collect_cumulative();
  std::array<uint64_t, 8> samples = rebucketTo8Buckets(all_sample_buckets);

  std::stringstream ss;
  ss << "Color Sampling, dark (0.0) to light (1.0): sampled frames: " << num_frames << '\n';
  if (num_frames == 0) {
    ss << "\tno color statistics collected\n";
    return ss.str();
  }

  ss << std::fixed << std::setprecision(3);
  ss << "\tbucket\t\t: # of displayed pixels at bucket value\n";
  for (auto i = 0u; i < samples.size(); i++) {
    ss << "\t" << i / static_cast<float>(samples.size()) << " to "
       << (i + 1) / static_cast<float>(samples.size()) << "\t: " << samples[i] << '\n';
  }

  // Add performance metrics
  auto metrics = get_metrics();
  ss << "\nPerformance Metrics:\n";
  ss << "\tFrames processed: " << metrics.total_frames_processed << '\n';
  ss << "\tFrames skipped: " << metrics.frames_skipped << '\n';
  ss << "\tFrames dropped: " << metrics.frames_dropped << '\n';
  ss << "\tRefresh rate: " << metrics.current_refresh_rate << "Hz\n";
  ss << "\tEffective sampling rate: " << metrics.effective_sampling_rate << "Hz\n";

  return ss.str();
}

HWC2::Error histogram::HistogramCollector::collect(
    uint64_t max_frames, uint64_t timestamp,
    int32_t out_samples_size[NUM_HISTOGRAM_COLOR_COMPONENTS],
    uint64_t *out_samples[NUM_HISTOGRAM_COLOR_COMPONENTS], uint64_t *out_num_frames) const {
  if (!out_samples_size || !out_num_frames)
    return HWC2::Error::BadParameter;

  out_samples_size[0] = 0;
  out_samples_size[1] = 0;
  out_samples_size[2] = 8;
  out_samples_size[3] = 0;

  uint64_t num_frames = 0;
  std::array<uint64_t, HIST_V_SIZE> samples;

  if (max_frames == 0 && timestamp == 0) {
    std::tie(num_frames, samples) = histogram->collect_cumulative();
  } else if (max_frames == 0) {
    std::tie(num_frames, samples) = histogram->collect_after(timestamp);
  } else if (timestamp == 0) {
    std::tie(num_frames, samples) = histogram->collect_max(max_frames);
  } else {
    std::tie(num_frames, samples) = histogram->collect_max_after(timestamp, max_frames);
  }

  auto samples_rebucketed = rebucketTo8Buckets(samples);
  *out_num_frames = num_frames;
  if (out_samples && out_samples[2])
    memcpy(out_samples[2], samples_rebucketed.data(), sizeof(uint64_t) * samples_rebucketed.size());

  return HWC2::Error::None;
}

HWC2::Error histogram::HistogramCollector::getAttributes(int32_t *format, int32_t *dataspace,
                                                         uint8_t *supported_components) const {
  if (!format || !dataspace || !supported_components)
    return HWC2::Error::BadParameter;

  *format = HAL_PIXEL_FORMAT_HSV_888;
  *dataspace = HAL_DATASPACE_UNKNOWN;
  *supported_components = HWC2_FORMAT_COMPONENT_2;
  return HWC2::Error::None;
}

void histogram::HistogramCollector::start() {
  start(implementation_defined_max_frame_ringbuffer);
}

void histogram::HistogramCollector::start(uint64_t max_frames) {
  std::unique_lock<decltype(mutex)> lk(mutex);
  if (started) {
    return;
  }

  started = true;
  max_frames_ = max_frames;
  histogram =
      histogram::Ringbuffer::create(max_frames, std::make_unique<histogram::DefaultTimeKeeper>());
  monitoring_thread = std::thread(&HistogramCollector::blob_processing_thread, this);
}

void histogram::HistogramCollector::stop() {
  std::unique_lock<decltype(mutex)> lk(mutex);
  if (!started) {
    return;
  }

  started = false;
  cv.notify_all();
  lk.unlock();

  if (monitoring_thread.joinable())
    monitoring_thread.join();
}

void histogram::HistogramCollector::notify_histogram_event(int blob_source_fd, BlobId id) {
  std::unique_lock<decltype(mutex)> lk(mutex);
  if (!started) {
    ALOGW("Discarding event blob-id: %X", id);
    return;
  }
  if (work_available) {
    ALOGI("notified of histogram event before consuming last one. prior event discarded");
  }

  work_available = true;
  blobwork = HistogramCollector::BlobWork{blob_source_fd, id};
  cv.notify_all();
}

bool histogram::HistogramCollector::should_sample() {
  auto now = std::chrono::steady_clock::now();
  
  // Adaptive sampling based on refresh rate
  if (adaptive_sampling_) {
    uint32_t refresh_rate = get_current_refresh_rate();
    metrics_.current_refresh_rate = refresh_rate;
    
    // Adjust sampling interval based on refresh rate
    if (refresh_rate >= 120) {
      sampling_interval_ = 3;  // Sample every 3rd frame at 120Hz
    } else if (refresh_rate >= 90) {
      sampling_interval_ = 2;  // Sample every other frame at 90Hz
    } else {
      sampling_interval_ = 1;  // Sample every frame at <=60Hz
    }
    
    // Reduce sampling for static content
    if (is_static_image()) {
      sampling_interval_ = std::max(sampling_interval_, 10u);
    }
  }
  
  frame_counter_++;
  metrics_.total_frames_processed++;
  
  // Apply sampling interval
  if (frame_counter_ % sampling_interval_ != 0) {
    metrics_.frames_skipped++;
    return false;
  }
  
  // Rate limiting to prevent CPU overload
  if (now - last_sample_time_ < std::chrono::milliseconds(5)) {
    metrics_.frames_dropped++;
    return false;
  }
  
  last_sample_time_ = now;
  
  // Update effective sampling rate
  metrics_.effective_sampling_rate = metrics_.current_refresh_rate / sampling_interval_;
  
  return true;
}

uint32_t histogram::HistogramCollector::get_current_refresh_rate() const {
  // Query display refresh rate from DRM
  // This is a simplified implementation - actual implementation would query DRM
  static uint32_t cached_rate = 60;
  
  // In production, this would read from /sys/class/drm/card0/mode or similar
  // For now, return a reasonable default
  return cached_rate;
}

bool histogram::HistogramCollector::is_static_image() const {
  // Compare current frame with last frame to detect static content
  // This would need access to the actual frame data
  // Simplified implementation
  static_frame_counter_++;
  if (static_frame_counter_ > 30) {  // If static for >30 frames
    return true;
  }
  return false;
}

histogram::HistogramCollector::PerformanceMetrics 
histogram::HistogramCollector::get_metrics() const {
  std::unique_lock<decltype(mutex)> lk(mutex);
  return metrics_;
}

void histogram::HistogramCollector::blob_processing_thread() {
  pthread_setname_np(pthread_self(), "histogram_blob");

  std::unique_lock<decltype(mutex)> lk(mutex);

  while (true) {
    // Use timeout to avoid blocking forever
    auto status = cv.wait_for(lk, std::chrono::milliseconds(100),
                             [this] { return !started || work_available; });
    
    if (!started) {
      return;
    }
    
    if (!status) {
      // Timeout - check if we need to update metrics
      continue;
    }

    auto work = blobwork;
    work_available = false;
    lk.unlock();

    // Check if we should sample this frame
    if (!should_sample()) {
      lk.lock();
      continue;
    }

    auto start_time = std::chrono::steady_clock::now();

    drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(work.fd, work.id);
    if (!blob || !blob->data) {
      lk.lock();
      continue;
    }
    
    // Insert the histogram data
    histogram->insert(*static_cast<struct drm_msm_hist *>(blob->data));
    drmModeFreePropertyBlob(blob);

    // Track processing time
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    metrics_.processing_time_us = duration.count();

    lk.lock();
  }
}
