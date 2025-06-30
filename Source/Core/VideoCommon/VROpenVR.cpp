#include "VROpenVR.h"
#include "Common/Logging/Log.h" // For logging
#include <chrono> // For std::chrono::seconds for sleep (if needed, not directly for attempts)
#include <thread> // For std::this_thread::sleep_for (if implementing delays between attempts here)


VROpenVR::VROpenVR()
    : m_ivr_system(nullptr),
      m_ivr_compositor(nullptr),
      m_initialized(false),
      m_left_controller_index(vr::k_unTrackedDeviceIndexInvalid),
      m_right_controller_index(vr::k_unTrackedDeviceIndexInvalid),
      m_controller_init_attempts_left(MAX_CONTROLLER_INIT_ATTEMPTS)
{
}

VROpenVR::~VROpenVR()
{
  // Shutdown should be called explicitly, but as a fallback.
  if (m_initialized)
  {
    Shutdown();
  }
}

bool VROpenVR::Init(vr::EVRApplicationType app_type)
{
  if (m_initialized)
  {
    // It might be an issue if Init is called again with a different app_type.
    // For now, assume if it's initialized, it's with the correct type needed previously.
    // Or, we could check if the existing m_ivr_system was inited with the same app_type,
    // but VR_Init doesn't really allow querying the current application type easily.
    // Safest might be to Shutdown and re-Init if app_type is different, but that's complex.
    // Given current usage (temp utility init, then main scene init), this simple check is okay.
    GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "VROpenVR already initialized, skipping re-initialization with new app_type {}.", static_cast<int>(app_type));
    return true;
  }

  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "VROpenVR initialization started with app_type: {}.", static_cast<int>(app_type));

  vr::EVRInitError eError = vr::VRInitError_None;
  m_ivr_system = vr::VR_Init(&eError, app_type);

  if (eError != vr::VRInitError_None)
  {
    m_ivr_system = nullptr;
    GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LERROR,
                     "VROpenVR initialization failed: {}", vr::VR_GetVRInitErrorAsEnglishDescription(eError));
    return false;
  }

  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "VR_Init successful");

  if (!vr::VRCompositor())
  {
    GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LERROR,
                     "Failed to initialize VR Compositor.");
    vr::VR_Shutdown();
    m_ivr_system = nullptr;
    return false;
  }
  m_ivr_compositor = vr::VRCompositor();
  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "VRCompositor successful.");

  //if (!m_ivr_system->IsHmdPresent())
  //{
  //  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LWARNING,
  //                   "HMD not detected. VR features may be limited or unavailable.");
  //  // Depending on desired behavior, we might still want to run without an HMD for testing.
  //  // For now, we'll consider it a non-fatal issue but log a warning.
  //}
  //else
  //{
  //  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "HMD detected.");
  //}

  // TODO: Get recommended render target size here if needed immediately.
  // uint32_t render_width, render_height;
  // m_ivr_system->GetRecommendedRenderTargetSize(&render_width, &render_height);
  // LOG_INFO(DS_VR, "Recommended render target size: %u x %u", render_width, render_height);

  m_initialized = true;
  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO,
                     "VROpenVR initialized successfully.");
  return true;
}

void VROpenVR::Shutdown()
{
  if (!m_initialized)
  {
    GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO,
                     "VROpenVR not initialized, skipping shutdown.");
    return;
  }

  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "Shutting down VROpenVR...");
  vr::VR_Shutdown();
  m_ivr_system = nullptr;
  m_ivr_compositor = nullptr;
  m_initialized = false;
  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO,
                     "VROpenVR shut down.");
}

