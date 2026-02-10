/*
 * Copyright 2025 ZenSpeak
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

#include "livekit/audio_encoder_factory.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/audio_codecs/audio_codec_pair_id.h"
#include "api/audio_codecs/audio_encoder.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_codecs/audio_format.h"
#include "api/environment/environment.h"
#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"

namespace livekit_ffi {

// ---------------------------------------------------------------------------
// Process-global Opus application mode (0 = voip, 1 = audio)
// ---------------------------------------------------------------------------

static std::atomic<int32_t> g_opus_application_mode{0};

void set_opus_application_mode_raw(int32_t mode) {
  g_opus_application_mode.store(mode, std::memory_order_release);
  RTC_LOG(LS_INFO) << "Opus application mode set to: "
                   << (mode == 1 ? "audio" : "voip");
}

int32_t get_opus_application_mode_raw() {
  return g_opus_application_mode.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// AudioEncoderFactoryWrapper
// ---------------------------------------------------------------------------

class AudioEncoderFactoryWrapper : public webrtc::AudioEncoderFactory {
 public:
  AudioEncoderFactoryWrapper()
      : inner_(webrtc::CreateBuiltinAudioEncoderFactory()) {}

  std::vector<webrtc::AudioCodecSpec> GetSupportedEncoders() override {
    return inner_->GetSupportedEncoders();
  }

  std::optional<webrtc::AudioCodecInfo> QueryAudioEncoder(
      const webrtc::SdpAudioFormat& format) override {
    return inner_->QueryAudioEncoder(format);
  }

  std::unique_ptr<webrtc::AudioEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpAudioFormat& format,
      Options options) override {
    // When the global mode is "audio" and the codec is Opus, inject the
    // `application=audio` parameter so the Opus encoder uses
    // OPUS_APPLICATION_AUDIO instead of the default OPUS_APPLICATION_VOIP.
    if (format.name == "opus" &&
        g_opus_application_mode.load(std::memory_order_acquire) == 1) {
      webrtc::SdpAudioFormat modified = format;
      modified.parameters["application"] = "audio";
      RTC_LOG(LS_INFO)
          << "AudioEncoderFactoryWrapper: creating Opus encoder with "
             "application=audio (music/general mode)";
      return inner_->Create(env, modified, std::move(options));
    }

    return inner_->Create(env, format, std::move(options));
  }

 private:
  rtc::scoped_refptr<webrtc::AudioEncoderFactory> inner_;
};

// ---------------------------------------------------------------------------
// Factory function
// ---------------------------------------------------------------------------

rtc::scoped_refptr<webrtc::AudioEncoderFactory>
CreateWrappedAudioEncoderFactory() {
  return webrtc::make_ref_counted<AudioEncoderFactoryWrapper>();
}

}  // namespace livekit_ffi
