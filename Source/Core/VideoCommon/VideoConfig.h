// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// IMPORTANT: UI etc should modify g_Config. Graphics code should read g_ActiveConfig.
// The reason for this is to get rid of race conditions etc when the configuration
// changes in the middle of a frame. This is done by copying g_Config to g_ActiveConfig
// at the start of every frame. Noone should ever change members of g_ActiveConfig
// directly.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h" // Added for Matrix33
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/VideoCommon.h"

constexpr int EFB_SCALE_AUTO_INTEGRAL = 0;

enum class AspectMode : int
{
  Auto,           // ~4:3 or ~16:9 (auto detected)
  ForceWide,      // ~16:9
  ForceStandard,  // ~4:3
  Stretch,
  Custom,         // Forced relative custom AR
  CustomStretch,  // Forced absolute custom AR
  Raw,            // Forced squared pixels
};

enum class StereoMode : int
{
  Off,          // Stereo disabled
  SBS,          // Side-by-Side
  TAB,          // Top-and-Bottom
  Anaglyph,     // Red-Cyan anaglyph
  QuadBuffer,   // Hardware quad-buffered stereo (e.g., professional GPUs)
  OpenVR,       // Used for OpenVR SDK (Vive, Index, many WMR, Oculus via OpenVR)
  OculusVR,     // Used for native Oculus SDK (distinct from OpenVR access to Oculus) - Keeping this from current if it exists
  OSVR,         // Open Source Virtual Reality - if we want to support its specific distortion
  Stereo3DVision, // NVIDIA 3D Vision (quad buffer via DirectX)
  VR920         // Vuzix VR920 HMD (old)
};

enum class ShaderCompilationMode : int
{
  Synchronous,
  SynchronousUberShaders,
  AsynchronousUberShaders,
  AsynchronousSkipRendering
};

enum class TextureFilteringMode : int
{
  Default,
  Nearest,
  Linear,
};

enum class AnisotropicFilteringMode : int
{
  Default = -1,
  Force1x = 0,
  Force2x = 1,
  Force4x = 2,
  Force8x = 3,
  Force16x = 4,
};

enum class OutputResamplingMode : int
{
  Default,
  Bilinear,
  BSpline,
  MitchellNetravali,
  CatmullRom,
  SharpBilinear,
  AreaSampling,
};

enum class ColorCorrectionRegion : int
{
  SMPTE_NTSCM,
  SYSTEMJ_NTSCJ,
  EBU_PAL,
};

enum class TriState : int
{
  Off,
  On,
  Auto
};

enum class FrameDumpResolutionType : int
{
  // Window resolution (not including potential back buffer black borders)
  WindowResolution,
  // The aspect ratio corrected XFB resolution (XFB pixels might not have been square)
  XFBAspectRatioCorrectedResolution,
  // The raw unscaled XFB resolution (based on "internal resolution" scale)
  XFBRawResolution,
};

enum class VertexLoaderType : int
{
  Native,
  Software,
  Compare
};

// Bitmask containing information about which configuration has changed for the backend.
enum ConfigChangeBits : u32
{
  CONFIG_CHANGE_BIT_HOST_CONFIG = (1 << 0),
  CONFIG_CHANGE_BIT_MULTISAMPLES = (1 << 1),
  CONFIG_CHANGE_BIT_STEREO_MODE = (1 << 2),
  CONFIG_CHANGE_BIT_TARGET_SIZE = (1 << 3),
  CONFIG_CHANGE_BIT_ANISOTROPY = (1 << 4),
  CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING = (1 << 5),
  CONFIG_CHANGE_BIT_VSYNC = (1 << 6),
  CONFIG_CHANGE_BIT_BBOX = (1 << 7),
  CONFIG_CHANGE_BIT_ASPECT_RATIO = (1 << 8),
  CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER = (1 << 9),
  CONFIG_CHANGE_BIT_HDR = (1 << 10),
};

// Static config per API
struct BackendInfo
{
  APIType api_type = APIType::Nothing;
  std::string DisplayName;