namespace
{
// Helper function to convert OpenVR HmdMatrix34_t to Common::Matrix44
Common::Matrix44 ConvertHmdMatrix34ToMatrix44(const vr::HmdMatrix34_t& mat34)
{
  return Common::Matrix44::FromArray({{
    mat34.m[0][0], mat34.m[0][1], mat34.m[0][2], mat34.m[0][3],
    mat34.m[1][0], mat34.m[1][1], mat34.m[1][2], mat34.m[1][3],
    mat34.m[2][0], mat34.m[2][1], mat34.m[2][2], mat34.m[2][3],
    0.0f,          0.0f,          0.0f,          1.0f
  }});
}

// Helper function to convert OpenVR HmdMatrix44_t to Common::Matrix44
Common::Matrix44 ConvertHmdMatrix44ToMatrix44(const vr::HmdMatrix44_t& mat44_openvr)
{
  // Common::Matrix44 data is std::array<float, 16> which can be initialized directly
  // from the float m[4][4] if we copy element by element in row-major order.
  std::array<float, 16> arr;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      arr[i * 4 + j] = mat44_openvr.m[i][j];
    }
  }
  return Common::Matrix44::FromArray(arr);
}
} // anonymous namespace

bool VROpenVR::GetHMDPose(float predicted_seconds_to_photon, Common::Matrix44& out_pose)
{
  if (!m_initialized || !m_ivr_system)
  {
    ERROR_LOG_FMT(VR, "VROpenVR not initialized or IVRSystem not available for GetHMDPose.");
    out_pose = Common::Matrix44::Identity();
    return false;
  }

  vr::TrackedDevicePose_t tracked_device_pose[vr::k_unMaxTrackedDeviceCount];
  m_ivr_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding,
                                               predicted_seconds_to_photon,
                                               tracked_device_pose,
                                               vr::k_unMaxTrackedDeviceCount);

  if (tracked_device_pose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
  {
    out_pose = ConvertHmdMatrix34ToMatrix44(
        tracked_device_pose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);
    return true;
  }
  else
  {
    WARN_LOG_FMT(VR, "HMD pose is not valid.");
    out_pose = Common::Matrix44::Identity(); // Return identity if pose is not valid
    return false;
  }
}

bool VROpenVR::GetEyeProjectionMatrix(vr::EVREye eye, float near_clip, float far_clip, Common::Matrix44& out_projection)
{
  if (!m_initialized || !m_ivr_system)
  {
    ERROR_LOG_FMT(VR, "VROpenVR not initialized or IVRSystem not available for GetEyeProjectionMatrix.");
    out_projection = Common::Matrix44::Identity();
    return false;
  }

  vr::HmdMatrix44_t mat = m_ivr_system->GetProjectionMatrix(eye, near_clip, far_clip);
  out_projection = ConvertHmdMatrix44ToMatrix44(mat);
  return true;
}

void VROpenVR::GetHMDRecommendedRenderTargetSize(uint32_t* width, uint32_t* height)
{
  if (!m_initialized || !m_ivr_system)
  {
    ERROR_LOG_FMT(VR, "VROpenVR not initialized or IVRSystem not available for GetHMDRecommendedRenderTargetSize.");
    *width = 0;
    *height = 0;
    return;
  }
  m_ivr_system->GetRecommendedRenderTargetSize(width, height);
}

Common::Matrix44 VROpenVR::GetRawEyeToHeadTransform(vr::EVREye eye)
{
  if (!m_initialized || !m_ivr_system)
  {
    ERROR_LOG_FMT(VR, "VROpenVR not initialized or IVRSystem not available for GetRawEyeToHeadTransform.");
    return Common::Matrix44::Identity();
  }
  vr::HmdMatrix34_t mat34 = m_ivr_system->GetEyeToHeadTransform(eye);
  return ConvertHmdMatrix34ToMatrix44(mat34);
}

long long VROpenVR::GetAdapterLUID()
{
  if (!m_initialized || !m_ivr_system)
  {
    ERROR_LOG_FMT(VR, "VROpenVR not initialized or IVRSystem not available for GetAdapterLUID.");
    return 0; // Return 0 to indicate failure or default
  }

  uint64_t adapter_luid = 0;
  // The third parameter pInstance for GetOutputDevice is for Vulkan. For DX, it should be nullptr.
  // If using DX11, texture type is TextureType_DirectX.
  // If using DX12, texture type is TextureType_DirectX12.
  // We are in D3DBase (DX11 context for now), so TextureType_DirectX.
  m_ivr_system->GetOutputDevice(&adapter_luid, vr::TextureType_DirectX, nullptr);

  if (adapter_luid == 0)
  {
    WARN_LOG_FMT(VR, "OpenVR did not provide a valid adapter LUID for DirectX.");
  }
  else
  {
    GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "OpenVR recommended adapter LUID: {}", adapter_luid);
  }
  return adapter_luid;
}

