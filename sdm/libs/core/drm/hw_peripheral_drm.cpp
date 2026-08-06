/*
Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
*/

#include <fcntl.h>
#include <math.h>
#include <utils/debug.h>
#include <utils/sys.h>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

#include "hw_peripheral_drm.h"

#define __CLASS__ "HWPeripheralDRM"

using sde_drm::DRMDisplayType;
using sde_drm::DRMOps;
using sde_drm::DRMPowerMode;
using sde_drm::DppsFeaturePayload;
using sde_drm::DRMDppsFeatureInfo;
using sde_drm::DRMPanelFeatureID;
using sde_drm::DRMPanelFeatureInfo;
using sde_drm::DRMSecureMode;
using sde_drm::DRMCWbCaptureMode;

namespace sdm {

// Proper Initialization of Static Constants scoped internally
const float HWPeripheralDRM::kDefaultMinLuminance = 0.02f;
const float HWPeripheralDRM::kDefaultMaxLuminance = 1800.0f;
const float HWPeripheralDRM::kMinPeakLuminance = 300.0f;
const float HWPeripheralDRM::kMaxPeakLuminance = 2000.0f;

// Static HDR EOTF helper mapping
static int32_t GetEOTF(const GammaTransfer &transfer) {
  int32_t hdr_transfer = -1;
  switch (transfer) {
    case Transfer_SMPTE_ST2084:
      hdr_transfer = HDR_EOTF_SMTPE_ST2084;
      break;
    case Transfer_HLG:
      hdr_transfer = HDR_EOTF_HLG;
      break;
    case Transfer_sRGB:
      hdr_transfer = 0;
      break;
    default:
      DLOGW("Unknown Transfer: %d", transfer);
  }
  return hdr_transfer;
}

HWPeripheralDRM::HWPeripheralDRM(int32_t display_id, BufferAllocator *buffer_allocator,
                                 HWInfoInterface *hw_info_intf)
  : HWDeviceDRM(buffer_allocator, hw_info_intf) {
  disp_type_ = DRMDisplayType::PERIPHERAL;
  device_name_ = "Peripheral";
  display_id_ = display_id;
  
  // Safely zero out tracking structs during instantiation
  memset(&hdr_metadata_, 0, sizeof(hdr_metadata_));
  memset(&hdr_reset_start_, 0, sizeof(hdr_reset_start_));
  memset(&hdr_reset_end_, 0, sizeof(hdr_reset_end_));
}

DisplayError HWPeripheralDRM::Init() {
  DisplayError ret = HWDeviceDRM::Init();
  if (ret != kErrorNone) {
    DLOGE("Init failed for %s", device_name_);
    return ret;
  }

  InitDestScaler();
  PopulateBitClkRates();
  CreatePanelFeaturePropertyMap();

  // Validate hardware HDR topology
  bool hdr_supported = connector_info_.panel_hdr_prop.hdr_enabled || 
                       connector_info_.ext_hdr_prop.hdr_supported;
  
  if (hdr_supported) {
    hw_panel_info_.hdr_enabled = true;
    hw_panel_info_.hdr_plus_enabled = connector_info_.ext_hdr_prop.hdr_plus_supported;
    hw_panel_info_.peak_luminance = kDefaultMaxLuminance;
    hw_panel_info_.blackness_level = kDefaultMinLuminance;
    hw_panel_info_.average_luminance = (kDefaultMaxLuminance + kDefaultMinLuminance) / 2.0f;
    
    InitMaxHDRMetaData();
  } else {
    hw_panel_info_.hdr_enabled = false;
    hw_panel_info_.hdr_plus_enabled = false;
  }
  
  // Set DCI-P3 uint32_t Primaries (value = float * 50000) for standard AMOLED tuning
  hw_panel_info_.primaries.white_point[0] = 15635; 
  hw_panel_info_.primaries.white_point[1] = 16450; 
  hw_panel_info_.primaries.red[0] = 34000;         
  hw_panel_info_.primaries.red[1] = 16000;         
  hw_panel_info_.primaries.green[0] = 13250;       
  hw_panel_info_.primaries.green[1] = 34500;       
  hw_panel_info_.primaries.blue[0] = 7500;         
  hw_panel_info_.primaries.blue[1] = 3000;         

  return kErrorNone;
}

DisplayError HWPeripheralDRM::UpdateHDRMetaData(HWLayers *hw_layers) {
  if (!hw_panel_info_.hdr_enabled) return kErrorNone;

  const HWHDRLayerInfo &hdr_layer_info = hw_layers->info.hdr_layer_info;
  HWHDRLayerInfo::HDROperation hdr_op = hdr_layer_info.operation;

  Layer hdr_layer = {};
  if (hdr_op == HWHDRLayerInfo::kSet && hdr_layer_info.layer_index > -1) {
    hdr_layer = *(hw_layers->info.stack->layers.at(UINT32(hdr_layer_info.layer_index)));
  }

  const LayerBuffer *layer_buffer = &hdr_layer.input_buffer;
  const MasteringDisplay &mastering_display = layer_buffer->color_metadata.masteringDisplayInfo;
  const ContentLightLevel &light_level = layer_buffer->color_metadata.contentLightLevel;
  const Primaries &primaries = mastering_display.primaries;

  if (hdr_op == HWHDRLayerInfo::kSet && hdr_layer_info.hdr_layers.size() == 1) {
    reset_hdr_flag_ = false;
    in_multiset_ = false;
    hdr_active_ = true;

    int32_t eotf = GetEOTF(layer_buffer->color_metadata.transfer);
    hdr_metadata_.hdr_supported = 1;
    hdr_metadata_.hdr_state = HDR_ENABLE;
    hdr_metadata_.eotf = (eotf < 0) ? 0 : UINT32(eotf);
    hdr_metadata_.white_point_x = primaries.whitePoint[0];
    hdr_metadata_.white_point_y = primaries.whitePoint[1];
    hdr_metadata_.display_primaries_x[0] = primaries.rgbPrimaries[0][0];
    hdr_metadata_.display_primaries_y[0] = primaries.rgbPrimaries[0][1];
    hdr_metadata_.display_primaries_x[1] = primaries.rgbPrimaries[1][0];
    hdr_metadata_.display_primaries_y[1] = primaries.rgbPrimaries[1][1];
    hdr_metadata_.display_primaries_x[2] = primaries.rgbPrimaries[2][0];
    hdr_metadata_.display_primaries_y[2] = primaries.rgbPrimaries[2][1];
    hdr_metadata_.min_luminance = mastering_display.minDisplayLuminance;
    hdr_metadata_.max_luminance = mastering_display.maxDisplayLuminance;
    hdr_metadata_.max_content_light_level = light_level.maxContentLightLevel;
    hdr_metadata_.max_average_light_level = light_level.minPicAverageLightLevel;

    if (hw_panel_info_.hdr_plus_enabled && hdr_layer_info.dyn_hdr_vsif_payload.size()) {
      hdr_metadata_.hdr_plus_payload = reinterpret_cast<uint64_t>
                                        (hdr_layer_info.dyn_hdr_vsif_payload.data());
      hdr_metadata_.hdr_plus_payload_size = UINT32(hdr_layer_info.dyn_hdr_vsif_payload.size());
      hdr_plus_supported_ = true;
    } else {
      hdr_metadata_.hdr_plus_payload = reinterpret_cast<uint64_t>(nullptr);
      hdr_metadata_.hdr_plus_payload_size = 0;
      hdr_plus_supported_ = false;
    }

    drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_HDR_METADATA, token_.conn_id, &hdr_metadata_);
    DumpHDRMetaData(hdr_op);

  } else if (hdr_op == HWHDRLayerInfo::kSet && !in_multiset_) {
    InitMaxHDRMetaData();
    in_multiset_ = true;
    reset_hdr_flag_ = false;
    hdr_active_ = true;
    drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_HDR_METADATA, token_.conn_id, &hdr_metadata_);

  } else if (hdr_op == HWHDRLayerInfo::kReset) {
    memset(&hdr_metadata_, 0, sizeof(hdr_metadata_));
    hdr_metadata_.hdr_supported = 1;
    hdr_metadata_.hdr_state = HDR_DISABLE;
    reset_hdr_flag_ = true;
    hdr_active_ = false;
    hdr_plus_supported_ = false;
    gettimeofday(&hdr_reset_start_, NULL);
    drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_HDR_METADATA, token_.conn_id, &hdr_metadata_);
  }

  return kErrorNone;
}

