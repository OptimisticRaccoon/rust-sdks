#ifndef WEBRTC_NVIDIA_AV1_ENCODER_IMPL_H_
#define WEBRTC_NVIDIA_AV1_ENCODER_IMPL_H_

#include <cuda.h>

#include <memory>
#include <vector>

#include "absl/types/optional.h"

#include "NvEncoder/NvEncoder.h"
#include "NvEncoder/NvEncoderCuda.h"

#include "api/environment/environment.h"
#include "api/transport/rtp/dependency_descriptor.h"
#include "api/video_codecs/scalability_mode.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_constants.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/svc/scalable_video_controller.h"

#ifdef _WIN32
#include <d3d11.h>
#include "NvEncoder/NvEncoderD3D11.h"
#endif

namespace webrtc {

class NvidiaAv1EncoderImpl : public VideoEncoder {
 public:
  struct LayerConfig {
    bool sending = true;
    bool key_frame_request = false;
    float max_frame_rate = 0;
    uint32_t target_bps = 0;
    uint32_t max_bps = 0;

    void SetStreamState(bool send_stream) {
      if (send_stream && !sending) {
        // Match H264/H265 behavior: force a keyframe when resuming.
        key_frame_request = true;
      }
      sending = send_stream;
    }
  };

 public:
  NvidiaAv1EncoderImpl(const webrtc::Environment& env,
                       CUcontext context,
                       CUmemorytype memory_type,
                       NV_ENC_BUFFER_FORMAT nv_format,
                       const SdpVideoFormat& format);
  ~NvidiaAv1EncoderImpl() override;

  int32_t InitEncode(const VideoCodec* codec_settings,
                     const Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;
  int32_t Release() override;

  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;

  void SetRates(const RateControlParameters& rc_parameters) override;
  EncoderInfo GetEncoderInfo() const override;

 private:
  int32_t ProcessEncodedFrame(std::vector<uint8_t>& packet,
                              const ::webrtc::VideoFrame& inputFrame,
                              bool requested_keyframe,
                              uint32_t nvenc_avg_qp);

  bool EnsureCudaEncoderInitialized(uint32_t width, uint32_t height);

#ifdef _WIN32
  int32_t EncodeD3D11Texture(ID3D11Texture2D* texture,
                             ID3D11Device* device,
                             const VideoFrame& input_frame,
                             const std::vector<VideoFrameType>* frame_types);
  bool EnsureD3D11EncoderInitialized(ID3D11Device* device,
                                     uint32_t width,
                                     uint32_t height,
                                     NV_ENC_BUFFER_FORMAT format);
#endif

 private:
  const webrtc::Environment& env_;
  EncodedImageCallback* encoded_image_callback_ = nullptr;

  std::unique_ptr<NvEncoder> encoder_;
  CUcontext cu_context_;
  CUmemorytype cu_memory_type_;
  NV_ENC_BUFFER_FORMAT nv_format_;
  NV_ENC_INITIALIZE_PARAMS nv_initialize_params_ = {};
  NV_ENC_CONFIG nv_encode_config_ = {};

  LayerConfig configuration_;
  EncodedImage encoded_image_;
  VideoCodec codec_;
  const SdpVideoFormat format_;
  bool sent_first_frame_ = false;
  std::vector<uint8_t> av1_sequence_header_;
  bool av1_sequence_header_logged_ = false;

#ifdef _WIN32
  std::unique_ptr<NvEncoderD3D11> d3d11_encoder_;
  bool d3d11_encoder_initialized_ = false;
  bool d3d11_encoder_init_failed_ = false;
#endif

  // SVC controller for generating Dependency Descriptor (DD) RTP header extension.
  // AV1 requires DD for SFU keyframe detection and subscriber stream forwarding.
  // Without this, the LiveKit SFU cannot forward AV1 streams to remote viewers.
  std::unique_ptr<ScalableVideoController> svc_controller_;
  // Stores the LayerFrameConfig from NextFrameConfig() between Encode and
  // ProcessEncodedFrame. Consumed (reset) after OnEncodeDone().
  absl::optional<ScalableVideoController::LayerFrameConfig> current_layer_frame_;

  // For SetRates throttling / avoiding reconfigure spam.
  uint32_t last_applied_target_bps_ = 0;
  uint32_t last_applied_max_bps_ = 0;
  uint32_t last_applied_fps_ = 0;
  int64_t last_reconfigure_time_ms_ = 0;
};

}  // namespace webrtc

#endif  // WEBRTC_NVIDIA_AV1_ENCODER_IMPL_H_

