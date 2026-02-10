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

#pragma once

#include <string>

#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace livekit_ffi {

// ============================================================================
// Encoder Selection Configuration
// ============================================================================

/// Encoder mode preference
enum class EncoderMode {
  Auto,          // Use best available (prefer hardware)
  SoftwareOnly,  // Force software encoders (libvpx, OpenH264, libaom)
  HardwareOnly,  // Force hardware encoders, fail if unavailable
};

/// Specific hardware backend to use (when mode is HardwareOnly or Auto)
enum class HardwareBackend {
  Any,              // First available hardware encoder
  NvidiaNvenc,      // NVIDIA NVENC (Windows/Linux)
  IntelQuickSync,   // Intel QuickSync via VAAPI (Linux) or MediaFoundation (Windows)
  AmdAmf,           // AMD AMF (future)
  AppleVideoToolbox,// Apple VideoToolbox (macOS/iOS)
  AndroidMediaCodec,// Android MediaCodec
};

/// Configuration for video encoder selection
struct VideoEncoderConfig {
  EncoderMode mode = EncoderMode::Auto;
  HardwareBackend backend = HardwareBackend::Any;
  bool allow_fallback = true;  // Fall back to software if hardware unavailable
  bool log_selection = true;   // Log which encoder was selected
  
  static VideoEncoderConfig Default() {
    return VideoEncoderConfig{};
  }
  
  static VideoEncoderConfig SoftwareOnly() {
    return VideoEncoderConfig{
      .mode = EncoderMode::SoftwareOnly,
      .backend = HardwareBackend::Any,
      .allow_fallback = false,
      .log_selection = true,
    };
  }
  
  static VideoEncoderConfig HardwarePreferred() {
    return VideoEncoderConfig{
      .mode = EncoderMode::Auto,
      .backend = HardwareBackend::Any,
      .allow_fallback = true,
      .log_selection = true,
    };
  }
  
  static VideoEncoderConfig NvencOnly(bool allow_fallback = false) {
    return VideoEncoderConfig{
      .mode = EncoderMode::HardwareOnly,
      .backend = HardwareBackend::NvidiaNvenc,
      .allow_fallback = allow_fallback,
      .log_selection = true,
    };
  }
};

/// Information about the selected encoder (for observability)
struct EncoderInfo {
  std::string name;             // e.g., "NVENC", "OpenH264", "libvpx"
  std::string codec;            // e.g., "H264", "VP8", "VP9"
  bool hardware_accelerated;
  HardwareBackend backend;      // Which backend (if hardware)
};

// ============================================================================
// Video Encoder Factory
// ============================================================================

class VideoEncoderFactory : public webrtc::VideoEncoderFactory {
  class InternalFactory : public webrtc::VideoEncoderFactory {
   public:
    explicit InternalFactory(const VideoEncoderConfig& config);

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode) const override;

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override;

    /// Get info about the last encoder that was created
    EncoderInfo GetLastEncoderInfo() const { return last_encoder_info_; }

   private:
    VideoEncoderConfig config_;
    std::vector<std::unique_ptr<webrtc::VideoEncoderFactory>> hardware_factories_;
    std::vector<std::unique_ptr<webrtc::VideoEncoderFactory>> software_factories_;
    mutable EncoderInfo last_encoder_info_;
    
    void InitializeFactories();
    bool ShouldUseHardware() const;
    bool IsBackendMatch(HardwareBackend actual, HardwareBackend requested) const;
  };

 public:
  VideoEncoderFactory();
  explicit VideoEncoderFactory(const VideoEncoderConfig& config);

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      std::optional<std::string> scalability_mode) const override;

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override;

  /// Get info about the last encoder that was created
  EncoderInfo GetLastEncoderInfo() const;

  /// Get the current configuration
  const VideoEncoderConfig& GetConfig() const { return config_; }

 private:
  VideoEncoderConfig config_;
  std::unique_ptr<InternalFactory> internal_factory_;
};

// ============================================================================
// Global Encoder Preference (Design A - Process-global)
// ============================================================================

/// Set the global encoder preference (must be called before creating factories)
void SetGlobalEncoderConfig(const VideoEncoderConfig& config);

/// Get the current global encoder preference
VideoEncoderConfig GetGlobalEncoderConfig();

/// Check if a specific hardware backend is available on this system
bool IsHardwareBackendAvailable(HardwareBackend backend);

/// Get list of available hardware backends on this system
std::vector<HardwareBackend> GetAvailableHardwareBackends();

// ============================================================================
// CXX Bridge Functions (use primitive types to avoid redefinition)
// ============================================================================

/// Set encoder config using primitive types (for CXX bridge)
/// mode: 0=Auto, 1=SoftwareOnly, 2=HardwareOnly
/// backend: 0=Any, 1=NvidiaNvenc, 2=IntelQuickSync, 3=AmdAmf, 4=AppleVideoToolbox, 5=AndroidMediaCodec
void set_encoder_config_raw(int32_t mode, int32_t backend, bool allow_fallback, bool log_selection);

/// Check if a hardware backend is available (for CXX bridge)
bool is_hardware_backend_available_raw(int32_t backend);

/// Get available backends as a bitmask (for CXX bridge)
/// Bit 0: NvidiaNvenc, Bit 1: IntelQuickSync, Bit 2: AmdAmf, Bit 3: AppleVideoToolbox, Bit 4: AndroidMediaCodec
uint32_t get_available_backends_mask();

/// Get supported codecs for a given hardware backend as a bitmask (for CXX bridge).
/// Bit 0: H264, Bit 1: H265, Bit 2: AV1
uint32_t get_hardware_backend_codecs_mask_raw(int32_t backend);

}  // namespace livekit_ffi
