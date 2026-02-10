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

#include "livekit/video_track.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_rotation.h"
#include "audio/remix_resample.h"
#include "common_audio/include/audio_util.h"
#include "livekit/media_stream.h"
#include "livekit/video_track.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/time_utils.h"
#include "webrtc-sys/src/video_track.rs.h"

namespace livekit_ffi {

namespace {
// Avoid stdout I/O from hot paths in production. Enable only for bring-up:
//   set ZENSPEAK_WEBRTC_DEBUG=1
bool WebrtcStdoutDebugEnabled() {
  static const bool enabled = []() -> bool {
    const char* v = std::getenv("ZENSPEAK_WEBRTC_DEBUG");
    if (!v || *v == '\0') return false;
    return !(v[0] == '0' && v[1] == '\0');
  }();
  return enabled;
}

}  // namespace

VideoTrack::VideoTrack(std::shared_ptr<RtcRuntime> rtc_runtime,
                       webrtc::scoped_refptr<webrtc::VideoTrackInterface> track)
    : MediaStreamTrack(rtc_runtime, std::move(track)) {}

VideoTrack::~VideoTrack() {
  webrtc::MutexLock lock(&mutex_);
  for (auto& sink : sinks_) {
    track()->RemoveSink(sink.get());
  }
}

void VideoTrack::add_sink(const std::shared_ptr<NativeVideoSink>& sink) const {
  webrtc::MutexLock lock(&mutex_);
  track()->AddOrUpdateSink(sink.get(),
                           webrtc::VideoSinkWants());  // TODO(theomonnom): Expose
                                                    // VideoSinkWants to Rust?
  sinks_.push_back(sink);
}

void VideoTrack::remove_sink(
    const std::shared_ptr<NativeVideoSink>& sink) const {
  webrtc::MutexLock lock(&mutex_);
  track()->RemoveSink(sink.get());
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void VideoTrack::set_should_receive(bool should_receive) const {
  track()->set_should_receive(should_receive);
}

bool VideoTrack::should_receive() const {
  return track()->should_receive();
}

ContentHint VideoTrack::content_hint() const {
  return static_cast<ContentHint>(track()->content_hint());
}

void VideoTrack::set_content_hint(ContentHint hint) const {
  track()->set_content_hint(
      static_cast<webrtc::VideoTrackInterface::ContentHint>(hint));
}

NativeVideoSink::NativeVideoSink(rust::Box<VideoSinkWrapper> observer)
    : observer_(std::move(observer)) {}

void NativeVideoSink::OnFrame(const webrtc::VideoFrame& frame) {
  observer_->on_frame(std::make_unique<VideoFrame>(frame));
}

void NativeVideoSink::OnDiscardedFrame() {
  observer_->on_discarded_frame();
}

void NativeVideoSink::OnConstraintsChanged(
    const webrtc::VideoTrackSourceConstraints& constraints) {
  VideoTrackSourceConstraints cst;
  cst.has_min_fps = constraints.min_fps.has_value();
  cst.min_fps = constraints.min_fps.value_or(0);
  cst.has_max_fps = constraints.max_fps.has_value();
  cst.max_fps = constraints.max_fps.value_or(0);
  observer_->on_constraints_changed(cst);
}

std::shared_ptr<NativeVideoSink> new_native_video_sink(
    rust::Box<VideoSinkWrapper> observer) {
  return std::make_shared<NativeVideoSink>(std::move(observer));
}

VideoTrackSource::InternalSource::InternalSource(
    const VideoResolution& resolution,
    bool is_screencast)
    : webrtc::AdaptedVideoTrackSource(4),
      resolution_(resolution),
      is_screencast_(is_screencast) {}

VideoTrackSource::InternalSource::~InternalSource() {}

bool VideoTrackSource::InternalSource::is_screencast() const {
  return is_screencast_;
}

std::optional<bool> VideoTrackSource::InternalSource::needs_denoising() const {
  return false;
}

webrtc::MediaSourceInterface::SourceState
VideoTrackSource::InternalSource::state() const {
  return SourceState::kLive;
}

bool VideoTrackSource::InternalSource::remote() const {
  return false;
}

VideoResolution VideoTrackSource::InternalSource::video_resolution() const {
  webrtc::MutexLock lock(&mutex_);
  return resolution_;
}

bool VideoTrackSource::InternalSource::on_captured_frame(
    const webrtc::VideoFrame& frame) {
  webrtc::MutexLock lock(&mutex_);

  int64_t aligned_timestamp_us = timestamp_aligner_.TranslateTimestamp(
      frame.timestamp_us(), webrtc::TimeMicros());

  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer =
      frame.video_frame_buffer();

  if (resolution_.height == 0 || resolution_.width == 0) {
    resolution_ = VideoResolution{static_cast<uint32_t>(buffer->width()),
                                  static_cast<uint32_t>(buffer->height())};
  }

  int adapted_width, adapted_height, crop_width, crop_height, crop_x, crop_y;
  if (!AdaptFrame(buffer->width(), buffer->height(), aligned_timestamp_us,
                  &adapted_width, &adapted_height, &crop_width, &crop_height,
                  &crop_x, &crop_y)) {
    static std::atomic<int> drop_count{0};
    int dropped = drop_count.fetch_add(1) + 1;
    // Log first drop unconditionally for diagnostics
    if (dropped == 1) {
      RTC_LOG(LS_WARNING) << "VideoTrackSource: AdaptFrame dropped first frame! "
                          << "(input: " << buffer->width() << "x" << buffer->height() 
                          << ", ts=" << aligned_timestamp_us << ")";
    } else if (WebrtcStdoutDebugEnabled() && (dropped % 60 == 0)) {
      std::cout << "VideoTrackSource: AdaptFrame dropped frame #" << dropped 
                << " (input: " << buffer->width() << "x" << buffer->height() 
                << ", ts=" << aligned_timestamp_us << ")" << std::endl;
    }
    return false;
  }

  if (adapted_width != frame.width() || adapted_height != frame.height()) {
    // Let the buffer implement CropAndScale. For GPU/native buffers, we provide a
    // GPU implementation (D3D11TextureBuffer::CropAndScale) to keep adaptation
    // while preserving zero-copy.
    buffer = buffer->CropAndScale(crop_x, crop_y, crop_width, crop_height,
                                  adapted_width, adapted_height);
  }

  webrtc::VideoRotation rotation = frame.rotation();
  if (apply_rotation() && rotation != webrtc::kVideoRotation_0) {
    // For native buffers, skip rotation too (encoder handles it)
    if (buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    buffer = buffer->ToI420();
    }
  }

  static std::atomic<int> accepted_count{0};
  int accepted = accepted_count.fetch_add(1) + 1;
  // Log first frame unconditionally for diagnostics
  if (accepted == 1) {
    RTC_LOG(LS_INFO) << "VideoTrackSource: First frame accepted ("
                     << adapted_width << "x" << adapted_height << ", is_screencast="
                     << is_screencast_ << ")";
  } else if (WebrtcStdoutDebugEnabled() && (accepted % 60 == 0)) {
    std::cout << "VideoTrackSource: Frame #" << accepted << " accepted ("
              << adapted_width << "x" << adapted_height << ")" << std::endl;
  }

  webrtc::VideoFrame new_frame = webrtc::VideoFrame::Builder()
              .set_video_frame_buffer(buffer)
              .set_rotation(rotation)
              .set_timestamp_us(aligned_timestamp_us)
                                     .build();
  OnFrame(new_frame);

  return true;
}

VideoTrackSource::VideoTrackSource(const VideoResolution& resolution, bool is_screencast) {
  source_ = webrtc::make_ref_counted<InternalSource>(resolution, is_screencast);
}

VideoResolution VideoTrackSource::video_resolution() const {
  return source_->video_resolution();
}

bool VideoTrackSource::on_captured_frame(
    const std::unique_ptr<VideoFrame>& frame) const {
  auto rtc_frame = frame->get();
  return source_->on_captured_frame(rtc_frame);
}

webrtc::scoped_refptr<VideoTrackSource::InternalSource> VideoTrackSource::get()
    const {
  return source_;
}

std::shared_ptr<VideoTrackSource> new_video_track_source(
    const VideoResolution& resolution) {
  return std::make_shared<VideoTrackSource>(resolution, false);
}

std::shared_ptr<VideoTrackSource> new_video_track_source_with_screencast(
    const VideoResolution& resolution,
    bool is_screencast) {
  return std::make_shared<VideoTrackSource>(resolution, is_screencast);
}

}  // namespace livekit_ffi

#ifdef _WIN32
// Include D3D11 frame buffer for GPU capture support
// Must be outside namespace livekit to avoid polluting std/Windows namespaces
#include "livekit/d3d11_frame_buffer.h"

namespace livekit_ffi {

bool capture_d3d11_frame(
    const std::shared_ptr<VideoTrackSource>& source,
    uint64_t texture_handle,
    uint64_t device_handle,
    uint32_t width,
    uint32_t height,
    uint32_t format,
    int64_t timestamp_us) {
  
  // Use std::cout for reliable logging (RTC_LOG doesn't appear in console)
  static std::atomic<bool> first_call{true};
  bool expected_first = true;
  if (first_call.compare_exchange_strong(expected_first, false)) {
    std::cout << "capture_d3d11_frame: first call - texture=0x" << std::hex << texture_handle 
              << " device=0x" << device_handle << std::dec
              << " " << width << "x" << height << " format=" << format << std::endl;
  }
  
  if (!source) {
    std::cout << "capture_d3d11_frame: FAILED - null source" << std::endl;
    return false;
  }
  
  if (texture_handle == 0 || device_handle == 0) {
    std::cout << "capture_d3d11_frame: FAILED - null texture or device handle" << std::endl;
    return false;
  }

  // Create D3D11TextureBuffer from handles
  auto* texture = reinterpret_cast<ID3D11Texture2D*>(texture_handle);
  auto* device = reinterpret_cast<ID3D11Device*>(device_handle);
  
  auto buffer = D3D11TextureBuffer::Create(texture, device);
  if (!buffer) {
    std::cout << "capture_d3d11_frame: FAILED - D3D11TextureBuffer::Create returned null" << std::endl;
    return false;
  }

  // Build the VideoFrame with the GPU buffer
  // IMPORTANT: WebRTC's AdaptFrame() uses timestamps for frame-rate adaptation.
  // If callers pass 0 (or any non-monotonic value), it can drop all frames.
  // So treat <= 0 as "use current time".
  if (timestamp_us <= 0) {
    timestamp_us = webrtc::TimeMicros();
  }
  webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(buffer)
      .set_timestamp_us(timestamp_us)
      .set_rotation(webrtc::kVideoRotation_0)
      .build();

  // Push to the source
  bool result = source->get()->on_captured_frame(frame);
  if (!result) {
    static std::atomic<bool> logged_once{false};
    bool expected = false;
    if (logged_once.compare_exchange_strong(expected, true)) {
      std::cout << "capture_d3d11_frame: on_captured_frame returned false (first occurrence)" << std::endl;
    }
  }
  return result;
}

bool capture_d3d11_frame_buffer_handle(
    const std::shared_ptr<VideoTrackSource>& source,
    uint64_t buffer_handle,
    int64_t timestamp_us) {
  static std::atomic<bool> first_call{true};
  bool expected_first = true;
  if (first_call.compare_exchange_strong(expected_first, false)) {
    std::cout << "capture_d3d11_frame_buffer_handle: first call - buffer=0x"
              << std::hex << buffer_handle << std::dec << std::endl;
  }

  if (!source) {
    std::cout << "capture_d3d11_frame_buffer_handle: FAILED - null source"
              << std::endl;
    return false;
  }
  if (buffer_handle == 0) {
    std::cout << "capture_d3d11_frame_buffer_handle: FAILED - null buffer handle"
              << std::endl;
    return false;
  }

  auto* buffer = reinterpret_cast<D3D11TextureBuffer*>(buffer_handle);

  // Ensure a valid monotonic timestamp.
  if (timestamp_us <= 0) {
    timestamp_us = webrtc::TimeMicros();
  }

  // Wrap the raw pointer in a refptr (AddRef). VideoFrame will keep it alive.
  rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer_ref(buffer);

  webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                 .set_video_frame_buffer(buffer_ref)
                                 .set_timestamp_us(timestamp_us)
                                 .set_rotation(webrtc::kVideoRotation_0)
                                 .build();

  bool result = source->get()->on_captured_frame(frame);
  if (!result) {
    static std::atomic<bool> logged_once{false};
    bool expected = false;
    if (logged_once.compare_exchange_strong(expected, true)) {
      std::cout << "capture_d3d11_frame_buffer_handle: on_captured_frame returned false (first occurrence)"
                << std::endl;
    }
  }
  return result;
}

}  // namespace livekit_ffi
#endif  // _WIN32