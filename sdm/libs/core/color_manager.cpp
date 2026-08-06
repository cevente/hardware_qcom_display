/* Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
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

#ifndef __COLOR_MANAGER_H__
#define __COLOR_MANAGER_H__

#include <stdlib.h>
#include <core/sdm_types.h>
#include <utils/locker.h>
#include <private/color_interface.h>
#include <private/snapdragon_color_intf.h>
#include <utils/sys.h>
#include <utils/debug.h>
#include <array>
#include <vector>
#include <map>
#include <string>
#include <mutex>
#include <atomic>
#include <cstring>
#include <algorithm>

#include "hw_interface.h"

namespace sdm {

using std::mutex;
using std::lock_guard;
using snapdragoncolor::HwConfigOutputParams;
using snapdragoncolor::HwConfigPayload;
using snapdragoncolor::ScPayload;
using snapdragoncolor::ScOps;
using snapdragoncolor::ScPostBlendInterface;
using snapdragoncolor::kScModeRenderIntent;
using snapdragoncolor::PostBlendInverseGammaHwConfig;
using snapdragoncolor::PostBlendGammaHwConfig;
using snapdragoncolor::PostBlendGamutHwConfig;
using snapdragoncolor::kPostBlendInverseGammaHwConfig;
using snapdragoncolor::kPostBlendGammaHwConfig;
using snapdragoncolor::kPostBlendGamutHwConfig;
using snapdragoncolor::kHwConfigPayloadParam;
using snapdragoncolor::GamutConfig;
using snapdragoncolor::GammaPostBlendConfig;
using snapdragoncolor::kPbIgc;
using snapdragoncolor::kPbGamut;
using snapdragoncolor::kPbGC;
using snapdragoncolor::kModeRenderInputParams;
using snapdragoncolor::kNeedsUpdate;
using snapdragoncolor::kSupportToneMap;

enum FeatureOps {
  kFeatureSwitchMode,
  kFeatureOpsMax,
};

class FeatureInterface {
 public:
  virtual ~FeatureInterface() {}
  virtual DisplayError Init() = 0;
  virtual DisplayError Deinit() = 0;
  virtual DisplayError SetParams(FeatureOps param_type, void *payload) = 0;
  virtual DisplayError GetParams(FeatureOps param_type, void *payload) = 0;
};

FeatureInterface* GetPostedStartFeatureCheckIntf(HWInterface *intf,
                                                 PPFeaturesConfig *config);

class STCIntfClient {
 public:
  STCIntfClient();
  ~STCIntfClient();
  
  DisplayError Init(const std::string &panel_name);
  DisplayError DeInit();

  // Property functions
  DisplayError SetProperty(const ScPayload &payload);
  DisplayError GetProperty(ScPayload *payload);

  // ProcessOps functions
  DisplayError ProcessOps(const ScOps op, const ScPayload &input, ScPayload *output);

 private:
  static constexpr const char* kStcIntfLib_ = "libsdm-color.so";  // FIXED: Use actual library
  
  DisplayError HandleFallbackOperations(const ScOps op, const ScPayload &input, ScPayload *output);
  DisplayError HandleFallbackRenderIntent(const ScPayload &input, ScPayload *output);
  
  DynLib stc_intf_lib_;
  ScPostBlendInterface *stc_intf_;
  ScPostBlendInterface* (*GetScPostBlendInterface)(uint32_t major_version, uint32_t minor_version);
  mutable mutex lock_;
  bool initialized_;
};

/*
 * ColorManager proxy to maintain necessary information to interact with underlying color service.
 * Each display object has its own proxy.
 */
class ColorManagerProxy {
 public:
  static DisplayError Init(const HWResourceInfo &hw_res_info);
  static void Deinit();

  /* Create ColorManagerProxy for this display object, following things need to be happening
   * 1. Instantiates concrete ColorInterface implementation.
   * 2. Pass all display object specific informations into it.
   * 3. Populate necessary resources.
   * 4. Need get panel name for hw_panel_info_.
   */
  static ColorManagerProxy *CreateColorManagerProxy(DisplayType type, HWInterface *hw_intf,
                                                    const HWDisplayAttributes &attribute,
                                                    const HWPanelInfo &panel_info,
                                                    DppsControlInterface *dpps_intf);

  /* need reverse the effect of CreateColorManagerProxy. */
  ~ColorManagerProxy();