  std::vector<std::string> Adapters;  // for D3D
  std::vector<u32> AAModes;

  // TODO: merge AdapterName and Adapters array
  std::string AdapterName;  // for OpenGL

  u32 MaxTextureSize = 16384;
  bool bUsesLowerLeftOrigin = false;
  bool bUsesExplictQuadBuffering = false;
  bool bSupportsExclusiveFullscreen = false;  // Note: Vulkan can change this at runtime.
  bool bSupportsDualSourceBlend = false;
  bool bSupportsPrimitiveRestart = false;
  bool bSupportsGeometryShaders = false;
  bool bSupportsComputeShaders = false;
  bool bSupports3DVision = false;
  bool bSupportsEarlyZ = false;         // needed by PixelShaderGen, so must stay in VideoCommon
  bool bSupportsBindingLayout = false;  // Needed by ShaderGen, so must stay in VideoCommon
  bool bSupportsBBox = false;
  bool bSupportsGSInstancing = false;  // Needed by GeometryShaderGen, so must stay in VideoCommon
  bool bSupportsPostProcessing = false;
  bool bSupportsPaletteConversion = false;
  bool bSupportsClipControl = false;  // Needed by VertexShaderGen, so must stay in VideoCommon
  bool bSupportsSSAA = false;
  bool bSupportsFragmentStoresAndAtomics = false;  // a.k.a. OpenGL SSBOs a.k.a. Direct3D UAVs
  bool bSupportsDepthClamp = false;  // Needed by VertexShaderGen, so must stay in VideoCommon
  bool bSupportsReversedDepthRange = false;
  bool bSupportsLogicOp = false;
  bool bSupportsMultithreading = false;
  bool bSupportsGPUTextureDecoding = false;
  bool bSupportsST3CTextures = false;
  bool bSupportsCopyToVram = false;
  bool bSupportsBitfield = false;  // Needed by UberShaders, so must stay in VideoCommon
  // Needed by UberShaders, so must stay in VideoCommon
  bool bSupportsDynamicSamplerIndexing = false;
  bool bSupportsBPTCTextures = false;
  bool bSupportsFramebufferFetch = false;  // Used as an alternative to dual-source blend on GLES
  bool bSupportsBackgroundCompiling = false;
  bool bSupportsLargePoints = false;
  bool bSupportsPartialDepthCopies = false;
  bool bSupportsDepthReadback = false;
  bool bSupportsShaderBinaries = false;
  bool bSupportsPipelineCacheData = false;
  bool bSupportsCoarseDerivatives = false;
  bool bSupportsTextureQueryLevels = false;
  bool bSupportsLodBiasInSampler = false;
  bool bSupportsSettingObjectNames = false;
  bool bSupportsPartialMultisampleResolve = false;
  bool bSupportsDynamicVertexLoader = false;
  bool bSupportsVSLinePointExpand = false;
  bool bSupportsGLLayerInFS = true;
  bool bSupportsHDROutput = false;
};

extern BackendInfo g_backend_info;

// NEVER inherit from this class.
enum class TGameCamera : int
{
  CAMERA_YAWPITCHROLL = 0,
  CAMERA_YAWPITCH,
  CAMERA_YAW,
  CAMERA_NONE
};

struct ProjectionHackConfig final
{
  bool m_enable;
  bool m_sznear;
  bool m_szfar;
  std::string m_znear;
  std::string m_zfar;
};

// NEVER inherit from this class.
struct VideoConfig final
{
  VideoConfig(); // Modified from default
  void Refresh();
  void VerifyValidity();
  static void Shutdown();
  void GameIniSave(); // Added from Hydra
  void GameIniReset(); // Added from Hydra
  void UpdateProjectionHack(); // Added from Hydra
  bool IsVSync() const; // Added from Hydra
  bool VRSettingsModified(); // Added from Hydra


