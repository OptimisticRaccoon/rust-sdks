/*
 * Copyright 2025 ZenSpeak
 */

#include "livekit/nvenc_settings.h"

#include <mutex>

#include "rtc_base/logging.h"

namespace livekit_ffi {

namespace {
std::mutex g_nvenc_mutex;
NvencSettings g_nvenc_settings = NvencSettings{};
bool g_nvenc_settings_initialized = false;
}  // namespace

void SetGlobalNvencSettings(const NvencSettings& settings) {
  std::lock_guard<std::mutex> lock(g_nvenc_mutex);
  g_nvenc_settings = settings;
  g_nvenc_settings_initialized = true;
  if (settings.log_settings) {
    RTC_LOG(LS_INFO) << "Global NVENC settings set: preset="
                     << static_cast<int>(settings.preset)
                     << " profile=" << static_cast<int>(settings.profile)
                     << " rc=" << static_cast<int>(settings.rate_control)
                     << " cqp=" << settings.cqp
                     << " keyint_s=" << settings.keyframe_interval_seconds;
  }
}

NvencSettings GetGlobalNvencSettings() {
  std::lock_guard<std::mutex> lock(g_nvenc_mutex);
  if (!g_nvenc_settings_initialized) {
    // Defaults only. (We intentionally do not parse env vars here yet.)
    g_nvenc_settings_initialized = true;
  }
  return g_nvenc_settings;
}

void set_nvenc_settings_raw(int32_t preset,
                            int32_t profile,
                            int32_t rate_control,
                            int32_t cqp,
                            int32_t keyframe_interval_seconds,
                            bool log_settings) {
  NvencSettings s;
  s.preset = static_cast<NvencPreset>(preset);
  s.profile = static_cast<NvencProfile>(profile);
  s.rate_control = static_cast<NvencRateControl>(rate_control);
  s.cqp = cqp;
  s.keyframe_interval_seconds = keyframe_interval_seconds;
  s.log_settings = log_settings;
  SetGlobalNvencSettings(s);
}

}  // namespace livekit_ffi

