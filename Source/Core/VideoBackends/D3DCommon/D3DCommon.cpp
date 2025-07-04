// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/DXShader.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Common/Assert.h"
#include "Common/DynamicLibrary.h"
#include "Common/HRWrap.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/AbstractShader.h"

// Simple pass-through vertex shader (HLSL)
static const char* s_simple_vs_code = R"(
struct VS_INPUT
{
    float4 Pos : POSITION;
    float2 Tex : TEXCOORD0;
};

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
};

PS_INPUT Main(VS_INPUT input)
{
    PS_INPUT output;
    output.Pos = input.Pos;
    output.Tex = input.Tex;
    return output;
}
)";

// Simple texture sampling pixel shader (HLSL)
static const char* s_simple_ps_code = R"(
Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
};

float4 Main(PS_INPUT input) : SV_Target
{
    return g_texture.Sample(g_sampler, input.Tex);
}
)";

namespace D3DCommon
{
pD3DCompile d3d_compile;

static Common::DynamicLibrary s_dxgi_library;
static Common::DynamicLibrary s_d3dcompiler_library;
static bool s_libraries_loaded = false;

static std::unique_ptr<AbstractShader> s_simple_vs_abs_shader;
static std::unique_ptr<AbstractShader> s_simple_ps_abs_shader;
static ID3D11VertexShader* s_simple_d3d_vs = nullptr;
static ID3D11PixelShader* s_simple_d3d_ps = nullptr;
static ID3D11InputLayout* s_simple_input_layout = nullptr;
static ID3D11SamplerState* s_linear_sampler = nullptr;

static HRESULT (*create_dxgi_factory)(REFIID riid, _COM_Outptr_ void** ppFactory);
static HRESULT (*create_dxgi_factory2)(UINT Flags, REFIID riid, void** ppFactory);

bool LoadLibraries()
{
  if (s_libraries_loaded)
    return true;

  if (!s_dxgi_library.Open("dxgi.dll"))
  {
    PanicAlertFmtT("Failed to load dxgi.dll");
    return false;
  }

  if (!s_d3dcompiler_library.Open(D3DCOMPILER_DLL_A))
  {
    PanicAlertFmtT("Failed to load {0}. If you are using Windows 7, try installing the "
                   "KB4019990 update package.",
                   D3DCOMPILER_DLL_A);
    s_dxgi_library.Close();
    return false;
  }

  // Required symbols.
  if (!s_d3dcompiler_library.GetSymbol("D3DCompile", &d3d_compile) ||
      !s_dxgi_library.GetSymbol("CreateDXGIFactory", &create_dxgi_factory))
  {
    PanicAlertFmtT("Failed to find one or more D3D symbols");
    s_d3dcompiler_library.Close();
    s_dxgi_library.Close();
    return false;
  }

  // Optional symbols.
  s_dxgi_library.GetSymbol("CreateDXGIFactory2", &create_dxgi_factory2);
  s_libraries_loaded = true;
  return true;
}

void UnloadLibraries()
{
  create_dxgi_factory = nullptr;
  create_dxgi_factory2 = nullptr;
  d3d_compile = nullptr;
  s_d3dcompiler_library.Close();
  s_dxgi_library.Close();
  s_libraries_loaded = false;
}

Microsoft::WRL::ComPtr<IDXGIFactory> CreateDXGIFactory(bool debug_device)
{
  Microsoft::WRL::ComPtr<IDXGIFactory> factory;

  // Use Win8.1 version if available.
  if (create_dxgi_factory2 &&
      SUCCEEDED(create_dxgi_factory2(debug_device ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                     IID_PPV_ARGS(factory.GetAddressOf()))))
  {
    return factory;
  }

  // Fallback to original version, without debug support.
  HRESULT hr = create_dxgi_factory(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    PanicAlertFmt("CreateDXGIFactory() failed: {}", Common::HRWrap(hr));
    return nullptr;
  }

  return factory;
}

std::vector<std::string> GetAdapterNames()
{
  Microsoft::WRL::ComPtr<IDXGIFactory> factory;
  HRESULT hr = create_dxgi_factory(IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr))
    return {};

  std::vector<std::string> adapters;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  while (factory->EnumAdapters(static_cast<UINT>(adapters.size()),
                               adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND)
  {
    std::string name;
    DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(adapter->GetDesc(&desc)))
      name = WStringToUTF8(desc.Description);

    adapters.push_back(std::move(name));
  }

  return adapters;
}

