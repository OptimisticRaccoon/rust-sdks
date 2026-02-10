// Must define WIN32_LEAN_AND_MEAN before any Windows headers to avoid WinSock conflicts
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include "nvidia_encoder_factory.h"

#include <memory>

#include "cuda_context.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "h264_encoder_impl.h"
#include "h265_encoder_impl.h"
#include "av1_encoder_impl.h"
#include "absl/container/inlined_vector.h"
#include "api/video_codecs/scalability_mode.h"
#include "rtc_base/logging.h"

namespace webrtc {

NvidiaVideoEncoderFactory::NvidiaVideoEncoderFactory() {
  std::map<std::string, std::string> baselineParameters = {
      {"profile-level-id", "42e01f"},
      {"level-asymmetry-allowed", "1"},
      {"packetization-mode", "1"},
  };
  supported_formats_.push_back(SdpVideoFormat("H264", baselineParameters));

  // Advertise HEVC/H265 with default parameters.
  supported_formats_.push_back(SdpVideoFormat("H265"));
  // Some stacks use 'HEVC' name.
  supported_formats_.push_back(SdpVideoFormat("HEVC"));

  // Advertise AV1 only if the driver actually supports it (Ada/Lovelace+ GPUs).
  // MUST include scalability_modes so WebRTC registers the Dependency Descriptor
  // (DD) RTP header extension in the SDP offer. Without this, the SFU cannot
  // parse frame dependencies and will never forward AV1 to subscribers.
  if (IsAv1Supported()) {
    absl::InlinedVector<ScalabilityMode, kScalabilityModeCount> av1_scalability = {
        ScalabilityMode::kL1T1,
    };
    supported_formats_.push_back(
        SdpVideoFormat(SdpVideoFormat::AV1Profile0(), av1_scalability));
  }

  /*std::map<std::string, std::string> highParameters = {
      {"profile-level-id", "4d0032"},
      {"level-asymmetry-allowed", "1"},
      {"packetization-mode", "1"},
  };

  supported_formats_.push_back(SdpVideoFormat("H264", highParameters));
  */
}

NvidiaVideoEncoderFactory::~NvidiaVideoEncoderFactory() {}

bool NvidiaVideoEncoderFactory::IsSupported() {
  if (!livekit_ffi::CudaContext::IsAvailable()) {
    RTC_LOG(LS_WARNING) << "Cuda Context is not available.";
    return false;
  }
  return true;
}

bool NvidiaVideoEncoderFactory::IsAv1Supported() {
  if (!IsSupported()) {
    return false;
  }

  livekit_ffi::CudaContext* ctx = livekit_ffi::CudaContext::GetInstance();
  if (!ctx->Initialize()) {
    return false;
  }

  try {
    // Create a minimal NVENC session and ask the driver which codec GUIDs it supports.
    NvEncoderCuda encoder(ctx->GetContext(), 64, 64, NV_ENC_BUFFER_FORMAT_NV12, 0);
    return encoder.IsEncodeGuidSupported(NV_ENC_CODEC_AV1_GUID);
  } catch (const NVENCException&) {
    return false;
  }
}

std::unique_ptr<VideoEncoder> NvidiaVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  // Check if the requested format is supported.
  for (const auto& supported_format : supported_formats_) {
    if (format.IsSameCodec(supported_format)) {
      if (!cu_context_) {
        cu_context_ = livekit_ffi::CudaContext::GetInstance();
        if (!cu_context_->Initialize()) {
          RTC_LOG(LS_ERROR) << "Failed to initialize CUDA context.";
          return nullptr;
        }
      }

      if (format.name == "H264") {
        RTC_LOG(LS_INFO) << "Using NVIDIA HW encoder (NVENC) for H264";
        return std::make_unique<NvidiaH264EncoderImpl>(
            env, cu_context_->GetContext(), CU_MEMORYTYPE_DEVICE,
            NV_ENC_BUFFER_FORMAT_IYUV, format);
      }

      if (format.name == "H265" || format.name == "HEVC") {
        RTC_LOG(LS_INFO) << "Using NVIDIA HW encoder (NVENC) for H265/HEVC";
        return std::make_unique<NvidiaH265EncoderImpl>(
            env, cu_context_->GetContext(), CU_MEMORYTYPE_DEVICE,
            NV_ENC_BUFFER_FORMAT_IYUV, format);
      }

      if (format.name == "AV1") {
        RTC_LOG(LS_INFO) << "Using NVIDIA HW encoder (NVENC) for AV1";
        return std::make_unique<NvidiaAv1EncoderImpl>(
            env, cu_context_->GetContext(), CU_MEMORYTYPE_DEVICE,
            NV_ENC_BUFFER_FORMAT_IYUV, format);
      }
    }
  }
  return nullptr;
}
std::vector<SdpVideoFormat> NvidiaVideoEncoderFactory::GetSupportedFormats()
    const {
  return supported_formats_;
}

std::vector<SdpVideoFormat> NvidiaVideoEncoderFactory::GetImplementations()
    const {
  return supported_formats_;
}

}  // namespace webrtc