void HWPeripheralDRM::DumpHDRMetaData(HWHDRLayerInfo::HDROperation operation) {
  DLOGI("AMOLED HDR Operation = %d, MaxDisplayLuminance = %d, EOTF = %d",
        operation, hdr_metadata_.max_luminance, hdr_metadata_.eotf);
}

void HWPeripheralDRM::InitMaxHDRMetaData() {
  memset(&hdr_metadata_, 0, sizeof(hdr_metadata_));
  hdr_metadata_.hdr_supported = 1;
  hdr_metadata_.hdr_state = HDR_ENABLE;
  hdr_metadata_.eotf = UINT32(GetEOTF(Transfer_SMPTE_ST2084));
  
  hdr_metadata_.white_point_x = 15635;
  hdr_metadata_.white_point_y = 16451;
  hdr_metadata_.display_primaries_x[0] = 35400;
  hdr_metadata_.display_primaries_y[0] = 14600;
  hdr_metadata_.display_primaries_x[1] = 8500;
  hdr_metadata_.display_primaries_y[1] = 39850;
  hdr_metadata_.display_primaries_x[2] = 6550;
  hdr_metadata_.display_primaries_y[2] = 2300;
  
  hdr_metadata_.min_luminance = 0;
  hdr_metadata_.max_luminance = 90000000;
  hdr_metadata_.max_content_light_level = 90000000;
  hdr_metadata_.max_average_light_level = 90000000;
}

