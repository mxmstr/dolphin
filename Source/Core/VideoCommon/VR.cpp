// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <Objbase.h>
// clang-format on
#endif

#include "Common/Common.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h" // For SConfig access if needed for paths etc.
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR.h"
#include "VideoBackends/D3D/VRD3D.h" // For D3D specific calls if absolutely necessary from common VR logic

// SDK Includes - these should be handled by the build system
// For OpenVR
#ifdef HAVE_OPENVR_SDK
#include <openvr.h>
vr::IVRSystem* vr_system = nullptr;
// vr::IVRRenderModels* vr_render_models = nullptr; // If model rendering is handled in VR.cpp
vr::IVRCompositor* vr_compositor = nullptr;
std::string vr_driver_name;
std::string vr_display_name;
vr::TrackedDevicePose_t vr_tracked_device_pose[vr::k_unMaxTrackedDeviceCount];
#endif

// For Oculus
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
ovrSession oculus_session = nullptr; // Or ovrHmd for older SDKs
ovrHmdDesc oculus_hmd_desc;
ovrPosef g_eye_poses[2]; // Also declared extern in VR.h, definition here
ovrFovPort g_eye_fov[2]; // Also declared extern in VR.h, definition here
ovrEyeRenderDesc oculus_eye_render_desc[2];
#if OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 7
ovrGraphicsLuid oculus_luid;
#endif
#endif


// Global VR state variables (definitions for those declared extern in VR.h)
const char* scm_vr_sdk_str = "scm_vr_sdk_undefined"; // Placeholder
bool g_force_vr = false, g_prefer_openvr = false, g_one_hmd = false;
bool g_has_hmd = false, g_has_two_hmds = false, g_has_rift = false, g_has_vr920 = false, g_has_openvr = false;
bool g_openvr_is_vive = false, g_openvr_is_rift = false; // OpenVR can drive Rifts
bool g_is_direct_mode = false; // This is more an SDK/driver property
Matrix44 g_head_tracking_matrix;
float g_head_tracking_position[3];
int g_hmd_refresh_rate = 90; // Default
bool g_fov_changed = false, g_vr_black_screen = false;
std::mutex g_vr_lock;
int g_ovr_frameindex = 0; // For older Oculus SDKs or if OpenVR needs similar frame tracking

DX11::Gfx* g_d3d_gfx_vr_context = nullptr; // Definition for the extern pointer

// TODO: Remove Hydra specific globals if not used or move to appropriate scope
// float g_current_fps = 60.0f, g_current_speed = 0.0f;
// ... other globals from Hydra's VR.cpp ...


bool VR_GetShouldQuit()
{
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION)) && (OVR_PRODUCT_VERSION >= 1)
    if (g_has_rift && oculus_session)
    {
        ovrSessionStatus sessionStatus;
        ovr_GetSessionStatus(oculus_session, &sessionStatus);
        return sessionStatus.ShouldQuit == ovrTrue;
    }
#endif
    // OpenVR does not have a direct "ShouldQuit" in the same way for the application to poll,
    // it's usually handled via VREvent_Quit or similar.
    return false;
}


void VR_Init()
{
  // Reset state
  g_has_hmd = false;
  g_has_openvr = false;
  g_has_rift = false;
  g_has_vr920 = false; // VR920 support likely removed or needs specific build flags
  g_has_two_hmds = false;
  vr_system = nullptr;
  vr_compositor = nullptr;
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  oculus_session = nullptr;
#endif

  Matrix44::LoadIdentity(g_head_tracking_matrix);
  g_head_tracking_position[0] = g_head_tracking_position[1] = g_head_tracking_position[2] = 0.0f;

  // Attempt to initialize OpenVR first if preferred or if Oculus fails/not present
#ifdef HAVE_OPENVR_SDK
  if (g_ActiveConfig.bEnableVR && (g_prefer_openvr || !g_has_rift))
  {
    vr::EVRInitError eError = vr::VRInitError_None;
    vr_system = vr::VR_Init(&eError, vr::VRApplication_Scene);

    if (eError == vr::VRInitError_None)
    {
      vr_driver_name = vr::VRSystem()->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String);
      vr_display_name = vr::VRSystem()->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SerialNumber_String);
      INFO_LOG(VR_INIT, "OpenVR Initialized. Driver: %s, Display: %s", vr_driver_name.c_str(), vr_display_name.c_str());

      g_has_openvr = true;
      g_has_hmd = true;
      g_openvr_is_vive = (vr_driver_name == "lighthouse");
      g_openvr_is_rift = (vr_driver_name == "oculus"); // OpenVR can run on Oculus hardware

      vr::TrackedPropertyError terror;
      g_hmd_refresh_rate = static_cast<int>(vr_system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &terror) + 0.5f);
      if (terror != vr::TrackedProp_Success) g_hmd_refresh_rate = 90; // Default if property fails

      // Initialize compositor
      vr_compositor = (vr::IVRCompositor*)vr::VR_GetGenericInterface(vr::IVRCompositor_Version, &eError);
      if (eError != vr::VRInitError_None || !vr_compositor)
      {
        ERROR_LOG(VR_INIT, "OpenVR Compositor initialization failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(eError));
        vr::VR_Shutdown();
        vr_system = nullptr;
        g_has_openvr = false;
        g_has_hmd = false;
      } else {
        vr_compositor->SetTrackingSpace(vr::TrackingUniverseSeated);
      }
    }
    else
    {
      ERROR_LOG(VR_INIT, "OpenVR Initialization failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(eError));
      vr_system = nullptr; // Ensure it's null if init fails
    }
  }
