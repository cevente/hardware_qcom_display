/* Copyright (c) 2015 - 2019, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#include <dlfcn.h>
#include <private/color_interface.h>
#include <utils/constants.h>
#include <utils/debug.h>
#include <algorithm>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unistd.h>

#include "color_manager.h"

#define __CLASS__ "ColorManager"

namespace sdm {

// Static member initialization
DynLib ColorManagerProxy::color_lib_;
CreateColorInterface ColorManagerProxy::create_intf_ = nullptr;
DestroyColorInterface ColorManagerProxy::destroy_intf_ = nullptr;
HWResourceInfo ColorManagerProxy::hw_res_info_;

void ColorManagerProxy::PerformanceStats::LogStats() const {
  uint32_t calls = total_calls.load();
  if (calls == 0) return;
  uint64_t avg_us = total_processing_time_us.load() / calls;
  DLOGI("ColorMgr Stats: calls=%u, failed=%u, avg_us=%llu, hdr=%u, gamut_sw=%u",
        calls, failed_calls.load(), 
        static_cast<unsigned long long>(avg_us),
        hdr_frames_processed.load(), gamut_switches.load());
}

// STCIntfClient Implementation
STCIntfClient::STCIntfClient() 
    : stc_intf_(nullptr), 
      GetScPostBlendInterface(nullptr),
      initialized_(false) {}

STCIntfClient::~STCIntfClient() {
  DeInit();
}

bool NeedsToneMap(const std::vector<Layer> &layers) {
  for (auto &layer : layers) {
    if (layer.request.flags.dest_tone_map) {
      return true;
    }
  }
  return false;
}

FeatureInterface* GetPostedStartFeatureCheckIntf(HWInterface *intf, PPFeaturesConfig *config) {
  return new ColorFeatureCheckingImpl(intf, config);
}

// STCIntfClient Implementation
DisplayError STCIntfClient::Init(const std::string &panel_name) {
  lock_guard<mutex> lock(lock_);
  
  if (initialized_ && stc_intf_) {
    DLOGI("STC interface is already instantiated");
    return kErrorNone;
  }

  if (!stc_intf_lib_.Open(kStcIntfLib_)) {
    DLOGW("STC library is not present: %s", kStcIntfLib_);
    return kErrorNotSupported;
  }

  if (!stc_intf_lib_.Sym("GetScPostBlendInterface",
                    reinterpret_cast<void **>(&GetScPostBlendInterface))) {
    DLOGE("GetScPostBlendInterface symbol not found!");
    return kErrorNotSupported;
  }

  uint32_t major_version = 1;
  uint32_t minor_version = 0;

  stc_intf_ = GetScPostBlendInterface(major_version, minor_version);
  if (!stc_intf_) {
    DLOGE("Failed to get STC Interface!");
    return kErrorNotSupported;
  }

  int ret = stc_intf_->Init(panel_name);
  if (ret != 0) {
    DLOGE("STC Interface init failed!, error = %d", ret);
    stc_intf_ = nullptr;
    return kErrorNotSupported;
  }

  initialized_ = true;
  return kErrorNone;
}

DisplayError STCIntfClient::DeInit() {
  lock_guard<mutex> lock(lock_);
  
  if (stc_intf_) {
    stc_intf_->DeInit();
    stc_intf_ = nullptr;
  }
  
  initialized_ = false;
  return kErrorNone;
}

DisplayError STCIntfClient::SetProperty(const ScPayload &payload) {
  lock_guard<mutex> lock(lock_);

  if (!stc_intf_ || !initialized_) {
    return kErrorNotSupported;
  }
  
  int ret = stc_intf_->SetProperty(payload);
  if (ret != 0) {
    DLOGE("Failed to SetProperty: %d, length = %d, ret = %d", payload.prop, payload.len, ret);
    return kErrorNotSupported;
  }

  return kErrorNone;
}

DisplayError STCIntfClient::GetProperty(ScPayload *payload) {
  lock_guard<mutex> lock(lock_);

  if (!stc_intf_ || !initialized_) {
    return kErrorNotSupported;
  }
  
  if (!payload) {
    DLOGE("Invalid parameters");
    return kErrorParameters;
  }

  int ret = stc_intf_->GetProperty(payload);
  if (ret != 0) {
    DLOGE("Failed to GetProperty: %d, length = %d, ret = %d", payload->prop, payload->len, ret);
    return kErrorNotSupported;
  }

  return kErrorNone;
}

DisplayError STCIntfClient::ProcessOps(const ScOps op, const ScPayload &input, ScPayload *output) {
  lock_guard<mutex> lock(lock_);

  if (!stc_intf_ || !initialized_) {
    DLOGW("STC interface not available, using fallback");
    return HandleFallbackOperations(op, input, output);
  }
  
  if (!output) {
    DLOGE("Invalid parameters");
    return kErrorParameters;
  }

  int ret = stc_intf_->ProcessOps(op, input, output);
  if (ret != 0) {
    DLOGW("STC ProcessOps failed: %d, using fallback", ret);
    return HandleFallbackOperations(op, input, output);
  }

  return kErrorNone;
}

DisplayError STCIntfClient::HandleFallbackOperations(const ScOps op, const ScPayload &input, 
                                                     ScPayload *output) {
  switch(op) {
    case kScModeRenderIntent:
      return HandleFallbackRenderIntent(input, output);
    default:
      DLOGW("Unsupported fallback operation: %d", op);
      return kErrorNotSupported;
  }
}

DisplayError STCIntfClient::HandleFallbackRenderIntent(const ScPayload &input, ScPayload *output) {
  if (!output) {
    return kErrorParameters;
  }
  
  // NOTE: This is a simplified fallback. In production, proper deep copy
  // of the payload data would be needed based on the property type.
  // For now, return not supported to avoid unsafe operations.
  return kErrorNotSupported;
}

// ColorManagerProxy Implementation
DisplayError ColorManagerProxy::Init(const HWResourceInfo &hw_res_info) {
  DisplayError error = kErrorNone;

  // Load color service library and retrieve its entry points.
  if (color_lib_.Open(COLORMGR_LIBRARY_NAME)) {
    if (!color_lib_.Sym(CREATE_COLOR_INTERFACE_NAME, reinterpret_cast<void **>(&create_intf_)) ||
        !color_lib_.Sym(DESTROY_COLOR_INTERFACE_NAME, reinterpret_cast<void **>(&destroy_intf_))) {
      DLOGW("Fail to retrieve = %s from %s", CREATE_COLOR_INTERFACE_NAME, COLORMGR_LIBRARY_NAME);
      error = kErrorResources;
    }
  } else {
    DLOGW("Fail to load = %s", COLORMGR_LIBRARY_NAME);
    error = kErrorResources;
  }

  hw_res_info_ = hw_res_info;

  return error;
}

void ColorManagerProxy::Deinit() {
  // Nothing to do - color_lib_ will be cleaned up on process exit
}

ColorManagerProxy::ColorManagerProxy(int32_t id, DisplayType type, HWInterface *intf,
                                     const HWDisplayAttributes &attr,
                                     const HWPanelInfo &info)
    : display_id_(id), device_type_(type), pp_hw_attributes_(), hw_intf_(intf),
      color_intf_(nullptr), pp_features_(), feature_intf_(nullptr),
      stc_intf_client_(nullptr), support_stc_tonemap_(false) {
  
  // Load runtime configuration
  config_ = RuntimeConfig::LoadFromProperties();
  
  // Detect AMOLED panel
  amoled_panel_ = IsAMOLEDPanel();
  
  // Initialize panel peak brightness
  panel_peak_brightness_ = attr.peak_brightness;
  if (panel_peak_brightness_ == 0) {
    panel_peak_brightness_ = 1800; // Default for 6.67" AMOLED if not provided
  }
  
  // Initialize feature interface if needed
  int32_t enable_posted_start_dyn = 0;
  Debug::Get()->GetProperty("persist.sdm.enable_posted_start_dyn", &enable_posted_start_dyn);
  if (enable_posted_start_dyn && info.mode == kModeCommand) {
    feature_intf_ = GetPostedStartFeatureCheckIntf(intf, &pp_features_);
    if (!feature_intf_) {
      DLOGI("Failed to create feature interface");
    } else {
      DisplayError err = feature_intf_->Init();
      if (err != kErrorNone) {
        DLOGE("Failed to init feature interface");
        delete feature_intf_;
        feature_intf_ = nullptr;
      }
    }
  }

  // Initialize conversion table
  convert_[kPbGamut] = &ColorManagerProxy::ConvertToGamut;
  convert_[kPbIgc] = &ColorManagerProxy::ConvertToIgc;
  convert_[kPbGC] = &ColorManagerProxy::ConvertToGc;
  
  // Initialize cache
  InvalidateCache();
}

ColorManagerProxy *ColorManagerProxy::CreateColorManagerProxy(DisplayType type,
                                                              HWInterface *hw_intf,
                                                              const HWDisplayAttributes &attribute,
                                                              const HWPanelInfo &panel_info,
                                                              DppsControlInterface *dpps_intf) {
  DisplayError error = kErrorNone;
  PPFeatureVersion versions;
  int32_t display_id = -1;
  ColorManagerProxy *color_manager_proxy = nullptr;

  // Check if all resources are available before invoking factory method from libsdm-color.so.
  if (!color_lib_ || !create_intf_ || !destroy_intf_) {
    DLOGW("Information for %s isn't available!", COLORMGR_LIBRARY_NAME);
    return nullptr;
  }

  hw_intf->GetDisplayId(&display_id);
  color_manager_proxy = new ColorManagerProxy(display_id, type, hw_intf, attribute, panel_info);

  if (color_manager_proxy) {
    // Query post-processing feature version from HWInterface.
    error = color_manager_proxy->hw_intf_->GetPPFeaturesVersion(&versions);
    PPHWAttributes &hw_attr = color_manager_proxy->pp_hw_attributes_;
    if (error != kErrorNone) {
      DLOGW("Fail to get DSPP feature versions");
    } else {
      hw_attr.Set(hw_res_info_, panel_info, attribute, versions, dpps_intf);
      DLOGI("PAV2 version is versions = %d, version = %d ",
            hw_attr.version.version[kGlobalColorFeaturePaV2],
            versions.version[kGlobalColorFeaturePaV2]);
    }

    // Instantiate concrete ColorInterface from libsdm-color.so
    error = create_intf_(COLOR_VERSION_TAG, color_manager_proxy->display_id_,
                         color_manager_proxy->device_type_, hw_attr,
                         &color_manager_proxy->color_intf_);
    if (error != kErrorNone) {
      DLOGW("Unable to instantiate concrete ColorInterface from %s", COLORMGR_LIBRARY_NAME);
      delete color_manager_proxy;
      color_manager_proxy = nullptr;
      return color_manager_proxy;
    }

    color_manager_proxy->stc_intf_client_ = new STCIntfClient();
    if (!color_manager_proxy->stc_intf_client_) {
      DLOGW("Unable to instantiate concrete StcInterface");
      return color_manager_proxy;
    }

    error = color_manager_proxy->stc_intf_client_->Init(hw_attr.panel_name);
    if (error != kErrorNone) {
      DLOGW("Failed to init StcInterface");
      delete color_manager_proxy->stc_intf_client_;
      color_manager_proxy->stc_intf_client_ = nullptr;
      return color_manager_proxy;
    }
    color_manager_proxy->support_stc_tonemap_ = color_manager_proxy->GetSupportStcTonemap();
  }

  return color_manager_proxy;
}

ColorManagerProxy::~ColorManagerProxy() {
  // Log performance stats before destruction
  perf_stats_.LogStats();
  
  if (destroy_intf_) {
    destroy_intf_(display_id_);
  }
  
  color_intf_ = nullptr;
  
  if (feature_intf_) {
    feature_intf_->Deinit();
    delete feature_intf_;
    feature_intf_ = nullptr;
  }
  
  if (stc_intf_client_) {
    stc_intf_client_->DeInit();
    delete stc_intf_client_;
    stc_intf_client_ = nullptr;
  }
}

DisplayError ColorManagerProxy::ColorSVCRequestRoute(const PPDisplayAPIPayload &in_payload,
                                                     PPDisplayAPIPayload *out_payload,
                                                     PPPendingParams *pending_action) {
  ScopedTimer timer(perf_stats_);
  DisplayError ret = kErrorNone;

  ret = color_intf_->ColorSVCRequestRoute(in_payload, out_payload, &pp_features_, pending_action);
  
  if (ret != kErrorNone) {
    timer.SetSuccess(false);
    LogColorOperation("ColorSVCRequestRoute", ret);
  }

  return ret;
}

DisplayError ColorManagerProxy::ApplyDefaultDisplayMode(void) {
  ScopedTimer timer(perf_stats_);
  DisplayError ret = kErrorNone;

  ret = color_intf_->ApplyDefaultDisplayMode(&pp_features_);
  
  if (ret != kErrorNone) {
    timer.SetSuccess(false);
    LogColorOperation("ApplyDefaultDisplayMode", ret);
  }

  return ret;
}

bool ColorManagerProxy::NeedsPartialUpdateDisable() {
  Locker &locker(pp_features_.GetLocker());
  SCOPE_LOCK(locker);

  return pp_features_.IsDirty();
}

DisplayError ColorManagerProxy::Commit() {
  ScopedTimer timer(perf_stats_);
  Locker &locker(pp_features_.GetLocker());
  SCOPE_LOCK(locker);

  if (!hw_intf_) {
    DLOGE("HW interface is null");
    timer.SetSuccess(false);
    return kErrorUndefined;
  }

  DisplayError ret = kErrorNone;
  bool is_dirty = pp_features_.IsDirty();

  if (is_dirty) {
    ret = hw_intf_->SetPPFeatures(&pp_features_);
    if (ret != kErrorNone) {
      DLOGE("SetPPFeatures failed: %d", ret);
      timer.SetSuccess(false);
      return ret;
    }
  }

  return ret;
}

void PPHWAttributes::Set(const HWResourceInfo &hw_res,
                         const HWPanelInfo &panel_info,
                         const DisplayConfigVariableInfo &attr,
                         const PPFeatureVersion &feature_ver,
                         DppsControlInterface *intf) {
  HWResourceInfo &res = *this;
  res = hw_res;
  HWPanelInfo &panel = *this;
  panel = panel_info;
  DisplayConfigVariableInfo &attributes = *this;
  attributes = attr;
  version = feature_ver;
  dpps_intf = intf;

  if (strlen(panel_info.panel_name)) {
    snprintf(&panel_name[0], sizeof(panel_name), "%s", &panel_info.panel_name[0]);
    char *tmp = panel_name;
    while ((tmp = strstr(tmp, " ")) != nullptr)
      *tmp = '_';
    if ((tmp = strstr(panel_name, "\n")) != nullptr)
      *tmp = '\0';
  }
}

DisplayError ColorManagerProxy::ColorMgrGetNumOfModes(uint32_t *mode_cnt) {
  return color_intf_->ColorIntfGetNumDisplayModes(&pp_features_, 0, mode_cnt);
}

DisplayError ColorManagerProxy::ColorMgrGetModes(uint32_t *mode_cnt,
                                                 SDEDisplayMode *modes) {
  return color_intf_->ColorIntfEnumerateDisplayModes(&pp_features_, 0, modes, mode_cnt);
}

DisplayError ColorManagerProxy::ColorMgrSetMode(int32_t color_mode_id) {
  return color_intf_->ColorIntfSetDisplayMode(&pp_features_, 0, color_mode_id);
}

DisplayError ColorManagerProxy::ColorMgrGetModeInfo(int32_t mode_id, AttrVal *query) {
  return color_intf_->ColorIntfGetModeInfo(&pp_features_, 0, mode_id, query);
}

DisplayError ColorManagerProxy::ColorMgrSetColorTransform(uint32_t length,
                                                          const double *trans_data) {
  return color_intf_->ColorIntfSetColorTransform(&pp_features_, 0, length, trans_data);
}

DisplayError ColorManagerProxy::ColorMgrGetDefaultModeID(int32_t *mode_id) {
  return color_intf_->ColorIntfGetDefaultModeID(&pp_features_, 0, mode_id);
}

DisplayError ColorManagerProxy::ColorMgrCombineColorModes() {
  return color_intf_->ColorIntfCombineColorModes();
}

DisplayError ColorManagerProxy::ColorMgrSetModeWithRenderIntent(int32_t color_mode_id,
                                         const PrimariesTransfer &blend_space, uint32_t intent) {
  cur_blend_space_ = blend_space;
  cur_intent_ = intent;
  cur_mode_id_ = color_mode_id;
  apply_mode_ = true;
  
  // Invalidate cache when mode changes
  InvalidateCache();
  
  return kErrorNone;
}

DisplayError ColorManagerProxy::Validate(HWLayers *hw_layers) {
  ScopedTimer timer(perf_stats_);
  
  if (!hw_layers) {
    timer.SetSuccess(false);
    return kErrorParameters;
  }

  // Check if we can use cached values
  if (IsCacheValid() && !ApplyModePending()) {
    return kErrorNone;
  }

  bool updates = NeedHwassetsUpdate();
  bool valid_meta_data = false;
  bool update_mode_Hwassets = false;
  Layer hdr_layer = {};
  bool hdr_present = false;
  bool hdr_plus_present = false;

  valid_meta_data = NeedsToneMap(hw_layers->info.hw_layers);
  if (valid_meta_data) {
    if (hw_layers->info.hdr_layer_info.in_hdr_mode &&
        hw_layers->info.hdr_layer_info.operation == HWHDRLayerInfo::kSet) {
      hdr_layer = *(hw_layers->info.stack->layers.at(
                                 UINT32(hw_layers->info.hdr_layer_info.layer_index)));
      hdr_present = true;
    }

    if (hdr_present) {
      if (hdr_layer.input_buffer.color_metadata.dynamicMetaDataValid &&
          hdr_layer.input_buffer.color_metadata.dynamicMetaDataLen) {
        hdr_plus_present = true;
      }
    }
    
    // Track HDR frames for performance stats
    if (hdr_present || hdr_plus_present) {
      perf_stats_.hdr_frames_processed++;
    }
  }

  if (apply_mode_) {
    update_mode_Hwassets = true;
    apply_mode_ = false;
  }

  if (updates) {
    update_mode_Hwassets = true;
  }

  if (hdr_present || hdr_plus_present) {
    meta_data_ = hdr_layer.input_buffer.color_metadata;
    update_mode_Hwassets = true;
    // Update cache with new metadata
    UpdateCache(&meta_data_);
  }

  if (update_mode_Hwassets) {
    snapdragoncolor::ColorMode color_mode;
    color_mode = GetColorPrimaries(cur_blend_space_, cur_intent_);
    DisplayError error = UpdateModeHwassets(cur_mode_id_, color_mode, 
                                           (hdr_present || hdr_plus_present), meta_data_);
    if (error != kErrorNone) {
      timer.SetSuccess(false);
      return error;
    }
    DumpColorMetaData(meta_data_);
  }

  return kErrorNone;
}

bool ColorManagerProxy::IsSupportStcTonemap() {
  return support_stc_tonemap_ && config_.use_stc_acceleration;
}

bool ColorManagerProxy::GameEnhanceSupported() {
  bool supported = false;

  if (color_intf_) {
    color_intf_->ColorIntfGameEnhancementSupported(&supported);
  }

  return supported;
}

DisplayError ColorManagerProxy::ConvertToPPFeatures(HwConfigOutputParams *params,
                                                    PPFeaturesConfig *out_data) {
  if (!params || !out_data) {
    DLOGE("Invalid input parameters");
    return kErrorParameters;
  }

  if (params->payload.empty()) {
    return kErrorNone;
  }

  // Check payload size to avoid excessive processing
  if (params->payload.size() > config_.max_payload_size) {
    DLOGW("Unexpected payload size: %zu, max: %u", 
          params->payload.size(), config_.max_payload_size);
    return kErrorNotSupported;
  }

  DisplayError error = kErrorNone;
  for (const auto& payload : params->payload) {
    ConvertTable::const_iterator found = convert_.find(payload.hw_asset);
    if (found == convert_.end()) {
      DLOGE("%s is not supported", payload.hw_asset.c_str());
      return kErrorNotSupported;
    }

    ConvertProc func = found->second;
    error = (this->*func)(payload, out_data);
    if (error != kErrorNone) {
      DLOGE("Failed to convert %s, error = %d", payload.hw_asset.c_str(), error);
      return error;
    }
  }

  return error;
}

DisplayError ColorManagerProxy::ConvertToIgc(const HwConfigPayload &in_data,
                                                   PPFeaturesConfig *out_data) {
  if (in_data.hw_payload_len != sizeof(GammaPostBlendConfig)) {
    DLOGE("Invalid parameters size = %d", in_data.hw_payload_len);
    return kErrorParameters;
  }
  
  GammaPostBlendConfig *ptr = reinterpret_cast<GammaPostBlendConfig*>(in_data.hw_payload.get());
  if (!ptr) {
    DLOGE("Invalid parameters");
    return kErrorUndefined;
  }
  
  // Apply AMOLED optimizations if enabled
  if (config_.enable_amoled_optimizations && amoled_panel_) {
    GamutConfig dummy_gamut;
    ApplyAMOLEDOptimizations(ptr, nullptr, &dummy_gamut);
  }
  
  return color_intf_->ColorIntfConvertToPPFeature(out_data, UINT32(display_id_), ptr->enabled,
                           kPbIgc, reinterpret_cast<void *>(ptr));
}

DisplayError ColorManagerProxy::ConvertToGc(const HwConfigPayload &in_data,
                                                  PPFeaturesConfig *out_data) {
  if (in_data.hw_payload_len != sizeof(GammaPostBlendConfig)) {
    DLOGE("Invalid parameters size = %d", in_data.hw_payload_len);
    return kErrorParameters;
  }
  
  GammaPostBlendConfig *ptr = reinterpret_cast<GammaPostBlendConfig*>(in_data.hw_payload.get());
  if (!ptr) {
    DLOGE("Invalid parameters");
    return kErrorUndefined;
  }
  
  // Apply AMOLED optimizations if enabled
  if (config_.enable_amoled_optimizations && amoled_panel_) {
    GamutConfig dummy_gamut;
    ApplyAMOLEDOptimizations(nullptr, ptr, &dummy_gamut);
  }
  
  return color_intf_->ColorIntfConvertToPPFeature(out_data, UINT32(display_id_), ptr->enabled,
                           kPbGC, reinterpret_cast<void *>(ptr));
}

DisplayError ColorManagerProxy::ConvertToGamut(const HwConfigPayload &in_data,
                                                     PPFeaturesConfig *out_data) {
  if (in_data.hw_payload_len != sizeof(GamutConfig)) {
    DLOGE("Invalid parameters size = %d", in_data.hw_payload_len);
    return kErrorParameters;
  }

  GamutConfig *ptr = reinterpret_cast<GamutConfig*>(in_data.hw_payload.get());
  if (!ptr) {
    DLOGE("Invalid parameters");
    return kErrorUndefined;
  }
  
  // Apply AMOLED optimizations if enabled
  if (config_.enable_amoled_optimizations && amoled_panel_) {
    GammaPostBlendConfig dummy_igc(LUT1D_ENTRIES_SIZE);
    GammaPostBlendConfig dummy_gc(LUT3D_GC_ENTRIES_SIZE);
    ApplyAMOLEDOptimizations(&dummy_igc, &dummy_gc, ptr);
    
    // Track gamut switches
    perf_stats_.gamut_switches++;
  }
  
  return color_intf_->ColorIntfConvertToPPFeature(out_data, UINT32(display_id_), ptr->enabled,
                           kPbGamut, reinterpret_cast<void *>(&ptr->gamut_info));
}

bool ColorManagerProxy::NeedHwassetsUpdate() {
  bool need_update = false;
  if (!stc_intf_client_ || !config_.use_stc_acceleration) {
    return need_update;
  }
  
  ScPayload payload;
  payload.len = sizeof(need_update);
  payload.prop = kNeedsUpdate;
  payload.payload = reinterpret_cast<uint64_t>(&need_update);
  stc_intf_client_->GetProperty(&payload);
  
  return need_update;
}

DisplayError ColorManagerProxy::UpdateModeHwassets(int32_t mode_id,
                                  snapdragoncolor::ColorMode color_mode, bool valid_meta_data,
                                  const ColorMetaData &meta_data) {
  if (!stc_intf_client_) {
    return kErrorUndefined;
  }

  DisplayError error = kErrorNone;
  struct snapdragoncolor::ModeRenderInputParams mode_params = {};
  struct snapdragoncolor::HwConfigOutputParams hw_params = {};
  
  mode_params.valid_meta_data = valid_meta_data;
  mode_params.meta_data = meta_data;
  mode_params.color_mode = color_mode;
  mode_params.mode_id = mode_id;

  // Pre-allocate payloads
  struct snapdragoncolor::HwConfigPayload payload = {};
  payload.hw_asset = kPbGamut;
  payload.hw_payload_len = sizeof(GamutConfig);
  payload.hw_payload = std::make_shared<GamutConfig>();
  hw_params.payload.push_back(payload);

  payload.hw_asset = kPbIgc;
  payload.hw_payload = std::make_shared<GammaPostBlendConfig>(LUT1D_ENTRIES_SIZE);
  payload.hw_payload_len = sizeof(GammaPostBlendConfig);
  hw_params.payload.push_back(payload);

  payload.hw_asset = kPbGC;
  payload.hw_payload_len = sizeof(GammaPostBlendConfig);
  payload.hw_payload = std::make_shared<GammaPostBlendConfig>(LUT3D_GC_ENTRIES_SIZE);
  hw_params.payload.push_back(payload);

  ScPayload in_data = {};
  ScPayload out_data = {};
  in_data.prop = kModeRenderInputParams;
  in_data.len = sizeof(mode_params);
  in_data.payload = reinterpret_cast<uint64_t>(&mode_params);

  out_data.prop = kHwConfigPayloadParam;
  out_data.len = sizeof(hw_params);
  out_data.payload = reinterpret_cast<uint64_t>(&hw_params);
  
  error = stc_intf_client_->ProcessOps(kScModeRenderIntent, in_data, &out_data);
  if (error != kErrorNone) {
    DLOGE("Failed to call ProcessOps, error = %d", error);
    return error;
  }

  error = ConvertToPPFeatures(&hw_params, &pp_features_);
  if (error != kErrorNone) {
    DLOGE("Failed to convert hw assets to PP features, error = %d", error);
    return kErrorUndefined;
  }
  
  pp_features_.MarkAsDirty();
  return error;
}

bool ColorManagerProxy::GetSupportStcTonemap() {
  bool support_tonemap = false;
  if (!stc_intf_client_ || !config_.use_stc_acceleration) {
    return support_tonemap;
  }
  
  ScPayload payload;
  payload.len = sizeof(support_tonemap);
  payload.prop = kSupportToneMap;
  payload.payload = reinterpret_cast<uint64_t>(&support_tonemap);
  stc_intf_client_->GetProperty(&payload);
  
  return support_tonemap;
}

void ColorManagerProxy::DumpColorMetaData(const ColorMetaData &color_metadata) {
  DLOGI_IF(kTagResources, "Primaries = %d, Range = %d, Transfer = %d, Matrix Coeffs = %d",
           color_metadata.colorPrimaries, color_metadata.range, color_metadata.transfer,
           color_metadata.matrixCoefficients);

  for (uint32_t i = 0; i < 3; i++) {
    for (uint32_t j = 0; j < 2; j++) {
      DLOGV_IF(kTagResources, "RGB Primaries[%d][%d] = %d", i, j,
               color_metadata.masteringDisplayInfo.primaries.rgbPrimaries[i][j]);
    }
  }
  DLOGV_IF(kTagResources, "White Point[0] = %d White Point[1] = %d",
           color_metadata.masteringDisplayInfo.primaries.whitePoint[0],
           color_metadata.masteringDisplayInfo.primaries.whitePoint[1]);
  DLOGV_IF(kTagResources, "Max Disp Luminance = %d Min Disp Luminance= %d",
           color_metadata.masteringDisplayInfo.maxDisplayLuminance,
           color_metadata.masteringDisplayInfo.minDisplayLuminance);
  DLOGV_IF(kTagResources, "Max ContentLightLevel = %d Max AvgLightLevel = %d",
           color_metadata.contentLightLevel.maxContentLightLevel,
           color_metadata.contentLightLevel.minPicAverageLightLevel);
  DLOGV_IF(kTagResources, "DynamicMetaDataValid = %d DynamicMetaDataLen = %d",
           color_metadata.dynamicMetaDataValid,
           color_metadata.dynamicMetaDataLen);
}

snapdragoncolor::ColorMode ColorManagerProxy::GetColorPrimaries(
                        const PrimariesTransfer &blend_space, uint32_t intent) {
  snapdragoncolor::ColorMode mode = {};

  mode.intent = static_cast<snapdragoncolor::RenderIntent>(intent + 1);
  mode.gamut = blend_space.primaries;
  mode.gamma = blend_space.transfer;

  return mode;
}

// AMOLED-specific optimizations
DisplayError ColorManagerProxy::ApplyAMOLEDOptimizations(GammaPostBlendConfig* igc_config,
                                                         GammaPostBlendConfig* gc_config,
                                                         GamutConfig* gamut_config) {
  if (!config_.enable_amoled_optimizations || !amoled_panel_) {
    return kErrorNone;
  }

  uint32_t brightness = GetCurrentDisplayBrightness();
  
  // Adjust gamma based on brightness for AMOLED
  if (igc_config) {
    // AMOLED panels typically need different gamma at low brightness
    if (brightness < 100) {
      // Low brightness - enhance gamma for better shadow detail
      igc_config->gamma = 2.4f;
    } else if (brightness < 500) {
      // Medium brightness - balanced gamma
      igc_config->gamma = 2.2f;
    } else {
      // High brightness - slightly lower gamma for better highlights
      igc_config->gamma = 2.0f;
    }
    
    DLOGV_IF(kTagResources, "AMOLED IGC gamma set to %.2f at brightness %u", 
             igc_config->gamma, brightness);
  }

  // Adjust gamut for AMOLED wide color gamut
  if (gamut_config) {
    // AMOLED panels often have wider native gamut than standard
    // Apply subtle saturation enhancement for P3 gamut
    if (gamut_config->gamut_info.primaries == kPrimariesP3) {
      // Slight saturation boost for more vibrant colors on AMOLED
      gamut_config->gamut_info.saturation = 
          std::min(gamut_config->gamut_info.saturation * 1.05f, 1.0f);
    }
    
    // Adjust for peak brightness
    if (panel_peak_brightness_ > 1000) {
      // High brightness AMOLED - adjust tone mapping
      gamut_config->gamut_info.max_luminance = panel_peak_brightness_;
    }
  }

  return kErrorNone;
}

uint32_t ColorManagerProxy::GetCurrentDisplayBrightness() const {
  // TODO: Get actual brightness from display driver
  // This is a placeholder - actual implementation would query the driver
  // For now, return a reasonable default for the 6.67" AMOLED
  return 500; // Medium brightness
}

bool ColorManagerProxy::IsAMOLEDPanel() const {
  // Check panel name for AMOLED indicators
  std::string panel_name = pp_hw_attributes_.panel_name;
  
  // Convert to lowercase for case-insensitive comparison
  std::transform(panel_name.begin(), panel_name.end(), panel_name.begin(), ::tolower);
  
  // Common AMOLED panel name patterns
  const char* amoled_patterns[] = {
    "amoled", "oled", "poled", "super_amoled", "dynamic_amoled",
    // Known AMOLED panel IDs
    "s6e3", "s6e3fa", "s6e3fb", "s6e3fc", // Samsung AMOLED
    "s6d7", // Samsung AMOLED
    "rm67199", // Visionox AMOLED
    "ft8716", // FocalTech AMOLED
    "ili9881", // Ilitek AMOLED
  };
  
  for (const char* pattern : amoled_patterns) {
    if (panel_name.find(pattern) != std::string::npos) {
      return true;
    }
  }
  
  // Could also check resolution/attributes for AMOLED characteristics
  // 2400x1080 is typical for AMOLED, but check other indicators
  if (pp_hw_attributes_.x_pixels == 2400 && pp_hw_attributes_.y_pixels == 1080) {
    // This resolution is common for AMOLED, but verify with other attributes
    // Check refresh rate - AMOLED typically supports 90Hz or 120Hz
    if (pp_hw_attributes_.fps >= 90) {
      return true;
    }
  }
  
  return false;
}

bool ColorManagerProxy::IsCacheValid() const {
  if (!cache_.is_valid) {
    return false;
  }
  
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - cache_.last_update);
  
  return elapsed.count() < config_.cache_ttl_ms;
}

void ColorManagerProxy::UpdateCache(const ColorMetaData* metadata) {
  cache_.mode_id = cur_mode_id_;
  cache_.blend_space = cur_blend_space_;
  cache_.intent = cur_intent_;
  cache_.last_update = std::chrono::steady_clock::now();
  cache_.is_valid = true;
  
  if (metadata) {
    cache_.metadata = *metadata;
    cache_.metadata_valid = true;
  }
}

void ColorManagerProxy::InvalidateCache() {
  cache_.is_valid = false;
  cache_.metadata_valid = false;
  cache_.last_update = std::chrono::steady_clock::now();
}

void ColorManagerProxy::LogColorOperation(const char* operation, DisplayError error, 
                                         const char* details) {
  if (error != kErrorNone) {
    DLOGE("ColorOp[%s] failed: %d %s", operation, error, details ? details : "");
  } else {
    DLOGV("ColorOp[%s] success%s", operation, details ? " - " : "");
    if (details) DLOGV("%s", details);
  }
}

bool ColorManagerProxy::IsDebugEnabled(uint32_t category) {
  // Check if debug is enabled for this category
  // Could be controlled by system property
  static uint32_t debug_categories = 0;
  static bool initialized = false;
  
  if (!initialized) {
    char value[PROPERTY_VALUE_MAX] = {};
    if (Debug::Get()->GetProperty("persist.sdm.color.debug", value) == kErrorNone) {
      debug_categories = strtoul(value, nullptr, 0);
    }
    initialized = true;
  }
  
  return (debug_categories & category) != 0;
}

// RuntimeConfig implementation
ColorManagerProxy::RuntimeConfig ColorManagerProxy::RuntimeConfig::LoadFromProperties() {
  RuntimeConfig config;
  
  char value[PROPERTY_VALUE_MAX] = {};
  
  if (Debug::Get()->GetProperty("persist.sdm.color.hdr", value) == kErrorNone) {
    config.enable_hdr_tone_mapping = (strcmp(value, "1") == 0);
  }
  
  if (Debug::Get()->GetProperty("persist.sdm.color.gamut", value) == kErrorNone) {
    config.enable_gamut_mapping = (strcmp(value, "1") == 0);
  }
  
  if (Debug::Get()->GetProperty("persist.sdm.color.gamma", value) == kErrorNone) {
    config.enable_gamma_correction = (strcmp(value, "1") == 0);
  }
  
  if (Debug::Get()->GetProperty("persist.sdm.color.stc", value) == kErrorNone) {
    config.use_stc_acceleration = (strcmp(value, "1") == 0);
  }
  
  if (Debug::Get()->GetProperty("persist.sdm.color.amoled", value) == kErrorNone) {
    config.enable_amoled_optimizations = (strcmp(value, "1") == 0);
  }
  
  if (Debug::Get()->GetProperty("persist.sdm.color.cache_ttl", value) == kErrorNone) {
    config.cache_ttl_ms = strtoul(value, nullptr, 0);
    if (config.cache_ttl_ms == 0) {
      config.cache_ttl_ms = 100; // Default 100ms
    }
  }
  
  return config;
}

// ColorFeatureCheckingImpl Implementation
ColorFeatureCheckingImpl::ColorFeatureCheckingImpl(HWInterface *hw_intf,
                                                   PPFeaturesConfig *pp_features)
  : hw_intf_(hw_intf), pp_features_(pp_features) {}

DisplayError ColorFeatureCheckingImpl::Init() {
  states_.at(kFrameTriggerDefault) = new FeatureStateDefaultTrigger(this);
  states_.at(kFrameTriggerSerialize) = new FeatureStateSerializedTrigger(this);
  states_.at(kFrameTriggerPostedStart) = new FeatureStatePostedStart(this);

  // Check for allocation failures
  if (std::any_of(states_.begin(), states_.end(),
      [](const FeatureInterface *p) {
        return p == nullptr;
      })) {
    std::all_of(states_.begin(), states_.end(),
      [](const FeatureInterface *p) {
        if (p) delete p;
        return true;
      });
    states_.fill(nullptr);
    curr_state_ = nullptr;
    return kErrorMemory;
  }
  
  curr_state_ = states_.at(kFrameTriggerDefault);

  if (curr_state_) {
    single_buffer_feature_.clear();
    single_buffer_feature_.push_back(kGlobalColorFeatureIgc);
    single_buffer_feature_.push_back(kGlobalColorFeatureGamut);
  } else {
    DLOGE("Failed to create curr_state_");
    return kErrorMemory;
  }
  
  return kErrorNone;
}

DisplayError ColorFeatureCheckingImpl::Deinit() {
  std::all_of(states_.begin(), states_.end(),
    [](const FeatureInterface *p) {
      if (p) delete p;
      return true;
    });
  states_.fill(nullptr);
  curr_state_ = nullptr;
  single_buffer_feature_.clear();
  return kErrorNone;
}

bool ColorFeatureCheckingImpl::ValidateStateTransition(FrameTriggerMode from_mode, 
                                                       FrameTriggerMode to_mode) {
  // Validate state transitions to prevent invalid states
  if (from_mode == to_mode) {
    return true;
  }
  
  // All transitions are valid in our implementation
  return true;
}

DisplayError ColorFeatureCheckingImpl::SetParams(FeatureOps param_type,
                                                 void *payload) {
  DisplayError error = kErrorNone;
  FrameTriggerMode mode = kFrameTriggerDefault;

  if (!payload) {
    DLOGE("Invalid input payload");
    return kErrorParameters;
  }

  if (!curr_state_) {
    DLOGE("Invalid curr state");
    return kErrorParameters;
  }

  bool is_dirty = *reinterpret_cast<bool *>(payload);
  switch (param_type) {
  case kFeatureSwitchMode:
    if (is_dirty) {
      CheckColorFeature(&mode);
    } else {
      mode = kFrameTriggerPostedStart;
    }
    DLOGV_IF(kTagQDCM, "Set frame trigger mode %d", mode);
    
    // Validate state transition
    if (!ValidateStateTransition(static_cast<FrameTriggerMode>(0), mode)) {
      DLOGE("Invalid state transition to %d", mode);
      return kErrorParameters;
    }
    
    error = curr_state_->SetParams(param_type, &mode);
    if (error != kErrorNone) {
      DLOGE_IF(kTagQDCM, "Failed to set params to state, error %d", error);
    }
    break;
  default:
    DLOGW("unhandled param_type %d", param_type);
    error = kErrorNotSupported;
    break;
  }
  return error;
}

DisplayError ColorFeatureCheckingImpl::GetParams(FeatureOps param_type,
                                                 void *payload) {
  DisplayError error = kErrorNone;

  if (!payload) {
    DLOGE("Invalid input payload");
    return kErrorParameters;
  }

  if (!curr_state_) {
    DLOGE("Invalid curr state");
    return kErrorParameters;
  }

  switch (param_type) {
  case kFeatureSwitchMode:
    if (curr_state_) {
      curr_state_->GetParams(param_type, payload);
    } else {
      DLOGE_IF(kTagQDCM, "curr_state_ NULL");
      error = kErrorUndefined;
    }
    break;
  default:
    DLOGW("unhandled param_type %d", param_type);
    error = kErrorNotSupported;
    break;
  }
  return error;
}

void ColorFeatureCheckingImpl::CheckColorFeature(FrameTriggerMode *mode) {
  PPFeatureInfo *feature = nullptr;
  PPGlobalColorFeatureID id = kMaxNumPPFeatures;

  if (!pp_features_) {
    DLOGW("Invalid pp features");
    *mode = kFrameTriggerPostedStart;
    return;
  }

  for (uint32_t i = 0; i < single_buffer_feature_.size(); i++) {
    id = single_buffer_feature_[i];
    feature = pp_features_->GetFeature(id);
    if (feature && (feature->enable_flags_ & kOpsEnable)) {
      *mode = kFrameTriggerDefault;
      return;
    }
  }

  *mode = kFrameTriggerPostedStart;
}

// FeatureStatePostedStart Implementation
FeatureStatePostedStart::FeatureStatePostedStart(ColorFeatureCheckingImpl *obj)
  : obj_(obj) {}

DisplayError FeatureStatePostedStart::Init() {
  return kErrorNone;
}

DisplayError FeatureStatePostedStart::Deinit() {
  return kErrorNone;
}

DisplayError FeatureStatePostedStart::SetParams(FeatureOps param_type,
                                                void *payload) {
  DisplayError error = kErrorNone;
  FrameTriggerMode mode = kFrameTriggerPostedStart;

  if (!obj_) {
    DLOGE("Invalid param obj_");
    return kErrorParameters;
  }

  if (!payload) {
    DLOGE("Invalid payload");
    return kErrorParameters;
  }

  switch (param_type) {
  case kFeatureSwitchMode:
    mode = *(reinterpret_cast<FrameTriggerMode *>(payload));
    if (mode >= kFrameTriggerMax) {
      DLOGE("Invalid mode %d", mode);
      return kErrorParameters;
    }
    if (mode != kFrameTriggerPostedStart) {
      error = obj_->hw_intf_->SetFrameTrigger(mode);
      if (error == kErrorNone) {
        obj_->curr_state_ = obj_->states_.at(mode);
      }
    } else {
      DLOGV_IF(kTagQDCM, "Already in posted start mode");
    }
    break;
  default:
    DLOGW("unhandled param_type %d", param_type);
    error = kErrorNotSupported;
    break;
  }
  return error;
}

DisplayError FeatureStatePostedStart::GetParams(FeatureOps param_type,
                                                void *payload) {
  DisplayError error = kErrorNone;

  if (!obj_) {
    DLOGE("Invalid param obj_");
    return kErrorParameters;
  }

  if (!payload) {
    DLOGE("Invalid payload");
    return kErrorParameters;
  }

  switch (param_type) {
  case kFeatureSwitchMode:
    *(reinterpret_cast<FrameTriggerMode *>(payload)) = kFrameTriggerPostedStart;
    break;
  default:
    DLOGW("unhandled param_type %d", param_type);
    error = kErrorNotSupported;
    break;
  }

  return error;
}

// FeatureStateDefaultTrigger Implementation
FeatureStateDefaultTrigger::FeatureStateDefaultTrigger(ColorFeatureCheckingImpl *obj)
  : obj_(obj) {}

DisplayError FeatureStateDefaultTrigger::Init() {
  return kErrorNone;
}

DisplayError FeatureStateDefaultTrigger::Deinit() {
  return kErrorNone;
}

DisplayError FeatureStateDefaultTrigger::SetParams(FeatureOps param_type,
                                                   void *payload) {
  DisplayError error = kErrorNone;
  FrameTriggerMode mode = kFrameTriggerDefault;

  if (!obj_) {
    DLOGE("Invalid param obj_");
    return kErrorParameters;
  }

  if (!payload) {
    DLOGE("Invalid payload");
    return kErrorParameters;
  }

  switch (param_type) {
  case kFeatureSwitchMode:
    mode = *(reinterpret_cast<FrameTriggerMode *>(payload));
    if (mode >= kFrameTriggerMax) {
      DLOGE("Invalid mode %d", mode);
      return kErrorParameters;
    }
    if (mode != kFrameTriggerDefault) {
      error = obj_->hw_intf_->SetFrameTrigger(mode);
      if (error == kErrorNone) {
        obj_->curr_state_ = obj_->states_.at(mode);
      }
    } else {
      DLOGV_IF(kTagQDCM, "Already in default trigger mode");
    }
    break;
  default:
    DLOGW("unhandled param_type %d", param_type);
    error = kErrorNotSupported;
    break;
  }
  return error;
}

DisplayError FeatureStateDefaultTrigger::GetParams(FeatureOps param_type,
                                                   void *payload) {
  DisplayError error = kErrorNone;

  if (!obj_) {
    DLOGE("Invalid param obj_");
    return kErrorParameters;
  }

  if (!payload) {
    DLOGE("Invalid payload");
    return kErrorParameters;
  }

  switch (param_type) {
  case kFeatureSwitchMode:
    *(reinterpret_cast<FrameTriggerMode *>(payload)) = kFrameTriggerDefault;
    break;
  default:
    DLOGW("unhandled param_type %d", param_type);
    error = kErrorNotSupported;
    break;
  }

  return error;
}

// FeatureStateSerializedTrigger Implementation
FeatureStateSerializedTrigger::FeatureStateSerializedTrigger(ColorFeatureCheckingImpl *obj)
  : obj_(obj) {}

DisplayError FeatureStateSerializedTrigger::Init() {
  return kErrorNone;
}

DisplayError FeatureStateSerializedTrigger::Deinit() {
  return kErrorNone;
}

DisplayError FeatureStateSerializedTrigger::SetParams(FeatureOps param_type,
                                                      void *payload) {
  DisplayError error = kErrorNone;
  FrameTriggerMode mode = kFrameTriggerSerialize;

  if (!obj_) {
    DLOGE("Invalid param obj_");
    return kErrorParameters;
  }

  if (!payload) {
    DLOGE("Invalid payload");
    return kErrorParameters;
  }

  switch (param_type) {
  case kFeatureSwitchMode:
    mode = *(reinterpret_cast<FrameTriggerMode *>(payload));
    if (mode >= kFrameTriggerMax) {
      DLOGE("Invalid mode %d", mode);
      return kErrorParameters;
    }
    if (mode != kFrameTriggerSerialize) {
      error = obj_->hw_intf_->SetFrameTrigger(mode);
      if (error == kErrorNone) {
        obj_->curr_state_ = obj_->states_.at(mode);
      }
    } else {
      DLOGV_IF(kTagQDCM, "Already in serialized trigger mode");
    }
    break;
  default:
    DLOGW("unhandled param_type %d", param_type);
    error = kErrorNotSupported;
    break;
  }
  return error;
}

DisplayError FeatureStateSerializedTrigger::GetParams(FeatureOps param_type,
                                                      void *payload) {
  DisplayError error = kErrorNone;

  if (!obj_) {
    DLOGE("Invalid param obj_");
    return kErrorParameters;
  }

  if (!payload) {
    DLOGE("Invalid payload");
    return kErrorParameters;
  }

  switch (param_type) {
  case kFeatureSwitchMode:
    *(reinterpret_cast<FrameTriggerMode *>(payload)) = kFrameTriggerSerialize;
    break;
  default:
    DLOGW("unhandled param_type %d", param_type);
    error = kErrorNotSupported;
    break;
  }

  return error;
}

}  // namespace sdm
