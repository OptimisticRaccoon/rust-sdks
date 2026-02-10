// Copyright 2025 ZenSpeak
//
// Encoder configuration bridge - uses primitive types to avoid CXX type conflicts

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    unsafe extern "C++" {
        include!("livekit/video_encoder_factory.h");

        /// Set encoder config using primitive types
        /// mode: 0=Auto, 1=SoftwareOnly, 2=HardwareOnly
        /// backend: 0=Any, 1=NvidiaNvenc, 2=IntelQuickSync, 3=AmdAmf, 4=AppleVideoToolbox, 5=AndroidMediaCodec
        fn set_encoder_config_raw(mode: i32, backend: i32, allow_fallback: bool, log_selection: bool);

        /// Check if a hardware backend is available
        fn is_hardware_backend_available_raw(backend: i32) -> bool;

        /// Get available backends as a bitmask
        /// Bit 0: NvidiaNvenc, Bit 1: IntelQuickSync, Bit 2: AmdAmf, Bit 3: AppleVideoToolbox, Bit 4: AndroidMediaCodec
        fn get_available_backends_mask() -> u32;

        /// Get supported codecs for a given hardware backend as a bitmask
        /// Bit 0: H264, Bit 1: H265, Bit 2: AV1
        fn get_hardware_backend_codecs_mask_raw(backend: i32) -> u32;
    }
}

// ============================================================================
// Type-safe Rust wrapper
// ============================================================================

/// Encoder mode preference
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(i32)]
pub enum EncoderMode {
    #[default]
    Auto = 0,
    SoftwareOnly = 1,
    HardwareOnly = 2,
}

/// Specific hardware backend
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(i32)]
pub enum HardwareBackend {
    #[default]
    Any = 0,
    NvidiaNvenc = 1,
    IntelQuickSync = 2,
    AmdAmf = 3,
    AppleVideoToolbox = 4,
    AndroidMediaCodec = 5,
}

/// Encoder configuration
#[derive(Debug, Clone, Default)]
pub struct EncoderConfig {
    pub mode: EncoderMode,
    pub backend: HardwareBackend,
    pub allow_fallback: bool,
    pub log_selection: bool,
}

impl EncoderConfig {
    pub fn new() -> Self {
        Self {
            mode: EncoderMode::Auto,
            backend: HardwareBackend::Any,
            allow_fallback: true,
            log_selection: true,
        }
    }

    pub fn software_only() -> Self {
        Self {
            mode: EncoderMode::SoftwareOnly,
            backend: HardwareBackend::Any,
            allow_fallback: false,
            log_selection: true,
        }
    }

    pub fn hardware_preferred() -> Self {
        Self {
            mode: EncoderMode::Auto,
            backend: HardwareBackend::Any,
            allow_fallback: true,
            log_selection: true,
        }
    }

    pub fn nvenc_only(allow_fallback: bool) -> Self {
        Self {
            mode: EncoderMode::HardwareOnly,
            backend: HardwareBackend::NvidiaNvenc,
            allow_fallback,
            log_selection: true,
        }
    }
}

/// Set the global encoder configuration
///
/// Must be called before creating any PeerConnectionFactory instances.
pub fn set_encoder_config(config: &EncoderConfig) {
    ffi::set_encoder_config_raw(
        config.mode as i32,
        config.backend as i32,
        config.allow_fallback,
        config.log_selection,
    );
}

/// Check if a specific hardware backend is available
pub fn is_backend_available(backend: HardwareBackend) -> bool {
    ffi::is_hardware_backend_available_raw(backend as i32)
}

/// Get list of available hardware backends
pub fn get_available_backends() -> Vec<HardwareBackend> {
    let mask = ffi::get_available_backends_mask();
    let mut backends = Vec::new();
    
    if mask & (1 << 0) != 0 { backends.push(HardwareBackend::NvidiaNvenc); }
    if mask & (1 << 1) != 0 { backends.push(HardwareBackend::IntelQuickSync); }
    if mask & (1 << 2) != 0 { backends.push(HardwareBackend::AmdAmf); }
    if mask & (1 << 3) != 0 { backends.push(HardwareBackend::AppleVideoToolbox); }
    if mask & (1 << 4) != 0 { backends.push(HardwareBackend::AndroidMediaCodec); }
    
    backends
}

/// Get supported codecs for a given hardware backend.
///
/// Returns uppercase codec names (e.g. "H264", "H265", "AV1").
pub fn get_backend_supported_codecs(backend: HardwareBackend) -> Vec<&'static str> {
    let mask = ffi::get_hardware_backend_codecs_mask_raw(backend as i32);
    let mut codecs = Vec::new();
    if mask & (1 << 0) != 0 {
        codecs.push("H264");
    }
    if mask & (1 << 1) != 0 {
        codecs.push("H265");
    }
    if mask & (1 << 2) != 0 {
        codecs.push("AV1");
    }
    codecs
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_enum_values() {
        assert_eq!(EncoderMode::Auto as i32, 0);
        assert_eq!(EncoderMode::SoftwareOnly as i32, 1);
        assert_eq!(EncoderMode::HardwareOnly as i32, 2);
        
        assert_eq!(HardwareBackend::Any as i32, 0);
        assert_eq!(HardwareBackend::NvidiaNvenc as i32, 1);
    }
}