  // General
  bool bVSync = false;
  bool bVSyncActive = false;
  bool bWidescreenHack = false;
  AspectMode aspect_mode{};
  int custom_aspect_width = 1;
  int custom_aspect_height = 1;
  AspectMode suggested_aspect_mode{};
  u32 widescreen_heuristic_transition_threshold = 0;
  float widescreen_heuristic_aspect_ratio_slop = 0.f;
  float widescreen_heuristic_standard_ratio = 0.f;
  float widescreen_heuristic_widescreen_ratio = 0.f;
  bool bCrop = false;  // Aspect ratio controls.
  bool bShaderCache = false;
  // From Hydra:
  bool bUseXFB; // Already present in some form, ensure it's compatible
  bool bUseRealXFB; // Already present in some form, ensure it's compatible

  // Enhancements
  u32 iMultisamples = 0;
  bool bSSAA = false;
  int iEFBScale = 0;
  int iInternalResolution; // Added from Hydra
  TextureFilteringMode texture_filtering_mode = TextureFilteringMode::Default;
  OutputResamplingMode output_resampling_mode = OutputResamplingMode::Default;
  AnisotropicFilteringMode iMaxAnisotropy = AnisotropicFilteringMode::Default; // bForceFiltering in Hydra, maps to texture_filtering_mode != Default
  std::string sPostProcessingShader;
  bool bForceTrueColor = false;
  bool bDisableCopyFilter = false; // Not in Hydra
  bool bArbitraryMipmapDetection = false; // Not in Hydra
  float fArbitraryMipmapDetectionThreshold = 0; // Not in Hydra
  bool bHDR = false; // Not in Hydra

  // Color Correction
  struct
  {
    // Color Space Correction:
    bool bCorrectColorSpace = false;
    ColorCorrectionRegion game_color_space = ColorCorrectionRegion::SMPTE_NTSCM;

    // Gamma Correction:
    bool bCorrectGamma = false;
    float fGameGamma = 2.35f;
    bool bSDRDisplayGammaSRGB = true;
    // Custom gamma when the display is not sRGB
    float fSDRDisplayCustomGamma = 2.2f;

    // HDR:
    // 203 is a good default value that matches the brightness of many SDR screens.
    // It's also the value recommended by the ITU.
    float fHDRPaperWhiteNits = 203.f;
  } color_correction;

  // Information
  bool bShowFPS = false;
  bool bShowFTimes = false;
  bool bShowVPS = false;
  bool bShowVTimes = false;
  bool bShowGraphs = false;
  bool bShowSpeed = false;
  bool bShowSpeedColors = false;
  int iPerfSampleUSec = 0;
  bool bOverlayStats = false;
  bool bOverlayProjStats = false;
  bool bOverlayScissorStats = false; // Not in Hydra
  bool bTexFmtOverlayEnable = false;
  bool bTexFmtOverlayCenter = false;
  bool bLogRenderTimeToFile = false;
  // From Hydra:
  bool bShowNetPlayPing;
  bool bShowNetPlayMessages;


  // Render
  bool bWireFrame = false;
  bool bDisableFog = false;

  // Utility
  bool bDumpTextures = false;
  bool bDumpMipmapTextures = false;
  bool bDumpBaseTextures = false;
  bool bHiresTextures = false;
  bool bCacheHiresTextures = false;
  bool bDumpEFBTarget = false;
  bool bDumpXFBTarget = false;
  bool bBorderlessFullscreen = false;
  bool bEnableGPUTextureDecoding = false;
  bool bPreferVSForLinePointExpansion = false; // Not in Hydra
  bool bGraphicMods = false; // Not in Hydra
  std::optional<GraphicsModGroupConfig> graphics_mod_config; // Not in Hydra
  // From Hydra:
  bool bConvertHiresTextures;
  bool bDumpFramesAsImages;
  bool bUseFFV1;
  std::string sDumpCodec;
  std::string sDumpFormat;
  std::string sDumpPath;
  bool bInternalResolutionFrameDumps;
  bool bFreeLook;
  int iBitrateKbps;


