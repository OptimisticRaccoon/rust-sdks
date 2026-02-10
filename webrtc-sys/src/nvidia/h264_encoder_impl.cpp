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

#include "h264_encoder_impl.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "absl/strings/match.h"
#include "absl/types/optional.h"
#include "api/video/video_codec_constants.h"
#include "api/video_codecs/scalability_mode.h"
#include <common_video/h264/h264_common.h>
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/svc/create_scalability_structure.h"
#include "modules/video_coding/utility/simulcast_rate_allocator.h"
#include "modules/video_coding/utility/simulcast_utility.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "system_wrappers/include/metrics.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/scale.h"

#include "livekit/nvenc_settings.h"

#ifdef _WIN32
#include "livekit/d3d11_frame_buffer.h"
#endif

namespace webrtc {

namespace {
// Avoid stdout I/O from hot paths in production. Enable only for bring-up:
//   set ZENSPEAK_NVENC_DEBUG=1
bool NvencStdoutDebugEnabled() {
  static const bool enabled = []() -> bool {
    const char* v = std::getenv("ZENSPEAK_NVENC_DEBUG");
    if (!v || *v == '\0') return false;
    // Treat "0" as disabled.
    return !(v[0] == '0' && v[1] == '\0');
  }();
  return enabled;
}

GUID NvencPresetGuid(const livekit_ffi::NvencSettings& s) {
  switch (s.preset) {
    case livekit_ffi::NvencPreset::UltraLowLatency:
      return NV_ENC_PRESET_P1_GUID;
    case livekit_ffi::NvencPreset::LowLatency:
      return NV_ENC_PRESET_P4_GUID;
    case livekit_ffi::NvencPreset::Quality:
      return NV_ENC_PRESET_P7_GUID;
  }
  return NV_ENC_PRESET_P4_GUID;
}

NV_ENC_TUNING_INFO NvencTuningInfo(const livekit_ffi::NvencSettings& s) {
  switch (s.preset) {
    case livekit_ffi::NvencPreset::UltraLowLatency:
      return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    case livekit_ffi::NvencPreset::LowLatency:
      // Keep legacy behavior: we used ULTRA_LOW_LATENCY tuning with a balanced preset (P4).
      return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    case livekit_ffi::NvencPreset::Quality:
      return NV_ENC_TUNING_INFO_HIGH_QUALITY;
  }
  return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
}

absl::optional<GUID> NvencProfileGuidOverride(const livekit_ffi::NvencSettings& s) {
  switch (s.profile) {
    case livekit_ffi::NvencProfile::Auto:
      return absl::nullopt;
    case livekit_ffi::NvencProfile::Baseline:
      return NV_ENC_H264_PROFILE_BASELINE_GUID;
    case livekit_ffi::NvencProfile::Main:
      return NV_ENC_H264_PROFILE_MAIN_GUID;
    case livekit_ffi::NvencProfile::High:
      return NV_ENC_H264_PROFILE_HIGH_GUID;
  }
  return absl::nullopt;
}

uint32_t NvencClampCqp(int32_t cqp) {
  // NVENC QP values are unsigned; keep this conservative.
  // Typical "good" range for screenshare: ~18-28.
  const int32_t clamped = std::clamp<int32_t>(cqp, 0, 51);
  return static_cast<uint32_t>(clamped);
}
}  // namespace

// Used by histograms. Values of entries should not be changed.
enum H264EncoderImplEvent {
  kH264EncoderEventInit = 0,
  kH264EncoderEventError = 1,
  kH264EncoderEventMax = 16,
};


NV_ENC_LEVEL H264LevelToNvEncLevel(webrtc::H264Level level) {
  switch (level) {
    case H264Level::kLevel1_b:
      return NV_ENC_LEVEL_H264_1b;
    case H264Level::kLevel1:
      return NV_ENC_LEVEL_H264_1;
    case H264Level::kLevel1_1:
      return NV_ENC_LEVEL_H264_11;
    case H264Level::kLevel1_2:
      return NV_ENC_LEVEL_H264_12;
    case H264Level::kLevel1_3:
      return NV_ENC_LEVEL_H264_13;
    case H264Level::kLevel2:
      return NV_ENC_LEVEL_H264_2;
    case H264Level::kLevel2_1:
      return NV_ENC_LEVEL_H264_21;
    case H264Level::kLevel2_2:
      return NV_ENC_LEVEL_H264_22;
    case H264Level::kLevel3:
      return NV_ENC_LEVEL_H264_3;
    case H264Level::kLevel3_1:
      return NV_ENC_LEVEL_H264_31;
    case H264Level::kLevel3_2:
      return NV_ENC_LEVEL_H264_32;
    case H264Level::kLevel4:
      return NV_ENC_LEVEL_H264_4;
    case H264Level::kLevel4_1:
      return NV_ENC_LEVEL_H264_41;
    case H264Level::kLevel4_2:
      return NV_ENC_LEVEL_H264_42;
    case H264Level::kLevel5:
      return NV_ENC_LEVEL_H264_5;
    case H264Level::kLevel5_1:
      return NV_ENC_LEVEL_H264_51;
    case H264Level::kLevel5_2:
      return NV_ENC_LEVEL_H264_52;
  }
  return NV_ENC_LEVEL_AUTOSELECT;  // Default value.
}


NvidiaH264EncoderImpl::NvidiaH264EncoderImpl(
    const webrtc::Environment& env,
    CUcontext context,
    CUmemorytype memory_type,
    NV_ENC_BUFFER_FORMAT nv_format,
    const SdpVideoFormat& format)
    : env_(env),
      encoder_(nullptr),
      cu_context_(context),
      cu_memory_type_(memory_type),
      cu_scaled_array_(nullptr),
      nv_format_(nv_format),
      packetization_mode_(
          H264EncoderSettings::Parse(format).packetization_mode),
      format_(format) {
  std::string hexString = format_.parameters.at("profile-level-id");
  std::optional<webrtc::H264ProfileLevelId> profile_level_id =
      webrtc::ParseH264ProfileLevelId(hexString.c_str());
  if (profile_level_id.has_value()) {
    profile_ = profile_level_id->profile;
    level_ = profile_level_id->level;
  }

  nv_enc_level_ = NV_ENC_LEVEL_AUTOSELECT;
  if (level_ != H264Level::kLevel1_b) {
    // Convert H264Level to NV_ENC_LEVEL.
    nv_enc_level_ = webrtc::H264LevelToNvEncLevel(level_);
  }

  // IMPORTANT: NVENC requires a valid profile GUID. If we leave it uninitialized,
  // nvEncInitializeEncoder commonly fails with NV_ENC_ERR_INVALID_PARAM.
  // Map WebRTC profile to NVENC profile GUIDs.
  switch (profile_) {
    case H264Profile::kProfileConstrainedBaseline:
    case H264Profile::kProfileBaseline:
      nv_profile_guid_ = NV_ENC_H264_PROFILE_BASELINE_GUID;
      break;
    case H264Profile::kProfileMain:
      nv_profile_guid_ = NV_ENC_H264_PROFILE_MAIN_GUID;
      break;
    case H264Profile::kProfileConstrainedHigh:
      nv_profile_guid_ = NV_ENC_H264_PROFILE_CONSTRAINED_HIGH_GUID;
      break;
    case H264Profile::kProfileHigh:
      nv_profile_guid_ = NV_ENC_H264_PROFILE_HIGH_GUID;
      break;
    default:
      nv_profile_guid_ = NV_ENC_H264_PROFILE_BASELINE_GUID;
      break;
  }

  RTC_CHECK_NE(cu_memory_type_, CU_MEMORYTYPE_HOST);
}

NvidiaH264EncoderImpl::~NvidiaH264EncoderImpl() {
  Release();
}

void NvidiaH264EncoderImpl::ReportInit() {
  if (has_reported_init_)
    return;
  RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
                            kH264EncoderEventInit, kH264EncoderEventMax);
  has_reported_init_ = true;
}

