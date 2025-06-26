#include "VROpenVR.h"
#include "Common/Logging/Log.h" // For logging
#include <d3d11.h> // For ID3D11Texture2D, though already in .h, good for cpp too.

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
    GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "VROpenVR already initialized, skipping re-initialization.");
    return true;
  }

  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO, "VROpenVR initialization started.");

  vr::EVRInitError eError = vr::VRInitError_None;
  m_ivr_system = vr::VR_Init(&eError, vr::VRApplication_Scene); // Using VRApplication_Scene for now

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

bool VROpenVR::GetEyeToHeadTransform(vr::EVREye eye, Common::Matrix44& out_matrix)
{
  if (!m_initialized || !m_ivr_system)
  {
    ERROR_LOG_FMT(VR, "VROpenVR not initialized or IVRSystem not available for GetEyeToHeadTransform.");
    out_matrix = Common::Matrix44::Identity();
    return false;
  }

  vr::HmdMatrix34_t mat = m_ivr_system->GetEyeToHeadTransform(eye);
  out_matrix = ConvertHmdMatrix34ToMatrix44(mat);
  return true;
}

bool VROpenVR::SubmitFrames(ID3D11Texture2D* left_eye_texture, ID3D11Texture2D* right_eye_texture)
{
  if (!m_initialized || !m_ivr_compositor)
  {
    ERROR_LOG_FMT(VR, "VROpenVR not initialized or IVRCompositor not available for SubmitFrames.");
    return false;
  }

  if (!left_eye_texture || !right_eye_texture)
  {
    ERROR_LOG_FMT(VR, "Null texture provided to SubmitFrames.");
    return false;
  }

  vr::Texture_t left_eye_vr_texture;
  left_eye_vr_texture.handle = (void*)left_eye_texture;
  left_eye_vr_texture.eType = vr::TextureType_DirectX; // For D3D11
  left_eye_vr_texture.eColorSpace = vr::ColorSpace_Auto; // Or ColorSpace_Gamma

  vr::EVRCompositorError error_left = m_ivr_compositor->Submit(vr::Eye_Left, &left_eye_vr_texture);
  if (error_left != vr::VRCompositorError_None)
  {
    ERROR_LOG_FMT(VR, "Failed to submit left eye texture: {}", "");//m_ivr_compositor->GetCompositorErrorNameFromEnum(error_left));
    // Continue to submit right eye even if left fails, for more complete debugging info.
  }

  vr::Texture_t right_eye_vr_texture;
  right_eye_vr_texture.handle = (void*)right_eye_texture;
  right_eye_vr_texture.eType = vr::TextureType_DirectX; // For D3D11
  right_eye_vr_texture.eColorSpace = vr::ColorSpace_Auto; // Or ColorSpace_Gamma

  vr::EVRCompositorError error_right = m_ivr_compositor->Submit(vr::Eye_Right, &right_eye_vr_texture);
  if (error_right != vr::VRCompositorError_None)
  {
    ERROR_LOG_FMT(VR, "Failed to submit right eye texture: {}", "");//m_ivr_compositor->GetCompositorErrorNameFromEnum(error_right));
  }

  // Consider the call successful if both submits are okay.
  return error_left == vr::VRCompositorError_None && error_right == vr::VRCompositorError_None;
}

bool VROpenVR::GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height)
{
  if (!m_initialized || !m_ivr_system)
  {
    ERROR_LOG_FMT(VR, "VROpenVR not initialized or IVRSystem not available for GetRecommendedRenderTargetSize.");
    if (width) *width = 0;
    if (height) *height = 0;
    return false;
  }

  m_ivr_system->GetRecommendedRenderTargetSize(width, height);
  GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO,
                   "Recommended render target size per eye: {} x {}", *width, *height);
  return true;
}

bool VROpenVR::IsInitialized() const
{
  return m_initialized;
}