void HWPeripheralDRM::InitDestScaler() {
  if (hw_resource_.hw_dest_scalar_info.count) {
    dest_scaler_blocks_used_ = 1;
    if (kQuadSplit == mixer_attributes_.split_type) {
      dest_scaler_blocks_used_ = 4;
    } else if (kDualSplit == mixer_attributes_.split_type) {
      dest_scaler_blocks_used_ = 2;
    }
    if (hw_resource_.hw_dest_scalar_info.count >=
        (hw_dest_scaler_blocks_used_ + dest_scaler_blocks_used_)) {
      hw_dest_scaler_blocks_used_ += dest_scaler_blocks_used_;
    } else {
      dest_scaler_blocks_used_ = 0;
    }
    scalar_data_.resize(dest_scaler_blocks_used_);
    dest_scalar_cache_.resize(dest_scaler_blocks_used_);
    mixer_attributes_.dest_scaler_blocks_used = dest_scaler_blocks_used_;
  }

  topology_control_ = UINT32(sde_drm::DRMTopologyControl::DSPP);
  if (dest_scaler_blocks_used_) {
    topology_control_ |= UINT32(sde_drm::DRMTopologyControl::DEST_SCALER);
  }
}

void HWPeripheralDRM::PopulateBitClkRates() {
  if (!hw_panel_info_.dyn_bitclk_support) {
    return;
  }

  uint32_t width = connector_info_.modes[current_mode_index_].mode.hdisplay;
  uint32_t height = connector_info_.modes[current_mode_index_].mode.vdisplay;

  for (auto &mode_info : connector_info_.modes) {
    auto &mode = mode_info.mode;
    if (mode.hdisplay == width && mode.vdisplay == height) {
      for (uint32_t index = 0; index < mode_info.dyn_bitclk_list.size(); index++) {
        if (std::find(bitclk_rates_.begin(), bitclk_rates_.end(),
              mode_info.dyn_bitclk_list[index]) == bitclk_rates_.end()) {
          bitclk_rates_.push_back(mode_info.dyn_bitclk_list[index]);
          DLOGI("Possible bit_clk_rates %" PRIu64, mode_info.dyn_bitclk_list[index]);
        }
      }
    }
  }

  hw_panel_info_.bitclk_rates = bitclk_rates_;
  DLOGI("bit_clk_rates Size %zu", bitclk_rates_.size());
}

DisplayError HWPeripheralDRM::SetDynamicDSIClock(uint64_t bit_clk_rate) {
  if (last_power_mode_ == DRMPowerMode::DOZE_SUSPEND || last_power_mode_ == DRMPowerMode::OFF) {
    return kErrorNotSupported;
  }

  if (doze_poms_switch_done_ || pending_poms_switch_) {
    return kErrorNotSupported;
  }

  if (vrefresh_) {
    return kErrorNotSupported;
  }

  if (GetSupportedBitClkRate(current_mode_index_, bit_clk_rate) ==
      connector_info_.modes[current_mode_index_].curr_bit_clk_rate) {
    return kErrorNone;
  }

  bit_clk_rate_ = bit_clk_rate;
  return kErrorNone;
}