  // Hacks
  bool bEFBAccessEnable = false;
  bool bEFBAccessDeferInvalidation = false; // Not in Hydra
  bool bPerfQueriesEnable = false;
  bool bBBoxEnable = false;
  bool bBBoxPreferStencilImplementation; // Added from Hydra (OpenGL-only)
  bool bCPUCull = false; // Not in Hydra, keep
  bool bForceProgressive; // Added from Hydra

  bool bEFBEmulateFormatChanges = false;
  bool bEFBCopyEnable; // Added from Hydra
  bool bEFBCopyClearDisable; // Added from Hydra
  bool bSkipEFBCopyToRam = false;
  bool bSkipXFBCopyToRam = false; // Not in Hydra, keep
  bool bDisableCopyToVRAM = false; // Not in Hydra, keep
  bool bDeferEFBCopies = false; // Not in Hydra, keep
  bool bImmediateXFB = false; // Not in Hydra, keep
  bool bSkipPresentingDuplicateXFBs = false; // bVISkip in current, keep logic from current
  bool bCopyEFBScaled = false;
  int iSafeTextureCache_ColorSamples = 0;
  ProjectionHackConfig phack; // Added from Hydra
  float fAspectRatioHackW = 1;  // Initial value needed for the first frame
  float fAspectRatioHackH = 1;
  bool bEnablePixelLighting = false;
  bool bFastDepthCalc = false;
  bool bVertexRounding = false;
  bool bVISkip = false; // Keep, part of bSkipPresentingDuplicateXFBs logic
  int iEFBAccessTileSize = 0; // Not in Hydra, keep
  int iLog;           // CONF_ bits, Added from Hydra
  int iSaveTargetId = 0;  // TODO: Should be dropped
  u32 iMissingColorValue = 0; // Not in Hydra, keep
  bool bFastTextureSampling = false; // Not in Hydra, keep
#ifdef __APPLE__
  bool bNoMipmapping = false;  // Used by macOS fifoci to work around an M1 bug
#endif

  // Stereoscopy
  StereoMode stereo_mode{}; // iStereoMode in Hydra
  bool stereo_per_eye_resolution_full = false; // Not in Hydra
  int iStereoDepth = 0;
  int iStereoConvergence = 0;
  int iStereoConvergencePercentage = 0;
  bool bStereoSwapEyes = false;
  bool bStereoEFBMonoDepth = false;
  int iStereoDepthPercentage = 0;

  // VR global (Added from Hydra)
  float fScale;
  float fLeanBackAngle;
  bool bStabilizeRoll;
  bool bStabilizePitch;
  bool bStabilizeYaw;
  bool bStabilizeX;
  bool bStabilizeY;
  bool bStabilizeZ;
  bool bKeyhole;
  float fKeyholeWidth;
  bool bKeyholeSnap;
  float fKeyholeSnapSize;
  bool bPullUp20fps;
  bool bPullUp30fps;
  bool bPullUp60fps;
  bool bPullUpAuto;
  bool bSynchronousTimewarp;
  bool bOpcodeWarningDisable;
  bool bReplayVertexData;
  bool bReplayOtherData;
  bool bPullUp20fpsTimewarp;
  bool bPullUp30fpsTimewarp;
  bool bPullUp60fpsTimewarp;
  bool bPullUpAutoTimewarp;
  bool bOpcodeReplay;
  bool bAsynchronousTimewarp; // In Hydra but seems OpenVR specific, might need adjustment
  bool bEnableVR; // Specific to Oculus in Hydra, make generic for OpenVR
  bool bLowPersistence;
  bool bDynamicPrediction;
  bool bOrientationTracking;
  bool bMagYawCorrection;
  bool bPositionTracking;
  bool bChromatic;
  bool bTimewarp;
  bool bVignette;
  bool bNoRestore;
  bool bFlipVertical;
  bool bSRGB;
  bool bOverdrive;
  bool bHqDistortion;
  bool bDisableNearClipping;
  bool bAutoPairViveControllers; // Vive specific, consider if needed for generic OpenVR
  bool bShowHands;
  bool bShowFeet;
  bool bShowController;
  bool bShowLaserPointer;
  bool bShowAimRectangle;
  bool bShowHudBox;
  bool bShow2DBox;
  bool bShowSensorBar;
  bool bShowGameCamera;
  bool bShowGameFrustum;
  bool bShowTrackingCamera;
  bool bShowTrackingVolume;
  bool bShowBaseStation;
  bool bMotionSicknessAlways;
  bool bMotionSicknessFreelook;
  bool bMotionSickness2D;
  bool bMotionSicknessLeftStick;
  bool bMotionSicknessRightStick;
  bool bMotionSicknessDPad;
  bool bMotionSicknessIR;
  int iMotionSicknessMethod;
  int iMotionSicknessSkybox;
  float fMotionSicknessFOV;

