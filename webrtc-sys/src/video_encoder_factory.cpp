/*
 * Copyright 2025 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "livekit/video_encoder_factory.h"

#include <cstdlib>
#include <mutex>
#include <string>

#include "api/environment/environment_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "livekit/objc_video_factory.h"
#include "media/base/media_constants.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "rtc_base/logging.h"

#if defined(RTC_USE_LIBAOM_AV1_ENCODER)
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#endif
#if defined(WEBRTC_USE_H264)
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#endif
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"

#ifdef WEBRTC_ANDROID
#include "livekit/android.h"
#endif

#if defined(USE_NVIDIA_VIDEO_ENCODER)
#include "nvidia/nvidia_encoder_factory.h"
#endif

#if defined(USE_VAAPI_VIDEO_CODEC)
#include "vaapi/vaapi_encoder_factory.h"
#endif

namespace livekit_ffi {

// ============================================================================
// Global Configuration (Design A - Process-global)
// ============================================================================

namespace {
std::mutex g_config_mutex;
VideoEncoderConfig g_global_config = VideoEncoderConfig::Default();
bool g_config_initialized = false;

// Parse encoder config from environment variables
// This allows Rust code to set preferences without CXX bridge
VideoEncoderConfig ParseEncoderConfigFromEnv() {
  VideoEncoderConfig config = VideoEncoderConfig::Default();
  
  // ZENSPEAK_ENCODER_MODE: auto, software, hardware
  if (const char* mode_str = std::getenv("ZENSPEAK_ENCODER_MODE")) {
    std::string mode(mode_str);
    if (mode == "software" || mode == "softwareOnly") {
      config.mode = EncoderMode::SoftwareOnly;
    } else if (mode == "hardware" || mode == "hardwareOnly") {
      config.mode = EncoderMode::HardwareOnly;
    } else {
      config.mode = EncoderMode::Auto;
    }
  }
  
  // ZENSPEAK_ENCODER_BACKEND: any, nvenc, vaapi, videotoolbox, mediacodec
  if (const char* backend_str = std::getenv("ZENSPEAK_ENCODER_BACKEND")) {
    std::string backend(backend_str);
    if (backend == "nvenc" || backend == "nvidia") {
      config.backend = HardwareBackend::NvidiaNvenc;
    } else if (backend == "vaapi" || backend == "quicksync" || backend == "intel") {
      config.backend = HardwareBackend::IntelQuickSync;
    } else if (backend == "videotoolbox" || backend == "apple") {
      config.backend = HardwareBackend::AppleVideoToolbox;
    } else if (backend == "mediacodec" || backend == "android") {
      config.backend = HardwareBackend::AndroidMediaCodec;
    } else if (backend == "amf" || backend == "amd") {
      config.backend = HardwareBackend::AmdAmf;
    } else {
      config.backend = HardwareBackend::Any;
    }
  }
  
  // ZENSPEAK_ENCODER_FALLBACK: 0/1
  if (const char* fallback_str = std::getenv("ZENSPEAK_ENCODER_FALLBACK")) {
    config.allow_fallback = (std::string(fallback_str) != "0");
  }
  
  // ZENSPEAK_ENCODER_LOG: 0/1
  if (const char* log_str = std::getenv("ZENSPEAK_ENCODER_LOG")) {
    config.log_selection = (std::string(log_str) != "0");
  }
  
  return config;
}

}  // namespace

void SetGlobalEncoderConfig(const VideoEncoderConfig& config) {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  g_global_config = config;
  g_config_initialized = true;
  if (config.log_selection) {
    RTC_LOG(LS_INFO) << "Global encoder config set: mode="
                     << static_cast<int>(config.mode)
                     << " backend=" << static_cast<int>(config.backend)
                     << " allow_fallback=" << config.allow_fallback;
  }
}

VideoEncoderConfig GetGlobalEncoderConfig() {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  // Lazy initialization from environment on first access
  if (!g_config_initialized) {
    g_global_config = ParseEncoderConfigFromEnv();
    g_config_initialized = true;
    if (g_global_config.log_selection) {
      RTC_LOG(LS_INFO) << "Encoder config initialized from environment: mode="
                       << static_cast<int>(g_global_config.mode)
                       << " backend=" << static_cast<int>(g_global_config.backend)
                       << " allow_fallback=" << g_global_config.allow_fallback;
    }
  }
  return g_global_config;
}

bool IsHardwareBackendAvailable(HardwareBackend backend) {
  switch (backend) {
    case HardwareBackend::Any:
      // Check if any hardware backend is available
#if defined(USE_NVIDIA_VIDEO_ENCODER)
      if (webrtc::NvidiaVideoEncoderFactory::IsSupported()) return true;
#endif
#if defined(USE_VAAPI_VIDEO_CODEC)
      if (webrtc::VAAPIVideoEncoderFactory::IsSupported()) return true;
#endif
#ifdef __APPLE__
      return true;  // VideoToolbox always available on Apple
#endif
#ifdef WEBRTC_ANDROID
      return true;  // MediaCodec available on Android
#endif
      return false;

    case HardwareBackend::NvidiaNvenc:
#if defined(USE_NVIDIA_VIDEO_ENCODER)
      return webrtc::NvidiaVideoEncoderFactory::IsSupported();
#else
      return false;
#endif

    case HardwareBackend::IntelQuickSync:
#if defined(USE_VAAPI_VIDEO_CODEC)
      return webrtc::VAAPIVideoEncoderFactory::IsSupported();
#else
      return false;
#endif

    case HardwareBackend::AppleVideoToolbox:
#ifdef __APPLE__
      return true;
#else
      return false;
#endif

    case HardwareBackend::AndroidMediaCodec:
#ifdef WEBRTC_ANDROID
      return true;
#else
      return false;
#endif

    case HardwareBackend::AmdAmf:
      // Not yet implemented
      return false;
  }
  return false;
}

std::vector<HardwareBackend> GetAvailableHardwareBackends() {
  std::vector<HardwareBackend> backends;
  
#if defined(USE_NVIDIA_VIDEO_ENCODER)
  if (webrtc::NvidiaVideoEncoderFactory::IsSupported()) {
    backends.push_back(HardwareBackend::NvidiaNvenc);
  }
#endif

#if defined(USE_VAAPI_VIDEO_CODEC)
  if (webrtc::VAAPIVideoEncoderFactory::IsSupported()) {
    backends.push_back(HardwareBackend::IntelQuickSync);
  }
#endif

#ifdef __APPLE__
  backends.push_back(HardwareBackend::AppleVideoToolbox);
#endif

#ifdef WEBRTC_ANDROID
  backends.push_back(HardwareBackend::AndroidMediaCodec);
#endif

  return backends;
}

// ============================================================================
// CXX Bridge Functions (primitive types)
// ============================================================================

void set_encoder_config_raw(int32_t mode, int32_t backend, bool allow_fallback, bool log_selection) {
  VideoEncoderConfig config;
  config.mode = static_cast<EncoderMode>(mode);
  config.backend = static_cast<HardwareBackend>(backend);
  config.allow_fallback = allow_fallback;
  config.log_selection = log_selection;
  SetGlobalEncoderConfig(config);
}

bool is_hardware_backend_available_raw(int32_t backend) {
  return IsHardwareBackendAvailable(static_cast<HardwareBackend>(backend));
}

uint32_t get_available_backends_mask() {
  uint32_t mask = 0;
  auto backends = GetAvailableHardwareBackends();
  for (const auto& b : backends) {
    switch (b) {
      case HardwareBackend::NvidiaNvenc:      mask |= (1 << 0); break;
      case HardwareBackend::IntelQuickSync:   mask |= (1 << 1); break;
      case HardwareBackend::AmdAmf:           mask |= (1 << 2); break;
      case HardwareBackend::AppleVideoToolbox: mask |= (1 << 3); break;
      case HardwareBackend::AndroidMediaCodec: mask |= (1 << 4); break;
      case HardwareBackend::Any: break;
    }
  }
  return mask;
}

uint32_t get_hardware_backend_codecs_mask_raw(int32_t backend) {
  const auto b = static_cast<HardwareBackend>(backend);

  uint32_t mask = 0;
  switch (b) {
    case HardwareBackend::Any: {
      // OR all hardware backends.
      const auto backends = GetAvailableHardwareBackends();
      for (const auto& hb : backends) {
        mask |= get_hardware_backend_codecs_mask_raw(static_cast<int32_t>(hb));
      }
      return mask;
    }
    case HardwareBackend::NvidiaNvenc: {
#if defined(USE_NVIDIA_VIDEO_ENCODER)
      if (!webrtc::NvidiaVideoEncoderFactory::IsSupported()) {
        return 0;
      }
      // H264 is always present for NVENC.
      mask |= (1u << 0);
#if defined(RTC_ENABLE_H265)
      mask |= (1u << 1);
#endif
      if (webrtc::NvidiaVideoEncoderFactory::IsAv1Supported()) {
        mask |= (1u << 2);
      }
      return mask;
#else
      return 0;
#endif
    }
    case HardwareBackend::IntelQuickSync:
    case HardwareBackend::AppleVideoToolbox:
    case HardwareBackend::AndroidMediaCodec:
    case HardwareBackend::AmdAmf:
      // Not implemented in this build.
      return 0;
  }
  return 0;
}

// ============================================================================
// Software Encoder Factory (libvpx, OpenH264, libaom)
// ============================================================================

using SoftwareFactory = webrtc::VideoEncoderFactoryTemplate<
    webrtc::LibvpxVp8EncoderTemplateAdapter,
#if defined(WEBRTC_USE_H264)
    webrtc::OpenH264EncoderTemplateAdapter,
#endif
#if defined(RTC_USE_LIBAOM_AV1_ENCODER)
    webrtc::LibaomAv1EncoderTemplateAdapter,
#endif
    webrtc::LibvpxVp9EncoderTemplateAdapter>;

// ============================================================================
// InternalFactory Implementation
// ============================================================================

VideoEncoderFactory::InternalFactory::InternalFactory(
    const VideoEncoderConfig& config)
    : config_(config) {
  InitializeFactories();
}

void VideoEncoderFactory::InternalFactory::InitializeFactories() {
  bool should_add_hardware = (config_.mode != EncoderMode::SoftwareOnly);
  bool should_add_software = (config_.mode != EncoderMode::HardwareOnly) ||
                              config_.allow_fallback;

  if (config_.log_selection) {
    RTC_LOG(LS_INFO) << "Initializing encoder factories: "
                     << "add_hardware=" << should_add_hardware
                     << " add_software=" << should_add_software
                     << " requested_backend=" << static_cast<int>(config_.backend);
  }

  // Add hardware factories based on configuration
  if (should_add_hardware) {
#ifdef __APPLE__
    if (config_.backend == HardwareBackend::Any ||
        config_.backend == HardwareBackend::AppleVideoToolbox) {
      hardware_factories_.push_back(livekit_ffi::CreateObjCVideoEncoderFactory());
      if (config_.log_selection) {
        RTC_LOG(LS_INFO) << "Added Apple VideoToolbox encoder factory";
      }
    }
#endif

#ifdef WEBRTC_ANDROID
    if (config_.backend == HardwareBackend::Any ||
        config_.backend == HardwareBackend::AndroidMediaCodec) {
      hardware_factories_.push_back(CreateAndroidVideoEncoderFactory());
      if (config_.log_selection) {
        RTC_LOG(LS_INFO) << "Added Android MediaCodec encoder factory";
      }
    }
#endif

#if defined(USE_NVIDIA_VIDEO_ENCODER)
    if (config_.backend == HardwareBackend::Any ||
        config_.backend == HardwareBackend::NvidiaNvenc) {
  if (webrtc::NvidiaVideoEncoderFactory::IsSupported()) {
        hardware_factories_.push_back(
            std::make_unique<webrtc::NvidiaVideoEncoderFactory>());
        if (config_.log_selection) {
          RTC_LOG(LS_INFO) << "Added NVIDIA NVENC encoder factory";
        }
      } else if (config_.log_selection) {
        RTC_LOG(LS_WARNING) << "NVIDIA NVENC requested but not supported on this system";
      }
    }
#endif

#if defined(USE_VAAPI_VIDEO_CODEC)
    if (config_.backend == HardwareBackend::Any ||
        config_.backend == HardwareBackend::IntelQuickSync) {
    if (webrtc::VAAPIVideoEncoderFactory::IsSupported()) {
        hardware_factories_.push_back(
            std::make_unique<webrtc::VAAPIVideoEncoderFactory>());
        if (config_.log_selection) {
          RTC_LOG(LS_INFO) << "Added VAAPI encoder factory";
        }
      } else if (config_.log_selection) {
        RTC_LOG(LS_WARNING) << "VAAPI requested but not supported on this system";
      }
    }
#endif
  }

  // Check if we got any hardware factories when hardware-only was requested
  if (config_.mode == EncoderMode::HardwareOnly &&
      hardware_factories_.empty() && !config_.allow_fallback) {
    RTC_LOG(LS_ERROR) << "HardwareOnly mode requested but no hardware encoders available!";
  }

  // Software factories are added to a separate list
  // (SoftwareFactory is used directly in Create())
  if (config_.log_selection) {
    RTC_LOG(LS_INFO) << "Encoder factory initialization complete: "
                     << hardware_factories_.size() << " hardware factories, "
                     << "software fallback " << (should_add_software ? "enabled" : "disabled");
  }
}

bool VideoEncoderFactory::InternalFactory::ShouldUseHardware() const {
  return config_.mode != EncoderMode::SoftwareOnly &&
         !hardware_factories_.empty();
}

bool VideoEncoderFactory::InternalFactory::IsBackendMatch(
    HardwareBackend actual,
    HardwareBackend requested) const {
  if (requested == HardwareBackend::Any) return true;
  return actual == requested;
}

std::vector<webrtc::SdpVideoFormat>
VideoEncoderFactory::InternalFactory::GetSupportedFormats() const {
  std::vector<webrtc::SdpVideoFormat> formats;

  // Add hardware formats first (if available and not software-only)
  if (config_.mode != EncoderMode::SoftwareOnly) {
    for (const auto& factory : hardware_factories_) {
      auto hw_formats = factory->GetSupportedFormats();
      formats.insert(formats.end(), hw_formats.begin(), hw_formats.end());
    }
  }

  // Add software formats (if not hardware-only, or as fallback)
  if (config_.mode != EncoderMode::HardwareOnly || config_.allow_fallback) {
    auto sw_formats = SoftwareFactory().GetSupportedFormats();
    formats.insert(formats.end(), sw_formats.begin(), sw_formats.end());
  }

  return formats;
}

VideoEncoderFactory::CodecSupport
VideoEncoderFactory::InternalFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    std::optional<std::string> scalability_mode) const {
  // Check hardware factories first
  for (const auto& factory : hardware_factories_) {
    auto support = factory->QueryCodecSupport(format, scalability_mode);
    if (support.is_supported) {
      return support;
    }
  }

  // Check software factory
  if (config_.mode != EncoderMode::HardwareOnly || config_.allow_fallback) {
  auto original_format =
        webrtc::FuzzyMatchSdpVideoFormat(SoftwareFactory().GetSupportedFormats(), format);
    if (original_format) {
      return SoftwareFactory().QueryCodecSupport(*original_format, scalability_mode);
    }
  }

  return webrtc::VideoEncoderFactory::CodecSupport{.is_supported = false};
}

std::unique_ptr<webrtc::VideoEncoder>
VideoEncoderFactory::InternalFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  
  // Try hardware factories first (if not software-only)
  if (config_.mode != EncoderMode::SoftwareOnly) {
    for (const auto& factory : hardware_factories_) {
    for (const auto& supported_format : factory->GetSupportedFormats()) {
        if (supported_format.IsSameCodec(format)) {
          auto encoder = factory->Create(env, format);
          if (encoder) {
            // Determine which backend this is
            HardwareBackend backend = HardwareBackend::Any;
#if defined(USE_NVIDIA_VIDEO_ENCODER)
            // Check if this is the NVIDIA factory
            if (dynamic_cast<webrtc::NvidiaVideoEncoderFactory*>(factory.get())) {
              backend = HardwareBackend::NvidiaNvenc;
            }
#endif
            last_encoder_info_ = EncoderInfo{
                .name = "Hardware",
                .codec = format.name,
                .hardware_accelerated = true,
                .backend = backend,
            };
            
            if (config_.log_selection) {
              RTC_LOG(LS_INFO) << "Selected hardware encoder for " << format.name
                               << " (backend=" << static_cast<int>(backend) << ")";
            }
            return encoder;
          }
        }
      }
    }
  }

  // Try software factory (if not hardware-only, or as fallback)
  if (config_.mode != EncoderMode::HardwareOnly || config_.allow_fallback) {
  auto original_format =
        webrtc::FuzzyMatchSdpVideoFormat(SoftwareFactory().GetSupportedFormats(), format);
  if (original_format) {
      auto encoder = SoftwareFactory().Create(env, *original_format);
      if (encoder) {
        std::string encoder_name = "Software";
#if defined(WEBRTC_USE_H264)
        if (format.name == "H264") encoder_name = "OpenH264";
#endif
        if (format.name == "VP8" || format.name == "VP9") encoder_name = "libvpx";
#if defined(RTC_USE_LIBAOM_AV1_ENCODER)
        if (format.name == "AV1") encoder_name = "libaom";
#endif
        
        last_encoder_info_ = EncoderInfo{
            .name = encoder_name,
            .codec = format.name,
            .hardware_accelerated = false,
            .backend = HardwareBackend::Any,
        };
        
        if (config_.log_selection) {
          RTC_LOG(LS_INFO) << "Selected software encoder: " << encoder_name
                           << " for " << format.name;
        }
        return encoder;
      }
    }
  }

  RTC_LOG(LS_ERROR) << "No VideoEncoder found for " << format.name
                    << " (mode=" << static_cast<int>(config_.mode) << ")";
  return nullptr;
}

// ============================================================================
// VideoEncoderFactory Implementation
// ============================================================================

VideoEncoderFactory::VideoEncoderFactory()
    : VideoEncoderFactory(GetGlobalEncoderConfig()) {}

VideoEncoderFactory::VideoEncoderFactory(const VideoEncoderConfig& config)
    : config_(config) {
  internal_factory_ = std::make_unique<InternalFactory>(config);
}

std::vector<webrtc::SdpVideoFormat> VideoEncoderFactory::GetSupportedFormats()
    const {
  return internal_factory_->GetSupportedFormats();
}

VideoEncoderFactory::CodecSupport VideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    std::optional<std::string> scalability_mode) const {
  return internal_factory_->QueryCodecSupport(format, scalability_mode);
}

std::unique_ptr<webrtc::VideoEncoder> VideoEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  std::unique_ptr<webrtc::VideoEncoder> encoder;
  if (format.IsCodecInList(internal_factory_->GetSupportedFormats())) {
    encoder = std::make_unique<webrtc::SimulcastEncoderAdapter>(
        env, internal_factory_.get(), nullptr, format);
  }
  return encoder;
}

EncoderInfo VideoEncoderFactory::GetLastEncoderInfo() const {
  return internal_factory_->GetLastEncoderInfo();
}

}  // namespace livekit_ffi
