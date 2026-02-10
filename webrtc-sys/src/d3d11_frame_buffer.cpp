/*
 * Copyright 2025 ZenSpeak
 *
 * D3D11 GPU texture-backed VideoFrameBuffer implementation
 */

#ifdef _WIN32

// Must define WIN32_LEAN_AND_MEAN before any Windows headers to avoid WinSock conflicts
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include "livekit/d3d11_frame_buffer.h"

#include <dxgi.h>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "api/make_ref_counted.h"
#include "libyuv/convert.h"
#include "rtc_base/logging.h"
#include "rtc_base/checks.h"

namespace livekit_ffi {

namespace {
struct PoolKeyHash {
  size_t operator()(const D3D11TextureBuffer::PoolKey& k) const noexcept {
    // Pointer identity is fine; textures must stay on the same device.
    size_t h = std::hash<void*>{}(k.device);
    h ^= (static_cast<size_t>(k.width) << 1);
    h ^= (static_cast<size_t>(k.height) << 17);
    h ^= (static_cast<size_t>(k.format) << 3);
    return h;
  }
};

struct PoolKeyEq {
  bool operator()(const D3D11TextureBuffer::PoolKey& a,
                  const D3D11TextureBuffer::PoolKey& b) const noexcept {
    return a.device == b.device && a.width == b.width && a.height == b.height &&
           a.format == b.format;
  }
};

// A tiny texture pool to avoid per-frame allocations when WebRTC adaptation requests
// a stable size/format.
//
// Hot-path requirement: avoid a single global mutex that every frame contends on.
// We keep a short-lived global map lock (lookup/insert only) and then use a per-key
// mutex for the actual vector pop/push.
struct PoolEntry {
  std::mutex mu;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> textures;
};

static std::mutex g_pool_map_mutex;
static std::unordered_map<D3D11TextureBuffer::PoolKey,
                          std::shared_ptr<PoolEntry>,
                          PoolKeyHash, PoolKeyEq>
    g_texture_pool;

constexpr size_t kMaxPoolPerKey = 6;

// Cache for video processor objects to avoid per-frame enumerator/processor creation.
// Note: input/output views are still created per-call because they are tied to textures.
struct VpKey {
  ID3D11Device* device;
  UINT src_width;
  UINT src_height;
  UINT dst_width;
  UINT dst_height;
  DXGI_FORMAT format;
};

struct VpKeyHash {
  size_t operator()(const VpKey& k) const noexcept {
    size_t h = std::hash<void*>{}(k.device);
    h ^= (static_cast<size_t>(k.src_width) << 1);
    h ^= (static_cast<size_t>(k.src_height) << 11);
    h ^= (static_cast<size_t>(k.dst_width) << 3);
    h ^= (static_cast<size_t>(k.dst_height) << 17);
    h ^= (static_cast<size_t>(k.format) << 5);
    return h;
  }
};

struct VpKeyEq {
  bool operator()(const VpKey& a, const VpKey& b) const noexcept {
    return a.device == b.device && a.src_width == b.src_width &&
           a.src_height == b.src_height && a.dst_width == b.dst_width &&
           a.dst_height == b.dst_height && a.format == b.format;
  }
};

struct VpEntry {
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
};

static std::mutex g_vp_cache_mutex;
static std::unordered_map<VpKey, VpEntry, VpKeyHash, VpKeyEq> g_vp_cache;
}  // namespace

// ============================================================================
// Pooled output texture lease (for direct rendering)
// ============================================================================
//
// This is used to let Rust render directly into a WebRTC-owned pooled output texture:
// - acquire pooled texture (ComPtr held here)
// - expose an AddRef'd ID3D11Texture2D* to Rust for RTV creation + rendering
// - finalize into a D3D11TextureBuffer that returns the texture to the pool on destruction
// - or abort and return the texture to the pool on lease destruction

struct PooledTextureLease {
  D3D11TextureBuffer::PoolKey key{};
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  bool finalized = false;
};

uint64_t acquire_d3d11_pooled_texture_lease(uint64_t device_handle,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t format) {
  auto* device = reinterpret_cast<ID3D11Device*>(device_handle);
  if (!device || width == 0 || height == 0) return 0;

  // Reuse the same global pool used by D3D11TextureBuffer.
  auto tex_opt = D3D11TextureBuffer::AcquirePooledTexture(
      device, static_cast<UINT>(width), static_cast<UINT>(height),
      static_cast<DXGI_FORMAT>(format));
  if (!tex_opt.has_value()) return 0;

  auto lease = std::make_unique<PooledTextureLease>();
  lease->device = device;  // AddRef
  lease->tex = std::move(*tex_opt);
  lease->key = D3D11TextureBuffer::PoolKey{
      device, static_cast<UINT>(width), static_cast<UINT>(height),
      static_cast<DXGI_FORMAT>(format)};
  lease->finalized = false;

  return reinterpret_cast<uint64_t>(lease.release());
}

uint64_t get_d3d11_pooled_texture_from_lease_addref(uint64_t lease_handle) {
  auto* lease = reinterpret_cast<PooledTextureLease*>(lease_handle);
  if (!lease || !lease->tex) return 0;
  auto* tex = lease->tex.Get();
  tex->AddRef();  // caller adopts this ref
  return reinterpret_cast<uint64_t>(tex);
}

uint64_t finalize_d3d11_pooled_texture_lease_to_frame_buffer(uint64_t lease_handle,
                                                             uint32_t width,
                                                             uint32_t height,
                                                             uint32_t format) {
  auto* lease = reinterpret_cast<PooledTextureLease*>(lease_handle);
  if (!lease || !lease->tex || !lease->device) return 0;

  auto buffer = D3D11TextureBuffer::CreateFromPooledTexture(
      std::move(lease->tex),
      lease->device.Get(),
      static_cast<UINT>(width),
      static_cast<UINT>(height),
      static_cast<DXGI_FORMAT>(format));
  if (!buffer) {
    delete lease;
    return 0;
  }

  lease->finalized = true;
  delete lease;  // lease is consumed

  // Transfer ownership to caller via raw pointer (same pattern as other exported fns).
  buffer->AddRef();
  return reinterpret_cast<uint64_t>(buffer.get());
}

void release_d3d11_pooled_texture_lease(uint64_t lease_handle) {
  auto* lease = reinterpret_cast<PooledTextureLease*>(lease_handle);
  if (!lease) return;

  if (!lease->finalized && lease->tex) {
    D3D11TextureBuffer::ReleasePooledTexture(lease->key, std::move(lease->tex));
  }
  delete lease;
}

// ============================================================================
// D3D11TextureBuffer Implementation
// ============================================================================

D3D11TextureBuffer::D3D11TextureBuffer(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    int width,
    int height,
    DXGI_FORMAT format)
    : texture_(std::move(texture)),
      device_(std::move(device)),
      width_(width),
      height_(height),
      format_(format) {
  RTC_LOG(LS_VERBOSE) << "D3D11TextureBuffer created: " << width << "x" << height
                      << " format=" << static_cast<int>(format);
}

D3D11TextureBuffer::~D3D11TextureBuffer() {
  if (poolable_ && texture_) {
    // Return to pool (move ownership out of this buffer).
    ReleasePooledTexture(pool_key_, std::move(texture_));
  }
  RTC_LOG(LS_VERBOSE) << "D3D11TextureBuffer destroyed";
}

rtc::scoped_refptr<D3D11TextureBuffer> D3D11TextureBuffer::CreateFromPooledTexture(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pooled_texture,
    ID3D11Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT format) {
  if (!pooled_texture || !device || width == 0 || height == 0) {
    return nullptr;
  }

  Microsoft::WRL::ComPtr<ID3D11Device> device_ptr = device;  // AddRef

  auto buffer = rtc::make_ref_counted<D3D11TextureBuffer>(
      std::move(pooled_texture),
      std::move(device_ptr),
      static_cast<int>(width),
      static_cast<int>(height),
      format);

  buffer->poolable_ = true;
  buffer->pool_key_ = PoolKey{device, width, height, format};
  return buffer;
}

std::optional<Microsoft::WRL::ComPtr<ID3D11Texture2D>>
D3D11TextureBuffer::AcquirePooledTexture(ID3D11Device* device,
                                        UINT width,
                                        UINT height,
                                        DXGI_FORMAT format) {
  if (!device || width == 0 || height == 0) return std::nullopt;

  PoolKey key{device, width, height, format};
  std::shared_ptr<PoolEntry> entry;
  {
    std::lock_guard<std::mutex> lock(g_pool_map_mutex);
    auto& slot = g_texture_pool[key];
    if (!slot) slot = std::make_shared<PoolEntry>();
    entry = slot;
  }

  {
    std::lock_guard<std::mutex> lock(entry->mu);
    if (!entry->textures.empty()) {
      auto tex = std::move(entry->textures.back());
      entry->textures.pop_back();
      return tex;
    }
  }

  // Create new texture.
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  desc.CPUAccessFlags = 0;
  desc.MiscFlags = 0;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
  if (FAILED(hr) || !tex) {
    RTC_LOG(LS_WARNING) << "D3D11TextureBuffer: failed to allocate pooled texture "
                        << width << "x" << height << " fmt=" << static_cast<int>(format)
                        << " hr=0x" << std::hex << hr;
    return std::nullopt;
  }
  return tex;
}

void D3D11TextureBuffer::ReleasePooledTexture(
    PoolKey key,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>&& tex) {
  if (!tex) return;
  std::shared_ptr<PoolEntry> entry;
  {
    std::lock_guard<std::mutex> lock(g_pool_map_mutex);
    auto& slot = g_texture_pool[key];
    if (!slot) slot = std::make_shared<PoolEntry>();
    entry = slot;
  }

  std::lock_guard<std::mutex> lock(entry->mu);
  if (entry->textures.size() >= kMaxPoolPerKey) {
    return;  // Drop it.
  }
  entry->textures.push_back(std::move(tex));
}

rtc::scoped_refptr<D3D11TextureBuffer> D3D11TextureBuffer::Create(
    ID3D11Texture2D* texture,
    ID3D11Device* device) {
  if (!texture || !device) {
    RTC_LOG(LS_ERROR) << "D3D11TextureBuffer::Create: null texture or device";
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);

  // Validate format - we support common capture formats
  switch (desc.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      break;
    default:
      RTC_LOG(LS_WARNING) << "D3D11TextureBuffer: unsupported format " 
                          << static_cast<int>(desc.Format);
      // Still allow creation, but ToI420() may fail
      break;
  }

  // Ownership semantics:
  // - `texture` is expected to already carry a live COM ref (AddRef'd) when passed
  //   across threads from Rust (see `GpuContext::scale_for_gpu`).
  //   So we ADOPT that ref via Attach (no extra AddRef), otherwise we'd leak 1 ref/frame.
  // - `device` is passed as a borrowed pointer; we take our own ref via assignment (AddRef).
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_ptr;
  texture_ptr.Attach(texture);  // adopts caller-owned ref

  Microsoft::WRL::ComPtr<ID3D11Device> device_ptr = device;  // AddRef

  return rtc::make_ref_counted<D3D11TextureBuffer>(
      std::move(texture_ptr),
      std::move(device_ptr),
      static_cast<int>(desc.Width),
      static_cast<int>(desc.Height),
      desc.Format);
}

rtc::scoped_refptr<D3D11TextureBuffer> D3D11TextureBuffer::CreateWithCopy(
    ID3D11Texture2D* source_texture,
    ID3D11Device* device,
    ID3D11DeviceContext* context) {
  if (!source_texture || !device || !context) {
    RTC_LOG(LS_ERROR) << "D3D11TextureBuffer::CreateWithCopy: null argument";
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC src_desc;
  source_texture->GetDesc(&src_desc);

  // Create a new texture for our copy
  D3D11_TEXTURE2D_DESC copy_desc = src_desc;
  copy_desc.Usage = D3D11_USAGE_DEFAULT;
  copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  copy_desc.CPUAccessFlags = 0;
  copy_desc.MiscFlags = 0;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> copy_texture;
  HRESULT hr = device->CreateTexture2D(&copy_desc, nullptr, &copy_texture);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to create copy texture: 0x" << std::hex << hr;
    return nullptr;
  }

  // Copy the source texture
  context->CopyResource(copy_texture.Get(), source_texture);

  Microsoft::WRL::ComPtr<ID3D11Device> device_ptr = device;  // AddRef

  return rtc::make_ref_counted<D3D11TextureBuffer>(
      std::move(copy_texture),
      std::move(device_ptr),
      static_cast<int>(src_desc.Width),
      static_cast<int>(src_desc.Height),
      src_desc.Format);
}

rtc::scoped_refptr<D3D11TextureBuffer> D3D11TextureBuffer::CreateWithPooledCopy(
    ID3D11Texture2D* source_texture,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    UINT width,
    UINT height,
    DXGI_FORMAT format) {
  if (!source_texture || !device || !context || width == 0 || height == 0) {
    RTC_LOG(LS_ERROR) << "D3D11TextureBuffer::CreateWithPooledCopy: null/invalid argument";
    return nullptr;
  }

  auto out_tex_opt = AcquirePooledTexture(device, width, height, format);
  if (!out_tex_opt.has_value()) {
    RTC_LOG(LS_WARNING) << "D3D11TextureBuffer::CreateWithPooledCopy: no pooled texture available";
    return nullptr;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> out_tex = std::move(*out_tex_opt);

  // Copy the source texture into the pooled output texture.
  // Threading: this must be invoked on the thread that owns `context` usage.
  context->CopyResource(out_tex.Get(), source_texture);

  Microsoft::WRL::ComPtr<ID3D11Device> device_ptr = device;  // AddRef

  auto buffer = rtc::make_ref_counted<D3D11TextureBuffer>(
      std::move(out_tex),
      std::move(device_ptr),
      static_cast<int>(width),
      static_cast<int>(height),
      format);

  // Mark as poolable so the output texture is returned to the global pool on destruction.
  buffer->poolable_ = true;
  buffer->pool_key_ = PoolKey{device, width, height, format};

  return buffer;
}

rtc::scoped_refptr<D3D11TextureBuffer> D3D11TextureBuffer::CreateWithPooledVideoProcessorScale(
    ID3D11Texture2D* source_texture,
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    UINT src_width,
    UINT src_height,
    UINT dst_width,
    UINT dst_height,
    DXGI_FORMAT format) {
  if (!source_texture || !device || !context || src_width == 0 || src_height == 0 ||
      dst_width == 0 || dst_height == 0) {
    RTC_LOG(LS_ERROR)
        << "D3D11TextureBuffer::CreateWithPooledVideoProcessorScale: null/invalid argument";
    return nullptr;
  }

  // Acquire pooled output texture.
  auto out_tex_opt = AcquirePooledTexture(device, dst_width, dst_height, format);
  if (!out_tex_opt.has_value()) {
    RTC_LOG(LS_WARNING)
        << "D3D11TextureBuffer::CreateWithPooledVideoProcessorScale: no pooled texture available";
    return nullptr;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> out_tex = std::move(*out_tex_opt);

  // Try Video Processor path (video engine). If unavailable, fail fast so caller can
  // fall back to shader scaling + pooled copy.
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
  HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&video_device));
  if (FAILED(hr) || !video_device) {
    return nullptr;
  }

  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_ctx;
  hr = context->QueryInterface(IID_PPV_ARGS(&video_ctx));
  if (FAILED(hr) || !video_ctx) {
    return nullptr;
  }

  // Cache enumerator + processor by (device, src/dst dims, format).
  VpKey key{device, src_width, src_height, dst_width, dst_height, format};
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
  {
    std::lock_guard<std::mutex> lock(g_vp_cache_mutex);
    auto it = g_vp_cache.find(key);
    if (it != g_vp_cache.end()) {
      enumerator = it->second.enumerator;
      processor = it->second.processor;
    } else {
      D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc = {};
      content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
      // Frame rate numbers do not need to be exact for scaling; keep consistent with existing path.
      content_desc.InputFrameRate.Numerator = 60;
      content_desc.InputFrameRate.Denominator = 1;
      content_desc.InputWidth = src_width;
      content_desc.InputHeight = src_height;
      content_desc.OutputFrameRate.Numerator = 60;
      content_desc.OutputFrameRate.Denominator = 1;
      content_desc.OutputWidth = dst_width;
      content_desc.OutputHeight = dst_height;
      content_desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

      hr = video_device->CreateVideoProcessorEnumerator(&content_desc, &enumerator);
      if (FAILED(hr) || !enumerator) {
        return nullptr;
      }

      hr = video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor);
      if (FAILED(hr) || !processor) {
        return nullptr;
      }

      g_vp_cache.emplace(key, VpEntry{enumerator, processor});
    }
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc = {};
  in_desc.FourCC = 0;
  in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  in_desc.Texture2D.MipSlice = 0;
  in_desc.Texture2D.ArraySlice = 0;

  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> in_view;
  hr = video_device->CreateVideoProcessorInputView(source_texture, enumerator.Get(), &in_desc,
                                                   &in_view);
  if (FAILED(hr) || !in_view) {
    return nullptr;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc = {};
  out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  out_desc.Texture2D.MipSlice = 0;

  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> out_view;
  hr = video_device->CreateVideoProcessorOutputView(out_tex.Get(), enumerator.Get(), &out_desc,
                                                    &out_view);
  if (FAILED(hr) || !out_view) {
    return nullptr;
  }

  RECT src_rect{0, 0, static_cast<LONG>(src_width), static_cast<LONG>(src_height)};
  RECT dst_rect{0, 0, static_cast<LONG>(dst_width), static_cast<LONG>(dst_height)};

  video_ctx->VideoProcessorSetStreamFrameFormat(processor.Get(), 0,
                                                D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_ctx->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src_rect);
  video_ctx->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dst_rect);
  video_ctx->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &dst_rect);

  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = in_view.Get();