void NvidiaH264EncoderImpl::ReportError() {
  if (has_reported_error_)
    return;
  RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
                            kH264EncoderEventError, kH264EncoderEventMax);
  has_reported_error_ = true;
}

int32_t NvidiaH264EncoderImpl::InitEncode(
    const VideoCodec* inst,
    const VideoEncoder::Settings& settings) {
  if (!inst || inst->codecType != kVideoCodecH264) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->maxFramerate == 0) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->width < 1 || inst->height < 1) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  int32_t release_ret = Release();
  if (release_ret != WEBRTC_VIDEO_CODEC_OK) {
    ReportError();
    return release_ret;
  }

  codec_ = *inst;
  if (NvencStdoutDebugEnabled()) {
    std::cout << "NVENC InitEncode: " << inst->width << "x" << inst->height
              << " @ " << inst->maxFramerate << "fps"
              << ", simulcast_streams=" << static_cast<int>(inst->numberOfSimulcastStreams)
              << ", startBitrate=" << inst->startBitrate << "kbps" << std::endl;
  }

  // Code expects simulcastStream resolutions to be correct, make sure they are
  // filled even when there are no simulcast layers.
  if (codec_.numberOfSimulcastStreams == 0) {
    codec_.simulcastStream[0].width = codec_.width;
    codec_.simulcastStream[0].height = codec_.height;
  }

  // Initialize encoded image. Default buffer size: size of unencoded data.
  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, codec_.width, codec_.height);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  encoded_image_.set_size(0);

  configuration_.sending = false;
  configuration_.frame_dropping_on = codec_.GetFrameDropEnabled();
  configuration_.key_frame_interval = codec_.H264()->keyFrameInterval;

  configuration_.width = codec_.width;
  configuration_.height = codec_.height;

  configuration_.max_frame_rate = codec_.maxFramerate;
  configuration_.target_bps = codec_.startBitrate * 1000;
  configuration_.max_bps = codec_.maxBitrate * 1000;

  // IMPORTANT:
  // Do NOT initialize NVENC here. WebRTC calls InitEncode() before any frames are
  // delivered and before we know whether we will receive native GPU frames or
  // CPU/I420 frames. Initializing NvEncoderCuda here can fail (NV_ENC_ERR_INVALID_PARAM)
  // and will prevent the D3D11 zero-copy path from ever being attempted.
  //
  // We instead lazily initialize:
  // - NvEncoderD3D11 on the first native D3D11 texture in Encode()
  // - NvEncoderCuda only if/when we actually need the CPU path.
  encoder_ = nullptr;

  SimulcastRateAllocator init_allocator(env_, codec_);
  VideoBitrateAllocation allocation =
      init_allocator.Allocate(VideoBitrateAllocationParameters(
          DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));
  return WEBRTC_VIDEO_CODEC_OK;
}

