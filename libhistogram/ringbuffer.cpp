/*
 * Copyright (C) 2018 The Android Open Source Project
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
#include <cutils/compiler.h>
#include <log/log.h>
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <cstring>

#include "ringbuffer.h"

nsecs_t histogram::DefaultTimeKeeper::current_time() const {
  return systemTime(SYSTEM_TIME_MONOTONIC);
}

histogram::Ringbuffer::Ringbuffer(size_t ringbuffer_size, std::unique_ptr<histogram::TimeKeeper> tk)
    : buffer_(std::make_unique<HistogramEntry[]>(ringbuffer_size)),
      rb_max_size_(ringbuffer_size),
      timekeeper(std::move(tk)),
      cumulative_frame_count_(0) {
  cumulative_bins_.fill(0);
  if (!buffer_) {
    ALOGE("Failed to allocate ringbuffer of size %zu", ringbuffer_size);
  }
}

std::unique_ptr<histogram::Ringbuffer> histogram::Ringbuffer::create(
    size_t ringbuffer_size, std::unique_ptr<histogram::TimeKeeper> tk) {
  if ((ringbuffer_size == 0) || !tk)
    return nullptr;
  return std::unique_ptr<histogram::Ringbuffer>(
      new histogram::Ringbuffer(ringbuffer_size, std::move(tk)));
}

void histogram::Ringbuffer::update_cumulative(nsecs_t now, uint64_t &count,
                                              std::array<uint64_t, HIST_V_SIZE> &bins) const {
  if (ringbuffer_size_ == 0)
    return;

  count++;
  const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::nanoseconds(now - buffer_[head_].start_timestamp));

  for (auto i = 0u; i < bins.size(); i++) {
    auto const increment = buffer_[head_].histogram.data[i] * delta.count();
    if (CC_UNLIKELY((bins[i] + increment < bins[i]) ||
                    (increment < buffer_[head_].histogram.data[i]))) {
      bins[i] = std::numeric_limits<uint64_t>::max();
    } else {
      bins[i] += increment;
    }
  }
}

void histogram::Ringbuffer::insert(drm_msm_hist const &frame) {
  std::unique_lock<decltype(mutex)> lk(mutex);
  auto now = timekeeper->current_time();

  // Update cumulative stats before overwriting
  if (ringbuffer_size_ > 0) {
    update_cumulative(now, cumulative_frame_count_, cumulative_bins_);
  }

  // Circular buffer implementation
  if (ringbuffer_size_ == rb_max_size_) {
    // Buffer is full, overwrite oldest
    size_t old_head = head_.load();
    head_.store((old_head + 1) % rb_max_size_);
  } else {
    // Buffer not full yet
    size_t current_size = ringbuffer_size_.load();
    ringbuffer_size_.store(current_size + 1);
  }

  // Store new entry
  size_t current_tail = tail_.load();
  buffer_[current_tail] = {frame, now, 0};
  tail_.store((current_tail + 1) % rb_max_size_);
}

bool histogram::Ringbuffer::resize(size_t ringbuffer_size) {
  std::unique_lock<decltype(mutex)> lk(mutex);
  if (ringbuffer_size == 0)
    return false;
  
  // Create new buffer
  auto new_buffer = std::make_unique<HistogramEntry[]>(ringbuffer_size);
  if (!new_buffer)
    return false;

  // Copy existing entries
  size_t copy_count = std::min(ringbuffer_size_.load(), ringbuffer_size);
  for (size_t i = 0; i < copy_count; i++) {
    size_t src_idx = (head_.load() + i) % rb_max_size_;
    new_buffer[i] = buffer_[src_idx];
  }

  // Update buffer
  buffer_ = std::move(new_buffer);
  rb_max_size_ = ringbuffer_size;
  head_.store(0);
  tail_.store(copy_count % ringbuffer_size);
  ringbuffer_size_.store(copy_count);
  
  return true;
}

histogram::Ringbuffer::Sample histogram::Ringbuffer::collect_cumulative() const {
  std::unique_lock<decltype(mutex)> lk(mutex);
  histogram::Ringbuffer::Sample sample{cumulative_frame_count_, cumulative_bins_};
  if (ringbuffer_size_ > 0) {
    update_cumulative(timekeeper->current_time(), std::get<0>(sample), std::get<1>(sample));
  }
  return sample;
}

histogram::Ringbuffer::Sample histogram::Ringbuffer::collect_ringbuffer_all() const {
  std::unique_lock<decltype(mutex)> lk(mutex);
  return collect_max(ringbuffer_size_.load(), lk);
}

histogram::Ringbuffer::Sample histogram::Ringbuffer::collect_after(nsecs_t timestamp) const {
  std::unique_lock<decltype(mutex)> lk(mutex);
  return collect_max_after(timestamp, ringbuffer_size_.load(), lk);
}

histogram::Ringbuffer::Sample histogram::Ringbuffer::collect_max(uint32_t max_frames) const {
  std::unique_lock<decltype(mutex)> lk(mutex);
  return collect_max(max_frames, lk);
}

histogram::Ringbuffer::Sample histogram::Ringbuffer::collect_max_after(nsecs_t timestamp,
                                                                       uint32_t max_frames) const {
  std::unique_lock<decltype(mutex)> lk(mutex);
  return collect_max_after(timestamp, max_frames, lk);
}

histogram::Ringbuffer::Sample histogram::Ringbuffer::collect_max(
    uint32_t max_frames, std::unique_lock<std::mutex> const &) const {
  size_t current_size = ringbuffer_size_.load();
  auto collect_first = std::min(static_cast<size_t>(max_frames), current_size);
  if (collect_first == 0)
    return {0, {}};
  
  std::array<uint64_t, HIST_V_SIZE> bins;
  bins.fill(0);
  
  // Start from the newest entry (head)
  size_t start_idx = head_.load();
  size_t count = 0;
  size_t idx = start_idx;
  
  while (count < collect_first && idx != tail_.load()) {
    const auto& entry = buffer_[idx];
    nsecs_t end_timestamp = entry.end_timestamp;
    if (idx == head_.load()) {
      end_timestamp = timekeeper->current_time();
    }
    
    const auto time_displayed = std::chrono::nanoseconds(end_timestamp - entry.start_timestamp);
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(time_displayed);
    
    for (auto i = 0u; i < HIST_V_SIZE; i++) {
      bins[i] += entry.histogram.data[i] * delta.count();
    }
    
    count++;
    idx = (idx + 1) % rb_max_size_;
  }
  
  return {collect_first, bins};
}

histogram::Ringbuffer::Sample histogram::Ringbuffer::collect_max_after(
    nsecs_t timestamp, uint32_t max_frames, std::unique_lock<std::mutex> const &lk) const {
  size_t current_size = ringbuffer_size_.load();
  if (current_size == 0)
    return {0, {}};
  
  // Find first entry with timestamp >= timestamp
  size_t idx = head_.load();
  size_t count = 0;
  size_t entries_before_timestamp = 0;
  
  while (idx != tail_.load()) {
    if (buffer_[idx].start_timestamp >= timestamp)
      break;
    entries_before_timestamp++;
    idx = (idx + 1) % rb_max_size_;
  }
  
  // Calculate how many entries to collect
  size_t entries_after_timestamp = current_size - entries_before_timestamp;
  size_t collect_count = std::min(static_cast<size_t>(max_frames), entries_after_timestamp);
  
  if (collect_count == 0)
    return {0, {}};
  
  return collect_max(collect_count, lk);
}
