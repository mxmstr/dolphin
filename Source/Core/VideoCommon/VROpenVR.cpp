#include "VROpenVR.h"
#include "Common/Logging/Log.h" // For logging
#include "Externals/OpenVR/include/openvr.h" // Actual OpenVR header

VROpenVR::VROpenVR() : m_ivr_system(nullptr), m_ivr_compositor(nullptr), m_initialized(false)
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

bool VROpenVR::Init()
{
  if (m_initialized)
  {
    LOG_INFO(DS_VR, "VROpenVR already initialized.");
    return true;
  }

  LOG_INFO(DS_VR, "Initializing VROpenVR...");

  vr::EVRInitError eError = vr::VRInitError_None;
  m_ivr_system = vr::VR_Init(&eError, vr::VRApplication_Scene); // Using VRApplication_Scene for now

  if (eError != vr::VRInitError_None)
  {
    m_ivr_system = nullptr;
    LOG_ERROR(DS_VR, "Unable to init VR runtime: %s", vr::VR_GetVRInitErrorAsEnglishDescription(eError));
    return false;
  }

  LOG_INFO(DS_VR, "VR_Init successful.");

  if (!vr::VRCompositor())
  {
    LOG_ERROR(DS_VR, "Failed to initialize VR Compositor.");
    vr::VR_Shutdown();
    m_ivr_system = nullptr;
    return false;
  }
  m_ivr_compositor = vr::VRCompositor();
  LOG_INFO(DS_VR, "VRCompositor successful.");

  if (!m_ivr_system->IsHmdPresent())
  {
    LOG_WARNING(DS_VR, "HMD not detected. VR features may be limited or unavailable.");
    // Depending on desired behavior, we might still want to run without an HMD for testing.
    // For now, we'll consider it a non-fatal issue but log a warning.
  }
  else
  {
    LOG_INFO(DS_VR, "HMD detected.");
  }

  // TODO: Get recommended render target size here if needed immediately.
  // uint32_t render_width, render_height;
  // m_ivr_system->GetRecommendedRenderTargetSize(&render_width, &render_height);
  // LOG_INFO(DS_VR, "Recommended render target size: %u x %u", render_width, render_height);

  m_initialized = true;
  LOG_INFO(DS_VR, "VROpenVR initialized successfully.");
  return true;
}

void VROpenVR::Shutdown()
{
  if (!m_initialized)
  {
    LOG_INFO(DS_VR, "VROpenVR not initialized, nothing to shut down.");
    return;
  }

  LOG_INFO(DS_VR, "Shutting down VROpenVR...");
  vr::VR_Shutdown();
  m_ivr_system = nullptr;
  m_ivr_compositor = nullptr;
  m_initialized = false;
  LOG_INFO(DS_VR, "VROpenVR shut down.");
}

namespace
{
// Helper function to convert OpenVR HmdMatrix34_t to Common::Matrix4x4f
Common::Matrix4x4f ConvertHmdMatrix34ToMatrix4x4f(const vr::HmdMatrix34_t& mat34)
{
  Common::Matrix4x4f mat44;
  mat44.mData[0][0] = mat34.m[0][0];
  mat44.mData[0][1] = mat34.m[0][1];
  mat44.mData[0][2] = mat34.m[0][2];
  mat44.mData[0][3] = mat34.m[0][3];
  mat44.mData[1][0] = mat34.m[1][0];
  mat44.mData[1][1] = mat34.m[1][1];
  mat44.mData[1][2] = mat34.m[1][2];
  mat44.mData[1][3] = mat34.m[1][3];
  mat44.mData[2][0] = mat34.m[2][0];
  mat44.mData[2][1] = mat34.m[2][1];
  mat44.mData[2][2] = mat34.m[2][2];
  mat44.mData[2][3] = mat34.m[2][3];
  mat44.mData[3][0] = 0.0f;
  mat44.mData[3][1] = 0.0f;
  mat44.mData[3][2] = 0.0f;
  mat44.mData[3][3] = 1.0f;
  return mat44;
}

// Helper function to convert OpenVR HmdMatrix44_t to Common::Matrix4x4f
Common::Matrix4x4f ConvertHmdMatrix44ToMatrix4x4f(const vr::HmdMatrix44_t& mat44_openvr)
{
  Common::Matrix4x4f mat44_dolphin;
  for (int i = 0; i < 4; ++i)
  {
    for (int j = 0; j < 4; ++j)
    {
      mat44_dolphin.mData[i][j] = mat44_openvr.m[i][j];
    }
  }
  return mat44_dolphin;
}
} // anonymous namespace

bool VROpenVR::GetHMDPose(float predicted_seconds_to_photon, Common::Matrix4x4f& out_pose)
{
  if (!m_initialized || !m_ivr_system)
  {
    LOG_ERROR(DS_VR, "VROpenVR not initialized or IVRSystem not available for GetHMDPose.");
    out_pose.Identity();
    return false;
  }

  vr::TrackedDevicePose_t tracked_device_pose[vr::k_unMaxTrackedDeviceCount];
  m_ivr_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding,
                                               predicted_seconds_to_photon,
                                               tracked_device_pose,
                                               vr::k_unMaxTrackedDeviceCount);

  if (tracked_device_pose[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
  {
    out_pose = ConvertHmdMatrix34ToMatrix4x4f(
        tracked_device_pose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);
    return true;
  }
  else
  {
    LOG_WARNING(DS_VR, "HMD pose is not valid.");
    out_pose.Identity(); // Return identity if pose is not valid
    return false;
  }
}

bool VROpenVR::GetEyeProjectionMatrix(vr::EVREye eye, float near_clip, float far_clip, Common::Matrix4x4f& out_projection)
{
  if (!m_initialized || !m_ivr_system)
  {
    LOG_ERROR(DS_VR, "VROpenVR not initialized or IVRSystem not available for GetEyeProjectionMatrix.");
    out_projection.Identity();
    return false;
  }

  vr::HmdMatrix44_t mat = m_ivr_system->GetProjectionMatrix(eye, near_clip, far_clip);
  out_projection = ConvertHmdMatrix44ToMatrix4x4f(mat);
  return true;
}