  hr = video_ctx->VideoProcessorBlt(processor.Get(), out_view.Get(), 0, 1, &stream);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING)
        << "D3D11TextureBuffer::CreateWithPooledVideoProcessorScale: VideoProcessorBlt failed hr=0x"
        << std::hex << hr;
    return nullptr;
  }

  // Wrap output texture as a new native buffer; mark it as poolable for reuse.
  Microsoft::WRL::ComPtr<ID3D11Device> device_ptr = device;  // AddRef
  auto buffer = rtc::make_ref_counted<D3D11TextureBuffer>(
      std::move(out_tex), std::move(device_ptr), static_cast<int>(dst_width),
      static_cast<int>(dst_height), format);
  buffer->poolable_ = true;
  buffer->pool_key_ = PoolKey{device, dst_width, dst_height, format};
  return buffer;
}

rtc::scoped_refptr<webrtc::VideoFrameBuffer> D3D11TextureBuffer::CropAndScale(
    int offset_x,
    int offset_y,
    int crop_width,
    int crop_height,
    int scaled_width,
    int scaled_height) {
  // Fast path: no-op.
  if (offset_x == 0 && offset_y == 0 && crop_width == width_ &&
      crop_height == height_ && scaled_width == width_ &&
      scaled_height == height_) {
    return rtc::scoped_refptr<webrtc::VideoFrameBuffer>(this);
  }

  if (!texture_ || !device_ || crop_width <= 0 || crop_height <= 0 ||
      scaled_width <= 0 || scaled_height <= 0) {
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }

  // Clamp crop to bounds.
  offset_x = std::max(0, offset_x);
  offset_y = std::max(0, offset_y);
  crop_width = std::min(crop_width, width_ - offset_x);
  crop_height = std::min(crop_height, height_ - offset_y);

  // Acquire output texture.
  auto out_tex_opt =
      AcquirePooledTexture(device_.Get(), static_cast<UINT>(scaled_width),
                           static_cast<UINT>(scaled_height), format_);
  if (!out_tex_opt.has_value()) {
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }
  auto out_tex = std::move(out_tex_opt.value());

  // Video processor path (hardware accelerated scaler).
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
  HRESULT hr = device_.As(&video_device);
  if (FAILED(hr) || !video_device) {
    // Fall back.
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }

  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
  device_->GetImmediateContext(&ctx);
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_ctx;
  if (!ctx || FAILED(ctx.As(&video_ctx)) || !video_ctx) {
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc = {};
  content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content_desc.InputFrameRate.Numerator = 60;
  content_desc.InputFrameRate.Denominator = 1;
  content_desc.InputWidth = static_cast<UINT>(width_);
  content_desc.InputHeight = static_cast<UINT>(height_);
  content_desc.OutputFrameRate.Numerator = 60;
  content_desc.OutputFrameRate.Denominator = 1;
  content_desc.OutputWidth = static_cast<UINT>(scaled_width);
  content_desc.OutputHeight = static_cast<UINT>(scaled_height);
  content_desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  hr = video_device->CreateVideoProcessorEnumerator(&content_desc, &enumerator);
  if (FAILED(hr) || !enumerator) {
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }

  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
  hr = video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor);
  if (FAILED(hr) || !processor) {
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc = {};
  in_desc.FourCC = 0;
  in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  in_desc.Texture2D.MipSlice = 0;
  in_desc.Texture2D.ArraySlice = 0;

  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> in_view;
  hr = video_device->CreateVideoProcessorInputView(texture_.Get(), enumerator.Get(),
                                                   &in_desc, &in_view);
  if (FAILED(hr) || !in_view) {
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc = {};
  out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  out_desc.Texture2D.MipSlice = 0;

  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> out_view;
  hr = video_device->CreateVideoProcessorOutputView(out_tex.Get(), enumerator.Get(),
                                                    &out_desc, &out_view);
  if (FAILED(hr) || !out_view) {
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }

  RECT src_rect{offset_x, offset_y, offset_x + crop_width, offset_y + crop_height};
  RECT dst_rect{0, 0, scaled_width, scaled_height};

  video_ctx->VideoProcessorSetStreamFrameFormat(processor.Get(), 0,
                                                D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_ctx->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src_rect);
  video_ctx->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dst_rect);
  video_ctx->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &dst_rect);

  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = in_view.Get();

  hr = video_ctx->VideoProcessorBlt(processor.Get(), out_view.Get(), 0, 1, &stream);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "D3D11TextureBuffer: VideoProcessorBlt failed hr=0x"
                        << std::hex << hr;
    return webrtc::VideoFrameBuffer::CropAndScale(offset_x, offset_y, crop_width,
                                                  crop_height, scaled_width,
                                                  scaled_height);
  }

  // Wrap output texture as a new native buffer; mark it as poolable for reuse.
  auto device_ptr = device_;
  auto result = rtc::make_ref_counted<D3D11TextureBuffer>(
      std::move(out_tex), std::move(device_ptr), scaled_width, scaled_height,
      format_);
  result->poolable_ = true;
  result->pool_key_ = PoolKey{device_.Get(), static_cast<UINT>(scaled_width),
                              static_cast<UINT>(scaled_height), format_};
  return result;
}