  DisplayError ColorSVCRequestRoute(const PPDisplayAPIPayload &in_payload,
                                    PPDisplayAPIPayload *out_payload,
                                    PPPendingParams *pending_action);
  DisplayError ApplyDefaultDisplayMode();
  DisplayError ColorMgrGetNumOfModes(uint32_t *mode_cnt);
  DisplayError ColorMgrGetModes(uint32_t *mode_cnt, SDEDisplayMode *modes);
  DisplayError ColorMgrSetMode(int32_t color_mode_id);
  DisplayError ColorMgrGetModeInfo(int32_t mode_id, AttrVal *query);
  DisplayError ColorMgrSetColorTransform(uint32_t length, const double *trans_data);
  DisplayError ColorMgrGetDefaultModeID(int32_t *mode_id);
  DisplayError ColorMgrCombineColorModes();
  bool NeedsPartialUpdateDisable();
  DisplayError Commit();
  DisplayError ColorMgrSetModeWithRenderIntent(int32_t color_mode_id,
                                               const PrimariesTransfer &blend_space,
                                               uint32_t intent);
  DisplayError Validate(HWLayers *hw_layers);
  bool IsSupportStcTonemap();
  bool GameEnhanceSupported();

 protected:
  ColorManagerProxy() {}
  ColorManagerProxy(int32_t id, DisplayType type, HWInterface *intf,
                    const HWDisplayAttributes &attr, const HWPanelInfo &info);

 private:
  static DynLib color_lib_;
  static CreateColorInterface create_intf_;
  static DestroyColorInterface destroy_intf_;
  static HWResourceInfo hw_res_info_;

  typedef DisplayError (ColorManagerProxy::*ConvertProc)(const HwConfigPayload &in_data,
                                        PPFeaturesConfig *out_data);
  typedef std::map<std::string, ConvertProc> ConvertTable;

  // Simple performance tracking
  struct PerformanceStats {
    std::atomic<uint32_t> total_calls{0};
    std::atomic<uint32_t> failed_calls{0};
    
    void RecordCall(bool success) {
      total_calls++;
      if (!success) failed_calls++;
    }
    
    void LogStats() const {
      uint32_t calls = total_calls.load();
      if (calls == 0) return;
      DLOGI("ColorMgr Stats: calls=%u, failed=%u", calls, failed_calls.load());
    }
  };

  // Runtime configuration with safe defaults
  struct RuntimeConfig {
    bool enable_hdr_tone_mapping = true;
    bool enable_gamut_mapping = true;
    bool enable_gamma_correction = true;
    bool use_stc_acceleration = true;
    bool enable_amoled_optimizations = true;
    uint32_t max_payload_size = 3;
    
    static RuntimeConfig LoadFromProperties();
  };

  bool NeedHwassetsUpdate();
  DisplayError UpdateModeHwassets(int32_t mode_id, snapdragoncolor::ColorMode color_mode,
                                  bool valid_meta_data, const ColorMetaData &meta_data);
  DisplayError ConvertToPPFeatures(HwConfigOutputParams *params, PPFeaturesConfig *out_data);
  DisplayError ConvertToIgc(const HwConfigPayload &in_data, PPFeaturesConfig *out_data);
  DisplayError ConvertToGc(const HwConfigPayload &in_data, PPFeaturesConfig *out_data);
  DisplayError ConvertToGamut(const HwConfigPayload &in_data, PPFeaturesConfig *out_data);
  void DumpColorMetaData(const ColorMetaData &color_metadata);
  snapdragoncolor::ColorMode GetColorPrimaries(const PrimariesTransfer &blend_space,
                                               uint32_t intent);
  bool GetSupportStcTonemap();
  
  // AMOLED-specific optimizations
  DisplayError ApplyAMOLEDOptimizations(GammaPostBlendConfig* igc_config, 
                                        GammaPostBlendConfig* gc_config,
                                        GamutConfig* gamut_config);
  uint32_t GetDisplayBrightnessNits() const;
  DisplayError UpdateBrightnessFromDriver();
  bool IsAMOLEDPanel() const;
  
  // Cache management (simplified, no raw pointer caching)
  struct ColorCache {
    int32_t mode_id = -1;
    ColorMetaData metadata = {};
    bool metadata_valid = false;
  };
  