bool NvidiaH264EncoderImpl::EnsureCudaEncoderInitialized(uint32_t width,
                                                        uint32_t height) {
  if (encoder_) {
    return true;
  }

  const CUresult result = cuCtxSetCurrent(cu_context_);
  if (result != CUDA_SUCCESS) {
    RTC_LOG(LS_ERROR) << "EnsureCudaEncoderInitialized: cuCtxSetCurrent failed";
    return false;
  }

  // Some NVIDIA GPUs have a limited Encode Session count.
  // We can't get the Session count, so catching NVENCException to avoid the crash.
  try {
    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      encoder_ =
          std::make_unique<NvEncoderCuda>(cu_context_, width, height, nv_format_, 0);
    } else {
      RTC_DCHECK_NOTREACHED();
      return false;
    }
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "EnsureCudaEncoderInitialized: Failed to create NvEncoderCuda: "
                      << e.what();
    encoder_ = nullptr;
    return false;
  }

  nv_initialize_params_ = {};
  nv_encode_config_ = {};
  nv_initialize_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
  nv_encode_config_.version = NV_ENC_CONFIG_VER;
  nv_initialize_params_.encodeConfig = &nv_encode_config_;

  GUID encodeGuid = NV_ENC_CODEC_H264_GUID;
  const livekit_ffi::NvencSettings nvenc = livekit_ffi::GetGlobalNvencSettings();
  GUID presetGuid = NvencPresetGuid(nvenc);

  encoder_->CreateDefaultEncoderParams(&nv_initialize_params_, encodeGuid,
                                       presetGuid, NvencTuningInfo(nvenc));

  uint32_t fps = static_cast<uint32_t>(configuration_.max_frame_rate);
  if (fps == 0) fps = 30;
  nv_initialize_params_.frameRateNum = fps;
  nv_initialize_params_.frameRateDen = 1;
  nv_initialize_params_.bufferFormat = nv_format_;

  if (auto profile_override = NvencProfileGuidOverride(nvenc)) {
    nv_encode_config_.profileGUID = *profile_override;
  } else {
    nv_encode_config_.profileGUID = nv_profile_guid_;
  }
  nv_encode_config_.gopLength = NVENC_INFINITE_GOPLENGTH;
  nv_encode_config_.frameIntervalP = 1;
  nv_encode_config_.encodeCodecConfig.h264Config.level = nv_enc_level_;
  nv_encode_config_.encodeCodecConfig.h264Config.idrPeriod =
      NVENC_INFINITE_GOPLENGTH;

  nv_encode_config_.rcParams.version = NV_ENC_RC_PARAMS_VER;
  switch (nvenc.rate_control) {
    case livekit_ffi::NvencRateControl::Cbr:
      nv_encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
      break;
    case livekit_ffi::NvencRateControl::Vbr:
      nv_encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
      break;
    case livekit_ffi::NvencRateControl::Cqp:
      nv_encode_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
      {
        const uint32_t qp = NvencClampCqp(nvenc.cqp);
        nv_encode_config_.rcParams.constQP = {qp, qp, qp};
      }
      break;
  }

  uint32_t target_bps = configuration_.target_bps;
  uint32_t max_bps = configuration_.max_bps;
  if (target_bps == 0) target_bps = 4000000;
  if (max_bps == 0) max_bps = target_bps * 2;

  nv_encode_config_.rcParams.averageBitRate = target_bps;
  nv_encode_config_.rcParams.maxBitRate = max_bps;

  // Conservative VBV for realtime.
  nv_encode_config_.rcParams.vbvBufferSize = max_bps;
  nv_encode_config_.rcParams.vbvInitialDelay = max_bps / 2;

  if (nvenc.keyframe_interval_seconds > 0 && fps > 0) {
    const uint32_t keyint =
        std::max<uint32_t>(1u, fps * static_cast<uint32_t>(nvenc.keyframe_interval_seconds));
    nv_encode_config_.gopLength = keyint;
    nv_encode_config_.encodeCodecConfig.h264Config.idrPeriod = keyint;
  }

  try {
    encoder_->CreateEncoder(&nv_initialize_params_);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "EnsureCudaEncoderInitialized: CreateEncoder failed: " << e.what();
    encoder_->DestroyEncoder();
    encoder_ = nullptr;
    return false;
  }

  RTC_LOG(LS_INFO) << "NVENC: Initialized CUDA (I420) fallback encoder: "
                   << width << "x" << height << " @" << fps << "fps";
  return true;
}