D3D11TextureInfo D3D11TextureBuffer::GetInfo() const {
  return D3D11TextureInfo{
      .width = static_cast<uint32_t>(width_),
      .height = static_cast<uint32_t>(height_),
      .format = static_cast<uint32_t>(format_),
      .is_shared = false,  // TODO: detect shared handle
  };
}

bool D3D11TextureBuffer::CreateStagingTexture() {
  if (staging_texture_) {
    return true;  // Already created
  }

  D3D11_TEXTURE2D_DESC staging_desc;
  texture_->GetDesc(&staging_desc);
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;

  HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr, &staging_texture_);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to create staging texture: 0x" << std::hex << hr;
    return false;
  }

  return true;
}

rtc::scoped_refptr<webrtc::I420BufferInterface> D3D11TextureBuffer::ToI420() {
  // Check cache first
  {
    std::lock_guard<std::mutex> lock(i420_cache_mutex_);
    if (cached_i420_) {
      return cached_i420_;
    }
  }

  // Perform readback
  auto result = ReadbackToI420();
  
  // Cache the result
  if (result) {
    std::lock_guard<std::mutex> lock(i420_cache_mutex_);
    cached_i420_ = result;
  }
  
  return result;
}

rtc::scoped_refptr<webrtc::I420BufferInterface> D3D11TextureBuffer::ReadbackToI420() {
  if (!CreateStagingTexture()) {
    return nullptr;
  }

  // Get device context
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  device_->GetImmediateContext(&context);

  // Copy GPU texture to staging texture
  context->CopyResource(staging_texture_.Get(), texture_.Get());

  // Map the staging texture
  D3D11_MAPPED_SUBRESOURCE mapped;
  HRESULT hr = context->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to map staging texture: 0x" << std::hex << hr;
    return nullptr;
  }

  // Create I420 buffer
  rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer =
      webrtc::I420Buffer::Create(width_, height_);

  // Convert based on source format
  int result = -1;
  switch (format_) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      result = libyuv::ARGBToI420(
          static_cast<const uint8_t*>(mapped.pData),
          mapped.RowPitch,
          i420_buffer->MutableDataY(),
          i420_buffer->StrideY(),
          i420_buffer->MutableDataU(),
          i420_buffer->StrideU(),
          i420_buffer->MutableDataV(),
          i420_buffer->StrideV(),
          width_,
          height_);
      break;

    case DXGI_FORMAT_R8G8B8A8_UNORM:
      // RGBA → I420 (swap R and B)
      result = libyuv::ABGRToI420(
          static_cast<const uint8_t*>(mapped.pData),
          mapped.RowPitch,
          i420_buffer->MutableDataY(),
          i420_buffer->StrideY(),
          i420_buffer->MutableDataU(),
          i420_buffer->StrideU(),
          i420_buffer->MutableDataV(),
          i420_buffer->StrideV(),
          width_,
          height_);
      break;

    case DXGI_FORMAT_NV12:
      // NV12 → I420
      result = libyuv::NV12ToI420(
          static_cast<const uint8_t*>(mapped.pData),
          mapped.RowPitch,
          static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * height_,
          mapped.RowPitch,
          i420_buffer->MutableDataY(),
          i420_buffer->StrideY(),
          i420_buffer->MutableDataU(),
          i420_buffer->StrideU(),
          i420_buffer->MutableDataV(),
          i420_buffer->StrideV(),
          width_,
          height_);
      break;

    default:
      RTC_LOG(LS_ERROR) << "Unsupported format for I420 conversion: " 
                        << static_cast<int>(format_);
      break;
  }

  context->Unmap(staging_texture_.Get(), 0);

  if (result != 0) {
    RTC_LOG(LS_ERROR) << "libyuv conversion failed: " << result;
    return nullptr;
  }

  return i420_buffer;
}