  void UpdateCache(const ColorMetaData* metadata);
  
  // Helper functions
  static void LogColorOperation(const char* operation, DisplayError error);
  
  ConvertTable convert_;
  RuntimeConfig config_;
  PerformanceStats perf_stats_;
  ColorCache cache_;

  int32_t display_id_;
  DisplayType device_type_;
  PPHWAttributes pp_hw_attributes_;
  HWInterface *hw_intf_;
  ColorInterface *color_intf_;
  PPFeaturesConfig pp_features_;
  FeatureInterface *feature_intf_;
  bool apply_mode_ = false;
  PrimariesTransfer cur_blend_space_ = {};
  uint32_t cur_intent_ = 0;
  int32_t cur_mode_id_ = -1;
  ColorMetaData meta_data_ = {};
  STCIntfClient *stc_intf_client_ = nullptr;
  bool support_stc_tonemap_ = false;
  bool amoled_panel_ = false;
  
  // Real brightness values from driver
  std::atomic<uint32_t> brightness_nits_{500};
  uint32_t panel_peak_brightness_ = 0;
};

// Add MarkAsClean to PPFeaturesConfig
class PPFeaturesConfig {
 public:
  void Reset();
  DisplayError RetrieveNextFeature(PPFeatureInfo **feature);
  void MarkAsDirty() { dirty_ = true; }
  void MarkAsClean() { dirty_ = false; }
  bool IsDirty() const { return dirty_; }
  PPFeatureInfo* GetFeature(PPGlobalColorFeatureID id);
  Locker& GetLocker() { return locker_; }
  
 private:
  PPFeatureInfo *feature_[kMaxNumPPFeatures];
  bool dirty_;
  uint32_t next_idx_;
  Locker locker_;
};

class ColorFeatureCheckingImpl : public FeatureInterface {
 public:
  explicit ColorFeatureCheckingImpl(HWInterface *hw_intf, PPFeaturesConfig *pp_features);
  virtual ~ColorFeatureCheckingImpl() { }

  DisplayError Init() override;
  DisplayError Deinit() override;
  DisplayError SetParams(FeatureOps param_type, void *payload) override;
  DisplayError GetParams(FeatureOps param_type, void *payload) override;

 private:
  friend class FeatureStatePostedStart;
  friend class FeatureStateDefaultTrigger;
  friend class FeatureStateSerializedTrigger;

  HWInterface *hw_intf_;
  PPFeaturesConfig *pp_features_;
  std::array<FeatureInterface*, kFrameTriggerMax> states_ = {{nullptr}};
  FeatureInterface *curr_state_ = nullptr;
  std::vector<PPGlobalColorFeatureID> single_buffer_feature_;
  void CheckColorFeature(FrameTriggerMode *mode);
};

class FeatureStatePostedStart : public FeatureInterface {
 public:
  explicit FeatureStatePostedStart(ColorFeatureCheckingImpl *obj);
  virtual ~FeatureStatePostedStart() {}

  DisplayError Init() override;
  DisplayError Deinit() override;
  DisplayError SetParams(FeatureOps param_type, void *payload) override;
  DisplayError GetParams(FeatureOps param_type, void *payload) override;

 private:
  ColorFeatureCheckingImpl *obj_;
};

class FeatureStateDefaultTrigger : public FeatureInterface {
 public:
  explicit FeatureStateDefaultTrigger(ColorFeatureCheckingImpl *obj);
  virtual ~FeatureStateDefaultTrigger() {}

  DisplayError Init() override;
  DisplayError Deinit() override;
  DisplayError SetParams(FeatureOps param_type, void *payload) override;
  DisplayError GetParams(FeatureOps param_type, void *payload) override;

 private:
  ColorFeatureCheckingImpl *obj_;
};

class FeatureStateSerializedTrigger : public FeatureInterface {
 public:
  explicit FeatureStateSerializedTrigger(ColorFeatureCheckingImpl *obj);
  virtual ~FeatureStateSerializedTrigger() {}

  DisplayError Init() override;
  DisplayError Deinit() override;
  DisplayError SetParams(FeatureOps param_type, void *payload) override;
  DisplayError GetParams(FeatureOps param_type, void *payload) override;

 private:
  ColorFeatureCheckingImpl *obj_;
};

}  // namespace sdm

#endif  // __COLOR_MANAGER_H__
