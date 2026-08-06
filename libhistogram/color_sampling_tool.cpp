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

#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include "histogram_collector.h"

void sigint_handler(int) {}

void show_usage(char *progname) {
  std::cout << "Usage: ./" + std::string(progname) + " {options} \n"
            << "Sample the V (as in HSV) channel of the pixels that were displayed onscreen.\n\n"
            << "\tOptions:\n"
            << "\t-h      display this help message\n"
            << "\t-o      write output to specified filename\n"
            << "\t-t NUM  Collect results over NUM seconds, and then exit\n"
            << "\t-m NUM  Only store the last NUM frames of statistics\n"
            << "\t-s NUM  Sample every NUM frames (default: 1)\n"
            << "\t-a      Enable adaptive sampling based on refresh rate (default: on)\n"
            << "\t-d      Disable adaptive sampling\n"
            << "\t-v      Verbose output with performance metrics\n";
}

int main(int argc, char **argv) {
  struct sigaction sigHandler;
  sigHandler.sa_handler = sigint_handler;
  sigemptyset(&sigHandler.sa_mask);
  sigHandler.sa_flags = 0;
  sigaction(SIGINT, &sigHandler, NULL);

  int c;
  char *output_filename = NULL;
  int timeout = -1;
  int sampling_interval = 1;
  bool adaptive_sampling = true;
  bool verbose = false;
  
  while ((c = getopt(argc, argv, "o:t:m:s:advh")) != -1) {
    switch (c) {
      case 'o':
        output_filename = optarg;
        break;
      case 't':
        timeout = strtol(optarg, NULL, 10);
        break;
      case 'm':
        // Max frames handled in histogram start
        break;
      case 's':
        sampling_interval = strtol(optarg, NULL, 10);
        if (sampling_interval < 1) {
          std::cerr << "Sampling interval must be >= 1\n";
          return EXIT_FAILURE;
        }
        break;
      case 'a':
        adaptive_sampling = true;
        break;
      case 'd':
        adaptive_sampling = false;
        break;
      case 'v':
        verbose = true;
        break;
      default:
      case 'h':
        show_usage(argv[0]);
        return EXIT_SUCCESS;
    }
  }

  histogram::HistogramCollector histogram;
  
  // Configure sampling
  histogram.set_sampling_interval(sampling_interval);
  histogram.set_adaptive_sampling(adaptive_sampling);
  
  if (verbose) {
    std::cout << "Configuration:\n";
    std::cout << "  Sampling interval: " << sampling_interval << "\n";
    std::cout << "  Adaptive sampling: " << (adaptive_sampling ? "enabled" : "disabled") << "\n";
  }
  
  histogram.start();

  bool cancelled_during_wait = false;
  if (timeout > 0) {
    std::cout << "Sampling for " << timeout << " seconds.\n";
    struct timespec request, remaining;
    request.tv_sec = timeout;
    request.tv_nsec = 0;
    cancelled_during_wait = (nanosleep(&request, &remaining) != 0);
  } else {
    std::cout << "Sampling until Ctrl-C is pressed\n";
    sigsuspend(&sigHandler.sa_mask);
  }

  std::cout << "Sampling results:\n";

  histogram.stop();

  if (cancelled_during_wait) {
    std::cout << "Timed histogram collection cancelled via signal\n";
    return EXIT_SUCCESS;
  }

  if (output_filename) {
    std::cout << "\nWriting statistics to: " << output_filename << '\n';
    std::ofstream output_file;
    output_file.open(output_filename);
    if (!output_file.is_open()) {
      std::cerr << "Error, could not open given file: " << output_filename << "\n";
      return EXIT_FAILURE;
    }
    output_file << histogram.Dump();
    output_file.close();
  } else {
    std::cout << histogram.Dump() << '\n';
  }
  
  if (verbose) {
    auto metrics = histogram.get_metrics();
    std::cout << "\nPerformance Summary:\n";
    std::cout << "  Total frames processed: " << metrics.total_frames_processed << "\n";
    std::cout << "  Frames skipped: " << metrics.frames_skipped << "\n";
    std::cout << "  Frames dropped: " << metrics.frames_dropped << "\n";
    std::cout << "  Processing time (us): " << metrics.processing_time_us << "\n";
    std::cout << "  Refresh rate: " << metrics.current_refresh_rate << "Hz\n";
    std::cout << "  Effective sampling rate: " << metrics.effective_sampling_rate << "Hz\n";
  }

  return EXIT_SUCCESS;
}
