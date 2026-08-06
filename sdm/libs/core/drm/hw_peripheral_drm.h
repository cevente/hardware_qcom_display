/*
Copyright (c) 2017-2024 The Linux Foundation. All rights reserved.
Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
*/

#ifndef __HW_PERIPHERAL_DRM_H__
#define __HW_PERIPHERAL_DRM_H__

#include <vector>
#include <string>
#include <sys/time.h>
#include "hw_device_drm.h"

#ifndef HDR_EOTF_SMTPE_ST2084
#define HDR_EOTF_SMTPE_ST2084 2
#endif
#ifndef HDR_EOTF_HLG
#define HDR_EOTF_HLG 3
#endif

#define HDR_DISABLE 0
#define HDR_ENABLE 1
#define MIN_HDR_RESET_WAITTIME 2

namespace sdm {

struct CWBConfig {
  bool enabled = false;
  sde_drm::DRMDisplayToken token = {};
};

class HWPeripheralDRM : public HWDeviceDRM, public PanelFeaturePropertyIntf {
 public:
  explicit HWPeripheralDRM(int32_t display_id, BufferAllocator *buffer_allocator,
                           HWInfoInterface *hw_info_intf);
  virtual ~HWPeripheralDRM() {}
  virtual PanelFeaturePropertyIntf *GetPanelFeaturePropertyIntf() { return this; }
  virtual int GetPanelFeature(PanelFeaturePropertyInfo *feature_info);
  virtual int SetPanelFeature(const PanelFeaturePropertyInfo &feature_info);
  
  // Public methods for AMOLED panel management
  bool IsHDRActive() const { return hdr_active_; }
  uint32_t GetCurrentRefreshRate() const { return current_refresh_rate_; }

 protected:
  virtual DisplayError Init();
  virtual DisplayError Validate(HWLayers *hw_layers);
  virtual DisplayError Commit(HWLayers *hw_layers);
  virtual DisplayError Flush(HWLayers *hw_layers);
  virtual DisplayError SetDppsFeature(void *payload, size_t size);
  virtual DisplayError GetDppsFeatureInfo(void *payload, size_t size);
  virtual DisplayError HandleSecureEvent(SecureEvent secure_event, HWLayers *hw_layers);
  virtual DisplayError ControlIdlePowerCollapse(bool enable, bool synchronous);
  virtual DisplayError PowerOn(const HWQosData &qos_data, shared_ptr<Fence> *release_fence);
  virtual DisplayError PowerOff(bool teardown);
  virtual DisplayError Doze(const HWQosData &qos_data, shared_ptr<Fence> *release_fence);
  virtual DisplayError DozeSuspend(const HWQosData &qos_data, shared_ptr<Fence> *release_fence);
  virtual DisplayError SetDisplayDppsAdROI(void *payload);
  virtual DisplayError SetDynamicDSIClock(uint64_t bit_clk_rate);
  virtual DisplayError GetDynamicDSIClock(uint64_t *bit_clk_rate);
  virtual DisplayError SetDisplayAttributes(uint32_t index);
  virtual DisplayError SetDisplayMode(const HWDisplayMode hw_display_mode);
  virtual DisplayError SetRefreshRate(uint32_t refresh_rate);
  virtual DisplayError TeardownConcurrentWriteback(void);
  virtual DisplayError SetFrameTrigger(FrameTriggerMode mode);
  virtual DisplayError SetPanelBrightness(int level);
  virtual DisplayError GetPanelBrightness(int *level);
  virtual void GetHWPanelMaxBrightness();
  virtual DisplayError SetBLScale(uint32_t level);
  virtual DisplayError GetPanelBrightnessBasePath(std::string *base_path);
  virtual DisplayError DelayFirstCommit();
  virtual DisplayError SetBlendSpace(const PrimariesTransfer &blend_space);

 private:
  void InitDestScaler();
  void SetDestScalarData(const HWLayersInfo &hw_layer_info);
  void ResetDestScalarCache();
  DisplayError SetupConcurrentWritebackModes();
  bool SetupConcurrentWriteback(const HWLayersInfo &hw_layer_info, bool validate,
                                int64_t *release_fence_fd);
  void ConfigureConcurrentWriteback(LayerStack *stack);
  void PostCommitConcurrentWriteback(LayerBuffer *output_buffer);
  void CreatePanelFeaturePropertyMap();
  void SetIdlePCState() {
    drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_IDLE_PC_STATE, token_.crtc_id,
                              idle_pc_state_);
  }
  void CacheDestScalarData();
  void PopulateBitClkRates();
  
  // HDR Related methods
  DisplayError UpdateHDRMetaData(HWLayers *hw_layers);
  void DumpHDRMetaData(HWHDRLayerInfo::HDROperation operation);
  void InitMaxHDRMetaData();
  
  // Refresh Rate Management
  bool IsRefreshRateSupported(uint32_t refresh_rate);
  
  // Luminance helpers - static for internal use
  static int32_t GetEOTF(const GammaTransfer &transfer);
  static float GetMaxOrAverageLuminance(float luminance);
  static float GetMinLuminance(float luminance, float max_luminance);

  struct DestScalarCache {
    SDEScaler scalar_data = {};
    uint32_t flags = {};
  };

  sde_drm_dest_scaler_data sde_dest_scalar_data_ = {};
  std::vector<SDEScaler> scalar_data_ = {};
  CWBConfig cwb_config_ = {};
  sde_drm::DRMIdlePCState idle_pc_state_ = sde_drm::DRMIdlePCState::NONE;
  bool idle_pc_enabled_ = true;
  std::vector<DestScalarCache> dest_scalar_cache_ = {};
  drm_msm_ad4_roi_cfg ad4_roi_cfg_ = {};
  bool needs_ds_update_ = false;
  std::vector<uint64_t> bitclk_rates_;
  std::string brightness_base_path_ = "";
  std::map<PanelFeaturePropertyID, sde_drm::DRMPanelFeatureID> panel_feature_property_map_ {};
  
  // Refresh rate management
  uint32_t current_refresh_rate_ = 60;
  uint32_t target_refresh_rate_ = 60;
  bool refresh_rate_change_pending_ = false;
  
  // Brightness and HDR management
  int current_brightness_ = 0;
  int target_brightness_ = 0;
  bool brightness_change_pending_ = false;
  bool hdr_brightness_boost_ = false;
  
  // HDR state
  drm_msm_ext_hdr_metadata hdr_metadata_ = {};
  struct timeval hdr_reset_start_ = {};
  struct timeval hdr_reset_end_ = {};
  bool reset_hdr_flag_ = false;
  bool in_multiset_ = false;
  bool hdr_active_ = false;
  bool hdr_plus_supported_ = false;
  
  // Power management
  bool low_power_mode_ = false;
  bool always_on_display_enabled_ = false;
  uint32_t idle_timeout_ms_ = 10000;
  
  // Panel constants for 1800 nits AMOLED
  static const float kDefaultMinLuminance;
  static const float kDefaultMaxLuminance;
  static const float kMinPeakLuminance;
  static const float kMaxPeakLuminance;
  static const float kHDRBrightnessBoostFactor;
};

}  // namespace sdm

#endif  // __HW_PERIPHERAL_DRM_H__
