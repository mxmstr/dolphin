// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <Objbase.h>
#include <mmdeviceapi.h>
#include <setupapi.h>
// clang-format on
#endif

#include "Common/Common.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR.h"

#ifdef HAVE_OPENVR
#include "VideoCommon/VROpenVR.h" // Included for OpenVR specifics
#endif
#include <Core/Config/GraphicsSettings.h>

const char* scm_vr_sdk_str = "OpenVR";

float g_current_fps = 60.0f, g_current_speed = 0.0f;

#ifdef HAVE_OPENVR
vr::IVRSystem* m_pHMD = nullptr;
vr::IVRRenderModels* m_pRenderModels = nullptr;
vr::IVRCompositor* m_pCompositor = nullptr;
std::string m_strDriver;
std::string m_strDisplay;
vr::TrackedDevicePose_t m_rTrackedDevicePose[vr::k_unMaxTrackedDeviceCount];
bool m_bUseCompositor = true; // This might be configurable or determined at runtime
bool m_rbShowTrackedDevice[vr::k_unMaxTrackedDeviceCount] = {false};
int m_iValidPoseCount = 0;
#endif

#ifdef _WIN32
LUID* g_hmd_luid = nullptr; // Potentially used for adapter matching with OpenVR on D3D
#endif

std::mutex g_vr_lock;

// Global variables to store the per-eye view matrices
Common::Matrix44 g_left_eye_view_matrix;
Common::Matrix44 g_right_eye_view_matrix;

// Default states for VR flags, assuming OpenVR.
bool g_vr_cant_motion_blur = true; // Some games/engines might inherently cause blur OpenVR can't stop
bool g_vr_must_motion_blur = false;
bool g_vr_has_dynamic_predict = false;    // OpenVR handles prediction
bool g_vr_has_configure_rendering = false;
bool g_vr_has_hq_distortion = true;      // Assume OpenVR provides good quality distortion
bool g_vr_has_configure_tracking = false;   // OpenVR runtime handles tracking configuration
bool g_vr_has_asynchronous_timewarp = true; // OpenVR compositor provides this
// bool g_vr_supports_extended = false; // This was an Oculus concept, removed in VR.h

bool g_vr_has_timewarp_tweak = false;    // Old Oculus SDK feature
bool g_vr_needs_DXGIFactory1 = true;   // Often true for D3D interop
bool g_vr_needs_endframe = false;        // Not a general OpenVR requirement for D3D submission
bool g_vr_can_disable_hsw = true;      // HSW is runtime dependent

bool g_vr_should_swap_buffers = true;  // Depends on D3D integration with OpenVR compositor
bool g_vr_dont_vsync = false;          // VR runtimes typically manage their own vsync

// General VR state variables
bool g_force_vr = false;
bool g_has_hmd = false;
bool g_has_openvr = false;
bool g_openvr_is_vive = false; // Determined at runtime by OpenVR
bool g_is_nes = false;
bool g_new_tracking_frame = true;
bool g_new_frame_tracker_for_efb_skip = true;
u32 skip_objects_count = 0;
Common::Matrix44 g_head_tracking_matrix;
Common::Vec3 g_head_tracking_position;
float g_left_hand_tracking_position[3] = {0};
float g_right_hand_tracking_position[3] = {0};
int g_hmd_window_width = 0, g_hmd_window_height = 0, g_hmd_window_x = 0, g_hmd_window_y = 0,
    g_hmd_refresh_rate = 90; // Default, will be updated by OpenVR
const char* g_hmd_device_name = nullptr;
float g_vr_speed = 0;
float vr_freelook_speed = 0;
bool g_fov_changed = false; // Flag to indicate if FOV was changed for motion sickness
bool g_vr_black_screen = false; // For motion sickness full black screen
bool g_vr_had_3D_already = false;
float vr_widest_3d_HFOV = 0;
float vr_widest_3d_VFOV = 0;
float vr_widest_3d_zNear = 0;
float vr_widest_3d_zFar = 0;
float g_game_camera_pos[3] = {0};
Common::Matrix44 g_game_camera_rotmat;

double g_older_tracking_time = 0, g_old_tracking_time = 0, g_last_tracking_time = 0;
float g_openvr_ipd = 0.064f; // Default IPD, updated by OpenVR

u8 g_vr_reading_wiimote_accel[5] = {0}, g_vr_reading_wiimote_ir[5] = {0},
   g_vr_reading_wiimote_ext[5] = {0};
bool g_vr_has_ir = false;
float g_vr_ir_x = 0, g_vr_ir_y = 0, g_vr_ir_z = 0;

ControllerStyle vr_left_controller = CS_HYDRA_LEFT, vr_right_controller = CS_HYDRA_RIGHT;

std::vector<TimewarpLogEntry> timewarp_logentries;

bool g_opcode_replay_enabled = false;
bool g_new_frame_just_rendered = false;
bool g_first_pass = true;
bool g_first_pass_vs_constants = true;
bool g_opcode_replay_frame = false;
bool g_opcode_replay_log_frame = false;
int skipped_opcode_replay_count = 0;

#ifdef _WIN32
static char hmd_device_name_buffer[MAX_PATH] = ""; // Renamed to avoid conflict
#endif

// For motion sickness FOV reduction
static bool g_reduce_fov_for_motion_sickness = false;
static float g_reduced_fov_tan_limit = 1.0f; // Default to a reasonable tangent (approx 90 deg total FOV)

void VR_NewVRFrame()
{
  g_new_tracking_frame = true;
  g_new_frame_tracker_for_efb_skip = true;
  if (!g_vr_had_3D_already)
  {
    g_game_camera_rotmat = Common::Matrix44::Identity();
  }
  g_vr_had_3D_already = false;
  skip_objects_count = 0;
  // ClearDebugProj(); // Assuming this is a general debug utility or will be handled

  // Motion sickness FOV reduction logic
  g_vr_speed = 0; // Reset speed calculation
  // TODO: Re-evaluate how g_vr_speed is calculated without HydraTLayer specifics if needed,
  // or rely on g_ActiveConfig settings directly.
  // For now, placeholder:
  if (g_ActiveConfig.bMotionSicknessAlways) g_vr_speed = 1.0f;

  g_reduce_fov_for_motion_sickness = false;
  g_vr_black_screen = false;
  bool old_fov_changed = g_fov_changed;
  g_fov_changed = false;

#ifdef HAVE_OPENVR
  if (g_has_openvr && g_ActiveConfig.iMotionSicknessMethod != 0) // 0 is typically "Off"
  {
    if (g_ActiveConfig.iMotionSicknessMethod == 2) // Full black screen
    {
      g_vr_black_screen = (g_vr_speed > 0.15f);
    }
    else if (g_ActiveConfig.iMotionSicknessMethod == 1) // Reduce FOV
    {
      if (g_vr_speed > 0.15f && g_ActiveConfig.fMotionSicknessFOV > 0.0f)
      {
        g_reduce_fov_for_motion_sickness = true;
        g_reduced_fov_tan_limit = tan(DEGREES_TO_RADIANS(g_ActiveConfig.fMotionSicknessFOV / 2.0f));
        g_fov_changed = true;
      }
    }
  }
#endif
  if (old_fov_changed != g_fov_changed) {
      // Notify if FOV state actually changed, might be needed by renderer to update something
  }
}

#ifdef HAVE_OPENVR
std::string GetTrackedDeviceString(vr::IVRSystem* pHmd, vr::TrackedDeviceIndex_t unDevice,
                                   vr::TrackedDeviceProperty prop,
                                   vr::TrackedPropertyError* peError = nullptr)
{
  uint32_t unRequiredBufferLen =
      pHmd->GetStringTrackedDeviceProperty(unDevice, prop, nullptr, 0, peError);
  if (unRequiredBufferLen == 0)
    return "";

  char* pchBuffer = new char[unRequiredBufferLen];
  pHmd->GetStringTrackedDeviceProperty(unDevice, prop, pchBuffer, unRequiredBufferLen, peError);
  std::string sResult = pchBuffer;
  delete[] pchBuffer;
  return sResult;
}

bool BInitCompositor()
{
  vr::EVRInitError peError = vr::VRInitError_None;
  m_pCompositor = (vr::IVRCompositor*)vr::VR_GetGenericInterface(vr::IVRCompositor_Version, &peError);

  if (peError != vr::VRInitError_None)
  {
    m_pCompositor = nullptr;
    NOTICE_LOG_FMT(VR, "Compositor initialization failed with error: {}: {}\n",
               vr::VR_GetVRInitErrorAsSymbol(peError),
               vr::VR_GetVRInitErrorAsEnglishDescription(peError));
    return false;
  }
  m_pCompositor->SetTrackingSpace(vr::TrackingUniverseSeated);
  return true;
}
#endif