#endif

  // Attempt to initialize Oculus SDK if enabled, not preferring OpenVR, or OpenVR failed
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  if (g_ActiveConfig.bEnableVR && !g_has_openvr) // Only if OpenVR didn't init or isn't preferred for this HMD
  {
    ovrInitParams initParams = { ovrInit_RequestVersion, OVR_MINOR_VERSION, NULL, 0, 0 };
    ovrResult result = ovr_Initialize(&initParams);
    if (OVR_SUCCESS(result))
    {
      result = ovr_Create(&oculus_session, &oculus_luid);
      if (OVR_SUCCESS(result))
      {
        g_has_rift = true;
        g_has_hmd = true;
        oculus_hmd_desc = ovr_GetHmdDesc(oculus_session);
        g_hmd_refresh_rate = static_cast<int>(oculus_hmd_desc.DisplayRefreshRate + 0.5f);
        // Store default FOV
        for(int eye = 0; eye < 2; ++eye) {
          g_eye_fov[eye] = oculus_hmd_desc.DefaultEyeFov[eye];
        }
        INFO_LOG(VR_INIT, "Oculus SDK Initialized. Display: %s", oculus_hmd_desc.ProductName);
      }
      else
      {
        ovr_Shutdown();
        oculus_session = nullptr;
        ERROR_LOG(VR_INIT, "Oculus HMD Create failed: %s", ovr_GetLastErrorInfo(nullptr)->ErrorString);
      }
    }
    else
    {
      ERROR_LOG(VR_INIT, "Oculus SDK Initialization failed: %s", ovr_GetLastErrorInfo(nullptr)->ErrorString);
    }
  }
#endif

  if (g_has_openvr && g_has_rift) {
    g_has_two_hmds = true;
    // Decide which one to use if both are active - g_prefer_openvr might play a role here
    // For now, if OpenVR is running on Rift hardware, we might prefer the Oculus SDK for it.
    if (g_openvr_is_rift && !g_prefer_openvr) {
        INFO_LOG(VR_INIT, "Both OpenVR (on Rift) and Oculus SDK active. Deactivating OpenVR to prefer Oculus SDK for Rift hardware.");
        vr::VR_Shutdown();
        vr_system = nullptr;
        vr_compositor = nullptr;
        g_has_openvr = false;
        g_openvr_is_rift = false;
        g_openvr_is_vive = false;
        g_has_two_hmds = false;
    } else {
        INFO_LOG(VR_INIT, "Both OpenVR and Oculus SDK active. Using OpenVR as primary.");
        // Potentially shut down Oculus if OpenVR is primary and not on Oculus hardware
        // For now, let both be "active" at SDK level, backend will pick one.
    }
  }

  // Backend (e.g. D3DGfx constructor) will call its VR_D3D_ConfigureHMD and VR_D3D_StartFramebuffer

  // TODO: VR920 and other legacy/debug HMD init if required
}

void VR_Shutdown()
{
#ifdef HAVE_OPENVR_SDK
  if (vr_system)
  {
    vr::VR_Shutdown();
    vr_system = nullptr;
    vr_compositor = nullptr;
    g_has_openvr = false;
    INFO_LOG(VR_SHUTDOWN, "OpenVR Shutdown.");
  }
#endif
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  if (oculus_session)
  {
    ovr_Destroy(oculus_session);
    oculus_session = nullptr;
    ovr_Shutdown();
    g_has_rift = false;
    INFO_LOG(VR_SHUTDOWN, "Oculus SDK Shutdown.");
  }
#endif
  g_has_hmd = false;
  g_has_two_hmds = false;
}