void VROpenVR::UpdateControllerIndices()
{
  if (!m_initialized || !m_ivr_system)
  {
    return;
  }

  // Reset indices
  m_left_controller_index = vr::k_unTrackedDeviceIndexInvalid;
  m_right_controller_index = vr::k_unTrackedDeviceIndexInvalid;

  // Iterate through all tracked devices
  for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
  {
    if (!m_ivr_system->IsTrackedDeviceConnected(i))
      continue;

    vr::ETrackedDeviceClass device_class = m_ivr_system->GetTrackedDeviceClass(i);
    if (device_class == vr::TrackedDeviceClass_Controller)
    {
      // Check which controller it is
      vr::VRControllerState_t controller_state;
      if (m_ivr_system->GetControllerState(i, &controller_state, sizeof(controller_state)))
      {
        if (controller_state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad))
        {
          if (m_left_controller_index == vr::k_unTrackedDeviceIndexInvalid)
          {
            m_left_controller_index = i; // Assign left controller
          }
          else
          {
            m_right_controller_index = i; // Assign right controller
          }
        }
      }
    }
  }

  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO,
                   "Updated controller indices: Left: {}, Right: {}",
                   m_left_controller_index, m_right_controller_index);
}

void VROpenVR::PollEventsAndUpdateControllers()
{
  if (!m_initialized || !m_ivr_system)
  {
    return;
  }

  PollEvents(); // Process OpenVR events

  // Update controller indices
  UpdateControllerIndices();

}

void VROpenVR::PollEvents()
{
  if (!m_initialized || !m_ivr_system)
  {
    return;
  }

  vr::VREvent_t event;
  while (m_ivr_system->PollNextEvent(&event, sizeof(event)))
  {
    // Log all events for now. Can be filtered later.
    // Using GENERIC_LOG_FMT for consistency, assuming VR is a valid LogType.
    GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LDEBUG, 
                     "OpenVR Event: {} ({})", 
                     m_ivr_system->GetEventTypeNameFromEnum(static_cast<vr::EVREventType>(event.eventType)),
                     static_cast<int>(event.eventType));

    switch (event.eventType)
    {
    case vr::VREvent_Quit: // User has quit from SteamVR dashboard or similar
    case vr::VREvent_ProcessQuit: // Another process has requested this app to quit
      // TODO: These events should ideally trigger a graceful shutdown of VR mode in Dolphin.
      // For now, just logging. Could post a host message or set a flag.
      GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LWARNING,
                       "Received OpenVR Quit Event type: {}. VR should be shut down.", event.eventType);
      // Example: Core::System::GetInstance().GetHost()->SetWantToStop(true); // This is hypothetical
      break;
    case vr::VREvent_InputFocusCaptured: // Scene app has gained input focus
      GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, 
                       "OpenVR Event: Input Focus Captured by process {}.", event.data.process.pid);
      break;
    case vr::VREvent_InputFocusReleased: // Scene app has lost input focus
      GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LWARNING, 
                       "OpenVR Event: Input Focus Released by process {}.", event.data.process.pid);
      // When focus is lost, a scene app should typically stop rendering or pause.
      break;
    case vr::VREvent_ChaperoneDataHasChanged:
      GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "OpenVR Event: Chaperone data has changed.");
      // TODO: Reload chaperone data if using it for anything.
      break;
    /*case vr::VREvent_Compositor_DeviceDisconnected:
       GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LWARNING, "OpenVR Event: Compositor device disconnected.");
       break;
    case vr::VREvent_Compositor_RequestDisconnect:
       GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LWARNING, "OpenVR Event: Compositor requested disconnect.");
       break;*/
    // Add more cases as needed
    default:
      break;
    }
  }
}