bool InitOpenVR()
{
#ifdef HAVE_OPENVR
  vr::EVRInitError eError = vr::VRInitError_None;
  m_pHMD = vr::VR_Init(&eError, vr::VRApplication_Scene);

  if (eError != vr::VRInitError_None)
  {
    m_pHMD = nullptr;
    ERROR_LOG_FMT(VR, "Unable to init OpenVR: {}: {}", vr::VR_GetVRInitErrorAsSymbol(eError),
              vr::VR_GetVRInitErrorAsEnglishDescription(eError));
    g_has_openvr = false;
    return false;
  }

  m_strDriver = GetTrackedDeviceString(m_pHMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String);
  g_openvr_is_vive = (m_strDriver == "lighthouse");
  NOTICE_LOG_FMT(VR, "OpenVR Driver: '{}'", m_strDriver.c_str());
  m_strDisplay = GetTrackedDeviceString(m_pHMD, vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SerialNumber_String);
  NOTICE_LOG_FMT(VR, "OpenVR Display: '{}'", m_strDisplay.c_str());

  m_pRenderModels = (vr::IVRRenderModels*)vr::VR_GetGenericInterface(vr::IVRRenderModels_Version, &eError);
  if (!m_pRenderModels)
  {
    m_pHMD = nullptr;
    vr::VR_Shutdown();
    ERROR_LOG_FMT(VR, "Unable to get OpenVR render model interface: {}: {}",
      vr::VR_GetVRInitErrorAsSymbol(eError), vr::VR_GetVRInitErrorAsEnglishDescription(eError));
    g_has_openvr = false;
    return false;
  }

  if (m_bUseCompositor && !BInitCompositor())
  {
      ERROR_LOG_FMT(VR, "Failed to initialize OpenVR Compositor!");
      // We might still be able to run without compositor for some very basic head tracking,
      // but rendering to HMD won't work. For now, treat as failure.
      m_pHMD = nullptr;
      vr::VR_Shutdown();
      g_has_openvr = false;
      return false;
  }

  g_has_openvr = true;
  g_has_hmd = true;

  vr::TrackedPropertyError error;
  g_hmd_refresh_rate = (int)(0.5f + m_pHMD->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &error));
  g_openvr_ipd = m_pHMD->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &error);
  if(error != vr::TrackedProp_Success) g_openvr_ipd = 0.064f; // Default if fetch fails

  // Set default VR capability flags for OpenVR
  g_vr_needs_DXGIFactory1 = true; // Generally true for D3D interop
  g_vr_cant_motion_blur = true;   // Application controls blur
  g_vr_has_dynamic_predict = true; // OpenVR handles prediction
  g_vr_has_configure_rendering = false;
  g_vr_has_configure_tracking = false;
  g_vr_has_hq_distortion = true; // Assume good quality from OpenVR
  g_vr_should_swap_buffers = true; // For D3D, app submits textures, compositor handles swap
  g_vr_has_timewarp_tweak = false;
  g_vr_has_asynchronous_timewarp = true; // Compositor provides this

  uint32_t width, height;
  m_pHMD->GetRecommendedRenderTargetSize(&width, &height);
  g_hmd_window_width = width;
  g_hmd_window_height = height;
  // g_hmd_window_x and g_hmd_window_y are not typically set by OpenVR this way.

#ifdef _WIN32
  m_pHMD->GetDXGIOutputInfo((int32_t*)&g_hmd_luid);
  if (g_hmd_luid != nullptr)
  {
    // Successfully got LUID. This can be used by D3D backend to select the correct adapter.
    // The VR-Hydra reference did this for Oculus. It might be useful for OpenVR too in some cases.
    // For now, we just store it. The D3D backend can decide to use it.
  }
#endif

  NOTICE_LOG_FMT(VR, "OpenVR Initialized Successfully. HMD: {}, IPD: {}m, Refresh: {}Hz, Rec Target: {}x{}",
    m_strDisplay, g_openvr_ipd, g_hmd_refresh_rate, g_hmd_window_width, g_hmd_window_height);

  return true;
#else
  return false;
#endif
}

void VR_Init()
{
    g_has_hmd = false;
    g_has_openvr = false;

  if (!Config::Get(Config::GLOBAL_VR_ENABLE_VR))
  {
    return;
  }

  g_has_openvr = false;
#ifdef _WIN32
  g_hmd_luid = nullptr;
#endif

  InitOpenVR();

  if (g_force_vr && !g_has_openvr)
  {
      WARN_LOG_FMT(VR, "Forcing VR mode, but OpenVR initialization failed.");
      // If g_force_vr is true, we might set g_has_hmd = true to attempt a "no SDK" fallback,
      // but for an OpenVR-only path, this probably means VR won't work.
      // g_has_hmd = true;
  }

  // Update window settings if OpenVR provided them (done in InitOpenVR)
  if (g_has_hmd && g_hmd_window_width > 0 && g_hmd_window_height > 0)
  {
    Config::SetCurrent(Config::MAIN_RENDER_WINDOW_WIDTH, g_hmd_window_width);
    Config::SetCurrent(Config::MAIN_RENDER_WINDOW_HEIGHT, g_hmd_window_height);
    // Note: Setting fullscreen resolution or window position might conflict with OpenVR's direct mode.
    // It's usually best to let OpenVR manage the HMD display.
    // SConfig::GetInstance().strFullscreenResolution = StringFromFormat("%dx%d", g_hmd_window_width, g_hmd_window_height);
    // SConfig::GetInstance().iRenderWindowXPos = g_hmd_window_x;
    // SConfig::GetInstance().iRenderWindowYPos = g_hmd_window_y;
  }
}

void VR_StopRendering()
{
  // For OpenVR, there isn't a direct equivalent to "stop rendering" that Oculus SDK had.
  // Frame submission is per-frame. Resource cleanup happens in VR_Shutdown or VR_StopFramebuffer.
}

void VR_Shutdown()
{
#ifdef HAVE_OPENVR
  if (g_has_openvr) // Check g_has_openvr instead of m_pHMD directly to avoid issues if m_pHMD was nulled prematurely
  {
    vr::VR_Shutdown(); // This is the global OpenVR shutdown.
    m_pHMD = nullptr;
    m_pCompositor = nullptr;
    m_pRenderModels = nullptr;
    g_has_openvr = false;
    g_has_hmd = false;
    NOTICE_LOG_FMT(VR, "OpenVR Shutdown.");
  }
#endif
}

void VR_RecenterHMD()
{
#ifdef HAVE_OPENVR
  if (g_has_openvr && m_pHMD) // m_pHMD check implies vr::VRSystem() is likely available
  {
    if (vr::VRChaperone())
    {
      vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated);
      NOTICE_LOG_FMT(VR, "OpenVR HMD recentered via Chaperone.");
    }
    else
    {
      WARN_LOG_FMT(VR, "OpenVR Chaperone interface not available for recentering.");
    }
  }
#endif
}

void VR_CheckStatus(bool* ShouldRecenter, bool* ShouldQuit)
{
  // OpenVR uses an event system. Recenter/Quit are typically triggered by user actions
  // (e.g., dashboard menu) or specific events, not polled this way.
  *ShouldRecenter = false;
  *ShouldQuit = false;
}

void VR_ConfigureHMDTracking()
{
  // OpenVR tracking is generally configured by its runtime.
  // This function can be empty for an OpenVR-only implementation.
}

void VR_ConfigureHMDPrediction()
{
  // OpenVR handles prediction internally.
  // This function can be empty.
}

void VR_GetEyePoses()
{
  // This function's original purpose was to populate Oculus-specific g_eye_poses.
  // In OpenVR, we get the HMD pose via m_rTrackedDevicePose and derive eye views.
  // This function is not strictly necessary for OpenVR if VR_UpdateHeadTrackingIfNeeded is used.
  // Kept empty to avoid breaking calls if any exist, but its utility is diminished.
}

#ifdef HAVE_OPENVR
void ProcessVREvent(const vr::VREvent_t& event)
{
  switch (event.eventType)
  {
  case vr::VREvent_TrackedDeviceActivated:
    NOTICE_LOG_FMT(VR, "OpenVR Device {} ({}) attached.", event.trackedDeviceIndex,
                 GetTrackedDeviceString(m_pHMD, event.trackedDeviceIndex, vr::Prop_RenderModelName_String).c_str());
    break;
  case vr::VREvent_TrackedDeviceDeactivated:
    NOTICE_LOG_FMT(VR, "OpenVR Device {} deactivated.", event.trackedDeviceIndex);
    break;
  case vr::VREvent_TrackedDeviceUpdated:
    // This event is frequent, maybe log less verbosely or only specific updates.
    // DEBUG_LOG_FMT(VR, "OpenVR Device {} updated.", event.trackedDeviceIndex);
    break;
  case vr::VREvent_IpdChanged:
    g_openvr_ipd = event.data.ipd.ipdMeters;
    NOTICE_LOG_FMT(VR, "OpenVR IPD changed to {:.4f}m", g_openvr_ipd);
    break;
  // Handle other relevant events like VREvent_Quit, VREvent_ProcessQuit, VREvent_ChaperoneDataHasChanged etc.
  case vr::VREvent_Quit: // Deprecated, use VREvent_ProcessQuit
  case vr::VREvent_ProcessQuit:
      NOTICE_LOG_FMT(VR, "OpenVR Quit Event received.");
      // Potentially set a flag that Core::Run() can check to initiate shutdown
      break;
  default:
    break;
  }
}


