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

#pragma once

#include <atomic>

#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/environment/environment.h"

namespace livekit_ffi {

// ============================================================================
// Process-global Opus application mode
// ============================================================================
//
// WebRTC's built-in Opus encoder defaults to OPUS_APPLICATION_VOIP, which
// applies speech-optimised processing (VAD gating, bandwidth narrowing, etc.).
// For screen-share / music audio we want OPUS_APPLICATION_AUDIO instead.
//
// Because the AudioEncoderFactory is shared by all tracks on the same
// PeerConnectionFactory, we use a process-global atomic that is set to
// "audio" just before publishing a screen-share audio track and reset to
// "voip" immediately after.  This is safe because:
//   1. LiveKit publishes tracks sequentially (one offer at a time).
//   2. Existing encoders are NOT recreated during renegotiation — only
//      the newly-added track triggers encoder construction.

/// Set the global Opus application mode.
/// 0 = voip (default, speech-optimised)
/// 1 = audio (music / general audio)
void set_opus_application_mode_raw(int32_t mode);

/// Get the current global Opus application mode.
int32_t get_opus_application_mode_raw();

// ============================================================================
// Wrapped Audio Encoder Factory
// ============================================================================

/// Create an AudioEncoderFactory that wraps the built-in one and injects
/// `application=audio` into the Opus SdpAudioFormat parameters when the
/// global mode is set to 1 (audio).
rtc::scoped_refptr<webrtc::AudioEncoderFactory>
CreateWrappedAudioEncoderFactory();

}  // namespace livekit_ffi