DXGI_FORMAT GetDXGIFormatForAbstractFormat(AbstractTextureFormat format, bool typeless)
{
  switch (format)
  {
  case AbstractTextureFormat::DXT1:
    return DXGI_FORMAT_BC1_UNORM;
  case AbstractTextureFormat::DXT3:
    return DXGI_FORMAT_BC2_UNORM;
  case AbstractTextureFormat::DXT5:
    return DXGI_FORMAT_BC3_UNORM;
  case AbstractTextureFormat::BPTC:
    return DXGI_FORMAT_BC7_UNORM;
  case AbstractTextureFormat::RGBA8:
    return typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
  case AbstractTextureFormat::BGRA8:
    return typeless ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
  case AbstractTextureFormat::RGB10_A2:
    return typeless ? DXGI_FORMAT_R10G10B10A2_TYPELESS : DXGI_FORMAT_R10G10B10A2_UNORM;
  case AbstractTextureFormat::RGBA16F:
    return typeless ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
  case AbstractTextureFormat::R16:
    return typeless ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R16_UNORM;
  case AbstractTextureFormat::R32F:
    return typeless ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT;
  case AbstractTextureFormat::D16:
    return DXGI_FORMAT_R16_TYPELESS;
  case AbstractTextureFormat::D24_S8:
    return DXGI_FORMAT_R24G8_TYPELESS;
  case AbstractTextureFormat::D32F:
    return DXGI_FORMAT_R32_TYPELESS;
  case AbstractTextureFormat::D32F_S8:
    return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  default:
    PanicAlertFmt("Unhandled texture format.");
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}
DXGI_FORMAT GetSRVFormatForAbstractFormat(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::DXT1:
    return DXGI_FORMAT_BC1_UNORM;
  case AbstractTextureFormat::DXT3:
    return DXGI_FORMAT_BC2_UNORM;
  case AbstractTextureFormat::DXT5:
    return DXGI_FORMAT_BC3_UNORM;
  case AbstractTextureFormat::BPTC:
    return DXGI_FORMAT_BC7_UNORM;
  case AbstractTextureFormat::RGBA8:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case AbstractTextureFormat::BGRA8:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case AbstractTextureFormat::RGB10_A2:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  case AbstractTextureFormat::RGBA16F:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case AbstractTextureFormat::R16:
    return DXGI_FORMAT_R16_UNORM;
  case AbstractTextureFormat::R32F:
    return DXGI_FORMAT_R32_FLOAT;
  case AbstractTextureFormat::D16:
    return DXGI_FORMAT_R16_UNORM;
  case AbstractTextureFormat::D24_S8:
    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  case AbstractTextureFormat::D32F:
    return DXGI_FORMAT_R32_FLOAT;
  case AbstractTextureFormat::D32F_S8:
    return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  default:
    PanicAlertFmt("Unhandled SRV format");
    return DXGI_FORMAT_UNKNOWN;
  }
}

DXGI_FORMAT GetRTVFormatForAbstractFormat(AbstractTextureFormat format, bool integer)
{
  switch (format)
  {
  case AbstractTextureFormat::RGBA8:
    return integer ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
  case AbstractTextureFormat::BGRA8:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case AbstractTextureFormat::RGB10_A2:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  case AbstractTextureFormat::RGBA16F:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case AbstractTextureFormat::R16:
    return integer ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R16_UNORM;
  case AbstractTextureFormat::R32F:
    return DXGI_FORMAT_R32_FLOAT;
  default:
    PanicAlertFmt("Unhandled RTV format");
    return DXGI_FORMAT_UNKNOWN;
  }
}
DXGI_FORMAT GetDSVFormatForAbstractFormat(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::D16:
    return DXGI_FORMAT_D16_UNORM;
  case AbstractTextureFormat::D24_S8:
    return DXGI_FORMAT_D24_UNORM_S8_UINT;
  case AbstractTextureFormat::D32F:
    return DXGI_FORMAT_D32_FLOAT;
  case AbstractTextureFormat::D32F_S8:
    return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  default:
    PanicAlertFmt("Unhandled DSV format");
    return DXGI_FORMAT_UNKNOWN;
  }
}

AbstractTextureFormat GetAbstractFormatForDXGIFormat(DXGI_FORMAT format)
{
  switch (format)
  {
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    return AbstractTextureFormat::RGBA8;

  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    return AbstractTextureFormat::BGRA8;

  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    return AbstractTextureFormat::RGB10_A2;

  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    return AbstractTextureFormat::RGBA16F;

  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_TYPELESS:
    return AbstractTextureFormat::R16;

  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_TYPELESS:
    return AbstractTextureFormat::R32F;

  case DXGI_FORMAT_D16_UNORM:
    return AbstractTextureFormat::D16;

  case DXGI_FORMAT_D24_UNORM_S8_UINT:
    return AbstractTextureFormat::D24_S8;

  case DXGI_FORMAT_D32_FLOAT:
    return AbstractTextureFormat::D32F;

  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    return AbstractTextureFormat::D32F_S8;

  case DXGI_FORMAT_BC1_UNORM:
    return AbstractTextureFormat::DXT1;
  case DXGI_FORMAT_BC2_UNORM:
    return AbstractTextureFormat::DXT3;
  case DXGI_FORMAT_BC3_UNORM:
    return AbstractTextureFormat::DXT5;
  case DXGI_FORMAT_BC7_UNORM:
    return AbstractTextureFormat::BPTC;

  default:
    return AbstractTextureFormat::Undefined;
  }
}