DisplayError HWPeripheralDRM::GetDynamicDSIClock(uint64_t *bit_clk_rate) {
  *bit_clk_rate = (uint32_t)connector_info_.modes[current_mode_index_].curr_bit_clk_rate;
  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetRefreshRate(uint32_t refresh_rate) {
  if (doze_poms_switch_done_ || pending_poms_switch_) {
    return kErrorNotSupported;
  }

  target_refresh_rate_ = refresh_rate;
  refresh_rate_change_pending_ = true;

  DisplayError error = HWDeviceDRM::SetRefreshRate(refresh_rate);
  if (error != kErrorNone) {
    return error;
  }

  current_refresh_rate_ = refresh_rate;
  refresh_rate_change_pending_ = false;

  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetDisplayMode(const HWDisplayMode hw_display_mode) {
  if (doze_poms_switch_done_ || pending_poms_switch_) {
    return kErrorNotSupported;
  }

  DisplayError error = HWDeviceDRM::SetDisplayMode(hw_display_mode);
  if (error != kErrorNone) {
    return error;
  }

  hw_panel_info_.bitclk_rates = bitclk_rates_;

  return kErrorNone;
}

DisplayError HWPeripheralDRM::Validate(HWLayers *hw_layers) {
  HWLayersInfo &hw_layer_info = hw_layers->info;
  SetDestScalarData(hw_layer_info);
  SetupConcurrentWriteback(hw_layer_info, true, nullptr);
  SetIdlePCState();

  UpdateHDRMetaData(hw_layers);

  return HWDeviceDRM::Validate(hw_layers);
}

DisplayError HWPeripheralDRM::Commit(HWLayers *hw_layers) {
  HWLayersInfo &hw_layer_info = hw_layers->info;
  SetDestScalarData(hw_layer_info);

  int64_t cwb_fence_fd = -1;
  bool has_fence = SetupConcurrentWriteback(hw_layer_info, false, &cwb_fence_fd);

  SetIdlePCState();
  UpdateHDRMetaData(hw_layers);

  DisplayError error = HWDeviceDRM::Commit(hw_layers);
  if (error != kErrorNone) {
    return error;
  }

  if (has_fence) {
    hw_layer_info.stack->output_buffer->release_fence = Fence::Create(INT(cwb_fence_fd),
                                                                      "release_cwb");
  }

  CacheDestScalarData();
  if (cwb_config_.enabled && (error == kErrorNone)) {
    PostCommitConcurrentWriteback(hw_layer_info.stack->output_buffer);
  }

  synchronous_commit_ = false;
  active_ = true;

  if (pending_poms_switch_) {
    HWDeviceDRM::SetDisplayMode(kModeCommand);
    hw_panel_info_.bitclk_rates = bitclk_rates_;
    doze_poms_switch_done_ = true;
    pending_poms_switch_ = false;
  }

  idle_pc_state_ = sde_drm::DRMIdlePCState::NONE;

  return error;
}

void HWPeripheralDRM::ResetDestScalarCache() {
  for (uint32_t j = 0; j < scalar_data_.size(); j++) {
    dest_scalar_cache_[j] = {};
  }
}

void HWPeripheralDRM::SetDestScalarData(const HWLayersInfo &hw_layer_info) {
  if (!hw_scale_ || !dest_scaler_blocks_used_) {
    return;
  }

  for (uint32_t i = 0; i < dest_scaler_blocks_used_; i++) {
    auto it = hw_layer_info.dest_scale_info_map.find(i);

    if (it == hw_layer_info.dest_scale_info_map.end()) {
      continue;
    }

    HWDestScaleInfo *dest_scale_info = it->second;
    SDEScaler *scale = &scalar_data_[i];
    hw_scale_->SetScaler(dest_scale_info->scale_data, scale);

    sde_drm_dest_scaler_cfg *dest_scalar_data = &sde_dest_scalar_data_.ds_cfg[i];
    dest_scalar_data->flags = 0;
    if (scale->scaler_v2.enable) {
      dest_scalar_data->flags |= SDE_DRM_DESTSCALER_ENABLE;
    }
    if (scale->scaler_v2.de.enable) {
      dest_scalar_data->flags |= SDE_DRM_DESTSCALER_ENHANCER_UPDATE;
    }
    if (dest_scale_info->scale_update) {
      dest_scalar_data->flags |= SDE_DRM_DESTSCALER_SCALE_UPDATE;
    }
    if (hw_panel_info_.partial_update) {
      dest_scalar_data->flags |= SDE_DRM_DESTSCALER_PU_ENABLE;
    }
    dest_scalar_data->index = i;
    dest_scalar_data->lm_width = dest_scale_info->mixer_width;
    dest_scalar_data->lm_height = dest_scale_info->mixer_height;
    dest_scalar_data->scaler_cfg = reinterpret_cast<uint64_t>(&scale->scaler_v2);

    if (std::memcmp(&dest_scalar_cache_[i].scalar_data, scale, sizeof(SDEScaler)) ||
        dest_scalar_cache_[i].flags != dest_scalar_data->flags) {
      needs_ds_update_ = true;
    }
  }

  if (needs_ds_update_) {
    sde_dest_scalar_data_.num_dest_scaler = UINT32(hw_layer_info.dest_scale_info_map.size());
    drm_atomic_intf_->Perform(DRMOps::CRTC_SET_DEST_SCALER_CONFIG, token_.crtc_id,
                              reinterpret_cast<uint64_t>(&sde_dest_scalar_data_));
  }
}

void HWPeripheralDRM::CacheDestScalarData() {
  if (needs_ds_update_) {
    for (uint32_t i = 0; i < sde_dest_scalar_data_.num_dest_scaler; i++) {
      dest_scalar_cache_[i].flags = sde_dest_scalar_data_.ds_cfg[i].flags;
      dest_scalar_cache_[i].scalar_data = scalar_data_[i];
    }
    needs_ds_update_ = false;
  }
}

DisplayError HWPeripheralDRM::Flush(HWLayers *hw_layers) {
  DisplayError err = HWDeviceDRM::Flush(hw_layers);
  if (err != kErrorNone) {
    return err;
  }

  ResetDestScalarCache();
  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetDppsFeature(void *payload, size_t size) {
  uint32_t obj_id = 0, object_type = 0, feature_id = 0;
  uint64_t value = 0;

  if (size != sizeof(DppsFeaturePayload)) {
    DLOGE("invalid payload size %zu, expected %zu", size, sizeof(DppsFeaturePayload));
    return kErrorParameters;
  }

  DppsFeaturePayload *feature_payload = reinterpret_cast<DppsFeaturePayload *>(payload);
  object_type = feature_payload->object_type;
  feature_id = feature_payload->feature_id;
  value = feature_payload->value;

  if (feature_id == sde_drm::kFeatureAd4Roi) {
    if (feature_payload->value) {
      DisplayDppsAd4RoiCfg *params = reinterpret_cast<DisplayDppsAd4RoiCfg *>
                                                      (feature_payload->value);
      if (!params) {
        DLOGE("invalid playload value %" PRIu64, feature_payload->value);
        return kErrorNotSupported;
      }

      ad4_roi_cfg_.h_x = params->h_start;
      ad4_roi_cfg_.h_y = params->h_end;
      ad4_roi_cfg_.v_x = params->v_start;
      ad4_roi_cfg_.v_y = params->v_end;
      ad4_roi_cfg_.factor_in = params->factor_in;
      ad4_roi_cfg_.factor_out = params->factor_out;

      value = (uint64_t)&ad4_roi_cfg_;
    }
  }

  if (object_type == DRM_MODE_OBJECT_CRTC) {
    obj_id = token_.crtc_id;
  } else if (object_type == DRM_MODE_OBJECT_CONNECTOR) {
    obj_id = token_.conn_id;
  } else {
    DLOGE("invalid object type 0x%x", object_type);
    return kErrorUndefined;
  }

  drm_atomic_intf_->Perform(DRMOps::DPPS_CACHE_FEATURE, obj_id, feature_id, value);
  return kErrorNone;
}

DisplayError HWPeripheralDRM::GetDppsFeatureInfo(void *payload, size_t size) {
  if (size != sizeof(DRMDppsFeatureInfo)) {
    DLOGE("invalid payload size %zu, expected %zu", size, sizeof(DRMDppsFeatureInfo));
    return kErrorParameters;
  }
  DRMDppsFeatureInfo *feature_info = reinterpret_cast<DRMDppsFeatureInfo *>(payload);
  feature_info->obj_id = token_.crtc_id;
  drm_mgr_intf_->GetDppsFeatureInfo(feature_info);
  return kErrorNone;
}

DisplayError HWPeripheralDRM::HandleSecureEvent(SecureEvent secure_event, HWLayers *hw_layers) {
  switch (secure_event) {
    case kSecureDisplayStart: {
      secure_display_active_ = true;
      if (hw_panel_info_.mode != kModeCommand) {
        DisplayError err = Flush(hw_layers);
        if (err != kErrorNone) {
          return err;
        }
      }
    }
    break;

    case kSecureDisplayEnd: {
      if (hw_panel_info_.mode != kModeCommand) {
        DisplayError err = Flush(hw_layers);
        if (err != kErrorNone) {
          return err;
        }
      }
      secure_display_active_ = false;
      synchronous_commit_ = true;
    }
    break;

    default:
      DLOGE("Invalid secure event %d", secure_event);
      return kErrorNotSupported;
  }

  return kErrorNone;
}

bool HWPeripheralDRM::SetupConcurrentWriteback(const HWLayersInfo &hw_layer_info, bool validate,
                                               int64_t *release_fence_fd) {
  bool enable = hw_resource_.has_concurrent_writeback && hw_layer_info.stack->output_buffer;
  if (!(enable || cwb_config_.enabled)) {
    return false;
  }

  bool setup_modes = enable && !cwb_config_.enabled && validate;
  if (setup_modes && (SetupConcurrentWritebackModes() == kErrorNone)) {
    cwb_config_.enabled = true;
  }

  if (cwb_config_.enabled) {
    if (enable) {
      ConfigureConcurrentWriteback(hw_layer_info.stack);

      if (!validate && release_fence_fd) {
        drm_atomic_intf_->Perform(DRMOps::CONNECTOR_GET_RETIRE_FENCE,
                                  cwb_config_.token.conn_id, release_fence_fd);
        return true;
      }
    } else {
      drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_CRTC, cwb_config_.token.conn_id, 0);
    }
  }

  return false;
}

DisplayError HWPeripheralDRM::TeardownConcurrentWriteback(void) {
  if (cwb_config_.enabled) {
    drm_mgr_intf_->UnregisterDisplay(&(cwb_config_.token));
    cwb_config_.enabled = false;
    registry_.Clear();
  }

  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetupConcurrentWritebackModes() {
  if (drm_mgr_intf_->RegisterDisplay(DRMDisplayType::VIRTUAL, &cwb_config_.token)) {
    DLOGE("RegisterDisplay failed for Concurrent Writeback");
    return kErrorResources;
  }

  std::vector<drmModeModeInfo> modes;
  for (auto &item : connector_info_.modes) {
    modes.push_back(item.mode);
  }

  struct sde_drm_wb_cfg cwb_cfg = {};
  cwb_cfg.connector_id = cwb_config_.token.conn_id;
  cwb_cfg.flags = SDE_DRM_WB_CFG_FLAGS_CONNECTED;
  cwb_cfg.count_modes = UINT32(modes.size());
  cwb_cfg.modes = (uint64_t)modes.data();

  int ret = -EINVAL;
#ifdef DRM_IOCTL_SDE_WB_CONFIG
  ret = drmIoctl(dev_fd_, DRM_IOCTL_SDE_WB_CONFIG, &cwb_cfg);
#endif
  if (ret) {
    drm_mgr_intf_->UnregisterDisplay(&(cwb_config_.token));
    DLOGE("Dump CWBConfig: mode_count %d flags %x", cwb_cfg.count_modes, cwb_cfg.flags);
    DumpConnectorModeInfo();
    return kErrorHardware;
  }

  return kErrorNone;
}

void HWPeripheralDRM::ConfigureConcurrentWriteback(LayerStack *layer_stack) {
  LayerBuffer *output_buffer = layer_stack->output_buffer;
  registry_.MapOutputBufferToFbId(output_buffer);

  drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_CRTC, cwb_config_.token.conn_id, token_.crtc_id);

  DRMCWbCaptureMode capture_mode = layer_stack->flags.post_processed_output ?
                                   DRMCWbCaptureMode::DSPP_OUT : DRMCWbCaptureMode::MIXER_OUT;
  drm_atomic_intf_->Perform(DRMOps::CRTC_SET_CAPTURE_MODE, token_.crtc_id, capture_mode);

  uint32_t fb_id = registry_.GetOutputFbId(output_buffer->handle_id);
  drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_OUTPUT_FB_ID, cwb_config_.token.conn_id, fb_id);

  bool secure = output_buffer->flags.secure;
  DRMSecureMode mode = secure ? DRMSecureMode::SECURE : DRMSecureMode::NON_SECURE;
  drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_FB_SECURE_MODE, cwb_config_.token.conn_id, mode);

  sde_drm::DRMRect dst = {};
  dst.left = 0;
  dst.top = 0;
  dst.right = display_attributes_[current_mode_index_].x_pixels;
  dst.bottom = display_attributes_[current_mode_index_].y_pixels;
  drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_OUTPUT_RECT, cwb_config_.token.conn_id, dst);
}