void UpdateEyeViewMatrices()
{
  if (!g_has_openvr || !m_pHMD)
    return;

  // Get the main head pose (as a model matrix, not a view matrix)
  Common::Matrix44 head_pose = g_head_tracking_matrix;
  head_pose = head_pose.Inverted(); // Convert from view matrix to model matrix

  // Get eye-to-head transforms from OpenVR
  vr::HmdMatrix34_t left_eye_to_head_34 = m_pHMD->GetEyeToHeadTransform(vr::Eye_Left);
  vr::HmdMatrix34_t right_eye_to_head_34 = m_pHMD->GetEyeToHeadTransform(vr::Eye_Right);

  // Convert vr::HmdMatrix34_t to Common::Matrix44
  // HmdMatrix34_t is row major: m[row][column]
  // Common::Matrix44 is also row major and takes std::array<float, 16>
  std::array<float, 16> left_eye_to_head_arr = {
      left_eye_to_head_34.m[0][0], left_eye_to_head_34.m[0][1], left_eye_to_head_34.m[0][2], left_eye_to_head_34.m[0][3],
      left_eye_to_head_34.m[1][0], left_eye_to_head_34.m[1][1], left_eye_to_head_34.m[1][2], left_eye_to_head_34.m[1][3],
      left_eye_to_head_34.m[2][0], left_eye_to_head_34.m[2][1], left_eye_to_head_34.m[2][2], left_eye_to_head_34.m[2][3],
      0.0f,                        0.0f,                        0.0f,                        1.0f
  };
  Common::Matrix44 left_eye_to_head = Common::Matrix44::FromArray(left_eye_to_head_arr);

  std::array<float, 16> right_eye_to_head_arr = {
      right_eye_to_head_34.m[0][0], right_eye_to_head_34.m[0][1], right_eye_to_head_34.m[0][2], right_eye_to_head_34.m[0][3],
      right_eye_to_head_34.m[1][0], right_eye_to_head_34.m[1][1], right_eye_to_head_34.m[1][2], right_eye_to_head_34.m[1][3],
      right_eye_to_head_34.m[2][0], right_eye_to_head_34.m[2][1], right_eye_to_head_34.m[2][2], right_eye_to_head_34.m[2][3],
      0.0f,                         0.0f,                         0.0f,                         1.0f
  };
  Common::Matrix44 right_eye_to_head = Common::Matrix44::FromArray(right_eye_to_head_arr);

  // Final View Matrix = inverse(head_pose * eye_to_head_transform)
  // which is equivalent to: inverse(eye_to_head_transform) * inverse(head_pose)
  // inverse(head_pose) is our original g_head_tracking_matrix.

  Common::Matrix44 inv_left_eye_mat = left_eye_to_head;
  inv_left_eye_mat = inv_left_eye_mat.Inverted();
  Common::Matrix44 inv_right_eye_mat = right_eye_to_head;
  inv_right_eye_mat = inv_right_eye_mat.Inverted();

  g_left_eye_view_matrix = inv_left_eye_mat * g_head_tracking_matrix;
  g_right_eye_view_matrix = inv_right_eye_mat * g_head_tracking_matrix;
}

void UpdateOpenVRHeadTracking()
{
  if (!m_pHMD)
    return;

  vr::VREvent_t event;
  while (m_pHMD->PollNextEvent(&event, sizeof(event)))
  {
    ProcessVREvent(event);
  }

  if (vr::VRCompositor())
  {
    vr::VRCompositor()->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
  }
  else
  {
    // Fallback if compositor not available (less ideal, lacks future prediction)
    m_pHMD->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0, m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount);
  }

  g_older_tracking_time = g_old_tracking_time;
  g_old_tracking_time = g_last_tracking_time;
  g_last_tracking_time = Common::Timer::NowMs() / 1000.0; // Ensure Timer::GetTimeMs exists and is appropriate

  m_iValidPoseCount = 0;
  if (m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
  {
    m_iValidPoseCount++;
    const vr::HmdMatrix34_t& mat = m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;

    // Convert OpenVR matrix (column-major) to our Matrix44 (row-major)
    // And OpenVR's +Y up, -Z forward to Dolphin's +Y up, +Z forward convention if necessary.
    // OpenVR HmdMatrix34_t is:
    // m[0][0] m[0][1] m[0][2] m[0][3] (right_x, up_x, back_x, pos_x)
    // m[1][0] m[1][1] m[1][2] m[1][3] (right_y, up_y, back_y, pos_y)
    // m[2][0] m[2][1] m[2][2] m[2][3] (right_z, up_z, back_z, pos_z)
    // Dolphin's Matrix44 is row-major.

    Common::Matrix44 openvr_pose_matrix;
    openvr_pose_matrix.data[0] = mat.m[0][0]; openvr_pose_matrix.data[1] = mat.m[0][1]; openvr_pose_matrix.data[2] = mat.m[0][2]; openvr_pose_matrix.data[3] = mat.m[0][3];
    openvr_pose_matrix.data[4] = mat.m[1][0]; openvr_pose_matrix.data[5] = mat.m[1][1]; openvr_pose_matrix.data[6] = mat.m[1][2]; openvr_pose_matrix.data[7] = mat.m[1][3];
    openvr_pose_matrix.data[8] = mat.m[2][0]; openvr_pose_matrix.data[9] = mat.m[2][1]; openvr_pose_matrix.data[10] = mat.m[2][2]; openvr_pose_matrix.data[11] = mat.m[2][3];
    openvr_pose_matrix.data[12] = 0; openvr_pose_matrix.data[13] = 0; openvr_pose_matrix.data[14] = 0; openvr_pose_matrix.data[15] = 1;

    // Assuming OpenVR provides pose in its standard coordinate system (+Y up, -Z forward, +X right)
    // And g_head_tracking_matrix is expected to be view matrix (camera transform)
    // The pose from OpenVR is HMD_tracking_reference_to_HMD_actual.
    // To get a view matrix, we need its inverse.
    // Also, positions from OpenVR are usually direct world positions.
    // g_head_tracking_position should store the HMD's world position.
    // g_head_tracking_matrix should store the HMD's world orientation as a view matrix.

    g_head_tracking_position = Common::Vec3(mat.m[0][3], mat.m[1][3], mat.m[2][3]);

    // Extract rotation part for g_head_tracking_matrix (as a view matrix, it's the inverse of HMD world orientation)
    Common::Matrix33 rot_matrix;
    rot_matrix.data[0] = mat.m[0][0]; rot_matrix.data[1] = mat.m[1][0]; rot_matrix.data[2] = mat.m[2][0]; // Transpose for view
    rot_matrix.data[3] = mat.m[0][1]; rot_matrix.data[4] = mat.m[1][1]; rot_matrix.data[5] = mat.m[2][1];
    rot_matrix.data[6] = mat.m[0][2]; rot_matrix.data[7] = mat.m[1][2]; rot_matrix.data[8] = mat.m[2][2];

    g_head_tracking_matrix = Common::Matrix44::FromMatrix33(rot_matrix);
    // The translation part of the view matrix is -R^T * T_world
    Common::Vec3 transformed_pos = rot_matrix * g_head_tracking_position;
    g_head_tracking_matrix *= Common::Matrix44::Translate(Common::Vec3(-transformed_pos.x, -transformed_pos.y, -transformed_pos.z));

  }

  // Call this new function inside UpdateOpenVRHeadTracking()
  if (m_rTrackedDevicePose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
  {
    UpdateEyeViewMatrices(); // Add this call
  }
}

#endif

void VR_UpdateHeadTrackingIfNeeded()
{
  if (true)//g_new_tracking_frame)
  {
    g_new_tracking_frame = false;
#ifdef HAVE_OPENVR
    if (g_has_openvr && m_pHMD)
      UpdateOpenVRHeadTracking();
#endif
  }
}

void VR_GetProjectionHalfTan(float& hmd_halftan)
{
#ifdef HAVE_OPENVR
  if (g_has_openvr && m_pHMD)
  {
    float left, right, top, bottom;
    // Get raw projection values for one eye (e.g., left)
    m_pHMD->GetProjectionRaw(vr::Eye_Left, &left, &right, &top, &bottom);
    // The largest absolute tangent value determines the "half tan" for the widest FOV component
    hmd_halftan = std::max({std::abs(left), std::abs(right), std::abs(top), std::abs(bottom)});

    if (g_reduce_fov_for_motion_sickness) {
        hmd_halftan = std::min(hmd_halftan, g_reduced_fov_tan_limit);
    }
    return;
  }
#endif
  // Default fallback if no VR system is active or call fails
  hmd_halftan = tan(DEGREES_TO_RADIANS(g_ActiveConfig.fMotionSicknessFOV > 0.f ? g_ActiveConfig.fMotionSicknessFOV / 2.0f : 45.0f));
}

void VR_GetProjectionMatrices(Common::Matrix44& left_eye, Common::Matrix44& right_eye, float znear, float zfar)
{
#ifdef HAVE_OPENVR
  if (g_has_openvr && m_pHMD)
  {
    vr::HmdMatrix44_t mat_left = m_pHMD->GetProjectionMatrix(vr::Eye_Left, znear, zfar);
    vr::HmdMatrix44_t mat_right = m_pHMD->GetProjectionMatrix(vr::Eye_Right, znear, zfar);

    // Convert OpenVR's HmdMatrix44_t to Common::Matrix44
    // OpenVR matrices are column-major, Common::Matrix44 is row-major.
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        left_eye.data[r * 4 + c] = mat_left.m[r][c]; // This assumes HmdMatrix44_t is row-major or direct copy is fine. Check OpenVR docs.
        right_eye.data[r * 4 + c] = mat_right.m[r][c]; // If column-major, needs transpose: left_eye.data[c * 4 + r] = mat_left.m[r][c];
      }
    }
    // If GetProjectionMatrix returns a matrix that needs to be adjusted for FOV reduction:
    if (g_reduce_fov_for_motion_sickness) {
        // This is a simplified symmetric FOV reduction.
        // A more accurate method would involve GetProjectionRaw, modifying tangents, then rebuilding the matrix.
        float scale = tan(atan(1.0f / left_eye.data[0]) * 2.0f * 0.5f) / g_reduced_fov_tan_limit; // Approximation
        if (scale > 1.0f) { // only narrow, don't widen
            left_eye.data[0] *= scale;  left_eye.data[5] *= scale; // Scale P[0][0] and P[1][1]
            right_eye.data[0] *= scale; right_eye.data[5] *= scale;
            g_fov_changed = true;
        }
    }
    return;
  }
