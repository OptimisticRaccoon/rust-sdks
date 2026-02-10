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

#include "av1_encoder_impl.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>

#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/svc/create_scalability_structure.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

#include "livekit/nvenc_settings.h"

#ifdef _WIN32
#include "livekit/d3d11_frame_buffer.h"
#endif

namespace webrtc {

namespace {
bool NvencStdoutDebugEnabled() {
  static const bool enabled = []() -> bool {
    const char* v = std::getenv("ZENSPEAK_NVENC_DEBUG");
    if (!v || *v == '\0') return false;
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
  // Keep legacy behavior: use ultra-low-latency tuning for realtime screenshare.
  switch (s.preset) {
    case livekit_ffi::NvencPreset::Quality:
      return NV_ENC_TUNING_INFO_HIGH_QUALITY;
    default:
      return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  }
}

uint32_t NvencClampCqp(int32_t cqp) {
  const int32_t clamped = std::clamp<int32_t>(cqp, 0, 51);
  return static_cast<uint32_t>(clamped);
}

// Detect AV1 keyframes by scanning OBUs for a Sequence Header (type 1).
// NVENC always emits a Sequence Header OBU before a keyframe, so its presence
// is a reliable keyframe indicator. This replaces checking the "was a keyframe
// requested" flag, which misses the mandatory first-frame IDR from NVENC.
bool Av1BitstreamIsKeyframe(const uint8_t* data, size_t size) {
  size_t pos = 0;
  while (pos < size) {
    uint8_t hdr = data[pos];
    int obu_type = (hdr >> 3) & 0x0F;
    if (obu_type == 1) {  // OBU_SEQUENCE_HEADER
      return true;
    }
    bool has_ext = (hdr >> 2) & 1;
    bool has_sz  = (hdr >> 1) & 1;
    pos++;  // header byte
    if (has_ext && pos < size) pos++;  // extension byte
    if (has_sz && pos < size) {
      // Read LEB128 size and skip past the OBU payload.
      uint64_t sz = 0;
      int shift = 0;
      while (pos < size) {
        uint8_t b = data[pos++];
        sz |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
      }
      pos += static_cast<size_t>(sz);
    } else if (!has_sz) {
      // OBU without size field consumes rest of bitstream; can't scan further.
      break;
    }
  }
  return false;
}

}  // namespace

NvidiaAv1EncoderImpl::NvidiaAv1EncoderImpl(const webrtc::Environment& env,
                                           CUcontext context,
                                           CUmemorytype memory_type,
                                           NV_ENC_BUFFER_FORMAT nv_format,
                                           const SdpVideoFormat& format)
    : env_(env),
      encoder_(nullptr),
      cu_context_(context),
      cu_memory_type_(memory_type),
      nv_format_(nv_format),
      format_(format) {
  RTC_CHECK_NE(cu_memory_type_, CU_MEMORYTYPE_HOST);
}

NvidiaAv1EncoderImpl::~NvidiaAv1EncoderImpl() {
  Release();
}

int32_t NvidiaAv1EncoderImpl::InitEncode(const VideoCodec* inst,
                                         const VideoEncoder::Settings& settings) {
  if (!inst || inst->codecType != kVideoCodecAV1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->width < 1 || inst->height < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  int32_t release_ret = Release();
  if (release_ret != WEBRTC_VIDEO_CODEC_OK) {
    return release_ret;
  }

  codec_ = *inst;

  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, codec_.width, codec_.height);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  encoded_image_.set_size(0);

  configuration_.sending = false;
  configuration_.key_frame_request = false;
  configuration_.max_frame_rate = codec_.maxFramerate > 0 ? codec_.maxFramerate : 30;
  configuration_.target_bps = codec_.startBitrate * 1000;
  configuration_.max_bps = codec_.maxBitrate * 1000;

  // Create L1T1 scalability controller for Dependency Descriptor generation.
  // AV1 over RTP requires the DD header extension (RFC 9579) so the SFU can
  // detect keyframes and manage frame dependencies for subscriber forwarding.
  // Without this, remote viewers cannot start decoding the stream.
  svc_controller_ = CreateScalabilityStructure(ScalabilityMode::kL1T1);
  if (!svc_controller_) {
    RTC_LOG(LS_ERROR) << "AV1: Failed to create L1T1 scalability structure";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  current_layer_frame_ = absl::nullopt;

  RTC_LOG(LS_INFO) << "NVENC AV1: InitEncode " << codec_.width << "x" << codec_.height
                    << " @" << configuration_.max_frame_rate << "fps";
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaAv1EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaAv1EncoderImpl::Release() {
#ifdef _WIN32
  d3d11_encoder_ = nullptr;
  d3d11_encoder_initialized_ = false;
  d3d11_encoder_init_failed_ = false;
#endif

  sent_first_frame_ = false;
  av1_sequence_header_.clear();
  av1_sequence_header_logged_ = false;
  svc_controller_ = nullptr;
  current_layer_frame_ = absl::nullopt;

  if (encoder_) {
    encoder_->DestroyEncoder();
    encoder_ = nullptr;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

bool NvidiaAv1EncoderImpl::EnsureCudaEncoderInitialized(uint32_t width,
                                                        uint32_t height) {
  if (encoder_) {
    return true;
  }

  const CUresult result = cuCtxSetCurrent(cu_context_);
  if (result != CUDA_SUCCESS) {
    return false;
  }

  try {
    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      // WebRTC expects raw AV1 elementary stream (OBUs). The NvCodec helper can
      // optionally wrap AV1 in an IVF container; disable that for WebRTC.
      encoder_ = std::make_unique<NvEncoderCuda>(cu_context_, width, height, nv_format_, 0,
                                                /*bMotionEstimationOnly=*/false,
                                                /*bOPInVideoMemory=*/false,
                                                /*bUseIVFContainer=*/false);
    } else {
      RTC_DCHECK_NOTREACHED();
      return false;
    }
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "AV1: Failed to create NvEncoderCuda: " << e.what();
    encoder_ = nullptr;
    return false;
  }

  nv_initialize_params_ = {};
  nv_encode_config_ = {};
  nv_initialize_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
  nv_encode_config_.version = NV_ENC_CONFIG_VER;
  nv_initialize_params_.encodeConfig = &nv_encode_config_;

  const livekit_ffi::NvencSettings nvenc = livekit_ffi::GetGlobalNvencSettings();
  encoder_->CreateDefaultEncoderParams(&nv_initialize_params_,
                                       NV_ENC_CODEC_AV1_GUID,
                                       NvencPresetGuid(nvenc),
                                       NvencTuningInfo(nvenc));

  uint32_t fps = static_cast<uint32_t>(configuration_.max_frame_rate);
  if (fps == 0) fps = 30;
  nv_initialize_params_.frameRateNum = fps;
  nv_initialize_params_.frameRateDen = 1;
  nv_initialize_params_.bufferFormat = nv_format_;

  // AV1 only has Main profile in NVENC.
  nv_encode_config_.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
  nv_encode_config_.gopLength = NVENC_INFINITE_GOPLENGTH;
  nv_encode_config_.frameIntervalP = 1;

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
  nv_encode_config_.rcParams.vbvBufferSize = max_bps;
  nv_encode_config_.rcParams.vbvInitialDelay = max_bps / 2;

  // AV1 codec-specific knobs
  nv_encode_config_.encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
  // Ensure decoders can start and recover quickly (analogous to repeatSPSPPS for H264).
  nv_encode_config_.encodeCodecConfig.av1Config.disableSeqHdr = 0;
  nv_encode_config_.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
  // Explicitly request "low overhead" OBU format (NOT Annex B). WebRTC's AV1 RTP
  // packetizer expects individual OBUs with size fields, not length-delimited TUs.
  nv_encode_config_.encodeCodecConfig.av1Config.outputAnnexBFormat = 0;
  // NOTE: Don't explicitly set level/tier - let NVENC auto-select based on resolution.
  // Setting level=0 can cause NV_ENC_ERR_INVALID_PARAM on some driver versions.
  if (nvenc.keyframe_interval_seconds > 0 && fps > 0) {
    const uint32_t keyint =
        std::max<uint32_t>(1u, fps * static_cast<uint32_t>(nvenc.keyframe_interval_seconds));
    nv_encode_config_.gopLength = keyint;
    nv_encode_config_.encodeCodecConfig.av1Config.idrPeriod = keyint;
  }

  try {
    encoder_->CreateEncoder(&nv_initialize_params_);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "AV1: CreateEncoder failed: " << e.what();
    encoder_->DestroyEncoder();
    encoder_ = nullptr;
    return false;
  }

  RTC_LOG(LS_INFO) << "NVENC: Initialized CUDA AV1 encoder: " << width << "x" << height
                   << " @" << fps << "fps";

  // Cache out-of-band sequence header (for AV1: sequence header OBU) so we can prepend it
  // to the first keyframe if needed (some decoders need this early to start).
  av1_sequence_header_.clear();
  try {
    encoder_->GetSequenceParams(av1_sequence_header_);
  } catch (const NVENCException& e) {
    RTC_LOG(LS_WARNING) << "AV1: GetSequenceParams failed (CUDA): " << e.what();
  }
  if (!av1_sequence_header_.empty()) {
    RTC_LOG(LS_INFO) << "AV1: cached sequence header (CUDA), bytes=" << av1_sequence_header_.size();
  }
  return true;
}

#ifdef _WIN32
bool NvidiaAv1EncoderImpl::EnsureD3D11EncoderInitialized(ID3D11Device* device,
                                                         uint32_t width,
                                                         uint32_t height,
                                                         NV_ENC_BUFFER_FORMAT format) {
  if (d3d11_encoder_initialized_ && d3d11_encoder_) {
    return true;
  }
  if (d3d11_encoder_init_failed_) {
    return false;
  }

  try {
    d3d11_encoder_ = std::make_unique<NvEncoderD3D11>(device, width, height, format/*,
                                                      nExtraOutputDelay=1*/);

    NV_ENC_INITIALIZE_PARAMS initParams = {};
    NV_ENC_CONFIG encodeConfig = {};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeConfig = &encodeConfig;

    const livekit_ffi::NvencSettings nvenc = livekit_ffi::GetGlobalNvencSettings();
    d3d11_encoder_->CreateDefaultEncoderParams(&initParams,
                                               NV_ENC_CODEC_AV1_GUID,
                                               NvencPresetGuid(nvenc),
                                               NvencTuningInfo(nvenc));

    uint32_t fps = static_cast<uint32_t>(configuration_.max_frame_rate);
    if (fps == 0) fps = 30;
    initParams.frameRateNum = fps;
    initParams.frameRateDen = 1;

    encodeConfig.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
    encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
    encodeConfig.frameIntervalP = 1;

    encodeConfig.rcParams.version = NV_ENC_RC_PARAMS_VER;
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

    uint32_t target_bps = configuration_.target_bps;
    uint32_t max_bps = configuration_.max_bps;
    if (target_bps == 0) target_bps = 4000000;
    if (max_bps == 0) max_bps = target_bps * 2;

    encodeConfig.rcParams.averageBitRate = target_bps;
    encodeConfig.rcParams.maxBitRate = max_bps;
    encodeConfig.rcParams.vbvBufferSize = max_bps;
    encodeConfig.rcParams.vbvInitialDelay = max_bps / 2;

    encodeConfig.encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    encodeConfig.encodeCodecConfig.av1Config.disableSeqHdr = 0;
    encodeConfig.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
    // Explicitly request "low overhead" OBU format (NOT Annex B).
    encodeConfig.encodeCodecConfig.av1Config.outputAnnexBFormat = 0;
    // NOTE: Don't explicitly set level/tier - let NVENC auto-select based on resolution.
    if (nvenc.keyframe_interval_seconds > 0 && fps > 0) {
      const uint32_t keyint =
          std::max<uint32_t>(1u, fps * static_cast<uint32_t>(nvenc.keyframe_interval_seconds));
      encodeConfig.gopLength = keyint;
      encodeConfig.encodeCodecConfig.av1Config.idrPeriod = keyint;
    }

    d3d11_encoder_->CreateEncoder(&initParams);
    d3d11_encoder_initialized_ = true;
    RTC_LOG(LS_INFO) << "NVENC AV1: D3D11 encoder initialized " << width << "x" << height;

    // Cache sequence header bytes for possible prepend on first keyframe.
    av1_sequence_header_.clear();
    try {
      d3d11_encoder_->GetSequenceParams(av1_sequence_header_);
    } catch (const NVENCException& e) {
      RTC_LOG(LS_WARNING) << "AV1: GetSequenceParams failed (D3D11): " << e.what();
    }
    if (!av1_sequence_header_.empty()) {
      RTC_LOG(LS_INFO) << "AV1: cached sequence header (D3D11), bytes=" << av1_sequence_header_.size();
    }
    return true;
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "NVENC AV1: D3D11 encoder init failed: " << e.what();
    d3d11_encoder_ = nullptr;
    d3d11_encoder_initialized_ = false;
    d3d11_encoder_init_failed_ = true;
    return false;
  }
}

int32_t NvidiaAv1EncoderImpl::EncodeD3D11Texture(ID3D11Texture2D* texture,
                                                 ID3D11Device* device,
                                                 const VideoFrame& input_frame,
                                                 const std::vector<VideoFrameType>* frame_types) {
  if (!texture || !device) {
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);

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
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  if (!EnsureD3D11EncoderInitialized(device, desc.Width, desc.Height, nvFormat)) {
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  bool send_key_frame = false;
  if (configuration_.key_frame_request && configuration_.sending) {
    send_key_frame = true;
  }
  if (frame_types && !frame_types->empty() &&
      (*frame_types)[0] == VideoFrameType::kVideoFrameKey) {
    send_key_frame = true;
  }
  if (!sent_first_frame_) {
    // Ensure decoders can start immediately.
    send_key_frame = true;
    sent_first_frame_ = true;
  }
  if (send_key_frame) {
    configuration_.key_frame_request = false;
  }

  // Advance the SVC controller state before encoding. For L1T1 this produces
  // exactly one LayerFrameConfig which we store for ProcessEncodedFrame.
  if (svc_controller_) {
    auto layer_frames = svc_controller_->NextFrameConfig(/*restart=*/send_key_frame);
    if (!layer_frames.empty()) {
      current_layer_frame_ = layer_frames[0];
    } else {
      // SVC controller not yet enabled (OnRatesUpdated not called yet).
      current_layer_frame_ = absl::nullopt;
    }
  }

  try {
    d3d11_encoder_->CopyToInputTexture(texture);

    NV_ENC_PIC_PARAMS pic_params = NV_ENC_PIC_PARAMS();
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.encodePicFlags = 0;
    if (send_key_frame) {
      // Force an IDR frame. The sequence header is automatically included on keyframes
      // due to repeatSeqHdr=1 in the encoder config. We do NOT set NV_ENC_PIC_FLAG_OUTPUT_SPSPPS
      // as that flag combined with repeatSeqHdr may cause duplicate/conflicting sequence headers.
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA | NV_ENC_PIC_FLAG_FORCEIDR;
    }

    std::vector<std::vector<uint8_t>> bit_stream;
    d3d11_encoder_->EncodeFrame(bit_stream, &pic_params);
    if (bit_stream.empty()) {
      RTC_LOG(LS_WARNING) << "AV1 D3D11: EncodeFrame returned 0 packets"
                          << " (keyframe_requested=" << send_key_frame << ")";
    }
    const auto& qp_vec = d3d11_encoder_->GetFrameAvgQP();
    for (size_t pkt_idx = 0; pkt_idx < bit_stream.size(); pkt_idx++) {
      uint32_t avg_qp = (pkt_idx < qp_vec.size()) ? qp_vec[pkt_idx] : 0;
      int32_t result = ProcessEncodedFrame(bit_stream[pkt_idx], input_frame, send_key_frame, avg_qp);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
    }
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "AV1 D3D11 EncodeFrame failed: " << e.what();
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}
#endif

int32_t NvidiaAv1EncoderImpl::Encode(const VideoFrame& input_frame,
                                     const std::vector<VideoFrameType>* frame_types) {
  if (!encoded_image_callback_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!configuration_.sending) {
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  if (frame_types != nullptr && !frame_types->empty()) {
    if ((*frame_types)[0] == VideoFrameType::kEmptyFrame) {
      return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }
  }

#ifdef _WIN32
  rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer = input_frame.video_frame_buffer();
  if (buffer->type() == VideoFrameBuffer::Type::kNative) {
    auto* d3d11_buffer = dynamic_cast<livekit_ffi::D3D11TextureBuffer*>(buffer.get());
    if (d3d11_buffer && d3d11_buffer->texture() && d3d11_buffer->device()) {
      int32_t result = EncodeD3D11Texture(d3d11_buffer->texture(),
                                          d3d11_buffer->device(),
                                          input_frame,
                                          frame_types);
      if (result != WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE) {
        return result;
      }
    }
  }
#endif

  // CPU fallback path (I420 -> CUDA). This is not zero-copy.
  webrtc::scoped_refptr<I420BufferInterface> frame_buffer =
      input_frame.video_frame_buffer()->ToI420();
  if (!frame_buffer) {
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }
  if (!EnsureCudaEncoderInitialized(frame_buffer->width(), frame_buffer->height())) {
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  bool send_key_frame = false;
  if (configuration_.key_frame_request && configuration_.sending) {
    send_key_frame = true;
  }
  if (frame_types && !frame_types->empty() &&
      (*frame_types)[0] == VideoFrameType::kVideoFrameKey) {
    send_key_frame = true;
  }
  if (!sent_first_frame_) {
    send_key_frame = true;
    sent_first_frame_ = true;
  }
  if (send_key_frame) {
    configuration_.key_frame_request = false;
  }

  // Advance the SVC controller state before encoding. For L1T1 this produces
  // exactly one LayerFrameConfig which we store for ProcessEncodedFrame.
  if (svc_controller_) {
    auto layer_frames = svc_controller_->NextFrameConfig(/*restart=*/send_key_frame);
    if (!layer_frames.empty()) {
      current_layer_frame_ = layer_frames[0];
    } else {
      current_layer_frame_ = absl::nullopt;
    }
  }

  try {
    const NvEncInputFrame* nv_enc_input_frame = encoder_->GetNextInputFrame();
    if (cu_memory_type_ == CU_MEMORYTYPE_DEVICE) {
      NvEncoderCuda::CopyToDeviceFrame(cu_context_,
                                       (void*)frame_buffer->DataY(),
                                       frame_buffer->StrideY(),
                                       (CUdeviceptr)nv_enc_input_frame->inputPtr,
                                       nv_enc_input_frame->pitch,
                                       frame_buffer->width(),
                                       frame_buffer->height(),
                                       CU_MEMORYTYPE_HOST,
                                       nv_enc_input_frame->bufferFormat,
                                       nv_enc_input_frame->chromaOffsets,
                                       nv_enc_input_frame->numChromaPlanes);
    } else {
      RTC_DCHECK_NOTREACHED();
    }

    NV_ENC_PIC_PARAMS pic_params = NV_ENC_PIC_PARAMS();
    pic_params.version = NV_ENC_PIC_PARAMS_VER;
    pic_params.encodePicFlags = 0;
    if (send_key_frame) {
      // Force an IDR frame. The sequence header is automatically included on keyframes
      // due to repeatSeqHdr=1 in the encoder config.
      pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEINTRA | NV_ENC_PIC_FLAG_FORCEIDR;
    }

    std::vector<std::vector<uint8_t>> bit_stream;
    encoder_->EncodeFrame(bit_stream, &pic_params);
    if (bit_stream.empty()) {
      RTC_LOG(LS_WARNING) << "AV1 CUDA: EncodeFrame returned 0 packets"
                          << " (keyframe_requested=" << send_key_frame << ")";
    }
    const auto& qp_vec = encoder_->GetFrameAvgQP();
    for (size_t pkt_idx = 0; pkt_idx < bit_stream.size(); pkt_idx++) {
      uint32_t avg_qp = (pkt_idx < qp_vec.size()) ? qp_vec[pkt_idx] : 0;
      int32_t result = ProcessEncodedFrame(bit_stream[pkt_idx], input_frame, send_key_frame, avg_qp);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
    }
  } catch (const NVENCException& e) {
    RTC_LOG(LS_ERROR) << "AV1 EncodeFrame failed: " << e.what();
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvidiaAv1EncoderImpl::ProcessEncodedFrame(std::vector<uint8_t>& packet,
                                                  const ::webrtc::VideoFrame& inputFrame,
                                                  bool requested_keyframe,
                                                  uint32_t nvenc_avg_qp) {
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
    encoded_image_._encodedWidth = codec_.width;
    encoded_image_._encodedHeight = codec_.height;
  }

  // Detect keyframe from ACTUAL bitstream, not the request flag. NVENC always
  // produces a keyframe (with Sequence Header OBU) for the first frame and
  // when forced via NV_ENC_PIC_FLAG_FORCEIDR, but the request flag only covers
  // the latter case. Missing the implicit first-frame keyframe causes the SVC
  // controller to emit delta-only DD metadata, so the SFU never starts forwarding.
  const bool is_keyframe = Av1BitstreamIsKeyframe(packet.data(), packet.size());

  encoded_image_.SetRtpTimestamp(inputFrame.rtp_timestamp());
  encoded_image_.SetSimulcastIndex(0);
  encoded_image_.ntp_time_ms_ = inputFrame.ntp_time_ms();
  encoded_image_.capture_time_ms_ = inputFrame.render_time_ms();
  encoded_image_.rotation_ = inputFrame.rotation();
  encoded_image_.content_type_ = VideoContentType::UNSPECIFIED;
  encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
  encoded_image_._frameType = is_keyframe ? VideoFrameType::kVideoFrameKey
                                         : VideoFrameType::kVideoFrameDelta;
  encoded_image_.SetColorSpace(inputFrame.color_space());

  // Strip leading Temporal Delimiter OBU if present. Required by the AV1 RTP
  // spec (RFC 9579 §5: "The TD OBU, if present, SHOULD be removed when
  // packetizing"). Also critical because WebRTC's RtpPacketizerAv1::ParseObus
  // treats an OBU with obu_has_size_field=0 as consuming ALL remaining bytes.
  // If NVENC emits a TD with has_size=0, the packetizer would consume the
  // entire buffer as TD payload, strip it, and generate zero RTP packets.
  const uint8_t* frame_data = packet.data();
  size_t frame_size = packet.size();
  if (frame_size > 0) {
    uint8_t hdr = frame_data[0];
    int obu_type = (hdr >> 3) & 0x0F;
    if (obu_type == 2) {  // Temporal Delimiter
      bool has_ext = (hdr >> 2) & 1;
      bool has_sz  = (hdr >> 1) & 1;
      size_t skip = 1;  // header byte
      if (has_ext && frame_size > skip) skip++;  // extension byte
      if (has_sz && frame_size > skip) {
        // Read past LEB128 size (TD payload is 0 bytes, so this is just 0x00).
        while (skip < frame_size && (frame_data[skip] & 0x80)) skip++;
        if (skip < frame_size) skip++;  // last byte of LEB128
      }
      // TD has 0 bytes of payload per AV1 spec, so skip == end of TD OBU.
      frame_data += skip;
      frame_size -= skip;
    }
  }

  encoded_image_.SetEncodedData(
      EncodedImageBuffer::Create(frame_data, frame_size));
  encoded_image_.set_size(frame_size);
  encoded_image_.qp_ = static_cast<int32_t>(nvenc_avg_qp);

  // Log keyframes and the first few frames for diagnostics.
  static std::atomic<int> frame_seq{0};
  int seq = frame_seq.fetch_add(1);
  if (is_keyframe || seq < 3) {
    RTC_LOG(LS_INFO) << "NVENC AV1: frame " << seq
                     << (is_keyframe ? " KEY" : " delta")
                     << " " << frame_size << "B"
                     << " " << encoded_image_._encodedWidth << "x"
                     << encoded_image_._encodedHeight
                     << (frame_data != packet.data() ? " td_stripped" : "")
                     << " dd=" << (current_layer_frame_.has_value() ? "y" : "n");
  }

  // ---------- CodecSpecificInfo + DD metadata ----------
  CodecSpecificInfo codecInfo = {};
  codecInfo.codecType = kVideoCodecAV1;
  codecInfo.end_of_picture = true;

  // Populate Dependency Descriptor (DD) metadata via the SVC controller.
  // The DD RTP header extension (RFC 9579) tells the SFU about keyframes
  // and frame dependencies so it can forward to subscribers.
  if (current_layer_frame_ && svc_controller_) {
    // If the actual encoded output is a keyframe (detected from bitstream),
    // update the layer frame config accordingly. This is critical because
    // NextFrameConfig(restart) was called before encoding based on the REQUEST
    // flag, but NVENC may produce a keyframe even without being asked (e.g.
    // the mandatory first IDR frame). The SVC controller must know the real
    // keyframe status to generate correct DD metadata.
    if (is_keyframe) {
      current_layer_frame_->Keyframe();
    }

    codecInfo.generic_frame_info =
        svc_controller_->OnEncodeDone(*current_layer_frame_);

    if (is_keyframe && codecInfo.generic_frame_info) {
      codecInfo.template_structure = svc_controller_->DependencyStructure();
      if (codecInfo.template_structure) {
        codecInfo.template_structure->resolutions = {
            RenderResolution(encoded_image_._encodedWidth,
                             encoded_image_._encodedHeight)};
      }
    }
    current_layer_frame_ = absl::nullopt;
  }

  const auto result = encoded_image_callback_->OnEncodedImage(encoded_image_, &codecInfo);
  if (result.error != EncodedImageCallback::Result::OK) {
    RTC_LOG(LS_ERROR) << "NVENC AV1: OnEncodedImage error=" << result.error
                      << " frame=" << seq;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo NvidiaAv1EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = true;
  info.implementation_name = "NVIDIA AV1 Encoder";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kNative,
                                  VideoFrameBuffer::Type::kI420};
  return info;
}

void NvidiaAv1EncoderImpl::SetRates(const RateControlParameters& parameters) {
  if (parameters.framerate_fps < 1.0) {
    return;
  }
  const uint32_t target_bps = static_cast<uint32_t>(parameters.bitrate.get_sum_bps());
  if (target_bps == 0) {
    configuration_.SetStreamState(false);
    return;
  }

  configuration_.SetStreamState(true);
  configuration_.target_bps = target_bps;
  configuration_.max_bps = std::max<uint32_t>(target_bps, configuration_.max_bps);
  configuration_.max_frame_rate = parameters.framerate_fps;

  // Notify the SVC controller about the bitrate allocation. This is CRITICAL:
  // ScalableVideoControllerNoLayering (L1T1) starts with enabled_=false and
  // only enables itself when OnRatesUpdated() is called with a non-zero bitrate.
  // Without this, NextFrameConfig() returns empty and no DD metadata is generated.
  if (svc_controller_) {
    svc_controller_->OnRatesUpdated(parameters.bitrate);
  }

  const uint32_t fps =
      std::max<uint32_t>(1u, static_cast<uint32_t>(parameters.framerate_fps));

  const int64_t now_ms = rtc::TimeMillis();
  const bool should_reconfigure =
      (last_applied_target_bps_ != target_bps) || (last_applied_fps_ != fps);
  if (!should_reconfigure) {
    return;
  }
  if (last_reconfigure_time_ms_ != 0 && (now_ms - last_reconfigure_time_ms_) < 250) {
    return;
  }

  last_applied_target_bps_ = target_bps;
  last_applied_fps_ = fps;
  last_reconfigure_time_ms_ = now_ms;

  const uint32_t max_bps = std::max<uint32_t>(target_bps, configuration_.max_bps);

#ifdef _WIN32
  if (d3d11_encoder_) {
    // Use GetInitializeParams to retrieve the CURRENT encoder parameters.
    // Only modify bitrate and framerate - NVENC's Reconfigure() has strict limits
    // on what can be changed at runtime. Creating new default params can introduce
    // differences that cause NV_ENC_ERR_INVALID_PARAM (error 8).
    try {
      NV_ENC_RECONFIGURE_PARAMS reconfig = {};
      reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;

      NV_ENC_INITIALIZE_PARAMS initParams = {};
      NV_ENC_CONFIG encodeConfig = {};
      initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
      encodeConfig.version = NV_ENC_CONFIG_VER;
      initParams.encodeConfig = &encodeConfig;

      // Get current params - don't create new defaults!
      d3d11_encoder_->GetInitializeParams(&initParams);

      // Only modify what Reconfigure() safely supports:
      initParams.frameRateNum = fps;
      initParams.frameRateDen = 1;

      // Update bitrate-related fields only
      encodeConfig.rcParams.averageBitRate = target_bps;
      encodeConfig.rcParams.maxBitRate = max_bps;
      encodeConfig.rcParams.vbvBufferSize = max_bps;
      encodeConfig.rcParams.vbvInitialDelay = max_bps / 2;

      reconfig.reInitEncodeParams = initParams;
      reconfig.reInitEncodeParams.encodeConfig = &encodeConfig;
      reconfig.resetEncoder = 0;
      reconfig.forceIDR = 0;

      d3d11_encoder_->Reconfigure(&reconfig);
    } catch (const NVENCException& e) {
      RTC_LOG(LS_WARNING) << "AV1 NvEncoderD3D11 Reconfigure failed: " << e.what();
    }
    return;
  }
#endif

  if (encoder_) {
    // Use GetInitializeParams like the D3D11 path - only modify bitrate/fps
    try {
      NV_ENC_RECONFIGURE_PARAMS reconfig = {};
      reconfig.version = NV_ENC_RECONFIGURE_PARAMS_VER;

      NV_ENC_INITIALIZE_PARAMS initParams = {};
      NV_ENC_CONFIG encodeConfig = {};
      initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
      encodeConfig.version = NV_ENC_CONFIG_VER;
      initParams.encodeConfig = &encodeConfig;

      // Get current params - don't create new defaults!
      encoder_->GetInitializeParams(&initParams);

      // Only modify what Reconfigure() safely supports:
      initParams.frameRateNum = fps;
      initParams.frameRateDen = 1;

      // Update bitrate-related fields only
      encodeConfig.rcParams.averageBitRate = target_bps;
      encodeConfig.rcParams.maxBitRate = max_bps;
      encodeConfig.rcParams.vbvBufferSize = max_bps;
      encodeConfig.rcParams.vbvInitialDelay = max_bps / 2;

      reconfig.reInitEncodeParams = initParams;
      reconfig.reInitEncodeParams.encodeConfig = &encodeConfig;
      reconfig.resetEncoder = 0;
      reconfig.forceIDR = 0;

      encoder_->Reconfigure(&reconfig);
    } catch (const NVENCException& e) {
      RTC_LOG(LS_WARNING) << "AV1 NvEncoderCuda Reconfigure failed: " << e.what();
    }
  }
}

}  // namespace webrtc