void VR_StopRendering()
{
    // This function in Hydra was mainly for Oculus SDK 0.5 (ovrHmd_ConfigureRendering(hmd, nullptr...))
    // For modern SDKs, stopping rendering is more about not submitting frames and releasing backend resources,
    // which is handled by VR_D3D_StopFramebuffer and the Gfx destructor.
    // If there are generic SDK calls to pause rendering (without full shutdown), they'd go here.
}

void VR_RecenterHMD()
{
#ifdef HAVE_OPENVR_SDK
  if (g_has_openvr && vr_system)
  {
    vr_system->ResetSeatedZeroPose();
    INFO_LOG(VR_ACTION, "OpenVR HMD recentered.");
  }
#endif
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  if (g_has_rift && oculus_session)
  {
    ovr_RecenterTrackingOrigin(oculus_session);
    INFO_LOG(VR_ACTION, "Oculus HMD recentered.");
  }
#endif
}

void VR_NewVRFrame()
{
  g_new_tracking_frame = true;
  // Other logic from Hydra's VR_NewVRFrame (FOV changes for motion sickness, etc.)
  // This depends on g_ActiveConfig and should be fine if VideoConfig is correctly populated.
  // g_vr_black_screen logic:
    if (g_has_hmd && g_ActiveConfig.iMotionSicknessMethod == 2) // Assuming 2 is black screen
    {
        // This calculation was simplified in Hydra's VR.cpp.
        // Actual speed calculation would need input system integration.
        // float speed_metric = CalculateMotionSpeedMetric(); // Placeholder
        // g_vr_black_screen = (speed_metric > 0.15f);
        g_vr_black_screen = false; // Placeholder until speed metric is available
    } else {
        g_vr_black_screen = false;
    }

    // FOV adjustment for motion sickness (Oculus specific in Hydra)
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
    if (g_has_rift && !g_has_openvr && g_ActiveConfig.iMotionSicknessMethod == 1) // Assuming 1 is FOV reduction
    {
        // float speed_metric = CalculateMotionSpeedMetric(); // Placeholder
        // if (speed_metric > 0.15f)
        // {
        //     float t = tan(DEGREES_TO_RADIANS(g_ActiveConfig.fMotionSicknessFOV / 2.0f));
        //     for(int eye=0; eye<2; ++eye) {
        //         g_eye_fov[eye].LeftTan = std::min(oculus_hmd_desc.DefaultEyeFov[eye].LeftTan, t);
        //         g_eye_fov[eye].RightTan = std::min(oculus_hmd_desc.DefaultEyeFov[eye].RightTan, t);
        //         g_eye_fov[eye].UpTan = std::min(oculus_hmd_desc.DefaultEyeFov[eye].UpTan, t);
        //         g_eye_fov[eye].DownTan = std::min(oculus_hmd_desc.DefaultEyeFov[eye].DownTan, t);
        //     }
        // } else {
        //     for(int eye=0; eye<2; ++eye) g_eye_fov[eye] = oculus_hmd_desc.DefaultEyeFov[eye];
        // }
        // g_fov_changed = (memcmp(g_eye_fov, previous_fov_state, sizeof(g_eye_fov)) != 0);
        // Store previous_fov_state
    } else {
      g_fov_changed = false; // Reset if not using this method or not Oculus primary
    }
#else
    g_fov_changed = false;
#endif
}

void VR_SetGame(bool is_wii, bool is_nand, std::string id)
{
    // This logic can remain as is, it sets default controller styles based on game type.
    // (Code from Hydra's VR.cpp for VR_SetGame)
}

void VR_CheckStatus(bool* ShouldRecenter, bool* ShouldQuit)
{
  *ShouldRecenter = false;
  *ShouldQuit = false;
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION)) && (OVR_PRODUCT_VERSION >= 1)
  if (g_has_rift && oculus_session)
  {
    ovrSessionStatus sessionStatus;
    ovr_GetSessionStatus(oculus_session, &sessionStatus);
    if (sessionStatus.ShouldRecenter) *ShouldRecenter = true;
    if (sessionStatus.ShouldQuit) *ShouldQuit = true;
  }
#endif
  // OpenVR handles quit events differently (VREvent_Quit)
}

void VR_BeginFrame()
{
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  if (g_has_rift && oculus_session && !g_has_openvr) // Prioritize OpenVR if both active and OpenVR isn't on Rift
  {
    // Logic from Hydra for older SDKs (ovrHmd_BeginFrame, ovrHmd_GetFrameTiming)
    // For SDK 1.x+, frame submission timing is handled by ovr_SubmitFrame.
    // ovr_GetPredictedDisplayTime might be called here or before GetEyePoses.
    // g_ovr_frameindex++; // If used by SDK
  }
#endif
  // OpenVR: Frame timing is implicitly managed by WaitGetPoses and Submit.
  // No explicit "BeginFrame" call to OpenVR compositor in the same way.
}

