// Copyright 2025 ZenSpeak
//
// D3D11 GPU texture frame buffer bridge - Windows only
// Uses opaque handles (u64) to avoid complex CXX type bridging

#[cfg(target_os = "windows")]
#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    /// Texture information for FFI
    #[derive(Debug, Clone)]
    pub struct D3D11TextureInfo {
        pub width: u32,
        pub height: u32,
        pub format: u32, // DXGI_FORMAT
        pub is_shared: bool,
    }

    unsafe extern "C++" {
        include!("livekit/d3d11_frame_buffer.h");

        /// Create a D3D11 frame buffer from raw pointers (passed as u64)
        /// texture_handle: ID3D11Texture2D* cast to u64
        /// device_handle: ID3D11Device* cast to u64
        /// Returns opaque frame buffer handle, or 0 on failure
        fn create_d3d11_frame_buffer_from_handles(
            texture_handle: u64,
            device_handle: u64,
            width: u32,
            height: u32,
            format: u32,
        ) -> u64;

        /// Create a D3D11 frame buffer by copying from a source texture into an internal pooled texture.
        fn create_d3d11_frame_buffer_from_handles_pooled_copy(
            source_texture_handle: u64,
            device_handle: u64,
            context_handle: u64,
            width: u32,
            height: u32,
            format: u32,
        ) -> u64;

        /// Create a D3D11 frame buffer by scaling a source texture into an internal pooled texture
        /// using the D3D11 Video Processor (video engine) when possible.
        fn create_d3d11_frame_buffer_from_handles_pooled_vp_scale(
            source_texture_handle: u64,
            device_handle: u64,
            context_handle: u64,
            src_width: u32,
            src_height: u32,
            dst_width: u32,
            dst_height: u32,
            format: u32,
        ) -> u64;

        /// Acquire a pooled output texture lease for direct rendering into WebRTC-owned pooled textures.
        fn acquire_d3d11_pooled_texture_lease(
            device_handle: u64,
            width: u32,
            height: u32,
            format: u32,
        ) -> u64;

        /// Get an AddRef'd `ID3D11Texture2D*` from the lease (returned as u64).
        fn get_d3d11_pooled_texture_from_lease_addref(lease_handle: u64) -> u64;

        /// Finalize the lease into a WebRTC D3D11 texture buffer and return the frame buffer handle.
        fn finalize_d3d11_pooled_texture_lease_to_frame_buffer(
            lease_handle: u64,
            width: u32,
            height: u32,
            format: u32,
        ) -> u64;

        /// Release a pooled texture lease without producing a frame buffer.
        fn release_d3d11_pooled_texture_lease(lease_handle: u64);

        /// Release a D3D11 frame buffer handle
        fn release_d3d11_frame_buffer(handle: u64);

        /// Get info about a D3D11 frame buffer
        fn get_d3d11_frame_buffer_info(handle: u64) -> D3D11TextureInfo;

        /// Check if D3D11 frame buffers are supported
        fn is_d3d11_frame_buffer_supported() -> bool;
    }
}

// ============================================================================
// Safe Rust Wrapper
// ============================================================================

/// Common DXGI formats as constants
#[cfg(target_os = "windows")]
pub mod dxgi_format {
    pub const B8G8R8A8_UNORM: u32 = 87;
    pub const R8G8B8A8_UNORM: u32 = 28;
    pub const NV12: u32 = 103;
    pub const R10G10B10A2_UNORM: u32 = 24;
}

/// Handle to a D3D11 frame buffer
/// Automatically releases on drop
#[cfg(target_os = "windows")]
#[derive(Debug)]
pub struct D3D11FrameBufferHandle {
    handle: u64,
}

/// Lease for a WebRTC pooled output texture.
///
/// Intended use:
/// - acquire lease (alloc/borrow pooled texture)
/// - get AddRef'd `ID3D11Texture2D*` to render into
/// - finalize into a `D3D11FrameBufferHandle` (WebRTC takes ownership of pooled texture)
/// - if aborted, the lease returns the texture to the pool on Drop
#[cfg(target_os = "windows")]
#[derive(Debug)]
pub struct D3D11PooledTextureLease {
    handle: u64,
}

#[cfg(target_os = "windows")]
impl D3D11FrameBufferHandle {
    /// Create from raw D3D11 texture and device handles (pointers cast to u64)
    ///
    /// # Safety
    /// - `texture_handle` must be a valid `ID3D11Texture2D*` cast to u64
    /// - `device_handle` must be a valid `ID3D11Device*` cast to u64
    /// - Both pointers must remain valid for the lifetime of this handle
    pub unsafe fn new(
        texture_handle: u64,
        device_handle: u64,
        width: u32,
        height: u32,
        format: u32,
    ) -> Option<Self> {
        if texture_handle == 0 || device_handle == 0 {
            return None;
        }

        let handle = ffi::create_d3d11_frame_buffer_from_handles(
            texture_handle,
            device_handle,
            width,
            height,
            format,
        );

        if handle != 0 {
            Some(Self { handle })
        } else {
            None
        }
    }

    /// Create from raw pointers (convenience wrapper)
    ///
    /// # Safety
    /// Same requirements as `new`
    pub unsafe fn from_raw_ptrs<T, D>(
        texture: *mut T,
        device: *mut D,
        width: u32,
        height: u32,
        format: u32,
    ) -> Option<Self> {
        Self::new(
            texture as u64,
            device as u64,
            width,
            height,
            format,
        )
    }

    /// Get the raw handle value (for passing to WebRTC)
    pub fn raw_handle(&self) -> u64 {
        self.handle
    }

    /// Get texture info
    pub fn info(&self) -> ffi::D3D11TextureInfo {
        ffi::get_d3d11_frame_buffer_info(self.handle)
    }

