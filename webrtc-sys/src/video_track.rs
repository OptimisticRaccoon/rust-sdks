// Copyright 2025 LiveKit, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use std::sync::Arc;

use cxx::UniquePtr;

use crate::{impl_thread_safety, video_frame::ffi::VideoFrame};

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    #[repr(i32)]
    pub enum ContentHint {
        None,
        Fluid,
        Detailed,
        Text,
    }

    #[derive(Debug)]
    pub struct VideoTrackSourceConstraints {
        pub has_min_fps: bool,
        pub min_fps: f64,
        pub has_max_fps: bool,
        pub max_fps: f64,
    }

    #[derive(Debug)]
    pub struct VideoResolution {
        pub width: u32,
        pub height: u32,
    }

    extern "C++" {
        include!("livekit/video_frame.h");
        include!("livekit/media_stream_track.h");

        type VideoFrame = crate::video_frame::ffi::VideoFrame;
        type MediaStreamTrack = crate::media_stream_track::ffi::MediaStreamTrack;
    }

    unsafe extern "C++" {
        include!("livekit/video_track.h");

        type VideoTrack;
        type NativeVideoSink;
        type VideoTrackSource;

        fn add_sink(self: &VideoTrack, sink: &SharedPtr<NativeVideoSink>);
        fn remove_sink(self: &VideoTrack, sink: &SharedPtr<NativeVideoSink>);
        fn set_should_receive(self: &VideoTrack, should_receive: bool);
        fn should_receive(self: &VideoTrack) -> bool;
        fn content_hint(self: &VideoTrack) -> ContentHint;
        fn set_content_hint(self: &VideoTrack, hint: ContentHint);
        fn new_native_video_sink(observer: Box<VideoSinkWrapper>) -> SharedPtr<NativeVideoSink>;

        fn video_resolution(self: &VideoTrackSource) -> VideoResolution;
        fn on_captured_frame(self: &VideoTrackSource, frame: &UniquePtr<VideoFrame>) -> bool;
        fn new_video_track_source(resolution: &VideoResolution) -> SharedPtr<VideoTrackSource>;
        fn new_video_track_source_with_screencast(
            resolution: &VideoResolution,
            is_screencast: bool,
        ) -> SharedPtr<VideoTrackSource>;
        fn video_to_media(track: SharedPtr<VideoTrack>) -> SharedPtr<MediaStreamTrack>;
        unsafe fn media_to_video(track: SharedPtr<MediaStreamTrack>) -> SharedPtr<VideoTrack>;
        fn _shared_video_track() -> SharedPtr<VideoTrack>;

        /// Capture a D3D11 GPU frame directly to a VideoTrackSource (Windows only)
        /// 
        /// This is the zero-copy GPU path: creates a D3D11TextureBuffer from the
        /// texture handles, wraps it in a VideoFrame, and pushes to the source.
        /// 
        /// Returns true if the frame was captured successfully.
        #[cfg(target_os = "windows")]
        fn capture_d3d11_frame(
            source: &SharedPtr<VideoTrackSource>,
            texture_handle: u64,
            device_handle: u64,
            width: u32,
            height: u32,
            format: u32,
            timestamp_us: i64,
        ) -> bool;

        /// Capture a D3D11 GPU frame buffer (already created) directly to a VideoTrackSource (Windows only).
        ///
        /// This is used for caching/repeating frames (e.g. minimum FPS during static content),
        /// because the underlying `D3D11TextureBuffer::Create` adopts (consumes) a COM ref on the
        /// texture; reusing raw texture pointers without owning a ref can crash.
        #[cfg(target_os = "windows")]
        fn capture_d3d11_frame_buffer_handle(
            source: &SharedPtr<VideoTrackSource>,
            buffer_handle: u64,
            timestamp_us: i64,
        ) -> bool;
    }

    extern "Rust" {
        type VideoSinkWrapper;

        fn on_frame(self: &VideoSinkWrapper, frame: UniquePtr<VideoFrame>);
        fn on_discarded_frame(self: &VideoSinkWrapper);
        fn on_constraints_changed(
            self: &VideoSinkWrapper,
            constraints: VideoTrackSourceConstraints,
        );
    }
}

impl_thread_safety!(ffi::VideoTrack, Send + Sync);
impl_thread_safety!(ffi::NativeVideoSink, Send + Sync);
impl_thread_safety!(ffi::VideoTrackSource, Send + Sync);

pub trait VideoSink: Send {
    fn on_frame(&self, frame: UniquePtr<VideoFrame>);
    fn on_discarded_frame(&self);
    fn on_constraints_changed(&self, constraints: ffi::VideoTrackSourceConstraints);
}

pub struct VideoSinkWrapper {
    observer: Arc<dyn VideoSink>,
}

impl VideoSinkWrapper {
    pub fn new(observer: Arc<dyn VideoSink>) -> Self {
        Self { observer }
    }

    fn on_frame(&self, frame: UniquePtr<VideoFrame>) {
        self.observer.on_frame(frame);
    }

    fn on_discarded_frame(&self) {
        self.observer.on_discarded_frame();
    }

    fn on_constraints_changed(&self, constraints: ffi::VideoTrackSourceConstraints) {
        self.observer.on_constraints_changed(constraints);
    }
}