int32_t NvidiaH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH264EncoderImpl::Release() {
#ifdef _WIN32
  // Release D3D11 encoder
  if (d3d11_encoder_) {
    d3d11_encoder_->DestroyEncoder();
    d3d11_encoder_ = nullptr;
  }
  d3d11_encoder_initialized_ = false;
  d3d11_encoder_init_failed_ = false;  // Reset so next session can try again
#endif

  if (encoder_) {
    encoder_->DestroyEncoder();
    encoder_ = nullptr;
  }
  if (cu_scaled_array_) {
    cuArrayDestroy(cu_scaled_array_);
    cu_scaled_array_ = nullptr;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!encoded_image_callback_) {
    RTC_LOG(LS_WARNING)
        << "InitEncode() has been called, but a callback function "
           "has not been set with RegisterEncodeCompleteCallback()";
    ReportError();
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  if (!configuration_.sending) {
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  if (frame_types != nullptr && !frame_types->empty()) {
    // Skip frame?
    if ((*frame_types)[0] == VideoFrameType::kEmptyFrame) {
      return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }
  }

#ifdef _WIN32
  // Check if this is a D3D11 texture (GPU-backed frame)
  // This enables the zero-copy GPU path via NvEncoderD3D11
  rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer = input_frame.video_frame_buffer();
  
  static std::atomic<bool> first_encode{true};
  static std::atomic<uint32_t> encode_log_counter{0};
  const uint32_t encode_log_n =
      encode_log_counter.fetch_add(1, std::memory_order_relaxed);
  const bool log_this = (encode_log_n == 0) || (encode_log_n % 120 == 0);
  bool expected = true;
  if (first_encode.compare_exchange_strong(expected, false)) {
    if (NvencStdoutDebugEnabled()) {
      std::cout << "NVENC Encode: first frame " << buffer->width() << "x" << buffer->height()
                << ", buffer type=" << static_cast<int>(buffer->type())
                << " (kNative=" << static_cast<int>(VideoFrameBuffer::Type::kNative) << ")"
                << ", encoder expects " << configuration_.width << "x" << configuration_.height
                << std::endl;
    }
  }
  
  if (buffer->type() == VideoFrameBuffer::Type::kNative) {
    // Try to get the D3D11 texture from our custom buffer type
    auto* d3d11_buffer = dynamic_cast<livekit_ffi::D3D11TextureBuffer*>(buffer.get());

    if (NvencStdoutDebugEnabled() && log_this) {
      std::cout << "NVENC: kNative buffer detected, d3d11_buffer="
                << (d3d11_buffer ? "valid" : "NULL") << std::endl;
    }

    if (d3d11_buffer && d3d11_buffer->texture() && d3d11_buffer->device()) {
      if (NvencStdoutDebugEnabled() && log_this) {
        std::cout << "NVENC: Trying D3D11 GPU texture path (zero-copy)" << std::endl;
      }
      int32_t result = EncodeD3D11Texture(d3d11_buffer->texture(), 
                                           d3d11_buffer->device(),
                                           input_frame, frame_types);
      if (NvencStdoutDebugEnabled() && (result != 0 || log_this)) {
        std::cout << "NVENC: EncodeD3D11Texture returned " << result << std::endl;
      }
      // Only return if successful or actual error - fall through on FALLBACK_SOFTWARE
      if (result != WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE) {
        return result;
      }
      // D3D11 path failed, fall through to CPU path
      if (NvencStdoutDebugEnabled()) {
        std::cout << "NVENC: D3D11 path unavailable, using CPU readback + CUDA encoder" << std::endl;
      }
    } else {
      // Not our D3D11 buffer type - fall through to CPU path
      if (NvencStdoutDebugEnabled()) {
        std::cout << "NVENC: kNative buffer but dynamic_cast failed or null texture/device" << std::endl;
      }
      RTC_LOG(LS_WARNING) << "NVENC: Unknown native buffer type, falling back to CPU path";
    }
  }
#endif

  // CPU path: convert to I420 and upload to CUDA
  webrtc::scoped_refptr<I420BufferInterface> frame_buffer =
      input_frame.video_frame_buffer()->ToI420();
  if (!frame_buffer) {
    RTC_LOG(LS_ERROR) << "Failed to convert "
                      << VideoFrameBufferTypeToString(
                             input_frame.video_frame_buffer()->type())
                      << " image to I420. Can't encode frame.";
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }
  RTC_CHECK(frame_buffer->type() == VideoFrameBuffer::Type::kI420);

  bool is_keyframe_needed = false;
  if (configuration_.key_frame_request && configuration_.sending) {
    is_keyframe_needed = true;
  }

  bool send_key_frame =
      is_keyframe_needed ||
      (frame_types && !frame_types->empty() &&
       (*frame_types)[0] == VideoFrameType::kVideoFrameKey);
  if (send_key_frame) {
    is_keyframe_needed = true;
    configuration_.key_frame_request = false;
  }

  RTC_DCHECK_EQ(configuration_.width, frame_buffer->width());
  RTC_DCHECK_EQ(configuration_.height, frame_buffer->height());

  if (!EnsureCudaEncoderInitialized(frame_buffer->width(), frame_buffer->height())) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  try {
    const NvEncInputFrame* nv_enc_input_frame = encoder_->GetNextInputFrame();

    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      NvEncoderCuda::CopyToDeviceFrame(
          cu_context_, (void*)frame_buffer->DataY(), frame_buffer->StrideY(),
          reinterpret_cast<CUdeviceptr>(nv_enc_input_frame->inputPtr),
          nv_enc_input_frame->pitch, input_frame.width(), input_frame.height(),
          CU_MEMORYTYPE_HOST, nv_enc_input_frame->bufferFormat,
          nv_enc_input_frame->chromaOffsets, nv_enc_input_frame->numChromaPlanes);
    }

    NV_ENC_PIC_PARAMS pic_params = NV_ENC_PIC_PARAMS();
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.encodePicFlags = 0;
    if (is_keyframe_needed) {
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA |
                                  NV_ENC_PIC_FLAG_FORCEIDR |
                                  NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
      configuration_.key_frame_request = false;
    }

    std::vector<std::vector<uint8_t>> bit_stream;
    encoder_->EncodeFrame(bit_stream, &pic_params);

    for (std::vector<uint8_t>& packet : bit_stream) {
      int32_t result = ProcessEncodedFrame(packet, input_frame);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
    }
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "Failed EncodeFrame NvEncoder " << e.what();
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaH264EncoderImpl::ProcessEncodedFrame(
    std::vector<uint8_t>& packet,
    const ::webrtc::VideoFrame& inputFrame) {
  // In the D3D11 zero-copy path we use `d3d11_encoder_` and `encoder_` (CUDA)
  // may remain null. Avoid null-deref by selecting the active encoder.
#ifdef _WIN32
  if (d3d11_encoder_) {
    encoded_image_._encodedWidth = d3d11_encoder_->GetEncodeWidth();
    encoded_image_._encodedHeight = d3d11_encoder_->GetEncodeHeight();
  } else
#endif
  if (encoder_) {
    encoded_image_._encodedWidth = encoder_->GetEncodeWidth();
    encoded_image_._encodedHeight = encoder_->GetEncodeHeight();
  } else {
    // Fallback (should not happen in normal operation).
    encoded_image_._encodedWidth = configuration_.width;
    encoded_image_._encodedHeight = configuration_.height;
  }
  encoded_image_.SetRtpTimestamp(inputFrame.rtp_timestamp());
  encoded_image_.SetSimulcastIndex(0);
  encoded_image_.ntp_time_ms_ = inputFrame.ntp_time_ms();
  encoded_image_.capture_time_ms_ = inputFrame.render_time_ms();
  encoded_image_.rotation_ = inputFrame.rotation();
  encoded_image_.content_type_ = VideoContentType::UNSPECIFIED;
  encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
  encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;
  encoded_image_.SetColorSpace(inputFrame.color_space());
  std::vector<H264::NaluIndex> naluIndices =
      H264::FindNaluIndices(MakeArrayView(packet.data(), packet.size()));
  for (uint32_t i = 0; i < naluIndices.size(); i++) {
    const H264::NaluType naluType =
        H264::ParseNaluType(packet[naluIndices[i].payload_start_offset]);
    if (naluType == H264::kIdr) {
      encoded_image_._frameType = VideoFrameType::kVideoFrameKey;
      break;
    }
  }

  encoded_image_.SetEncodedData(
      EncodedImageBuffer::Create(packet.data(), packet.size()));
  encoded_image_.set_size(packet.size());

  h264_bitstream_parser_.ParseBitstream(encoded_image_);
  encoded_image_.qp_ = h264_bitstream_parser_.GetLastSliceQp().value_or(-1);

  CodecSpecificInfo codecInfo;
  codecInfo.codecType = kVideoCodecH264;
  codecInfo.codecSpecific.H264.packetization_mode =
      H264PacketizationMode::NonInterleaved;

  const auto result =
      encoded_image_callback_->OnEncodedImage(encoded_image_, &codecInfo);
  if (result.error != EncodedImageCallback::Result::OK) {
    RTC_LOG(LS_ERROR) << "Encode m_encodedCompleteCallback failed "
                      << result.error;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo NvidiaH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  // Enable native handle support for GPU frame passthrough (D3D11 textures)
  // This tells WebRTC that we can consume GPU-backed frames directly
  info.supports_native_handle = true;
  info.implementation_name = "NVIDIA H264 Encoder";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  // Accept both I420 (CPU) and kNative (GPU D3D11 textures)
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kNative, VideoFrameBuffer::Type::kI420};
  return info;
}

void NvidiaH264EncoderImpl::SetRates(
    const RateControlParameters& parameters) {
  if (parameters.framerate_fps < 1.0) {
    RTC_LOG(LS_WARNING) << "Invalid frame rate: " << parameters.framerate_fps;
    return;
  }

  const uint32_t target_bps =
      static_cast<uint32_t>(parameters.bitrate.get_sum_bps());
  if (target_bps == 0) {
    configuration_.SetStreamState(false);
    return;
  }

  const uint32_t fps =
      std::max<uint32_t>(1u, static_cast<uint32_t>(parameters.framerate_fps));

  // NOTE: webrtc::VideoCodec uses kbps for bitrates; RateControlParameters uses bps.
  // Keep both representations correct.
  codec_.maxFramerate = fps;
  codec_.maxBitrate = target_bps / 1000;

  configuration_.target_bps = target_bps;
  configuration_.max_frame_rate = parameters.framerate_fps;

  // WebRTC doesn't provide a separate "max bitrate" here; for NVENC we ensure max >= avg.
  // Keep existing max if it's higher; otherwise track target.
  uint32_t max_bps = configuration_.max_bps;
  if (max_bps == 0 || max_bps < target_bps) {
    max_bps = target_bps;
  }
  configuration_.max_bps = max_bps;

  if (configuration_.target_bps) {
    configuration_.SetStreamState(true);
  } else {
    configuration_.SetStreamState(false);
  }

  // If NVENC is already running, reconfigure it to track WebRTC's bandwidth estimate.
  // This is critical for "production-ready" behavior; otherwise the encoder can stay
  // stuck at the initial (often low) startup bitrate.
  const int64_t now_ms = rtc::TimeMillis();
  const bool values_changed =
      (last_applied_target_bps_ != target_bps) ||
      (last_applied_max_bps_ != max_bps) ||
      (last_applied_fps_ != fps);
  const bool too_soon = (now_ms - last_reconfigure_time_ms_) < 200;

  static std::atomic<uint32_t> setrates_log_counter{0};
  const uint32_t setrates_n =
      setrates_log_counter.fetch_add(1, std::memory_order_relaxed);
  const bool log_setrates = (setrates_n == 0) || (setrates_n % 30 == 0);

  if (values_changed && !too_soon) {
#ifdef _WIN32
    if (d3d11_encoder_initialized_ && d3d11_encoder_) {
      try {
        NV_ENC_RECONFIGURE_PARAMS reconfig = {};
        reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;

        NV_ENC_INITIALIZE_PARAMS initParams = {};
        NV_ENC_CONFIG encodeConfig = {};
        initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        encodeConfig.version = NV_ENC_CONFIG_VER;
        initParams.encodeConfig = &encodeConfig;

        d3d11_encoder_->GetInitializeParams(&initParams);

        // Update rate control + framerate.
        initParams.frameRateNum = fps;
        initParams.frameRateDen = 1;

        // IMPORTANT:
        // WebRTC calls SetRates() frequently as bandwidth estimates change. NVENC's reconfigure
        // path can fail if we attempt "structural" changes (NVENC docs: incompatible changes may
        // fail, e.g. GOP structure). To keep reconfigure robust, we only update bitrate-related
        // fields + FPS here and avoid touching preset/profile/rate-control mode.
        //
        // This also avoids accidentally overriding the encoder's initialization configuration
        // (e.g. if global NVENC settings select VBR/CQP).
        encodeConfig.rcParams.averageBitRate = target_bps;
        encodeConfig.rcParams.maxBitRate = max_bps;
        encodeConfig.rcParams.vbvBufferSize = max_bps;
        encodeConfig.rcParams.vbvInitialDelay = max_bps / 2;

        reconfig.reInitEncodeParams = initParams;
        // Don't force IDR for every bandwidth update.
        reconfig.resetEncoder = 0;
        reconfig.forceIDR = 0;

        d3d11_encoder_->Reconfigure(&reconfig);
        if (NvencStdoutDebugEnabled() && log_setrates) {
          std::cout << "NvEncoderD3D11 Reconfigure OK: "
                    << (target_bps / 1000) << "kbps (max "
                    << (max_bps / 1000) << "kbps) @ " << fps << "fps"
                    << std::endl;
        }
      } catch (const NVENCException& e) {
        RTC_LOG(LS_WARNING) << "NvEncoderD3D11 Reconfigure failed: " << e.what();
        if (NvencStdoutDebugEnabled()) {
          std::cout << "NvEncoderD3D11 Reconfigure failed: " << e.what() << std::endl;
        }
      }
    }
#endif  // _WIN32

    if (encoder_) {
      try {
        NV_ENC_RECONFIGURE_PARAMS reconfig = {};
        reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;

        NV_ENC_INITIALIZE_PARAMS initParams = {};
        NV_ENC_CONFIG encodeConfig = {};
        initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        encodeConfig.version = NV_ENC_CONFIG_VER;
        initParams.encodeConfig = &encodeConfig;

        encoder_->GetInitializeParams(&initParams);

        initParams.frameRateNum = fps;
        initParams.frameRateDen = 1;

        // See D3D11 path notes above: keep existing rate-control mode/profile, only update bitrate + FPS.
        encodeConfig.rcParams.averageBitRate = target_bps;
        encodeConfig.rcParams.maxBitRate = max_bps;
        encodeConfig.rcParams.vbvBufferSize = max_bps;
        encodeConfig.rcParams.vbvInitialDelay = max_bps / 2;

        reconfig.reInitEncodeParams = initParams;
        reconfig.resetEncoder = 0;
        reconfig.forceIDR = 0;

        encoder_->Reconfigure(&reconfig);
        if (NvencStdoutDebugEnabled() && log_setrates) {
          std::cout << "NvEncoderCuda Reconfigure OK: "
                    << (target_bps / 1000) << "kbps (max "
                    << (max_bps / 1000) << "kbps) @ " << fps << "fps"
                    << std::endl;
        }
      } catch (const NVENCException& e) {
        RTC_LOG(LS_WARNING) << "NvEncoderCuda Reconfigure failed: " << e.what();
        if (NvencStdoutDebugEnabled()) {
          std::cout << "NvEncoderCuda Reconfigure failed: " << e.what() << std::endl;
        }
      }
    }

    last_applied_target_bps_ = target_bps;
    last_applied_max_bps_ = max_bps;
    last_applied_fps_ = fps;
    last_reconfigure_time_ms_ = now_ms;
  } else if (log_setrates && !too_soon) {
    // Useful to confirm WebRTC is actually driving bitrate, even if values didn't change.
    if (NvencStdoutDebugEnabled()) {
      std::cout << "NVENC SetRates: "
                << (target_bps / 1000) << "kbps (max "
                << (max_bps / 1000) << "kbps) @ " << fps << "fps"
                << " (d3d11=" << (d3d11_encoder_initialized_ ? "on" : "off")
                << ", cuda=" << (encoder_ ? "on" : "off") << ")"
                << std::endl;
    }
  }
}

void NvidiaH264EncoderImpl::LayerConfig::SetStreamState(bool send_stream) {
  if (send_stream && !sending) {
    // Need a key frame if we have not sent this stream before.
    key_frame_request = true;
  }
  sending = send_stream;
}

#ifdef _WIN32
// ============================================================================
// D3D11 Zero-Copy Encoding via NvEncoderD3D11
// ============================================================================

bool NvidiaH264EncoderImpl::EnsureD3D11EncoderInitialized(
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    NV_ENC_BUFFER_FORMAT format) {
  
  // Already initialized
  if (d3d11_encoder_initialized_ && d3d11_encoder_) {
    return true;
  }
  
  // Remember if init already failed - don't retry every frame
  if (d3d11_encoder_init_failed_) {
    return false;
  }

  try {
    // Create D3D11 encoder
    d3d11_encoder_ = std::make_unique<NvEncoderD3D11>(
        device, width, height, format/*, nExtraOutputDelay=1*/);
    
    // Use the SDK's recommended pattern: get default params, modify, then create
    NV_ENC_INITIALIZE_PARAMS initParams = {};
    NV_ENC_CONFIG encodeConfig = {};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeConfig = &encodeConfig;
    
    // Get default params for H264 with low-latency tuning
    if (NvencStdoutDebugEnabled()) {
      std::cout << "NvEncoderD3D11: Calling CreateDefaultEncoderParams..." << std::endl;
    }
    
    const livekit_ffi::NvencSettings nvenc = livekit_ffi::GetGlobalNvencSettings();
    d3d11_encoder_->CreateDefaultEncoderParams(&initParams,
                                               NV_ENC_CODEC_H264_GUID,
                                               NvencPresetGuid(nvenc),
                                               NvencTuningInfo(nvenc));
    
    if (NvencStdoutDebugEnabled()) {
      std::cout << "NvEncoderD3D11: CreateDefaultEncoderParams OK, encodeWidth="
                << initParams.encodeWidth << " encodeHeight=" << initParams.encodeHeight
                << std::endl;
    }
    
    // Override with our settings
    uint32_t fps = static_cast<uint32_t>(configuration_.max_frame_rate);
    if (fps == 0) fps = 30;  // Fallback to 30fps if not configured
    initParams.frameRateNum = fps;
    initParams.frameRateDen = 1;
    
    // Configure rate control for real-time streaming
    if (auto profile_override = NvencProfileGuidOverride(nvenc)) {
      encodeConfig.profileGUID = *profile_override;
    } else {
      encodeConfig.profileGUID = nv_profile_guid_;
    }
    encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;  // No periodic IDR, only on request
    encodeConfig.frameIntervalP = 1;  // No B-frames for low latency
    switch (nvenc.rate_control) {
      case livekit_ffi::NvencRateControl::Cbr:
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        break;
      case livekit_ffi::NvencRateControl::Vbr:
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        break;
      case livekit_ffi::NvencRateControl::Cqp:
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        {
          const uint32_t qp = NvencClampCqp(nvenc.cqp);
          encodeConfig.rcParams.constQP = {qp, qp, qp};
        }
        break;
    }
    
    // Use configured bitrates with sane defaults
    uint32_t target_bps = configuration_.target_bps;
    uint32_t max_bps = configuration_.max_bps;
    if (target_bps == 0) target_bps = 4000000;  // 4 Mbps default
    if (max_bps == 0) max_bps = target_bps * 2;
    
    encodeConfig.rcParams.averageBitRate = target_bps;
    encodeConfig.rcParams.maxBitRate = max_bps;
    encodeConfig.rcParams.vbvBufferSize = max_bps;  // 1 second buffer
    encodeConfig.rcParams.vbvInitialDelay = max_bps / 2;  // 0.5 second initial delay
    
    // H264 specific settings for low latency
    encodeConfig.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;  // Include SPS/PPS with each IDR

    if (nvenc.keyframe_interval_seconds > 0 && fps > 0) {
      const uint32_t keyint =
          std::max<uint32_t>(1u, fps * static_cast<uint32_t>(nvenc.keyframe_interval_seconds));
      encodeConfig.gopLength = keyint;
      encodeConfig.encodeCodecConfig.h264Config.idrPeriod = keyint;
    }
    
    if (NvencStdoutDebugEnabled()) {
      std::cout << "NvEncoderD3D11 init: " << width << "x" << height
                << " @ " << fps << "fps, " << (target_bps / 1000) << "kbps"
                << ", format=" << static_cast<int>(format) << std::endl;
    }
    
    d3d11_encoder_->CreateEncoder(&initParams);
    d3d11_encoder_initialized_ = true;
    
    RTC_LOG(LS_INFO) << "NvEncoderD3D11 initialized for zero-copy GPU encoding: "
                     << width << "x" << height;
    return true;
    
  } catch (const NVENCException& e) {
    RTC_LOG(LS_WARNING) << "NvEncoderD3D11 init failed: " << e.what() 
                        << " - will use CPU readback + CUDA encoder";
    d3d11_encoder_ = nullptr;
    d3d11_encoder_initialized_ = false;
    d3d11_encoder_init_failed_ = true;  // Don't retry on future frames
    return false;
  }
}

int32_t NvidiaH264EncoderImpl::EncodeD3D11Texture(
    ID3D11Texture2D* texture,
    ID3D11Device* device,
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  
  if (!texture || !device) {
    RTC_LOG(LS_ERROR) << "EncodeD3D11Texture: null texture or device";
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  // Get texture description
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  
  static std::atomic<bool> first_frame{true};
  bool expected = true;
  if (first_frame.compare_exchange_strong(expected, false)) {
    if (NvencStdoutDebugEnabled()) {
      std::cout << "EncodeD3D11Texture: First frame - " << desc.Width << "x" << desc.Height
                << ", DXGI format=" << static_cast<int>(desc.Format) << std::endl;
    }
  }

  // Determine the NV_ENC buffer format based on DXGI format
  NV_ENC_BUFFER_FORMAT nvFormat;
  switch (desc.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      nvFormat = NV_ENC_BUFFER_FORMAT_ARGB;
      break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      nvFormat = NV_ENC_BUFFER_FORMAT_ABGR;
      break;
    case DXGI_FORMAT_NV12:
      nvFormat = NV_ENC_BUFFER_FORMAT_NV12;
      break;
    default:
      RTC_LOG(LS_WARNING) << "Unsupported D3D11 texture format: " << desc.Format
                          << ". Falling back to CPU path.";
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  // Initialize D3D11 encoder if needed
  if (!EnsureD3D11EncoderInitialized(device, desc.Width, desc.Height, nvFormat)) {
    RTC_LOG(LS_WARNING) << "D3D11 encoder init failed, falling back to CPU path";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  bool is_keyframe_needed = false;
  if (configuration_.key_frame_request && configuration_.sending) {
    is_keyframe_needed = true;
  }

  bool send_key_frame =
      is_keyframe_needed ||
      (frame_types && !frame_types->empty() &&
       (*frame_types)[0] == VideoFrameType::kVideoFrameKey);
  if (send_key_frame) {
    is_keyframe_needed = true;
    configuration_.key_frame_request = false;
  }

  try {
    // Copy the source texture to the encoder's input buffer
    // NvEncoderD3D11 handles format conversion internally
    d3d11_encoder_->CopyToInputTexture(texture);

    // Encode the frame
    NV_ENC_PIC_PARAMS pic_params = NV_ENC_PIC_PARAMS();
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.encodePicFlags = 0;
    if (is_keyframe_needed) {
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA |
                                  NV_ENC_PIC_FLAG_FORCEIDR |
                                  NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
      configuration_.key_frame_request = false;
    }

    std::vector<std::vector<uint8_t>> bit_stream;
    d3d11_encoder_->EncodeFrame(bit_stream, &pic_params);

    for (std::vector<uint8_t>& packet : bit_stream) {
      int32_t result = ProcessEncodedFrame(packet, input_frame);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
    }

  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "D3D11 encode failed: " << e.what();
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}
#endif  // _WIN32

}  // namespace webrtc