void SetDebugObjectName(IUnknown* resource, std::string_view name)
{
  if (!g_ActiveConfig.bEnableValidationLayer)
    return;

  Microsoft::WRL::ComPtr<ID3D11DeviceChild> child11;
  Microsoft::WRL::ComPtr<ID3D12DeviceChild> child12;
  if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(child11.GetAddressOf()))))
  {
    child11->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.length()),
                            name.data());
  }
  else if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(child12.GetAddressOf()))))
  {
    child12->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.length()),
                            name.data());
  }
}

static void InitializeSimpleShaders()
{
    if (s_simple_d3d_vs && s_simple_d3d_ps && s_simple_input_layout && s_linear_sampler)
        return;

    // Compile Vertex Shader
    auto vs_bytecode_opt = DX11::DXShader::CompileShader(DX11::D3D::feature_level, ShaderStage::Vertex, s_simple_vs_code);
    if (vs_bytecode_opt)
    {
        s_simple_vs_abs_shader = DX11::DXShader::CreateFromBytecode(ShaderStage::Vertex, std::move(*vs_bytecode_opt), "SimpleVS_D3DCommon");
    }
    if (!s_simple_vs_abs_shader)
    {
        ERROR_LOG_FMT(VIDEO, "Failed to compile/create simple vertex shader for D3DCommon::DrawVideoQuad");
        return;
    }
    s_simple_d3d_vs = static_cast<DX11::DXShader*>(s_simple_vs_abs_shader.get())->GetD3DVertexShader();

    // Create Input Layout
    const D3D11_INPUT_ELEMENT_DESC input_elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    auto* dx_vs = static_cast<DX11::DXShader*>(s_simple_vs_abs_shader.get());
    HRESULT hr = DX11::D3D::device->CreateInputLayout(input_elements, ARRAYSIZE(input_elements),
                                dx_vs->GetByteCode().data(),
                                dx_vs->GetByteCode().size(),
                                &s_simple_input_layout);
    if (FAILED(hr))
    {
        ERROR_LOG_FMT(VIDEO, "Failed to create input layout for D3DCommon::DrawVideoQuad: %08x", hr);
        s_simple_vs_abs_shader.reset();
        s_simple_d3d_vs = nullptr;
        return;
    }
    
    // Compile Pixel Shader
    auto ps_bytecode_opt = DX11::DXShader::CompileShader(DX11::D3D::feature_level, ShaderStage::Pixel, s_simple_ps_code);
    if (ps_bytecode_opt)
    {
        s_simple_ps_abs_shader = DX11::DXShader::CreateFromBytecode(ShaderStage::Pixel, std::move(*ps_bytecode_opt), "SimplePS_D3DCommon");
    }
    if (!s_simple_ps_abs_shader)
    {
        ERROR_LOG_FMT(VIDEO, "Failed to compile/create simple pixel shader for D3DCommon::DrawVideoQuad");
        s_simple_vs_abs_shader.reset();
        s_simple_d3d_vs = nullptr;
        SAFE_RELEASE(s_simple_input_layout); // s_simple_input_layout is a COM object
        return;
    }
    s_simple_d3d_ps = static_cast<DX11::DXShader*>(s_simple_ps_abs_shader.get())->GetD3DPixelShader();

    // Create Sampler State
    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp_desc.MinLOD = 0;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = DX11::D3D::device->CreateSamplerState(&samp_desc, &s_linear_sampler);
    if (FAILED(hr))
    {
        ERROR_LOG_FMT(VIDEO, "Failed to create sampler state for D3DCommon::DrawVideoQuad: {}", hr);
        s_simple_vs_abs_shader.reset();
        s_simple_ps_abs_shader.reset();
        s_simple_d3d_vs = nullptr;
        s_simple_d3d_ps = nullptr;
        SAFE_RELEASE(s_simple_input_layout);
        // s_linear_sampler will be nullptr here, SAFE_RELEASE is fine
    }
}