// ============================================================================
// C Functions for CXX Bridge
// ============================================================================

uint64_t create_d3d11_frame_buffer_from_handles(
    uint64_t texture_handle,
    uint64_t device_handle,
    uint32_t width,
    uint32_t height,
    uint32_t format) {
  
  if (texture_handle == 0 || device_handle == 0) {
    return 0;
  }

  auto* texture = reinterpret_cast<ID3D11Texture2D*>(texture_handle);
  auto* device = reinterpret_cast<ID3D11Device*>(device_handle);

  auto buffer = D3D11TextureBuffer::Create(texture, device);
  if (!buffer) {
    return 0;
  }

  // Transfer ownership to caller via raw pointer
  // Caller must call release_d3d11_frame_buffer when done
  buffer->AddRef();
  return reinterpret_cast<uint64_t>(buffer.get());
}

uint64_t create_d3d11_frame_buffer_from_handles_pooled_copy(
    uint64_t source_texture_handle,
    uint64_t device_handle,
    uint64_t context_handle,
    uint32_t width,
    uint32_t height,
    uint32_t format) {
  if (source_texture_handle == 0 || device_handle == 0 || context_handle == 0) {
    return 0;
  }

  auto* source_texture = reinterpret_cast<ID3D11Texture2D*>(source_texture_handle);
  auto* device = reinterpret_cast<ID3D11Device*>(device_handle);
  auto* context = reinterpret_cast<ID3D11DeviceContext*>(context_handle);

  auto buffer = D3D11TextureBuffer::CreateWithPooledCopy(
      source_texture,
      device,
      context,
      static_cast<UINT>(width),
      static_cast<UINT>(height),
      static_cast<DXGI_FORMAT>(format));
  if (!buffer) {
    return 0;
  }

  // Transfer ownership to caller via raw pointer.
  buffer->AddRef();
  return reinterpret_cast<uint64_t>(buffer.get());
}

