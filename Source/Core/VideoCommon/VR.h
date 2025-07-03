// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

// Distances are in metres, angles are in degrees.
// These constants are now primarily in VideoConfig.h, but keeping a few key ones here for reference
// if VR.cpp uses them before VideoConfig is fully processed, or for defaults if config load fails.
const float DEFAULT_VR_AIM_DISTANCE = 7.0f;

#define VR_PLAYER1 0
#define VR_PLAYER2 1
#define VR_PLAYER3 2
#define VR_PLAYER4 3
#define VR_PLAYER_NONE 4
#define VR_PLAYER_DEFAULT 5
#define VR_PLAYER_OTHERS 6
#define VR_PLAYER_ALL 7
#define VR_MIRROR_LEFT 0
#define VR_MIRROR_RIGHT 1
#define VR_MIRROR_DISABLED 2
#define VR_MIRROR_WARPED 3
#define VR_MIRROR_BOTH 4

// Button definitions might be better placed in an input-related VR header if used extensively by input system.
// For now, keeping them as they were in Hydra for VideoCommon direct use.
#define OCULUS_BUTTON_A 1
// ... (other Oculus and Vive button defines from Hydra's VR.h can be here if needed by VR.cpp logic)
// For brevity, I'll omit the full list here, assuming they are not directly used by the functions
// I'm focusing on adapting in VR.cpp for the rendering pipeline. If VR.cpp gesture/input logic
// is ported, these will be necessary.

#define VIVE_SPECIAL_DPAD_UP 0x1
// ... (Vive special button defines)


extern const char* scm_vr_sdk_str; // Likely related to build versioning

#include <atomic>
#include <mutex>
#include <string>
#include <vector> // For TimewarpLogEntry

#include "Common/MathUtil.h"   // For Matrix44, Matrix33
#include "VideoCommon/DataReader.h" // For TimewarpLogEntry's DataReader

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define RADIANS_TO_DEGREES(rad) ((float)rad * (float)(180.0 / M_PI))
#define DEGREES_TO_RADIANS(deg) ((float)deg * (float)(M_PI / 180.0))

// Forward declarations if needed from SDKs, or include minimal SDK headers
// This depends on how much SDK interaction is in VR.h vs VR.cpp
// For OpenVR
#ifdef HAVE_OPENVR_SDK
namespace vr { class IVRSystem; struct TrackedDevicePose_t; }
#endif
// For Oculus (if specific types are exposed in VR.h, which Hydra did)
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
#include "OVR_CAPI.h" // Minimal include, or forward declare ovrPosef, ovrFovPort etc.
#endif


// Main emulator interface (generic VR SDK interactions)
void VR_Init();
void VR_Shutdown();
void VR_StopRendering(); // Generic rendering stop, if any, beyond backend resource release
void VR_RecenterHMD();
void VR_NewVRFrame(); // Called each emulated frame
void VR_SetGame(bool is_wii, bool is_nand, std::string id); // Game identification for default settings
void VR_CheckStatus(bool* ShouldRecenter, bool* ShouldQuit); // For HMD status like recenter requests
bool VR_GetShouldQuit(); // Added for timewarp loop

// VR rendering lifecycle (called by the graphics backend e.g. D3DGfx)
void VR_BeginFrame();    // Begin VR frame (SDK specific)
void VR_GetEyePoses();   // Get latest eye poses from SDK
void VR_UpdateHeadTrackingIfNeeded(); // Updates g_head_tracking_matrix etc.

// Projection and view information (used by VideoCommon::VertexShaderManager or backend)
void VR_GetProjectionHalfTan(float& hmd_halftan); // For FOV calculations
void VR_GetProjectionMatrices(Matrix44& left_eye, Matrix44& right_eye, float znear, float zfar);
void VR_GetEyeOffsets(float posLeft[3], float posRight[3]); // Renamed from GetEyePos for clarity
void VR_GetFovTextureSize(int* width, int* height); // Recommended texture size per eye

// Audio (if VR SDK provides preferred audio device)
std::wstring VR_GetAudioDeviceId();

// Input (more complex input should be in InputCommon)
// Basic HMD gesture/button if simple enough for VideoCommon use.
bool VR_GetHMDGestures(u32* gestures);
// Add other simple VR input getters if necessary, e.g., for basic remote.

// HMD description and capabilities (global state, initialized by VR_Init)
extern bool g_force_vr, g_prefer_openvr, g_one_hmd;
extern bool g_has_hmd, g_has_two_hmds, g_has_rift, g_has_vr920, g_has_openvr, g_openvr_is_vive, g_openvr_is_rift;
extern bool g_is_direct_mode;
// extern bool g_vr_needs_endframe; // This logic is now more tied to backend Present/SwapImpl
extern Matrix44 g_head_tracking_matrix;
extern float g_head_tracking_position[3];
// extern float g_left_hand_tracking_position[3], g_right_hand_tracking_position[3]; // Hand tracking more for InputCommon
extern int g_hmd_refresh_rate;
// extern const char* g_hmd_device_name; // Backend (D3DBase) might handle this now
extern bool g_fov_changed, g_vr_black_screen; // For motion sickness FOV reduction

// extern float g_current_fps, g_current_speed; // These are likely updated by Core or Main

// Opcode Replay Buffer (If this feature is kept)
struct TimewarpLogEntry
{
  DataReader timewarp_log;
  bool is_preprocess_log;
};
extern std::vector<TimewarpLogEntry> timewarp_logentries;
extern bool g_opcode_replay_enabled;
// extern bool g_new_frame_just_rendered; // Likely managed by graphics backend
extern bool g_first_pass_vs_constants; // Used by shader cache in Hydra
extern bool g_opcode_replay_frame;
extern bool g_opcode_replay_log_frame;
// extern int skipped_opcode_replay_count; // Internal to VR.cpp's OpcodeReplayBuffer

extern std::mutex g_vr_lock; // If needed for thread safety with VR SDK calls

namespace Core
{
extern std::atomic<u32> g_drawn_vr; // Counter for VR frames drawn
}

// Oculus specific types if they are passed around (try to keep VR.h generic)
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
extern ovrPosef g_eye_poses[2]; // Populated by VR_GetEyePoses
extern ovrFovPort g_eye_fov[2]; // Current FOV, potentially modified for motion sickness
#endif

// OpenVR specific types if passed around
#ifdef HAVE_OPENVR_SDK
// extern vr::TrackedDevicePose_t m_rTrackedDevicePose[vr::k_unMaxTrackedDeviceCount]; // Managed in VR.cpp
#endif

// Helper to get the OpenVR system pointer if needed by other modules (e.g. InputCommon)
// Returns nullptr if OpenVR is not active or not compiled.
#ifdef HAVE_OPENVR_SDK
vr::IVRSystem* VR_GetOpenVRSystem();
#endif

#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
ovrHmd VR_GetOculusHMD();
#endif

// Global pointer to the D3DGfx instance, needed by some VRD3D functions called from VR.cpp
// This is a temporary solution to bridge the gap. Ideally, context is passed.
namespace DX11 { class Gfx; }
extern DX11::Gfx* g_d3d_gfx_vr_context;


// Debugging
extern bool debug_nextScene; // From Hydra

// Frame index for Oculus SDK
extern int g_ovr_frameindex; // Used by older Oculus SDKs, may not be needed for OpenVR focus.