static void ShutdownSimpleShaders()
{
    s_simple_vs_abs_shader.reset();
    s_simple_ps_abs_shader.reset();
    s_simple_d3d_vs = nullptr; 
    s_simple_d3d_ps = nullptr;
    SAFE_RELEASE(s_simple_input_layout);
    SAFE_RELEASE(s_linear_sampler);
}


void DrawVideoQuad(
    ID3D11ShaderResourceView* texture_srv,
    const D3D11_RECT& source_rect, // Source rectangle in texture pixels
    float dest_width,              // Destination width in screen pixels
    float dest_height,             // Destination height in screen pixels
    float sx_scale,                // Texture coordinate X scale (typically 1.0 or for sub-regions)
    float sy_scale,                // Texture coordinate Y scale (typically 1.0 or for sub-regions)
    float gamma,                   // Gamma correction value
    int eye,                       // Eye index (0 for left, 1 for right) or slice for texture arrays
    VideoQuadType type             // Shader type to use
)
{
    InitializeSimpleShaders();
    if (!s_simple_d3d_vs || !s_simple_d3d_ps || !s_simple_input_layout || !s_linear_sampler)
    {
        ERROR_LOG_FMT(VIDEO, "Cannot DrawVideoQuad, simple shaders not initialized properly.");
        return;
    }

    DX11::D3D::stateman->SetVertexShader(s_simple_d3d_vs);
    DX11::D3D::stateman->SetPixelShader(s_simple_d3d_ps);
    DX11::D3D::stateman->SetInputLayout(s_simple_input_layout);
    DX11::D3D::stateman->SetSampler(0, s_linear_sampler); // Assuming sampler at slot 0
    DX11::D3D::stateman->SetTexture(0, texture_srv);     // Assuming texture at slot 0

    struct Vertex
    {
        float x, y, z;
        float u, v;
    };

    // Simple screen quad, texcoords assume full texture [0,1] range.
    // source_rect, sx_scale, sy_scale would be used to adjust these UVs.
    Vertex vertices[] = {
        {-1.0f,  1.0f, 0.0f, 0.0f, 0.0f}, // Top-left
        { 1.0f,  1.0f, 0.0f, 1.0f, 0.0f}, // Top-right
        {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f}, // Bottom-left
        { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f}, // Bottom-right
    };

    u16 indices[] = {0, 1, 2, 2, 1, 3};

    ID3D11Buffer* vertex_buffer = nullptr;
    D3D11_SUBRESOURCE_DATA vb_data = {vertices, 0, 0};
    CD3D11_BUFFER_DESC vb_desc(sizeof(vertices), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE);
    HRESULT hr = DX11::D3D::device->CreateBuffer(&vb_desc, &vb_data, &vertex_buffer);
     if (FAILED(hr)) {
        ERROR_LOG_FMT(VIDEO, "Failed to create vertex buffer for DrawVideoQuad: {}", hr);
        DX11::D3D::stateman->SetPixelShader(nullptr);
        DX11::D3D::stateman->SetVertexShader(nullptr);
        DX11::D3D::stateman->SetInputLayout(nullptr);
        DX11::D3D::stateman->SetSampler(0, nullptr);
        return;
    }

    ID3D11Buffer* index_buffer = nullptr;
    D3D11_SUBRESOURCE_DATA ib_data = {indices, 0, 0};
    CD3D11_BUFFER_DESC ib_desc(sizeof(indices), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_IMMUTABLE);
    hr = DX11::D3D::device->CreateBuffer(&ib_desc, &ib_data, &index_buffer);
    if (FAILED(hr)) {
        ERROR_LOG_FMT(VIDEO, "Failed to create index buffer for DrawVideoQuad: {}", hr);
        SAFE_RELEASE(vertex_buffer);
        DX11::D3D::stateman->SetPixelShader(nullptr);
        DX11::D3D::stateman->SetVertexShader(nullptr);
        DX11::D3D::stateman->SetInputLayout(nullptr);
        DX11::D3D::stateman->SetSampler(0, nullptr);
        return;
    }
    
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    DX11::D3D::context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    DX11::D3D::context->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R16_UINT, 0);
    DX11::D3D::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    DX11::D3D::stateman->Apply();
    DX11::D3D::context->DrawIndexed(ARRAYSIZE(indices), 0, 0);

    SAFE_RELEASE(vertex_buffer);
    SAFE_RELEASE(index_buffer);

    DX11::D3D::stateman->SetPixelShader(nullptr);
    DX11::D3D::stateman->SetVertexShader(nullptr);
    DX11::D3D::stateman->SetInputLayout(nullptr);
    DX11::D3D::stateman->SetSampler(0, nullptr);
}

void ShutdownD3DCommonResources()
{
    ShutdownSimpleShaders();
}
}  // namespace D3DCommon