void HWPeripheralDRM::PostCommitConcurrentWriteback(LayerBuffer *output_buffer) {
  bool enabled = hw_resource_.has_concurrent_writeback && output_buffer;

  if (!enabled) {
    TeardownConcurrentWriteback();
  }
}

DisplayError HWPeripheralDRM::ControlIdlePowerCollapse(bool enable, bool synchronous) {
  if (enable == idle_pc_enabled_) {
    return kErrorNone;
  }
  idle_pc_state_ = enable ? sde_drm::DRMIdlePCState::ENABLE : sde_drm::DRMIdlePCState::DISABLE;
  synchronous_commit_ = !enable ? synchronous : false;
  idle_pc_enabled_ = enable;
  return kErrorNone;
}

DisplayError HWPeripheralDRM::PowerOn(const HWQosData &qos_data,
                                      shared_ptr<Fence> *release_fence) {
  DTRACE_SCOPED();
  if (!drm_atomic_intf_) {
    DLOGE("DRM Atomic Interface is null!");
    return kErrorUndefined;
  }

  if (first_cycle_ || delay_first_commit_) {
    return kErrorDeferred;
  }

  if (switch_mode_valid_ && doze_poms_switch_done_ && (current_mode_index_ == cmd_mode_index_)) {
    HWDeviceDRM::SetDisplayMode(kModeVideo);
    hw_panel_info_.bitclk_rates = bitclk_rates_;
    doze_poms_switch_done_ = false;
  }

  if (!idle_pc_enabled_) {
    drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_IDLE_PC_STATE, token_.crtc_id,
                              sde_drm::DRMIdlePCState::ENABLE);
  }

  if (sde_dest_scalar_data_.num_dest_scaler) {
    drm_atomic_intf_->Perform(DRMOps::CRTC_SET_DEST_SCALER_CONFIG, token_.crtc_id,
                              reinterpret_cast<uint64_t>(&sde_dest_scalar_data_));
    needs_ds_update_ = true;
  }

  DisplayError err = HWDeviceDRM::PowerOn(qos_data, release_fence);
  if (err != kErrorNone) {
    return err;
  }
  idle_pc_state_ = sde_drm::DRMIdlePCState::NONE;
  idle_pc_enabled_ = true;
  pending_poms_switch_ = false;
  active_ = true;

  CacheDestScalarData();

  return kErrorNone;
}

