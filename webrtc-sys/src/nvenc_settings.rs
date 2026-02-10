// Copyright 2025 ZenSpeak
//
// CXX bridge for process-global NVENC settings.

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    unsafe extern "C++" {
        include!("livekit/nvenc_settings.h");

        // preset: 0=UltraLowLatency, 1=LowLatency, 2=Quality
        // profile: 0=Auto, 1=Baseline, 2=Main, 3=High
        // rate_control: 0=CBR, 1=VBR, 2=CQP
        fn set_nvenc_settings_raw(
            preset: i32,
            profile: i32,
            rate_control: i32,
            cqp: i32,
            keyframe_interval_seconds: i32,
            log_settings: bool,
        );
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(i32)]
pub enum NvencPreset {
    #[default]
    UltraLowLatency = 0,
    LowLatency = 1,
    Quality = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(i32)]
pub enum NvencProfile {
    #[default]
    Auto = 0,
    Baseline = 1,
    Main = 2,
    High = 3,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(i32)]
pub enum NvencRateControl {
    #[default]
    Cbr = 0,
    Vbr = 1,
    Cqp = 2,
}

#[derive(Debug, Clone, Copy)]
pub struct NvencSettings {
    pub preset: NvencPreset,
    pub profile: NvencProfile,
    pub rate_control: NvencRateControl,
    pub cqp: i32,
    pub keyframe_interval_seconds: i32,
    pub log_settings: bool,
}

impl Default for NvencSettings {
    fn default() -> Self {
        Self {
            // Default matches prior behavior: preset P4 + ultra-low-latency tuning.
            preset: NvencPreset::LowLatency,
            profile: NvencProfile::Auto,
            rate_control: NvencRateControl::Cbr,
            cqp: 23,
            keyframe_interval_seconds: 0,
            log_settings: true,
        }
    }
}

pub fn set_nvenc_settings(settings: NvencSettings) {
    ffi::set_nvenc_settings_raw(
        settings.preset as i32,
        settings.profile as i32,
        settings.rate_control as i32,
        settings.cqp,
        settings.keyframe_interval_seconds,
        settings.log_settings,
    );
}

