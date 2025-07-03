// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#define OVR_D3D_VERSION 11 // This might be outdated or unnecessary

#include <windows.h>
#include "VideoCommon/VR.h"
#include "VideoCommon/VideoConfig.h" // For g_ActiveConfig
// #include "VideoCommon/VR920.h" // Assuming VR920 support is not a priority for now

#include <d3d11.h>

// Forward declare Gfx from DX11 namespace
namespace DX11 {
class Gfx;
class DXTexture; // For m_frontBuffer
}

// Oculus SDK Headers - these will need to be managed by the build system
// And ensure they are compatible with the OpenVR version being used if both are enabled.
// For now, assume OpenVR is the primary target as per the initial problem description for VR-Reloaded-5
#ifdef HAVE_OPENVR_SDK // This preprocessor guard might be different in VR-Reloaded-5
#include <openvr.h>
#else
// Fallback or error if OpenVR SDK is not found by the build system
// Or, if Oculus SDK is also intended to be supported, include its D3D headers here.
// #include "OVR_CAPI_D3D.h"
// #include "OculusSystemLibraryHeaderD3D11.h" // Hydra's way of handling Oculus SDK parts
#endif


namespace DX11
{
// VRD3D should ideally not directly depend on global g_renderer or g_ActiveConfig if possible.
// Pass necessary config/state via parameters.

// Called from D3DGfx
void VR_D3D_ConfigureHMD(); // Renamed to avoid conflict with VideoCommon/VR.h
void VR_D3D_StartFramebuffer(DX11::Gfx* gfx_context);
void VR_D3D_StopFramebuffer(DX11::Gfx* gfx_context);

// Called from D3DGfx during SwapImpl/PresentBackbuffer
void VR_D3D_RenderToEyeBuffer(DX11::Gfx* gfx_context, int eye); // Simplified hmd_number for now
void VR_D3D_SubmitFrameToHMD(); // Combines PresentHMDFrame logic

// Potentially keep Timewarp separate if its logic is substantial
void VR_D3D_DrawTimewarpFrame(DX11::Gfx* gfx_context);


// Internal helper functions or variables if needed for D3D specific VR management
// For example, storing OpenVR texture handles or Oculus swap chains.
// These would be private to VRD3D.cpp.

} // namespace DX11