  int iVRPlayer, iVRPlayer2, iMirrorPlayer;
  int iMirrorStyle;
  float fTimeWarpTweak;
  u32 iExtraTimewarpedFrames;
  u32 iExtraVideoLoops;
  u32 iExtraVideoLoopsDivider;

  std::string sLeftTexture;
  std::string sRightTexture;
  std::string sGCLeftTexture;
  std::string sGCRightTexture;

  // VR per game (Added from Hydra)
  float fUnitsPerMetre;
  float fFreeLookSensitivity;
  float fHudThickness;
  float fHudDistance;
  float fHud3DCloser;
  float fCameraForward;
  float fCameraPitch;
  float fAimDistance;
  float fMinFOV;
  float fN64FOV;
  float fScreenHeight;
  float fScreenThickness;
  float fScreenDistance;
  float fScreenRight;
  float fScreenUp;
  float fScreenPitch;
  float fTelescopeMaxFOV;
  float fReadPitch;

  float fHudDespPosition0;
  float fHudDespPosition1;
  float fHudDespPosition2;
  Common::Matrix33 matrixHudrot; // Matrix33 will need to be defined or replaced with an equivalent

  u32 iCameraMinPoly;
  bool bDisable3D;
  bool bHudFullscreen;
  bool bHudOnTop;
  bool bDontClearScreen;
  bool bCanReadCameraAngles;
  bool bDetectSkybox;
  int iTelescopeEye;
  int iMetroidPrime;
  // VR layer debugging
  int iSelectedLayer;
  int iFlashState;

  // D3D only config, mostly to be merged into the above
  int iAdapter = 0;

  // VideoSW Debugging (Added from Hydra, might be less relevant now or moved elsewhere)
  int drawStart;
  int drawEnd;
  bool bZComploc;
  bool bZFreeze;
  bool bDumpObjects;
  bool bDumpTevStages;
  bool bDumpTevTextureFetches;

  // Metal only config
  TriState iManuallyUploadBuffers = TriState::Auto;
  TriState iUsePresentDrawable = TriState::Auto;

  // Enable API validation layers, currently only supported with Vulkan.
  bool bEnableValidationLayer = false;

  // Multithreaded submission, currently only supported with Vulkan.
  bool bBackendMultithreading = true;

  // Early command buffer execution interval in number of draws.
  // Currently only supported with Vulkan.
  int iCommandBufferExecuteInterval = 0;

  // Shader compilation settings.
  bool bWaitForShadersBeforeStarting = false;
  ShaderCompilationMode iShaderCompilationMode{};

  // Number of shader compiler threads.
  // 0 disables background compilation.
  // -1 uses an automatic number based on the CPU threads.
  int iShaderCompilerThreads = 0;
  int iShaderPrecompilerThreads = 0;

  // Loading custom drivers on Android
  std::string customDriverLibraryName;

  // Vertex loader
  VertexLoaderType vertex_loader_type; // Not in Hydra

  // Shader compilation settings (already present, some differences from Hydra)
  // bool bBackgroundShaderCompiling; // Hydra: part of VideoConfig, Current: part of ShaderCompilationMode logic
  // bool bDisableSpecializedShaders; // Hydra: part of VideoConfig, Current: part of ShaderCompilationMode logic
  // bool bPrecompileUberShaders; // Hydra: part of VideoConfig, Current: related to ShaderCompilationMode logic