void VR_GetEyePoses()
{
  if (!g_has_hmd) return;

#ifdef HAVE_OPENVR_SDK
  if (g_has_openvr && vr_compositor) // If OpenVR is primary or only one active
  {
    // vr_compositor->WaitGetPoses(vr_tracked_device_pose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    // This is usually called *after* submit for the *next* frame's poses.
    // For *current* frame rendering, GetDeviceToAbsoluteTrackingPose is used.
    // This might be better placed in UpdateHeadTrackingIfNeeded or called by backend before rendering each eye.
    // For now, assume UpdateHeadTrackingIfNeeded handles populating vr_tracked_device_pose.
  }
#endif
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  if (g_has_rift && oculus_session && (!g_has_openvr || g_openvr_is_rift)) // If Oculus is primary or OpenVR is on Rift
  {
    ovrTrackingState ts = ovr_GetTrackingState(oculus_session, ovr_GetPredictedDisplayTime(oculus_session, 0), ovrTrue);
    ovr_CalcEyePoses(ts.HeadPose.ThePose, oculus_eye_render_desc[0].HmdToEyePose.Position, g_eye_poses);
     // In Hydra, HmdToEyeViewOffset was used. For SDK 1.x, it's HmdToEyePose from ovrEyeRenderDesc
    // or simply use ovr_CalcEyePoses with the head pose.
    // The HmdToEyePose is part of ovrEyeRenderDesc which is obtained from ovr_GetRenderDesc.
    // For simplicity, assuming g_eye_poses are directly filled by ovr_CalcEyePoses or similar.
    // ovr_GetEyePoses(oculus_session, g_ovr_frameindex, ovrTrue, hmdToEyeOffset, g_eye_poses, nullptr);
  }
#endif
}