#endif
  // Fallback if no VR
  left_eye = Common::Matrix44::Perspective(DEGREES_TO_RADIANS(90.0f), 1.0f, znear, zfar);
  right_eye = left_eye;
}

void VR_GetEyePos(float* posLeft, float* posRight)
{
#ifdef HAVE_OPENVR
  if (g_has_openvr && m_pHMD)
  {
    vr::TrackedPropertyError error;
    g_openvr_ipd = m_pHMD->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &error);
    if (error != vr::TrackedProp_Success) {
        g_openvr_ipd = 0.064f; // Default if fetch fails
        WARN_LOG_FMT(VR, "Failed to get IPD from OpenVR: {}. Using default: {}", vr::VRSystem()->GetPropErrorNameFromEnum(error), g_openvr_ipd);
    }
    posLeft[0] = -g_openvr_ipd / 2.0f;
    posRight[0] = g_openvr_ipd / 2.0f;
    posLeft[1] = posRight[1] = 0;
    posLeft[2] = posRight[2] = 0;

    // Update g_ActiveConfig.iStereoDepth based on IPD
    // The user requested: g_ActiveConfig.iStereoDepth = g_openvr_ipd * 1000;
    // Assuming g_ActiveConfig is accessible here. If not, this needs adjustment.
    // Accessing g_ActiveConfig directly might not be ideal; a setter function or notification
    // to the config system would be cleaner if available.
    // For now, directly setting as requested.
    if (g_ActiveConfig.bEnableVR) // Only adjust if VR is active
    {
        g_ActiveConfig.iStereoDepth = static_cast<int>(g_openvr_ipd * 1000.0f);
        // It might be good to log this change or notify relevant systems if iStereoDepth is changed.
        // DEBUG_LOG_FMT(VR, "Updated iStereoDepth to {} based on IPD {:.4f}m", g_ActiveConfig.iStereoDepth, g_openvr_ipd);
    }
    return;
  }
#endif
  // Default fallback
  posLeft[0] = -0.032f;
  posRight[0] = 0.032f;
  posLeft[1] = posRight[1] = 0;
  posLeft[2] = posRight[2] = 0;
  // In fallback, perhaps also set a default iStereoDepth if VR is forced without OpenVR?
  // if (g_force_vr && g_ActiveConfig.bEnableVR) {
  //    g_ActiveConfig.iStereoDepth = static_cast<int>(0.064f * 1000.0f); // Default IPD
  // }
}

void VR_GetFovTextureSize(int* width, int* height)
{
#ifdef HAVE_OPENVR
    if(g_has_openvr && m_pHMD)
    {
        uint32_t w, h;
        m_pHMD->GetRecommendedRenderTargetSize(&w, &h);
        *width = static_cast<int>(w);
        *height = static_cast<int>(h);
        return;
    }
#endif
    *width = 1280;
    *height = 720;
}

std::wstring VR_GetAudioDeviceId()
{
#ifdef HAVE_OPENVR
  if (g_has_openvr && m_pHMD)
  {
    char audio_device_id[vr::k_unMaxPropertyStringSize];
    vr::TrackedPropertyError error;
    m_pHMD->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_Audio_DefaultPlaybackDeviceId_String, audio_device_id, sizeof(audio_device_id), &error);
    if (error == vr::TrackedProp_Success && audio_device_id[0] != '\0')
    {
        return UTF8ToWString(std::string(audio_device_id));
    }
    else if (error != vr::TrackedProp_Success && error != vr::TrackedProp_ValueNotProvidedByDevice && error != vr::TrackedProp_UnknownProperty)
    {
        // Don't warn if property just isn't there, some HMDs might not provide it.
        WARN_LOG_FMT(VR, "Failed to get OpenVR audio device ID: {}", vr::VRSystem()->GetPropErrorNameFromEnum(error));
    }
  }
#endif
  return std::wstring();
}

// Removed VR_GetRemoteButtons, VR_GetTouchButtons, VR_SetTouchVibration, VR_GetHMDGestures

// 0 = unknown, 1 = buttons, -1 = analog
static int s_vive_button_mode[2] = {};
static bool s_vive_was_touched[2] = {};
static float s_vive_initial_touch_x[2] = {};
static float s_vive_initial_touch_y[2] = {};

