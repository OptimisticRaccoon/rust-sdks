// Copyright 2025 ZenSpeak
//
// Opus application mode bridge — uses primitive types to avoid CXX type conflicts.
//
// The built-in WebRTC Opus encoder defaults to OPUS_APPLICATION_VOIP.
// For screen-share / music audio we want OPUS_APPLICATION_AUDIO.
// This module exposes a process-global toggle that the custom
// AudioEncoderFactoryWrapper reads when creating Opus encoders.

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    unsafe extern "C++" {
        include!("livekit/audio_encoder_factory.h");

        /// Set the global Opus application mode.
        /// 0 = voip (default), 1 = audio (music / general)
        fn set_opus_application_mode_raw(mode: i32);

        /// Get the current global Opus application mode.
        fn get_opus_application_mode_raw() -> i32;
    }
}

// ============================================================================
// Type-safe Rust wrapper
// ============================================================================

/// Opus application mode for the encoder.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(i32)]
pub enum OpusApplicationMode {
    /// Speech-optimised (OPUS_APPLICATION_VOIP) — default.
    #[default]
    Voip = 0,
    /// Music / general audio (OPUS_APPLICATION_AUDIO).
    Audio = 1,
}

/// Set the process-global Opus application mode.
///
/// Set to [`OpusApplicationMode::Audio`] **before** publishing a
/// screen-share audio track and reset to [`OpusApplicationMode::Voip`]
/// immediately after.  The next Opus encoder created by the factory
/// will use the specified mode.
pub fn set_opus_application_mode(mode: OpusApplicationMode) {
    ffi::set_opus_application_mode_raw(mode as i32);
}

/// Get the current process-global Opus application mode.
pub fn get_opus_application_mode() -> OpusApplicationMode {
    match ffi::get_opus_application_mode_raw() {
        1 => OpusApplicationMode::Audio,
        _ => OpusApplicationMode::Voip,
    }
}
