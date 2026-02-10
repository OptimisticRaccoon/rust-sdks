/*
 * Copyright 2025 ZenSpeak
 *
 * NVENC runtime configuration (process-global).
 *
 * This is intentionally separate from encoder selection (software vs hardware)
 * so we can expose OBS-style knobs (preset/profile/rate-control) from Rust/UI.
 *
 * NOTE: This config is only read by NVIDIA encoder implementations. It is a no-op
 * for other encoders/backends.
 */

#pragma once

#include <cstdint>

namespace livekit_ffi {

enum class NvencPreset {
  UltraLowLatency = 0,
  LowLatency = 1,
  Quality = 2,
};

enum class NvencProfile {
  Auto = 0,
  Baseline = 1,
  Main = 2,
  High = 3,
};

enum class NvencRateControl {
  Cbr = 0,
  Vbr = 1,
  Cqp = 2,
};

struct NvencSettings {
  // Default matches prior behavior: preset P4 + ultra-low-latency tuning.
  NvencPreset preset = NvencPreset::LowLatency;
  NvencProfile profile = NvencProfile::Auto;
  NvencRateControl rate_control = NvencRateControl::Cbr;

  // For CQP mode only.
  // Typical ranges for H.264: ~18-28. Lower = better quality.
  int32_t cqp = 23;

  // If > 0, set NVENC gopLength/idrPeriod based on (fps * seconds).
  // If 0, keep current behavior (often "infinite gop; keyframes on request").
  int32_t keyframe_interval_seconds = 0;

  // Log applied settings via RTC_LOG when changed/used.
  bool log_settings = true;
};

void SetGlobalNvencSettings(const NvencSettings& settings);
NvencSettings GetGlobalNvencSettings();

// CXX bridge (primitive types)
// preset: 0=UltraLowLatency, 1=LowLatency, 2=Quality
// profile: 0=Auto, 1=Baseline, 2=Main, 3=High
// rate_control: 0=CBR, 1=VBR, 2=CQP
void set_nvenc_settings_raw(int32_t preset,
                            int32_t profile,
                            int32_t rate_control,
                            int32_t cqp,
                            int32_t keyframe_interval_seconds,
                            bool log_settings);

}  // namespace livekit_ffi