DisplayError HWPeripheralDRM::PowerOff(bool teardown) {
  DTRACE_SCOPED();

  if (delay_first_commit_) {
    delay_first_commit_ = false;
  }

  DisplayError err = HWDeviceDRM::PowerOff(teardown);
  if (err != kErrorNone) {
    return err;
  }

  pending_poms_switch_ = false;
  active_ = false;

  return kErrorNone;
}

DisplayError HWPeripheralDRM::Doze(const HWQosData &qos_data, shared_ptr<Fence> *release_fence) {
  DTRACE_SCOPED();

  // Safely scale doze brightness by reducing output directly in power loop
  if (current_brightness_ == 0) {
    GetPanelBrightness(&current_brightness_);
  }
  int doze_brightness = static_cast<int>(hw_panel_info_.panel_max_brightness * 0.15f);
  HWDeviceDRM::SetPanelBrightness(doze_brightness);

  if (!first_cycle_ && switch_mode_valid_ && !doze_poms_switch_done_ &&
    (current_mode_index_ == video_mode_index_)) {
    if (active_) {
      HWDeviceDRM::SetDisplayMode(kModeCommand);
      hw_panel_info_.bitclk_rates = bitclk_rates_;
      doze_poms_switch_done_ = true;
    } else {
      pending_poms_switch_ = true;
    }
  }

  DisplayError err = HWDeviceDRM::Doze(qos_data, release_fence);
  if (err != kErrorNone) {
    return err;
  }

  if (first_cycle_) {
    active_ = true;
  }

  return kErrorNone;
}