void ProcessViveTouchpad(int hand, bool touched, bool pressed, float x, float y, u32* specials,
                         float analogs[])
{
  *specials = 0;
  analogs[0] = 0;
  analogs[1] = 0;
  if (!touched && !pressed)
  {
    s_vive_button_mode[hand] = 0;
  }
  else
  {
    if (pressed)
    {
      s_vive_button_mode[hand] = 1;
      // dpad or classic controller diamond button layout
      if (x < -1.0f / 3.0f)
        *specials |= VIVE_SPECIAL_DPAD_LEFT;
      else if (x > 1.0f / 3.0f)
        *specials |= VIVE_SPECIAL_DPAD_RIGHT;
      else if (-1.0f / 3.0f < y && y < 1.0f / 3.0f)
        *specials |= VIVE_SPECIAL_DPAD_MIDDLE;
      if (y < -1.0f / 3.0f)
        *specials |= VIVE_SPECIAL_DPAD_DOWN;
      else if (y > 1.0f / 3.0f)
        *specials |= VIVE_SPECIAL_DPAD_UP;
      // GameCube style buttons
      float angle = RADIANS_TO_DEGREES(atan2f(y, x));
      float dd = x * x + y * y;
#define A_RADIUS 0.372f
#define INNER_XY_RADIUS 0.498f
#define OUTER_XY_RADIUS 0.856f
#define INNER_B_RADIUS 0.544f
#define B_MIN_ANGLE -170.0f
#define B_MAX_ANGLE -134.0f
#define EMPTY_MIN_ANGLE -100.0f
#define EMPTY_MAX_ANGLE -52.0f
#define X_MIN_ANGLE -18.0f
#define X_MAX_ANGLE 39.0f
#define Y_MIN_ANGLE 76.0f
#define Y_MAX_ANGLE 137.0f
      // pressing just the A button
      if (dd < A_RADIUS * A_RADIUS)
      {
        *specials |= VIVE_SPECIAL_GC_A;
      }
      else
      {
        // pressing the B button
        if (angle > B_MIN_ANGLE && angle < B_MAX_ANGLE)
        {
          *specials |= VIVE_SPECIAL_GC_B;
          // pressing in between A and B counts as both
          if (dd < INNER_B_RADIUS * INNER_B_RADIUS)
            *specials |= VIVE_SPECIAL_GC_A;
        }
        else
        {
          // pressing the X button (may also be pressing Y)
          if (angle >= X_MIN_ANGLE && angle < Y_MIN_ANGLE)
          {
            *specials |= VIVE_SPECIAL_GC_X;
            // pressing in between A and X counts as both
            if (dd < INNER_XY_RADIUS * INNER_XY_RADIUS)
              *specials |= VIVE_SPECIAL_GC_A;
          }
          // pressing the Y button (may also be pressing X)
          if (angle > X_MAX_ANGLE && angle <= Y_MAX_ANGLE)
          {
            *specials |= VIVE_SPECIAL_GC_Y;
            // pressing in between A and Y counts as both
            if (dd < INNER_XY_RADIUS * INNER_XY_RADIUS)
              *specials |= VIVE_SPECIAL_GC_A;
          }
          // pressing the empty space below B and X
          else if (angle >= EMPTY_MIN_ANGLE && angle <= EMPTY_MAX_ANGLE &&
                   dd > INNER_B_RADIUS * INNER_B_RADIUS)
          {
            *specials |= VIVE_SPECIAL_GC_EMPTY;
          }
        }
      }
      // quadrant buttons, for NES, TurboGraphx, sideways wiimote, etc.
      if (y > -0.15)
      {
        if (x < 0.15)
          *specials |= VIVE_SPECIAL_TOPLEFT;
        if (x > -0.15)
          *specials |= VIVE_SPECIAL_TOPRIGHT;
      }
      if (y < 0.15)
      {
        if (x < 0.15)
          *specials |= VIVE_SPECIAL_BOTTOMLEFT;
        if (x > -0.15)
          *specials |= VIVE_SPECIAL_BOTTOMRIGHT;
      }

      // 6 buttons diagonally, for N64, sega, arcade
      angle = DEGREES_TO_RADIANS(40);
      float xd = x * cos(angle) + y * sin(angle);
      float yd = y * cos(angle) - x * sin(angle);
      // top row
      if (yd >= -0.1)
      {
        if (xd < -1 / 3.0f + 0.04f)
          *specials |= VIVE_SPECIAL_SIX_X;
        if (xd > -1 / 3.0f - 0.04f && xd < 1 / 3.0f + 0.04f)
          *specials |= VIVE_SPECIAL_SIX_Y;
        if (xd > 1 / 3.0f - 0.04f)
          *specials |= VIVE_SPECIAL_SIX_Z;
      }
      // bottom row
      if (yd <= 0.1)
      {
        if (xd < -1 / 3.0f + 0.04f)
          *specials |= VIVE_SPECIAL_SIX_A;
        if (xd > -1 / 3.0f - 0.04f && xd < 1 / 3.0f + 0.04f)
          *specials |= VIVE_SPECIAL_SIX_B;
        if (xd > 1 / 3.0f - 0.04f)
          *specials |= VIVE_SPECIAL_SIX_C;
      }
      // todo: various wiimote face button layouts
    }
    if (!s_vive_was_touched[hand])
    {
      // touching for first time
      s_vive_initial_touch_x[hand] = x;
      s_vive_initial_touch_y[hand] = y;
    }
    else if (!pressed && s_vive_button_mode[hand] == 0)
    {
      const float min_dist = 0.4f;
      float dx = x - s_vive_initial_touch_x[hand];
      float dy = y - s_vive_initial_touch_y[hand];
      float dist_squared = dx * dx + dy * dy;
      if (dist_squared > min_dist * min_dist)
        s_vive_button_mode[hand] = -1;
    }
    if (s_vive_button_mode[hand] < 0)
    {
      analogs[0] = x;
      analogs[1] = y;
    }
  }
  s_vive_was_touched[hand] = touched;
}

double last_good_tracking_time = 0;
bool VR_GetViveButtons(u32* buttons, u32* touches, u64* specials, float triggers[], float axes[])
{
  *buttons = 0;
  *touches = 0;
#if defined(HAVE_OPENVR)
  bool result = false;
  if (m_pHMD)
  {
    // find the controllers for each hand, 100 = not found
    vr::TrackedDeviceIndex_t left_hand = vr::k_unTrackedDeviceIndexInvalid;
    vr::TrackedDeviceIndex_t right_hand = vr::k_unTrackedDeviceIndexInvalid;

    for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
    {
      if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_LeftHand)
        left_hand = i;
      else if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_RightHand)
        right_hand = i;
    }
    // Fallback if roles aren't assigned (e.g. some generic OpenVR controllers)
    if (left_hand == vr::k_unTrackedDeviceIndexInvalid || right_hand == vr::k_unTrackedDeviceIndexInvalid) {
        for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (m_pHMD->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
                if (left_hand == vr::k_unTrackedDeviceIndexInvalid && i != right_hand) left_hand = i;
                else if (right_hand == vr::k_unTrackedDeviceIndexInvalid && i != left_hand) right_hand = i;
            }
        }
    }


    if (g_ActiveConfig.bAutoPairViveControllers &&
        (left_hand == vr::k_unTrackedDeviceIndexInvalid || right_hand == vr::k_unTrackedDeviceIndexInvalid ||
         (left_hand != vr::k_unTrackedDeviceIndexInvalid && !m_rTrackedDevicePose[left_hand].bPoseIsValid) ||
         (right_hand != vr::k_unTrackedDeviceIndexInvalid && !m_rTrackedDevicePose[right_hand].bPoseIsValid)))
    {
      double t = Common::Timer::NowMs() / 1000.0;
      if (t - last_good_tracking_time > 1.0)
      {
        VR_PairViveControllers();
        last_good_tracking_time = t + 20;
      }
    }
    else
    {
      last_good_tracking_time = Common::Timer::NowMs() / 1000.0;
    }

    vr::VRControllerState_t states[2];
    memset(states, 0, sizeof(states));

    if (left_hand != vr::k_unTrackedDeviceIndexInvalid && m_pHMD->GetControllerState(left_hand, &states[0], sizeof(vr::VRControllerState_t)))
      result = true;
    if (right_hand != vr::k_unTrackedDeviceIndexInvalid && m_pHMD->GetControllerState(right_hand, &states[1], sizeof(vr::VRControllerState_t)))
      result = true;

    *buttons =
        (states[0].ulButtonPressed & 0xFF) | ((states[0].ulButtonPressed >> 24) & 0xFF00) |
        (((states[1].ulButtonPressed & 0xFF) | ((states[1].ulButtonPressed >> 24) & 0xFF00)) << 16);
    *touches =
        (states[0].ulButtonTouched & 0xFF) | ((states[0].ulButtonTouched >> 24) & 0xFF00) |
        (((states[1].ulButtonTouched & 0xFF) | ((states[1].ulButtonTouched >> 24) & 0xFF00)) << 16);

    // Axis 0 is touchpad/thumbstick, Axis 1 is trigger
    if (left_hand != vr::k_unTrackedDeviceIndexInvalid) {
      axes[4] = states[0].rAxis[0].x; // Left X
      axes[5] = states[0].rAxis[0].y; // Left Y
      triggers[0] = states[0].rAxis[1].x; // Left Trigger
      ProcessViveTouchpad(0, (states[0].ulButtonTouched & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0,
                          (states[0].ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0,
                          states[0].rAxis[0].x, states[0].rAxis[0].y, ((u32*)specials), &axes[0]);
    }
    if (right_hand != vr::k_unTrackedDeviceIndexInvalid) {
      axes[6] = states[1].rAxis[0].x; // Right X
      axes[7] = states[1].rAxis[0].y; // Right Y
      triggers[1] = states[1].rAxis[1].x; // Right Trigger
      ProcessViveTouchpad(1, (states[1].ulButtonTouched & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0,
                          (states[1].ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0,
                          states[1].rAxis[0].x, states[1].rAxis[0].y, ((u32*)specials) + 1, &axes[2]);
    }
    return result;
  }
#endif
  return false;
}

bool VR_ViveHapticPulse(int hands, int microseconds)
{
#if defined(HAVE_OPENVR)
  if (g_has_openvr && m_pHMD && hands)
  {
    vr::TrackedDeviceIndex_t left_hand = vr::k_unTrackedDeviceIndexInvalid;
    vr::TrackedDeviceIndex_t right_hand = vr::k_unTrackedDeviceIndexInvalid;
    for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
    {
      if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_LeftHand)
        left_hand = i;
      else if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_RightHand)
        right_hand = i;
    }
     // Fallback if roles aren't assigned
    if (left_hand == vr::k_unTrackedDeviceIndexInvalid || right_hand == vr::k_unTrackedDeviceIndexInvalid) {
        for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (m_pHMD->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
                if (left_hand == vr::k_unTrackedDeviceIndexInvalid && i != right_hand) left_hand = i;
                else if (right_hand == vr::k_unTrackedDeviceIndexInvalid && i != left_hand) right_hand = i;
            }
        }
    }

    if ((hands & 1) && left_hand != vr::k_unTrackedDeviceIndexInvalid)
      m_pHMD->TriggerHapticPulse(left_hand, 0, static_cast<unsigned short>(microseconds));
    if ((hands & 2) && right_hand != vr::k_unTrackedDeviceIndexInvalid)
      m_pHMD->TriggerHapticPulse(right_hand, 0, static_cast<unsigned short>(microseconds));
    return true;
  }