uint64_t create_d3d11_frame_buffer_from_handles_pooled_vp_scale(
    uint64_t source_texture_handle,
    uint64_t device_handle,
    uint64_t context_handle,
    uint32_t src_width,
    uint32_t src_height,
    uint32_t dst_width,
    uint32_t dst_height,
    uint32_t format) {
  if (source_texture_handle == 0 || device_handle == 0 || context_handle == 0) {
    return 0;
  }

  auto* source_texture = reinterpret_cast<ID3D11Texture2D*>(source_texture_handle);
  auto* device = reinterpret_cast<ID3D11Device*>(device_handle);
  auto* context = reinterpret_cast<ID3D11DeviceContext*>(context_handle);

  auto buffer = D3D11TextureBuffer::CreateWithPooledVideoProcessorScale(
      source_texture,
      device,
      context,
      static_cast<UINT>(src_width),
      static_cast<UINT>(src_height),
      static_cast<UINT>(dst_width),
      static_cast<UINT>(dst_height),
      static_cast<DXGI_FORMAT>(format));
  if (!buffer) {
    return 0;
  }

  buffer->AddRef();
  return reinterpret_cast<uint64_t>(buffer.get());
}

void release_d3d11_frame_buffer(uint64_t handle) {
  if (handle == 0) {
    return;
  }
  
  auto* buffer = reinterpret_cast<D3D11TextureBuffer*>(handle);
  buffer->Release();
}

D3D11TextureInfo get_d3d11_frame_buffer_info(uint64_t handle) {
  if (handle == 0) {
    return D3D11TextureInfo{0, 0, 0, false};
  }
  
  auto* buffer = reinterpret_cast<D3D11TextureBuffer*>(handle);
  return buffer->GetInfo();
}

bool is_d3d11_frame_buffer_supported() {
  // Check if D3D11 is available by attempting to get a device
  // This is a lightweight check
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  
  D3D_FEATURE_LEVEL feature_levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  D3D_FEATURE_LEVEL actual_level;
  
  HRESULT hr = D3D11CreateDevice(
      nullptr,  // Default adapter
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,  // No software rasterizer
      0,        // Flags
      feature_levels,
      ARRAYSIZE(feature_levels),
      D3D11_SDK_VERSION,
      &device,
      &actual_level,
      &context);
  
  return SUCCEEDED(hr);
}

}  // namespace livekit_ffi

#endif  // _WIN32