DisplayError HWPeripheralDRM::DozeSuspend(const HWQosData &qos_data,
                                          shared_ptr<Fence> *release_fence) {
  DTRACE_SCOPED();
  
  int doze_suspend_brightness = static_cast<int>(hw_panel_info_.panel_max_brightness * 0.05f);
  HWDeviceDRM::SetPanelBrightness(doze_suspend_brightness);

  if (switch_mode_valid_ && !doze_poms_switch_done_ &&
    (current_mode_index_ == video_mode_index_)) {
    HWDeviceDRM::SetDisplayMode(kModeCommand);
    hw_panel_info_.bitclk_rates = bitclk_rates_;
    doze_poms_switch_done_ = true;
  }

  DisplayError err = HWDeviceDRM::DozeSuspend(qos_data, release_fence);
  if (err != kErrorNone) {
    return err;
  }

  pending_poms_switch_ = false;
  active_ = true;

  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetDisplayAttributes(uint32_t index) {
  if (doze_poms_switch_done_ || pending_poms_switch_ || bit_clk_rate_) {
    return kErrorNotSupported;
  }

  HWDeviceDRM::SetDisplayAttributes(index);
  hw_panel_info_.bitclk_rates = bitclk_rates_;

  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetDisplayDppsAdROI(void *payload) {
  DisplayError err = kErrorNone;
  struct sde_drm::DppsFeaturePayload feature_payload = {};

  if (!payload) {
    DLOGE("Invalid payload parameter");
    return kErrorParameters;
  }

  feature_payload.object_type = DRM_MODE_OBJECT_CRTC;
  feature_payload.feature_id = sde_drm::kFeatureAd4Roi;
  feature_payload.value = (uint64_t)(payload);

  err = SetDppsFeature(&feature_payload, sizeof(feature_payload));
  if (err != kErrorNone) {
    DLOGE("Faid to SetDppsFeature feature_id = %d, err = %d",
           sde_drm::kFeatureAd4Roi, err);
  }

  return err;
}

DisplayError HWPeripheralDRM::SetFrameTrigger(FrameTriggerMode mode) {
  sde_drm::DRMFrameTriggerMode drm_mode = sde_drm::DRMFrameTriggerMode::FRAME_DONE_WAIT_DEFAULT;
  switch (mode) {
  case kFrameTriggerDefault:
    drm_mode = sde_drm::DRMFrameTriggerMode::FRAME_DONE_WAIT_DEFAULT;
    break;
  case kFrameTriggerSerialize:
    drm_mode = sde_drm::DRMFrameTriggerMode::FRAME_DONE_WAIT_SERIALIZE;
    break;
  case kFrameTriggerPostedStart:
    drm_mode = sde_drm::DRMFrameTriggerMode::FRAME_DONE_WAIT_POSTED_START;
    break;
  default:
    DLOGE("Invalid frame trigger mode %d", (int32_t)mode);
    return kErrorParameters;
  }

  int ret = drm_atomic_intf_->Perform(DRMOps::CONNECTOR_SET_FRAME_TRIGGER,
                                      token_.conn_id, drm_mode);
  if (ret) {
    DLOGE("Failed to perform CONNECTOR_SET_FRAME_TRIGGER, drm_mode %d, ret %d", drm_mode, ret);
    return kErrorUndefined;
  }
  return kErrorNone;
}

DisplayError HWPeripheralDRM::SetPanelBrightness(int level) {
  if (pending_doze_) {
    DLOGI("Doze state pending!! Skip for now");
    return kErrorDeferred;
  }

  // Allow framework to smoothly handle brightness transitions via node logic
  char buffer[kMaxSysfsCommandLength] = {0};

  if (brightness_base_path_.empty()) {
    return kErrorHardware;
  }

  std::string brightness_node(brightness_base_path_ + "brightness");
  int fd = Sys::open_(brightness_node.c_str(), O_RDWR);
  if (fd < 0) {
    DLOGE("Failed to open node = %s, error = %s ", brightness_node.c_str(),
          strerror(errno));
    return kErrorFileDescriptor;
  }

  int32_t bytes = snprintf(buffer, kMaxSysfsCommandLength, "%d\n", level);
  ssize_t ret = Sys::pwrite_(fd, buffer, static_cast<size_t>(bytes), 0);
  if (ret <= 0) {
    DLOGE("Failed to write to node = %s, error = %s ", brightness_node.c_str(),
          strerror(errno));
    Sys::close_(fd);
    return kErrorHardware;
  }

  Sys::close_(fd);

  return kErrorNone;
}

DisplayError HWPeripheralDRM::GetPanelBrightness(int *level) {
  char value[kMaxStringLength] = {0};

  if (!level) {
    DLOGE("Invalid input, null pointer.");
    return kErrorParameters;
  }

  if (brightness_base_path_.empty()) {
    return kErrorHardware;
  }

  std::string brightness_node(brightness_base_path_ + "brightness");
  int fd = Sys::open_(brightness_node.c_str(), O_RDWR);
  if (fd < 0) {
    DLOGE("Failed to open brightness node = %s, error = %s", brightness_node.c_str(),
           strerror(errno));
    return kErrorFileDescriptor;
  }

  if (Sys::pread_(fd, value, sizeof(value), 0) > 0) {
    *level = atoi(value);
  } else {
    DLOGE("Failed to read panel brightness");
    Sys::close_(fd);
    return kErrorHardware;
  }

  Sys::close_(fd);

  return kErrorNone;
}

void HWPeripheralDRM::GetHWPanelMaxBrightness() {
  char value[kMaxStringLength] = {0};
  hw_panel_info_.panel_max_brightness = 255.0f;

  char s[kMaxStringLength] = {};
  snprintf(s, sizeof(s), "/sys/class/backlight/panel%d-backlight/",
           static_cast<int>(connector_info_.type_id - 1));
  brightness_base_path_.assign(s);

  std::string brightness_node(brightness_base_path_ + "max_brightness");
  int fd = Sys::open_(brightness_node.c_str(), O_RDONLY);
  if (fd < 0) {
    DLOGE("Failed to open max brightness node = %s, error = %s", brightness_node.c_str(),
          strerror(errno));
    return;
  }

  if (Sys::pread_(fd, value, sizeof(value), 0) > 0) {
    hw_panel_info_.panel_max_brightness = static_cast<float>(atof(value));
    DLOGI_IF(kTagDriverConfig, "Max brightness = %f", hw_panel_info_.panel_max_brightness);
  } else {
    DLOGE("Failed to read max brightness. error = %s", strerror(errno));
  }

  Sys::close_(fd);
  return;
}

DisplayError HWPeripheralDRM::SetBLScale(uint32_t level) {
  int ret = drm_atomic_intf_->Perform(DRMOps::DPPS_CACHE_FEATURE,
              token_.conn_id, sde_drm::kFeatureSvBlScale, level);
  if (ret) {
    DLOGE("Failed to set backlight scale level %d, ret %d", level, ret);
    return kErrorUndefined;
  }
  return kErrorNone;
}

DisplayError HWPeripheralDRM::GetPanelBrightnessBasePath(std::string *base_path) {
  if (!base_path) {
    DLOGE("Invalid base_path is null pointer");
    return kErrorParameters;
  }

  if (brightness_base_path_.empty()) {
    DLOGE("brightness_base_path_ is empty");
    return kErrorHardware;
  }

  *base_path = brightness_base_path_;
  return kErrorNone;
}

void HWPeripheralDRM::CreatePanelFeaturePropertyMap() {
  panel_feature_property_map_.clear();

  panel_feature_property_map_[kPanelFeatureDsppRCInfo] = sde_drm::kDRMPanelFeatureDsppRCInfo;
  panel_feature_property_map_[kPanelFeatureRCInitCfg] = sde_drm::kDRMPanelFeatureRCInit;
}
int HWPeripheralDRM::GetPanelFeature(PanelFeaturePropertyInfo *feature_info) {
  int ret = 0;
  DRMPanelFeatureInfo drm_feature = {};

  if (!feature_info) {
    DLOGE("Invalid object pointer of PanelFeaturePropertyInfo");
    return -EINVAL;
  }

  auto it = panel_feature_property_map_.find(feature_info->prop_id);
  if (it ==  panel_feature_property_map_.end()) {
    DLOGE("Failed to find prop-map entry for id %d", feature_info->prop_id);
    return -EINVAL;
  }

  drm_feature.prop_id = panel_feature_property_map_[feature_info->prop_id];
  drm_feature.prop_ptr = feature_info->prop_ptr;
  drm_feature.prop_size = feature_info->prop_size;

  switch (feature_info->prop_id) {
    case kPanelFeatureSPRInitCfg:
    case kPanelFeatureDsppIndex:
    case kPanelFeatureDsppSPRInfo:
    case kPanelFeatureDsppDemuraInfo:
    case kPanelFeatureDsppRCInfo:
    case kPanelFeatureRCInitCfg:
      drm_feature.obj_type = DRM_MODE_OBJECT_CRTC;
      drm_feature.obj_id =  token_.crtc_id;
     break;
    case kPanelFeatureSPRPackType:
      drm_feature.obj_type = DRM_MODE_OBJECT_CONNECTOR;
      drm_feature.obj_id =  token_.conn_id;
     break;
    default:
     DLOGE("obj id population for property %d not implemented", feature_info->prop_id);
     return -EINVAL;
  }

  drm_mgr_intf_->GetPanelFeature(&drm_feature);

  feature_info->version = drm_feature.version;
  feature_info->prop_size = drm_feature.prop_size;

  return ret;
}

int HWPeripheralDRM::SetPanelFeature(const PanelFeaturePropertyInfo &feature_info) {
  int ret = 0;
  DRMPanelFeatureInfo drm_feature = {};
  
  // Utilizes dot operator correctly for constant reference resolution
  drm_feature.prop_id = panel_feature_property_map_[feature_info.prop_id];
  drm_feature.prop_ptr = feature_info.prop_ptr;
  drm_feature.version = feature_info.version;
  drm_feature.prop_size = feature_info.prop_size;

  switch (feature_info.prop_id) {
    case kPanelFeatureSPRInitCfg:
    case kPanelFeatureRCInitCfg:
      drm_feature.obj_type = DRM_MODE_OBJECT_CRTC;
      drm_feature.obj_id =  token_.crtc_id;
     break;
    case kPanelFeatureSPRPackType:
      drm_feature.obj_type = DRM_MODE_OBJECT_CONNECTOR;
      drm_feature.obj_id =  token_.conn_id;
     break;
    default:
     DLOGE("Set Panel feature property %d not implemented", feature_info.prop_id);
     return -EINVAL;
  }

  DLOGI("Set Panel feature property %d", feature_info.prop_id);
  drm_mgr_intf_->SetPanelFeature(drm_feature);

  return ret;
}
DisplayError HWPeripheralDRM::DelayFirstCommit() {
  delay_first_commit_ = true;
  return kErrorNone;
}
}  // namespace sdm