#endif
  return false;
}

float left_hand_old_velocity[3] = {}, left_hand_older_velocity[3] = {};
float right_hand_old_velocity[3] = {}, right_hand_older_velocity[3] = {};

bool VR_GetAccel(int index, bool sideways, bool has_extension, float* gx, float* gy, float* gz)
{
#if defined(HAVE_OPENVR)
  if (g_has_openvr && m_pHMD)
  {
    vr::TrackedDeviceIndex_t controller_index = vr::k_unTrackedDeviceIndexInvalid;
    // Determine which controller based on 'index' (assuming 0 for right, 1 for left, or similar logic)
    // This needs to map to the actual tracked device indices.
    // For simplicity, let's assume index 0 is right hand, index 1 is left.
    // This should be made more robust if Wiimote emulation maps differently.
     for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
    {
      if (index == 0 && m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_RightHand) {
        controller_index = i;
        break;
      }
      // Potentially add logic for left hand if index == 1
    }
     if (controller_index == vr::k_unTrackedDeviceIndexInvalid) {
         // Fallback if role not assigned
         for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (m_pHMD->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
                // Simple fallback: first controller found for index 0
                if (index == 0) { controller_index = i; break;}
            }
         }
     }


    if (controller_index == vr::k_unTrackedDeviceIndexInvalid || !m_rTrackedDevicePose[controller_index].bPoseIsValid)
    {
      return false;
    }

    const vr::HmdMatrix34_t& pose_matrix = m_rTrackedDevicePose[controller_index].mDeviceToAbsoluteTracking;
    Common::Matrix33 m; // Orientation of the controller
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        m.data[r * 3 + c] = pose_matrix.m[r][c]; // Assuming row-major storage in Common::Matrix33

    // Get linear acceleration (already in tracking space coordinates)
    Common::Vec3 linear_acceleration(
        m_rTrackedDevicePose[controller_index].vVelocity.v[0] - right_hand_old_velocity[0], // Simplified, should use proper time delta
        m_rTrackedDevicePose[controller_index].vVelocity.v[1] - right_hand_old_velocity[1],
        m_rTrackedDevicePose[controller_index].vVelocity.v[2] - right_hand_old_velocity[2]
    );
    // Update old velocities (this is a crude differentiation, OpenVR might provide acceleration directly or better velocity)
    // A proper time delta (g_last_tracking_time - g_old_tracking_time) should be used.
    // Calculate acceleration from velocity and time delta.
    // This assumes g_last_tracking_time, g_old_tracking_time, and right_hand_old_velocity are correctly updated elsewhere.
    // A more robust solution would involve checking if (g_last_tracking_time - g_old_tracking_time) is non-zero.
    // For now, we proceed assuming it's valid as per the existing structure.
    float dt = static_cast<float>(g_last_tracking_time - g_old_tracking_time); // More accurate delta
    if (dt < 0.0001f) dt = 0.0001f; // Prevent division by zero or very small numbers

    linear_acceleration = Common::Vec3(
        (m_rTrackedDevicePose[controller_index].vVelocity.v[0] - right_hand_old_velocity[0]) / dt,
        (m_rTrackedDevicePose[controller_index].vVelocity.v[1] - right_hand_old_velocity[1]) / dt,
        (m_rTrackedDevicePose[controller_index].vVelocity.v[2] - right_hand_old_velocity[2]) / dt
    );

    // Update old velocities for the next frame's calculation
    right_hand_older_velocity[0] = right_hand_old_velocity[0];
    right_hand_older_velocity[1] = right_hand_old_velocity[1];
    right_hand_older_velocity[2] = right_hand_old_velocity[2];
    right_hand_old_velocity[0] = m_rTrackedDevicePose[controller_index].vVelocity.v[0];
    right_hand_old_velocity[1] = m_rTrackedDevicePose[controller_index].vVelocity.v[1];
    right_hand_old_velocity[2] = m_rTrackedDevicePose[controller_index].vVelocity.v[2];

    // Transform world-space acceleration (which includes gravity if standing still) to controller's local space
    // For tilt sensing (gravity vector in controller space):
    // We need the inverse of the controller's rotation matrix to transform the world's up vector (0,1,0 or 0,-1,0 depending on convention)
    // into controller space. Or, simpler, the Y column of the controller's orientation matrix (if it's world_to_local)
    // represents where the controller's Y axis points in world space. We want where world Y points in controller space.
    // If m is local_to_world, then m.Transpose() is world_to_local.
    Common::Matrix33 world_to_local_orientation = m.Transposed();
    Common::Vec3 world_gravity_vector(0.0f, -1.0f, 0.0f); // Assuming -Y is world down (standard graphics)
                                                        // OpenVR might use +Y up. If so, (0,1,0)
                                                        // Let's assume OpenVR world is +Y up for now.
    world_gravity_vector = Common::Vec3(0.0f, 1.0f, 0.0f);
    Common::Vec3 local_gravity = world_to_local_orientation * world_gravity_vector;

    // Combine local_gravity with physical acceleration (which should ideally be without gravity)
    // The accelerometer reading = physical acceleration + local_gravity_effect
    // Wiimote gx, gy, gz expect values in Gs. 1G = 9.8 m/s^2.
    // OpenVR vPhysicalAcceleration is in m/s^2.
    float physical_acc_x_g = linear_acceleration.x / 9.80665f;
    float physical_acc_y_g = linear_acceleration.y / 9.80665f;
    float physical_acc_z_g = linear_acceleration.z / 9.80665f;

    // Wiimote axes: +X right, +Y up (pointing away from IR camera), +Z towards buttons
    // OpenVR controller axes: +X right, +Y up, -Z forward (standard for controllers)
    // This mapping needs to be precise based on how the Vive controller is held like a Wiimote.

    if (sideways) // Wiimote held sideways (NES style)
    {
        // This mapping depends on how the Vive controller is oriented when held sideways.
        // Assuming standard Vive controller orientation:
        // Wiimote +X (right)  -> Vive Controller -Z (forward)
        // Wiimote +Y (up, away from IR) -> Vive Controller +X (right)
        // Wiimote +Z (buttons) -> Vive Controller +Y (up)
        *gx = -local_gravity.z + physical_acc_z_g; // Wiimote X from Vive -Z
        *gy =  local_gravity.x + physical_acc_x_g; // Wiimote Y from Vive +X
        *gz =  local_gravity.y + physical_acc_y_g; // Wiimote Z from Vive +Y
    }
    else // Wiimote held normally
    {
        // Wiimote +X (right) -> Vive Controller +X (right)
        // Wiimote +Y (up, away from IR) -> Vive Controller +Y (up)
        // Wiimote +Z (buttons) -> Vive Controller -Z (forward)
        *gx =  local_gravity.x + physical_acc_x_g;
        *gy =  local_gravity.y + physical_acc_y_g;
        *gz = -local_gravity.z - physical_acc_z_g; // Note: physical_acc_z is typically forward, Wiimote Z is toward buttons (often also forward)
                                                // This needs careful axis alignment check.
    }
    return true;
  }
#endif
  return false;
}

