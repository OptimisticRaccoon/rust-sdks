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

#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>
#include <stdint.h>
#include <vector>

#include "NvEncoder.h"

using Microsoft::WRL::ComPtr;

/**
 *  @brief Encoder for D3D11 device memory.
 *  
 *  This encoder can directly accept D3D11 textures as input, avoiding
 *  any CPU-GPU copies. NVENC handles format conversion internally.
 */
class NvEncoderD3D11 : public NvEncoder {
 public:
  /**
   *  @brief Constructor for NvEncoderD3D11
   *  @param pD3D11Device - D3D11 device to use for encoding
   *  @param nWidth - Width of the input frames
   *  @param nHeight - Height of the input frames
   *  @param eBufferFormat - Buffer format (e.g., NV_ENC_BUFFER_FORMAT_ARGB)
   *  @param nExtraOutputDelay - Extra output delay for async encoding
   */
  NvEncoderD3D11(ID3D11Device* pD3D11Device,
                 uint32_t nWidth,
                 uint32_t nHeight,
                 NV_ENC_BUFFER_FORMAT eBufferFormat,
                 uint32_t nExtraOutputDelay = 3);
  
  virtual ~NvEncoderD3D11();

  /**
   *  @brief Copy data from a source texture to the encoder's input buffer
   *  @param pSrcTexture - Source D3D11 texture
   *  @param nSrcSubresource - Subresource index (usually 0)
   */
  void CopyToInputTexture(ID3D11Texture2D* pSrcTexture, 
                          uint32_t nSrcSubresource = 0);

  /**
   *  @brief Get the D3D11 device context
   */
  ID3D11DeviceContext* GetD3D11DeviceContext() { return m_pD3D11DeviceContext.Get(); }

  /**
   *  @brief Get the D3D11 device
   */
  ID3D11Device* GetD3D11Device() { return m_pD3D11Device.Get(); }

 protected:
  /**
   *  @brief Release input buffers
   */
  virtual void ReleaseInputBuffers() override;

 private:
  /**
   *  @brief Allocate input buffers as D3D11 textures
   */
  virtual void AllocateInputBuffers(int32_t numInputBuffers) override;

  /**
   *  @brief Release D3D11 resources
   */
  void ReleaseD3D11Resources();

  /**
   *  @brief Convert NV_ENC_BUFFER_FORMAT to DXGI_FORMAT
   */
  DXGI_FORMAT GetD3D11Format(NV_ENC_BUFFER_FORMAT eBufferFormat);

 private:
  ComPtr<ID3D11Device> m_pD3D11Device;
  ComPtr<ID3D11DeviceContext> m_pD3D11DeviceContext;
  std::vector<ComPtr<ID3D11Texture2D>> m_vInputTextures;
};

#endif  // _WIN32