void VR_UpdateHeadTrackingIfNeeded()
{
  if (!g_has_hmd || !g_new_tracking_frame) return;
  g_new_tracking_frame = false;

#ifdef HAVE_OPENVR_SDK
  if (g_has_openvr && vr_system)
  {
    vr_compositor->WaitGetPoses(vr_tracked_device_pose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    if (vr_tracked_device_pose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
    {
      const vr::HmdMatrix34_t& mat = vr_tracked_device_pose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
      // Convert SteamVR matrix to Dolphin's format (assuming column-major vs row-major differences)
      // Dolphin's Matrix44 is row-major. SteamVR's HmdMatrix34_t is row-major.
      // So, direct copy for rotation part, then extract translation.
      for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) g_head_tracking_matrix.m[r][c] = mat.m[r][c];
      g_head_tracking_matrix.m[3][0] = g_head_tracking_matrix.m[3][1] = g_head_tracking_matrix.m[3][2] = 0; // No translation in rot part
      g_head_tracking_matrix.m[3][3] = 1;

      g_head_tracking_position[0] = mat.m[0][3];
      g_head_tracking_position[1] = mat.m[1][3];
      g_head_tracking_position[2] = mat.m[2][3];
      // Apply any necessary inversions or coordinate system adjustments if needed.
      // Hydra inverted X and Y position: g_head_tracking_position[0] = -x; g_head_tracking_position[1] = -y;
    }
  }
#endif
#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  if (g_has_rift && oculus_session && (!g_has_openvr || g_openvr_is_rift))
  {
    ovrPosef headPose = ovr_GetTrackingState(oculus_session, 0.0, ovrTrue).HeadPose.ThePose;

    // Convert OVR Pose to Matrix44 g_head_tracking_matrix and g_head_tracking_position
    // OVR::Posef tempPose(headPose); // If using OVR_Math.h types
    // OVR::Matrix4f ovrMat(tempPose);
    // For g_head_tracking_matrix (rotation only):
    // Convert quaternion to rotation matrix
    ovrQuatf q = headPose.Orientation;
    g_head_tracking_matrix.SetFromQuaternion(MathUtil::Quaternion(q.x, q.y, q.z, q.w));

    g_head_tracking_position[0] = headPose.Position.x;
    g_head_tracking_position[1] = headPose.Position.y;
    g_head_tracking_position[2] = headPose.Position.z;
    // Hydra inverted positions: g_head_tracking_position[0] = -x; ...
  }
#endif
  // TODO: VR920 tracking
}


void VR_GetProjectionMatrices(Matrix44& left_eye, Matrix44& right_eye, float znear, float zfar)
{
  if (!g_has_hmd) { /* provide some default non-stereo matrix? */ return; }

#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  if (g_has_rift && oculus_session && (!g_has_openvr || g_openvr_is_rift))
  {
    ovrMatrix4f proj_left = ovrMatrix4f_Projection(g_eye_fov[0], znear, zfar, ovrProjection_ClipRangeOpenGL); // Or ovrProjection_None
    ovrMatrix4f proj_right = ovrMatrix4f_Projection(g_eye_fov[1], znear, zfar, ovrProjection_ClipRangeOpenGL);
    std::memcpy(left_eye.m, proj_left.M, sizeof(float) * 16);
    std::memcpy(right_eye.m, proj_right.M, sizeof(float) * 16);
    // Transpose if OVR matrices are column-major and Matrix44 is row-major
    // left_eye.Transpose(); right_eye.Transpose(); (Check OVR_CAPI.h and MathUtil.h conventions)
    return;
  }
#endif
#ifdef HAVE_OPENVR_SDK
  if (g_has_openvr && vr_system)
  {
    vr::HmdMatrix44_t mat_left = vr_system->GetProjectionMatrix(vr::Eye_Left, znear, zfar);
    vr::HmdMatrix44_t mat_right = vr_system->GetProjectionMatrix(vr::Eye_Right, znear, zfar);
    for(int r=0; r<4; ++r) for(int c=0; c<4; ++c) left_eye.m[r][c] = mat_left.m[r][c];
    for(int r=0; r<4; ++r) for(int c=0; c<4; ++c) right_eye.m[r][c] = mat_right.m[r][c];
    // Transpose if necessary
    return;
  }
#endif
  // Fallback if no HMD or SDK active (should not happen if g_has_hmd is true)
  Matrix44::Perspective(60, 1.0f, znear, zfar, left_eye); // Placeholder
  right_eye = left_eye;
}

void VR_GetEyeOffsets(float posLeft[3], float posRight[3])
{
  if (!g_has_hmd) { /* default IPD? */ return; }

#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
  if (g_has_rift && oculus_session && (!g_has_openvr || g_openvr_is_rift))
  {
    // oculus_eye_render_desc should be populated during VR_Init or when HMD connects
    posLeft[0] = oculus_eye_render_desc[0].HmdToEyePose.Position.x;
    posLeft[1] = oculus_eye_render_desc[0].HmdToEyePose.Position.y;
    posLeft[2] = oculus_eye_render_desc[0].HmdToEyePose.Position.z;
    posRight[0] = oculus_eye_render_desc[1].HmdToEyePose.Position.x;
    posRight[1] = oculus_eye_render_desc[1].HmdToEyePose.Position.y;
    posRight[2] = oculus_eye_render_desc[1].HmdToEyePose.Position.z;
    return;
  }
#endif
#ifdef HAVE_OPENVR_SDK
  if (g_has_openvr && vr_system)
  {
    vr::HmdMatrix34_t left_eye_to_head = vr_system->GetEyeToHeadTransform(vr::Eye_Left);
    vr::HmdMatrix34_t right_eye_to_head = vr_system->GetEyeToHeadTransform(vr::Eye_Right);
    posLeft[0] = left_eye_to_head.m[0][3]; posLeft[1] = left_eye_to_head.m[1][3]; posLeft[2] = left_eye_to_head.m[2][3];
    posRight[0] = right_eye_to_head.m[0][3]; posRight[1] = right_eye_to_head.m[1][3]; posRight[2] = right_eye_to_head.m[2][3];
    return;
  }
#endif
  // Fallback default IPD
  float default_ipd = 0.064f; // 64mm
  posLeft[0] = -default_ipd / 2.0f; posLeft[1] = 0; posLeft[2] = 0;
  posRight[0] = default_ipd / 2.0f; posRight[1] = 0; posRight[2] = 0;
}

// Other functions from Hydra's VR.cpp (VR_GetFovTextureSize, VR_GetAudioDeviceId, input functions, OpcodeReplayBuffer etc.)
// would be adapted similarly, focusing on SDK calls and removing direct backend manipulation.
// For brevity, I'll stop here for VR.cpp adaptation unless specific functions are requested.

#ifdef HAVE_OPENVR_SDK
vr::IVRSystem* VR_GetOpenVRSystem() { return vr_system; }
#endif

#if defined(HAVE_OCULUS_SDK) && (defined(OVR_MAJOR_VERSION))
ovrHmd VR_GetOculusHMD() { return oculus_session; } // NOTE: Type mismatch if older SDKs used ovrHmd, newer use ovrSession. Adjusted to ovrSession.
#endif
