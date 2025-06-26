#pragma once

#include <memory>
#include "Common/Matrix.h" // For Common::Matrix44
#include <openvr.h>
#include <d3d11.h> // For ID3D11Texture2D

// Forward declare OpenVR types to avoid including openvr.h in this header if possible,
// though for IVRSystem and IVRCompositor we might need the full definition.
// For now, let's assume openvr.h will be included in VROpenVR.cpp.
//namespace vr
//{
//  class IVRSystem;
//  class IVRCompositor;
//  enum EVREye;
//} // namespace vr

class VROpenVR
{
public:
  VROpenVR();
  ~VROpenVR();

  bool Init();
  void Shutdown();

  // Gets the HMD pose in tracking space.
  // predicted_seconds_to_photon: Time from now when the pose will be displayed.
  // out_pose: The Common::Matrix44 to store the HMD's pose.
  // Returns true if the pose was successfully retrieved.
  bool GetHMDPose(float predicted_seconds_to_photon, Common::Matrix44& out_pose);

  // Gets the projection matrix for a given eye.
  // eye: Which eye to get the projection matrix for (vr::Eye_Left or vr::Eye_Right).
  // near_clip: The near clipping plane distance.
  // far_clip: The far clipping plane distance.
  // out_projection: The Common::Matrix44 to store the projection matrix.
  // Returns true if the matrix was successfully retrieved.
  bool GetEyeProjectionMatrix(vr::EVREye eye, float near_clip, float far_clip, Common::Matrix44& out_projection);

  // Gets the transformation from eye space to head space for a given eye.
  // This matrix is typically a translation based on IPD.
  // out_matrix: The Common::Matrix44 to store the eye-to-head transform.
  // Returns true if the matrix was successfully retrieved.
  bool GetEyeToHeadTransform(vr::EVREye eye, Common::Matrix44& out_matrix);

  // Submits the rendered frames for both eyes to the HMD.
  // left_eye_texture: Pointer to the D3D11 texture resource for the left eye.
  // right_eye_texture: Pointer to the D3D11 texture resource for the right eye.
  // Returns true if submission was successful for both eyes.
  bool SubmitFrames(ID3D11Texture2D* left_eye_texture, ID3D11Texture2D* right_eye_texture);

  // Gets the recommended render target size for each eye from the VR system.
  // width: Pointer to store the recommended width.
  // height: Pointer to store the recommended height.
  // Returns true if the size was successfully retrieved.
  bool GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height);

  bool IsInitialized() const;

  // Control submission based on VR focus state (to be updated by event loop)
  void SetHaveCompositorFocus(bool has_focus);
  bool CanSubmitFrames() const;

private:
  vr::IVRSystem* m_ivr_system;
  vr::IVRCompositor* m_ivr_compositor;
  bool m_initialized;
  bool m_has_compositor_focus; // True if we believe we have compositor focus

  // TODO: Add any other necessary private members, e.g., for device indices.
};