bool VR_GetNunchuckAccel(int index, float* gx, float* gy, float* gz)
{
#if defined(HAVE_OPENVR)
  // Similar logic to VR_GetAccel, but for the left controller if used as a Nunchuk.
  // This requires identifying the left controller and its pose.
  if (g_has_openvr && m_pHMD && index == 0) // Assuming index 0 for the primary (or only) nunchuk
  {
    vr::TrackedDeviceIndex_t nunchuk_controller_index = vr::k_unTrackedDeviceIndexInvalid;
    for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_LeftHand) {
            nunchuk_controller_index = i;
            break;
        }
    }
    // Fallback if role not set
    if (nunchuk_controller_index == vr::k_unTrackedDeviceIndexInvalid) {
         for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (m_pHMD->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
                // A simple heuristic: if right hand is known, this is not it.
                // This isn't robust if multiple generic controllers are present.
                vr::ETrackedControllerRole role = m_pHMD->GetControllerRoleForTrackedDeviceIndex(i);
                if (role != vr::TrackedControllerRole_RightHand) {
                     nunchuk_controller_index = i;
                     break;
                }
            }
         }
    }


    if (nunchuk_controller_index == vr::k_unTrackedDeviceIndexInvalid || !m_rTrackedDevicePose[nunchuk_controller_index].bPoseIsValid)
      return false;

    const vr::HmdMatrix34_t& pose_matrix = m_rTrackedDevicePose[nunchuk_controller_index].mDeviceToAbsoluteTracking;
    Common::Matrix33 m;
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        m.data[r * 3 + c] = pose_matrix.m[r][c]; // Assuming m.data is row-major access m[row][col] style

    // Calculate acceleration from velocity and time delta.
    float dt = static_cast<float>(g_last_tracking_time - g_old_tracking_time);
    if (dt < 0.0001f) dt = 0.0001f; // Prevent division by zero

    Common::Vec3 linear_acceleration(
        (m_rTrackedDevicePose[nunchuk_controller_index].vVelocity.v[0] - left_hand_old_velocity[0]) / dt,
        (m_rTrackedDevicePose[nunchuk_controller_index].vVelocity.v[1] - left_hand_old_velocity[1]) / dt,
        (m_rTrackedDevicePose[nunchuk_controller_index].vVelocity.v[2] - left_hand_old_velocity[2]) / dt
    );

    // Update old velocities for the next frame's calculation
    left_hand_older_velocity[0] = left_hand_old_velocity[0];
    left_hand_older_velocity[1] = left_hand_old_velocity[1];
    left_hand_older_velocity[2] = left_hand_old_velocity[2];
    left_hand_old_velocity[0] = m_rTrackedDevicePose[nunchuk_controller_index].vVelocity.v[0];
    left_hand_old_velocity[1] = m_rTrackedDevicePose[nunchuk_controller_index].vVelocity.v[1];
    left_hand_old_velocity[2] = m_rTrackedDevicePose[nunchuk_controller_index].vVelocity.v[2];

    Common::Matrix33 world_to_local_orientation = m.Transposed();
    Common::Vec3 world_gravity_vector(0.0f, 1.0f, 0.0f); // Assuming +Y up for OpenVR world
    Common::Vec3 local_gravity = world_to_local_orientation * world_gravity_vector;

    float physical_acc_x_g = linear_acceleration.x / 9.80665f;
    float physical_acc_y_g = linear_acceleration.y / 9.80665f;
    float physical_acc_z_g = linear_acceleration.z / 9.80665f;

    // Nunchuk axes: +X right, +Y top (buttons up), +Z forward (stick direction)
    // Vive controller (held naturally): +X right, +Y up (thumbstick/trackpad face), -Z forward (trigger direction)
    // Mapping depends on how a Vive controller is imagined as a Nunchuk.
    // Typical Nunchuk orientation: Stick forward, C/Z buttons on top.
    // If Vive controller is held similarly (trigger down, thumbstick forward):
    // Nunchuk +X (right) -> Vive +X
    // Nunchuk +Y (top)   -> Vive +Y
    // Nunchuk +Z (fwd)   -> Vive -Z
    *gx =  local_gravity.x + physical_acc_x_g;
    *gy =  local_gravity.y + physical_acc_y_g;
    *gz = -local_gravity.z - physical_acc_z_g;
    return true;
  }
#endif
  return false;
}

bool VR_GetIR(int index, double* irx, double* iry, double* irz)
{
#if defined(HAVE_OPENVR)
  // OpenVR does not directly provide IR dot tracking like a Wiimote sensor bar.
  // This function would need to simulate IR based on controller pointing if desired.
  // The existing g_vr_has_ir, g_vr_ir_x, etc. flags would need to be populated by such simulation.
  // For a direct port, if these are not populated by other means, this will return false.
  if (g_has_openvr && g_vr_has_ir) // Check if simulation has populated these
  {
      *irx = g_vr_ir_x;
      *iry = g_vr_ir_y;
      *irz = g_vr_ir_z; // Depth component
      return true;
  }
#endif
  return false;
}

// Removed VR_GetHMDGestures (was Oculus-specific tap gesture)


void VR_UpdateWiimoteReportingMode(int index, u8 accel, u8 ir, u8 ext)
{
  g_vr_reading_wiimote_accel[index] = accel;
  g_vr_reading_wiimote_ir[index] = ir;
  g_vr_reading_wiimote_ext[index] = ext;
}

bool VR_GetLeftControllerPos(float* pos, float* thumbpos, Common::Matrix33* m)
{
#if defined(HAVE_OPENVR)
  if (g_has_openvr && m_pHMD)
  {
    vr::TrackedDeviceIndex_t controller_index = vr::k_unTrackedDeviceIndexInvalid;
    for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_LeftHand) {
            controller_index = i;
            break;
        }
    }
    // Fallback
    if (controller_index == vr::k_unTrackedDeviceIndexInvalid) {
         for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (m_pHMD->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
                if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) != vr::TrackedControllerRole_RightHand) { // Not right, assume left
                     controller_index = i;
                     break;
                }
            }
         }
    }

    if (controller_index != vr::k_unTrackedDeviceIndexInvalid && m_rTrackedDevicePose[controller_index].bPoseIsValid)
    {
      const vr::HmdMatrix34_t& pose_matrix = m_rTrackedDevicePose[controller_index].mDeviceToAbsoluteTracking;
      pos[0] = pose_matrix.m[0][3];
      pos[1] = pose_matrix.m[1][3];
      pos[2] = pose_matrix.m[2][3];
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
          m->data[r * 3 + c] = pose_matrix.m[r][c]; // Assuming row-major in Common::Matrix33

      vr::VRControllerState_t cs;
      if (m_pHMD->GetControllerState(controller_index, &cs, sizeof(cs)))
      {
        thumbpos[0] = cs.rAxis[0].x; // Axis 0: Touchpad/Thumbstick X
        thumbpos[1] = cs.rAxis[0].y; // Axis 0: Touchpad/Thumbstick Y
        if (cs.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) // Or k_EButton_A if it's a thumbstick press
          thumbpos[2] = 1; // Pressed
        else if (cs.ulButtonTouched & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad))
          thumbpos[2] = 0; // Touched
        else
          thumbpos[2] = -1; // Not touched
        // Example: Check if menu button is also used for a special mode
        // if ((cs.ulButtonPressed | cs.ulButtonTouched) & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) { ... }
      }
      else
      {
        thumbpos[0] = thumbpos[1] = 0;
        thumbpos[2] = -1;
      }
      return true;
    }
  }
#endif
  // Fallback default position
  pos[0] = -0.15f; pos[1] = -0.30f; pos[2] = -0.4f;
  thumbpos[0] = thumbpos[1] = 0; thumbpos[2] = -1;
  if (m) *m = Common::Matrix33::Identity();
  return false; // Indicate failure or fallback
}

bool VR_GetRightControllerPos(float* pos, float* thumbpos, Common::Matrix33* m)
{
#if defined(HAVE_OPENVR)
  if (g_has_openvr && m_pHMD)
  {
    vr::TrackedDeviceIndex_t controller_index = vr::k_unTrackedDeviceIndexInvalid;
     for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) == vr::TrackedControllerRole_RightHand) {
            controller_index = i;
            break;
        }
    }
    // Fallback
    if (controller_index == vr::k_unTrackedDeviceIndexInvalid) {
         for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (m_pHMD->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
                 if (m_pHMD->GetControllerRoleForTrackedDeviceIndex(i) != vr::TrackedControllerRole_LeftHand) { // Not left, assume right
                     controller_index = i;
                     break;
                }
            }
         }
    }

    if (controller_index != vr::k_unTrackedDeviceIndexInvalid && m_rTrackedDevicePose[controller_index].bPoseIsValid)
    {
      const vr::HmdMatrix34_t& pose_matrix = m_rTrackedDevicePose[controller_index].mDeviceToAbsoluteTracking;
      pos[0] = pose_matrix.m[0][3];
      pos[1] = pose_matrix.m[1][3];
      pos[2] = pose_matrix.m[2][3];
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
          m->data[r * 3 + c] = pose_matrix.m[r][c];

      vr::VRControllerState_t cs;
      if (m_pHMD->GetControllerState(controller_index, &cs, sizeof(cs)))
      {
        thumbpos[0] = cs.rAxis[0].x;
        thumbpos[1] = cs.rAxis[0].y;
        if (cs.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad))
          thumbpos[2] = 1;
        else if (cs.ulButtonTouched & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad))
          thumbpos[2] = 0;
        else
          thumbpos[2] = -1;
      }
      else
      {
        thumbpos[0] = thumbpos[1] = 0;
        thumbpos[2] = -1;
      }
      return true;
    }
  }
#endif
  pos[0] = 0.15f; pos[1] = -0.30f; pos[2] = -0.4f;
  thumbpos[0] = thumbpos[1] = 0; thumbpos[2] = -1;
  if (m) *m = Common::Matrix33::Identity();
  return false;
}

