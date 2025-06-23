#pragma once

#include <memory>
#include "Common/Matrix.h" // For Matrix4x4f

// Forward declare OpenVR types to avoid including openvr.h in this header if possible,
// though for IVRSystem and IVRCompositor we might need the full definition.
// For now, let's assume openvr.h will be included in VROpenVR.cpp.
namespace vr
{
  class IVRSystem;
  class IVRCompositor;
  enum EVREye;
} // namespace vr

class VROpenVR
{
public:
  VROpenVR();
  ~VROpenVR();

  bool Init();
  void Shutdown();

  // Gets the HMD pose in tracking space.
  // predicted_seconds_to_photon: Time from now when the pose will be displayed.
  // out_pose: The Matrix4x4f to store the HMD's pose.
  // Returns true if the pose was successfully retrieved.
  bool GetHMDPose(float predicted_seconds_to_photon, Common::Matrix4x4f& out_pose);

  // Gets the projection matrix for a given eye.
  // eye: Which eye to get the projection matrix for (vr::Eye_Left or vr::Eye_Right).
  // near_clip: The near clipping plane distance.
  // far_clip: The far clipping plane distance.
  // out_projection: The Matrix4x4f to store the projection matrix.
  // Returns true if the matrix was successfully retrieved.
  bool GetEyeProjectionMatrix(vr::EVREye eye, float near_clip, float far_clip, Common::Matrix4x4f& out_projection);

  // TODO: Add methods for submitting frames to the compositor later.
  // TODO: Add methods for getting recommended render target size later.

private:
  vr::IVRSystem* m_ivr_system;
  vr::IVRCompositor* m_ivr_compositor;
  bool m_initialized;

  // TODO: Add any other necessary private members, e.g., for device indices.
};