  // Utility
  // Hydra specific utility functions that need to be merged or adapted:
  // bool RealXFBEnabled() const { return bUseXFB && bUseRealXFB; }
  // bool VirtualXFBEnabled() const { return bUseXFB && !bUseRealXFB; }
  // bool EFBCopiesToTextureEnabled() const { return bEFBCopyEnable && bSkipEFBCopyToRam; }
  // bool EFBCopiesToRamEnabled() const { return bEFBCopyEnable && !bSkipEFBCopyToRam; }
  // bool BBoxUseFragmentShaderImplementation() const - logic refers to g_backend_info
  // bool CanPrecompileUberShaders() const;
  // bool CanBackgroundCompileShaders() const;


  bool UseVSForLinePointExpand() const
  {
    if (!g_backend_info.bSupportsVSLinePointExpand)
      return false;
    if (!g_backend_info.bSupportsGeometryShaders)
      return true;
    return bPreferVSForLinePointExpansion;
  }
  bool MultisamplingEnabled() const { return iMultisamples > 1; }
  bool ExclusiveFullscreenEnabled() const
  {
    return g_backend_info.bSupportsExclusiveFullscreen && !bBorderlessFullscreen;
  }
  bool UseGPUTextureDecoding() const
  {
    return g_backend_info.bSupportsGPUTextureDecoding && bEnableGPUTextureDecoding;
  }
  // Reverted to current logic: iEFBScale != 1 means not native resolution
  bool UseVertexRounding() const { return bVertexRounding && iEFBScale != 1; }

  bool ManualTextureSamplingWithCustomTextureSizes() const
  {
    // If manual texture sampling is disabled, we don't need to do anything.
    if (bFastTextureSampling) // Keep this check from current
      return false;
    // Hi-res textures break the wrapping logic used by manual texture sampling, as a texture's
    // size won't match the size the game sets.
    if (bHiresTextures)
      return true;
    // Hi-res EFB copies (but not native-resolution EFB copies at higher internal resolutions)
    // also result in different texture sizes that need special handling.
    // Corrected to use iEFBScale != 1 (current logic)
    if (iEFBScale != 1 && bCopyEFBScaled)
      return true;
    // Stereoscopic 3D changes the number of layers some textures have (EFB copies have 2 layers,
    // while game textures still have 1), meaning bounds checks need to be added.
    if (stereo_mode != StereoMode::Off)
      return true;
    // Otherwise, manual texture sampling can use the sizes games specify directly.
    return false;
  }
  bool UsingUberShaders() const; // Already present
  u32 GetShaderCompilerThreads() const; // Already present
  u32 GetShaderPrecompilerThreads() const; // Already present

  // Added from Hydra reference (or modified)
  bool RealXFBEnabled() const { return bUseXFB && bUseRealXFB; }
  bool VirtualXFBEnabled() const { return bUseXFB && !bUseRealXFB; }
  bool EFBCopiesToTextureEnabled() const { return bEFBCopyEnable && bSkipEFBCopyToRam; }
  bool EFBCopiesToRamEnabled() const { return bEFBCopyEnable && !bSkipEFBCopyToRam; }
  bool BBoxUseFragmentShaderImplementation() const
  {
    // Logic from Hydra, uses g_backend_info
    if (g_backend_info.api_type == APIType::OpenGL && bBBoxPreferStencilImplementation)
      return false;
    return g_backend_info.bSupportsBBox && g_backend_info.bSupportsFragmentStoresAndAtomics;
  }
  bool CanPrecompileUberShaders() const; // Added from Hydra
  bool CanBackgroundCompileShaders() const; // Added from Hydra


  float GetCustomAspectRatio() const { return (float)custom_aspect_width / custom_aspect_height; }
};

extern VideoConfig g_Config;
extern VideoConfig g_ActiveConfig;
extern VideoConfig g_SavedConfig; // Added for VR settings state saving

// Called every frame.
void UpdateActiveConfig();
// CheckForConfigChanges is specific to current, keep. Hydra only has UpdateActiveConfig.
void CheckForConfigChanges();