void VR_SetGame(bool is_wii, bool is_nand, std::string id)
{
  g_is_nes = false;
  if (!is_wii)
  {
    vr_left_controller = CS_GC_LEFT;
    vr_right_controller = CS_GC_RIGHT;
  }
  else if (!is_nand)
  {
    vr_left_controller = CS_NUNCHUK_UNREAD;
    vr_right_controller = CS_WIIMOTE;
  }
  else
  {
    char c = id.empty() ? ' ' : id[0];
    switch (c)
    {
    case 'C': case 'X':
      vr_left_controller = CS_ARCADE_LEFT; vr_right_controller = CS_ARCADE_RIGHT; break;
    case 'E':
      vr_left_controller = CS_ARCADE_LEFT; vr_right_controller = CS_ARCADE_RIGHT; break;
    case 'F':
      g_is_nes = true;
      if (id.length() > 3 && id[3] == 'J') {
        vr_left_controller = CS_FAMICON_LEFT; vr_right_controller = CS_FAMICON_RIGHT;
      } else {
        vr_left_controller = CS_NES_LEFT; vr_right_controller = CS_NES_RIGHT;
      }
      break;
    case 'J':
      vr_left_controller = CS_SNES_LEFT;
      vr_right_controller = (id.length() > 3 && id[3] == 'E') ? CS_SNES_NTSC_RIGHT : CS_SNES_RIGHT;
      break;
    case 'L':
      vr_left_controller = CS_SEGA_LEFT; vr_right_controller = CS_SEGA_RIGHT; break;
    case 'M':
      vr_left_controller = CS_GENESIS_LEFT; vr_right_controller = CS_GENESIS_RIGHT; break;
    case 'N':
      vr_left_controller = CS_N64_LEFT; vr_right_controller = CS_N64_RIGHT; break;
    case 'P': case 'Q':
      vr_left_controller = CS_TURBOGRAFX_LEFT; vr_right_controller = CS_TURBOGRAFX_RIGHT; break;
    case 'H': case 'W':
    default:
      vr_left_controller = CS_NUNCHUK_UNREAD; vr_right_controller = CS_WIIMOTE; break;
    }
  }
}

ControllerStyle VR_GetHydraStyle(int hand)
{
  if (hand) // Right hand
  {
    if (vr_right_controller == CS_WIIMOTE && g_vr_reading_wiimote_ir[0])
      return CS_WIIMOTE_IR; // Use IR if Wiimote and IR is being read
    return vr_right_controller;
  }
  else // Left hand
  {
    if (vr_left_controller == CS_NUNCHUK_UNREAD && g_vr_reading_wiimote_ext[0])
      return CS_NUNCHUK; // Use Nunchuk if extension is read
    return vr_left_controller;
  }
}

bool VR_PairViveControllers()
{
#ifdef _WIN32
  HWND SteamVRStatusWindow = FindWindowA("Qt5QWindowIcon", "SteamVR Status");
  if (!SteamVRStatusWindow)
    return false;
  POINT C;
  BOOL hasC = GetCursorPos(&C);
  INPUT input[16] = {}; // Max 8 keybd + 8 mouse events, though fewer used
  RECT r;
  if (!GetWindowRect(SteamVRStatusWindow, &r))
    return false;
  SetCursorPos(r.left + 100, r.top + 100); // Position inside window for context menu

  // Simulate Right Click
  input[0].type = INPUT_MOUSE; input[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
  input[1].type = INPUT_MOUSE; input[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
  SendInput(2, input, sizeof(INPUT));
  Sleep(100); // Allow context menu to appear

  // Simulate KeyDown (x2) then Enter for "Pair Controller" (assuming it's 3rd item)
  // This is fragile and depends on SteamVR UI
  for (int i = 0; i < 2; ++i) { // Navigate down twice
    input[0].type = INPUT_KEYBOARD; input[0].ki.wVk = VK_DOWN; input[0].ki.dwFlags = 0;
    input[1].type = INPUT_KEYBOARD; input[1].ki.wVk = VK_DOWN; input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, input, sizeof(INPUT));
    Sleep(50);
  }
  input[0].type = INPUT_KEYBOARD; input[0].ki.wVk = VK_RETURN; input[0].ki.dwFlags = 0;
  input[1].type = INPUT_KEYBOARD; input[1].ki.wVk = VK_RETURN; input[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(2, input, sizeof(INPUT));
  Sleep(100);

  if (hasC) SetCursorPos(C.x, C.y); // Restore cursor
  return true;
#else
  return false;
#endif
}

void OpcodeReplayBuffer()
{
  // This function's logic relied on g_ActiveConfig settings that might have been Oculus-specific
  // or tied to the old dual-SDK structure. It also directly manipulated g_ActiveConfig values.
  // Given the goal is to simplify and focus on OpenVR, and this feature was marked "In Alpha",
  // it's best to disable or remove it unless its utility and compatibility with a pure OpenVR
  // setup can be clearly established and reimplemented.
  // For now, it will be effectively disabled by not running its core logic.
  if (g_opcode_replay_enabled) // If it was somehow enabled, clear logs and disable
  {
      timewarp_logentries.clear();
      // timewarp_logentries.resize(0); // .clear() is sufficient
  }
  g_opcode_replay_enabled = false;
  g_opcode_replay_log_frame = false;
  g_opcode_replay_frame = false; // Ensure this is also reset
}

void OpcodeReplayBufferInline()
{
  // Similar to OpcodeReplayBuffer, this was an experimental feature.
  // Disabling its core logic for now.
  if (g_opcode_replay_enabled)
  {
      timewarp_logentries.clear();
  }
  g_opcode_replay_enabled = false;
  g_opcode_replay_log_frame = false;
  g_opcode_replay_frame = false;
  // g_Config.iExtraVideoLoopsDivider = 0; // g_Config is not directly accessible here.
                                        // This was likely VideoConfig& g_Config.
                                        // If this needs to be set, it should be via g_ActiveConfig or similar.
                                        // For now, removing this line as the feature is disabled.
}

#ifdef HAVE_OPENVR
// Make sure to include D3D11 header for ID3D11Resource if not already included via other headers.
// It's likely included via D3DBase.h or similar if this file is part of D3D backend compilation unit,
// but good to be mindful. <d3d11.h>
// Also, ensure m_pCompositor is valid before calling.

void VR_SubmitFrameD3D(ID3D11Resource* pD3DTexture)
{
  if (!g_has_openvr || !m_pCompositor || !pD3DTexture)
  {
    // Log an error or warning if submission is attempted without OpenVR, compositor, or texture
    if (!pD3DTexture) WARN_LOG_FMT(VR, "VR_SubmitFrameD3D called with null texture.");
    return;
  }

  // Texture_t for submitting to the compositor.
  // For D3D11, pHandle is ID3D11Resource*.
  // The API implies that for texture arrays, submitting Eye_Left uses slice 0 and Eye_Right uses slice 1.
  // No explicit slice index or bounds seem necessary if the texture array is set up correctly
  // and the HMD expects full-frame textures per eye.
  vr::Texture_t eyeTexture = {pD3DTexture, vr::TextureType_DirectX, vr::ColorSpace_Auto};

  // Submit for Left Eye (implicitly slice 0 of the texture array)
  vr::EVRCompositorError CErrorLeft = vr::VRCompositor()->Submit(vr::Eye_Left, &eyeTexture);
  if (CErrorLeft != vr::VRCompositorError_None)
  {
    ERROR_LOG_FMT(VR, "OpenVR: Failed to submit left eye texture. Error: {}", static_cast<int>(CErrorLeft));
  }

  // Submit for Right Eye (implicitly slice 1 of the texture array)
  vr::EVRCompositorError CErrorRight = vr::VRCompositor()->Submit(vr::Eye_Right, &eyeTexture);
  if (CErrorRight != vr::VRCompositorError_None)
  {
    ERROR_LOG_FMT(VR, "OpenVR: Failed to submit right eye texture. Error: {}", static_cast<int>(CErrorRight));
  }

  // IMPORTANT: After submitting, OpenVR needs to know the application is done with its frame.
  // This is typically done by calling vr::VRCompositor()->PostPresentHandoff()
  // if you are managing your own D3D device and swap chain for the HMD.
  // However, if the application is rendering to a texture that OpenVR then copies or uses,
  // this might not be strictly necessary, but it's good practice.
  // The older Dolphin VR fork might not have had this.
  // Let's assume for now that Submit is enough and PostPresentHandoff is handled by OpenVR runtime
  // if it's managing the display. If screen flickering or black screen occurs, this is a place to investigate.

  // vr::VRCompositor()->PostPresentHandoff(); // Consider if needed.

  // After submission, it's also common to wait for the frame to be "really" presented
  // to synchronize game loop with HMD refresh rate. This is done by WaitGetPoses.
  // Since WaitGetPoses is already called in UpdateOpenVRHeadTracking, it might cover this.
  // If not, a WaitGetPoses call here might be needed for smooth rendering.
  // vr::VRCompositor()->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
}
#endif