    /// Get width
    pub fn width(&self) -> u32 {
        self.info().width
    }

    /// Get height
    pub fn height(&self) -> u32 {
        self.info().height
    }

    /// Get DXGI format
    pub fn format(&self) -> u32 {
        self.info().format
    }

    /// Create a frame buffer by copying from a source texture into an internal pooled texture.
    ///
    /// This avoids per-frame `CreateTexture2D` when repeatedly creating buffers at a stable
    /// resolution/format.
    ///
    /// # Safety
    /// - `source_texture_handle` must be a valid `ID3D11Texture2D*` cast to u64 and must remain
    ///   valid for the duration of the call
    /// - `device_handle` must be a valid `ID3D11Device*` cast to u64
    /// - `context_handle` must be a valid `ID3D11DeviceContext*` cast to u64 and must be used on
    ///   a thread that owns the context usage (D3D11 immediate contexts are not thread-safe by default)
    pub unsafe fn new_pooled_copy(
        source_texture_handle: u64,
        device_handle: u64,
        context_handle: u64,
        width: u32,
        height: u32,
        format: u32,
    ) -> Option<Self> {
        if source_texture_handle == 0 || device_handle == 0 || context_handle == 0 {
            return None;
        }

        let handle = ffi::create_d3d11_frame_buffer_from_handles_pooled_copy(
            source_texture_handle,
            device_handle,
            context_handle,
            width,
            height,
            format,
        );

        if handle != 0 { Some(Self { handle }) } else { None }
    }

    /// Create a frame buffer by scaling a source texture into a pooled output texture using
    /// the D3D11 Video Processor (video engine) when available.
    ///
    /// This is intended to reduce contention with 3D shader cores (vs pixel-shader scaling)
    /// while preserving lifetime safety (the returned buffer owns a pooled output texture).
    ///
    /// # Safety
    /// - `source_texture_handle` must be a valid `ID3D11Texture2D*` cast to u64 and must remain
    ///   valid for the duration of the call
    /// - `device_handle` must be a valid `ID3D11Device*` cast to u64
    /// - `context_handle` must be a valid `ID3D11DeviceContext*` cast to u64 and must be used on
    ///   a thread that owns the context usage (D3D11 immediate contexts are not thread-safe by default)
    pub unsafe fn new_pooled_vp_scale(
        source_texture_handle: u64,
        device_handle: u64,
        context_handle: u64,
        src_width: u32,
        src_height: u32,
        dst_width: u32,
        dst_height: u32,
        format: u32,
    ) -> Option<Self> {
        if source_texture_handle == 0 || device_handle == 0 || context_handle == 0 {
            return None;
        }

        let handle = ffi::create_d3d11_frame_buffer_from_handles_pooled_vp_scale(
            source_texture_handle,
            device_handle,
            context_handle,
            src_width,
            src_height,
            dst_width,
            dst_height,
            format,
        );

        if handle != 0 { Some(Self { handle }) } else { None }
    }
}

#[cfg(target_os = "windows")]
impl D3D11PooledTextureLease {
    pub fn acquire(device_handle: u64, width: u32, height: u32, format: u32) -> Option<Self> {
        if device_handle == 0 || width == 0 || height == 0 {
            return None;
        }
        let handle = ffi::acquire_d3d11_pooled_texture_lease(device_handle, width, height, format);
        if handle != 0 {
            Some(Self { handle })
        } else {
            None
        }
    }

    /// Get an AddRef'd `ID3D11Texture2D*` (as u64) for rendering.
    pub fn texture_handle_addref(&self) -> u64 {
        if self.handle == 0 {
            return 0;
        }
        ffi::get_d3d11_pooled_texture_from_lease_addref(self.handle)
    }

    /// Finalize into a WebRTC-owned frame buffer handle (consumes the lease).
    pub fn finalize(mut self, width: u32, height: u32, format: u32) -> Option<D3D11FrameBufferHandle> {
        if self.handle == 0 {
            return None;
        }
        let fb = ffi::finalize_d3d11_pooled_texture_lease_to_frame_buffer(self.handle, width, height, format);
        // Prevent Drop from releasing after finalize; C++ consumes the lease.
        self.handle = 0;
        if fb != 0 {
            Some(D3D11FrameBufferHandle { handle: fb })
        } else {
            None
        }
    }
}

#[cfg(target_os = "windows")]
impl Drop for D3D11PooledTextureLease {
    fn drop(&mut self) {
        if self.handle != 0 {
            ffi::release_d3d11_pooled_texture_lease(self.handle);
            self.handle = 0;
        }
    }
}

#[cfg(target_os = "windows")]
impl Drop for D3D11FrameBufferHandle {
    fn drop(&mut self) {
        if self.handle != 0 {
            ffi::release_d3d11_frame_buffer(self.handle);
        }
    }
}

// Handles can be sent between threads (the underlying D3D11 texture is thread-safe)
#[cfg(target_os = "windows")]
unsafe impl Send for D3D11FrameBufferHandle {}

/// Check if D3D11 frame buffers are supported on this system
#[cfg(target_os = "windows")]
pub fn is_supported() -> bool {
    ffi::is_d3d11_frame_buffer_supported()
}

#[cfg(not(target_os = "windows"))]
pub fn is_supported() -> bool {
    false
}


#[cfg(test)]
mod tests {
    #[allow(unused_imports)]
    use super::*;

    #[test]
    #[cfg(target_os = "windows")]
    fn test_dxgi_format_constants() {
        assert_eq!(dxgi_format::B8G8R8A8_UNORM, 87);
        assert_eq!(dxgi_format::NV12, 103);
    }

    #[test]
    fn test_is_supported_compiles() {
        // Just verify the function compiles on all platforms
        let _ = is_supported();
    }
}
