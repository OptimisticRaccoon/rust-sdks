/*
 * Copyright 2017-2024 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

#ifdef _WIN32

// Must include winsock2.h before windows.h to avoid conflicts
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "NvEncoderD3D11.h"

NvEncoderD3D11::NvEncoderD3D11(ID3D11Device* pD3D11Device,
                               uint32_t nWidth,
                               uint32_t nHeight,
                               NV_ENC_BUFFER_FORMAT eBufferFormat,
                               uint32_t nExtraOutputDelay)
    : NvEncoder(NV_ENC_DEVICE_TYPE_DIRECTX,
                pD3D11Device,
                nWidth,
                nHeight,
                eBufferFormat,
                nExtraOutputDelay,
                false,   // bMotionEstimationOnly
                false,   // bOutputInVideoMemory
                false,   // bDX12Encode
                false) {  // bUseIVFContainer (WebRTC expects raw elementary stream, not IVF)
  
  if (!pD3D11Device) {
    NVENC_THROW_ERROR("D3D11 device is null", NV_ENC_ERR_INVALID_PTR);
  }

  m_pD3D11Device = pD3D11Device;
  m_pD3D11Device->GetImmediateContext(&m_pD3D11DeviceContext);

  if (!m_pD3D11DeviceContext) {
    NVENC_THROW_ERROR("Failed to get D3D11 device context", 
                      NV_ENC_ERR_INVALID_DEVICE);
  }
}

NvEncoderD3D11::~NvEncoderD3D11() {
  ReleaseD3D11Resources();
}

void NvEncoderD3D11::ReleaseD3D11Resources() {
  // Unregister resources before releasing textures
  if (!m_hEncoder) {
    return;
  }
  
  UnregisterInputResources();
  
  // Release textures
  m_vInputTextures.clear();
}

void NvEncoderD3D11::ReleaseInputBuffers() {
  ReleaseD3D11Resources();
}

void NvEncoderD3D11::AllocateInputBuffers(int32_t numInputBuffers) {
  if (!IsHWEncoderInitialized()) {
    NVENC_THROW_ERROR("Encoder initialization failed",
                      NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
  }

  // Get the DXGI format from the buffer format
  DXGI_FORMAT dxgiFormat = GetD3D11Format(GetPixelFormat());
  if (dxgiFormat == DXGI_FORMAT_UNKNOWN) {
    NVENC_THROW_ERROR("Unsupported buffer format for D3D11",
                      NV_ENC_ERR_UNSUPPORTED_PARAM);
  }

  // Create input textures
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = GetMaxEncodeWidth();
  desc.Height = GetMaxEncodeHeight();
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = dxgiFormat;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  desc.CPUAccessFlags = 0;

  m_vInputTextures.resize(numInputBuffers);
  std::vector<void*> inputFrames;

  for (int i = 0; i < numInputBuffers; i++) {
    HRESULT hr = m_pD3D11Device->CreateTexture2D(&desc, nullptr, 
                                                  &m_vInputTextures[i]);
    if (FAILED(hr)) {
      NVENC_THROW_ERROR("Failed to create D3D11 input texture",
                        NV_ENC_ERR_OUT_OF_MEMORY);
    }
    inputFrames.push_back(m_vInputTextures[i].Get());
  }

  // Register all input textures with the encoder using base class method
  RegisterInputResources(inputFrames,
                         NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX,
                         GetMaxEncodeWidth(),
                         GetMaxEncodeHeight(),
                         0,  // pitch (0 for D3D11 textures)
                         GetPixelFormat(),
                         false);  // bReferenceFrame
}

void NvEncoderD3D11::CopyToInputTexture(ID3D11Texture2D* pSrcTexture,
                                        uint32_t nSrcSubresource) {
  // Get the current input frame index
  int bufferIndex = m_iToSend % m_nEncoderBuffer;
  
  if (bufferIndex >= static_cast<int>(m_vInputTextures.size())) {
    NVENC_THROW_ERROR("Invalid buffer index", NV_ENC_ERR_INVALID_CALL);
  }

  // Get our destination texture
  ID3D11Texture2D* pDstTexture = m_vInputTextures[bufferIndex].Get();

  // Copy the source texture to our registered input texture
  m_pD3D11DeviceContext->CopySubresourceRegion(
      pDstTexture,      // Destination
      0,                // DstSubresource
      0, 0, 0,          // DstX, DstY, DstZ
      pSrcTexture,      // Source
      nSrcSubresource,  // SrcSubresource
      nullptr);         // pSrcBox (null = entire resource)
}

DXGI_FORMAT NvEncoderD3D11::GetD3D11Format(NV_ENC_BUFFER_FORMAT eBufferFormat) {
  switch (eBufferFormat) {
    case NV_ENC_BUFFER_FORMAT_NV12:
      return DXGI_FORMAT_NV12;
    case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
      return DXGI_FORMAT_P010;
    case NV_ENC_BUFFER_FORMAT_ARGB:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case NV_ENC_BUFFER_FORMAT_ABGR:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case NV_ENC_BUFFER_FORMAT_ARGB10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case NV_ENC_BUFFER_FORMAT_ABGR10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

#endif  // _WIN32
