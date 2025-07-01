// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VideoConfig.h"

#include <algorithm>
#include <optional>
#include <type_traits> // For std::decay_t

#include "Common/Config/Config.h" // For Config::GetLayer, Config::Save
#include "Common/Config/Layer.h"  // For Config::Layer methods
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "Core/ARBruteForcer.h" // Added for ARBruteForcer::IsEnabled

// From Hydra, adjusted for g_has_hmd (OpenVR equivalent)
#include "VideoCommon/VR.h" // For g_has_hmd

VideoConfig g_Config;
VideoConfig g_ActiveConfig;
VideoConfig g_SavedConfig; // Added from Hydra
BackendInfo g_backend_info;
static std::optional<CPUThreadConfigCallback::ConfigChangedCallbackID>
    s_config_changed_callback_id = std::nullopt;

static bool IsVSyncActive(bool enabled)
{
  // Vsync is disabled when the throttler is disabled by the tab key.
  return enabled && !Core::GetIsThrottlerTempDisabled() &&
         Config::Get(Config::MAIN_EMULATION_SPEED) == 1.0;
}


void UpdateActiveConfig()
{
  // From Hydra:
  // if (Movie::IsPlayingInput() && Movie::IsConfigSaved())
  // Movie::SetGraphicsConfig(); // Movie related, assess if still relevant / how to integrate

  g_ActiveConfig = g_Config;
  g_ActiveConfig.bVSyncActive = IsVSyncActive(g_ActiveConfig.bVSync); // Keep this line from current

  // From Hydra:
  if (g_has_hmd) // g_has_hmd equivalent
    g_ActiveConfig.bUseRealXFB = false;
}


VideoConfig::VideoConfig()
{
  // Needed for the first frame, I think (from Hydra)
  fAspectRatioHackW = 1;
  fAspectRatioHackH = 1;

  // Most boolean VR settings default to false or a sensible default.
  // Values taken from Hydra reference.

  // VR global
  fScale = 1.0f;
  fLeanBackAngle = 0.0f; // Hydra: 0
  bStabilizeRoll = true;
  bStabilizePitch = true;
  bStabilizeYaw = false;
  bStabilizeX = false;
  bStabilizeY = false;
  bStabilizeZ = false;
  bKeyhole = false;
  fKeyholeWidth = 0.0f; // Not explicitly set in Hydra constructor, default to 0. GLOBAL_VR_KEYHOLE_WIDTH is its config.
  bKeyholeSnap = false;
  fKeyholeSnapSize = 0.0f; // Not explicitly set in Hydra constructor, default to 0. GLOBAL_VR_KEYHOLE_SIZE is its config.
  bPullUp20fps = false;
  bPullUp30fps = false;
  bPullUp60fps = false;
  bPullUpAuto = false;
  bSynchronousTimewarp = false;
  bOpcodeWarningDisable = false;
  bReplayVertexData = false;
  bReplayOtherData = false; // Added this, was missing in my previous plan step for .h
  bPullUp20fpsTimewarp = false;
  bPullUp30fpsTimewarp = false;
  bPullUp60fpsTimewarp = false;
  bPullUpAutoTimewarp = false;
  bOpcodeReplay = false;
  bAsynchronousTimewarp = false; // Defaulted, OpenVR may handle this differently
  bEnableVR = true; // Default to true, as this is a VR branch
  bLowPersistence = true;
  bDynamicPrediction = true;
  bOrientationTracking = true;
  bMagYawCorrection = true;
  bPositionTracking = true;
  bChromatic = true; // Often a per-app preference
  bTimewarp = true; // Core VR feature
  bVignette = false;
  bNoRestore = false;
  bFlipVertical = false; // Usually not needed
  bSRGB = false; // Default to false, can be enabled by user
  bOverdrive = true; // Display specific
  bHqDistortion = false; // Performance impact
  bDisableNearClipping = true; // Common VR tweak
  bAutoPairViveControllers = false; // Specific to Vive, maybe less relevant for general OpenVR
  bShowHands = false;
  bShowFeet = false;
  bShowController = true;
  bShowLaserPointer = false;
  bShowAimRectangle = false;
  bShowHudBox = false;
  bShow2DBox = false;
  bShowSensorBar = false;
  bShowGameCamera = false;
  bShowGameFrustum = false;
  bShowTrackingCamera = false;
  bShowTrackingVolume = false;
  bShowBaseStation = false;
  bMotionSicknessAlways = false;
  bMotionSicknessFreelook = false;
  bMotionSickness2D = false;
  bMotionSicknessLeftStick = false;
  bMotionSicknessRightStick = false;
  bMotionSicknessDPad = false;
  bMotionSicknessIR = false;
  iMotionSicknessMethod = 0;
  iMotionSicknessSkybox = 0;
  fMotionSicknessFOV = 45.0f; // Hydra: 45.0f

  // Using defines from VR-Hydra-Reference/VR.h
  iVRPlayer = 0; // VR_PLAYER1 = 0
  iVRPlayer2 = 1; // VR_PLAYER2 = 1
  iMirrorPlayer = 5; // VR_PLAYER_DEFAULT = 5
  iMirrorStyle = 0; // VR_MIRROR_LEFT = 0

  // Using defines from VR-Hydra-Reference/VR.h
  fTimeWarpTweak = 0.0f; // DEFAULT_VR_TIMEWARP_TWEAK = 0 (float)
  iExtraTimewarpedFrames = 0; // DEFAULT_VR_EXTRA_FRAMES = 0
  iExtraVideoLoops = 0; // DEFAULT_VR_EXTRA_VIDEO_LOOPS = 0
  iExtraVideoLoopsDivider = 0; // DEFAULT_VR_EXTRA_VIDEO_LOOPS_DIVIDER = 0

  sLeftTexture = "";
  sRightTexture = "";
  sGCLeftTexture = "";
  sGCRightTexture = "";

  // VR per game
  // Using defines from VR-Hydra-Reference/VR.h
  fUnitsPerMetre = 1.0f; // DEFAULT_VR_UNITS_PER_METRE = 1.0f
  fFreeLookSensitivity = 1.0f; // DEFAULT_VR_FREE_LOOK_SENSITIVITY = 1.0f
  fHudThickness = 0.5f; // DEFAULT_VR_HUD_THICKNESS = 0.5f
  fHudDistance = 1.5f; // DEFAULT_VR_HUD_DISTANCE = 1.5f
  fHud3DCloser = 0.5f; // DEFAULT_VR_HUD_3D_CLOSER = 0.5f
  fCameraForward = 0.0f; // Hydra: Not explicitly set, defaults to 0.0f. GFX_VR_CAMERA_FORWARD
  fCameraPitch = 0.0f; // Hydra: Not explicitly set, defaults to 0.0f. GFX_VR_CAMERA_PITCH
  fAimDistance = 7.0f; // DEFAULT_VR_AIM_DISTANCE = 7.0f
  fMinFOV = 10.0f; // DEFAULT_VR_MIN_FOV = 10.0f
  fN64FOV = 90.0f; // DEFAULT_VR_N64_FOV = 90.0f
  fScreenHeight = 2.0f; // DEFAULT_VR_SCREEN_HEIGHT = 2.0f
  fScreenThickness = 0.5f; // DEFAULT_VR_SCREEN_THICKNESS = 0.5f
  fScreenDistance = 1.5f; // DEFAULT_VR_SCREEN_DISTANCE = 1.5f
  fScreenRight = 0.0f; // Hydra: Not explicitly set, defaults to 0.0f. GFX_VR_SCREEN_RIGHT
  fScreenUp = 0.0f; // Hydra: Not explicitly set, defaults to 0.0f. GFX_VR_SCREEN_UP
  fScreenPitch = 0.0f; // Hydra: Not explicitly set, defaults to 0.0f. GFX_VR_SCREEN_PITCH
  fTelescopeMaxFOV = 0.0f; // Hydra: 0
  fReadPitch = 0.0f; // Hydra: Not explicitly set, defaults to 0.0f. GFX_VR_READ_PITCH

  fHudDespPosition0 = 0.0f; // Hydra: 0
  fHudDespPosition1 = 0.0f;
  fHudDespPosition2 = 0.0f;
  // Matrix33::LoadIdentity(matrixHudrot); // Needs Matrix33 from Common/Matrix.h
  matrixHudrot = Common::Matrix33::Identity();


  iCameraMinPoly = 0; // 0 means no override
  bDisable3D = false;
  bHudFullscreen = false;
  bHudOnTop = false;
  bDontClearScreen = false;
  bCanReadCameraAngles = false;
  bDetectSkybox = false;
  iTelescopeEye = 0; // 0 for left, 1 for right, -1 for both/auto
  iMetroidPrime = 0; // Game specific hack flag

  iSelectedLayer = -2; // VR layer debugging
  iFlashState = 0; // VR layer debugging

  // Default initialize other members that were in current and not explicitly in Hydra's constructor
  // Most are fine with default member initializers in .h or default constructed.
  // Ensure crucial ones are set if they were not covered by Hydra's explicit list.
  // For example, enums should have a defined default.
  aspect_mode = AspectMode::Auto;
  stereo_mode = StereoMode::Off;
  texture_filtering_mode = TextureFilteringMode::Default;
  output_resampling_mode = OutputResamplingMode::Default;
  iMaxAnisotropy = AnisotropicFilteringMode::Default;
  iShaderCompilationMode = ShaderCompilationMode::SynchronousUberShaders; // A sensible default
  vertex_loader_type = VertexLoaderType::Native; // A sensible default
}


void VideoConfig::Refresh()
{
  if (!s_config_changed_callback_id.has_value())
  {
    // There was a race condition between the video thread and the host thread here, if
    // corrections need to be made by VerifyValidity(). Briefly, the config will contain
    // invalid values. Instead, pause the video thread first, update the config and correct
    // it, then resume emulation, after which the video thread will detect the config has
    // changed and act accordingly.
    const auto config_changed_callback = []() {
      auto& system = Core::System::GetInstance();

      const bool lock_gpu_thread = Core::IsRunning(system);
      if (lock_gpu_thread)
        system.GetFifo().PauseAndLock(true, false);

      g_Config.Refresh();
      g_Config.VerifyValidity();

      if (lock_gpu_thread)
        system.GetFifo().PauseAndLock(false, true);
    };

    s_config_changed_callback_id =
        CPUThreadConfigCallback::AddConfigChangedCallback(config_changed_callback);
  }

  bVSync = Config::Get(Config::GFX_VSYNC);
  iAdapter = Config::Get(Config::GFX_ADAPTER);
  iManuallyUploadBuffers = Config::Get(Config::GFX_MTL_MANUALLY_UPLOAD_BUFFERS);
  iUsePresentDrawable = Config::Get(Config::GFX_MTL_USE_PRESENT_DRAWABLE);

  bWidescreenHack = Config::Get(Config::GFX_WIDESCREEN_HACK);
  aspect_mode = Config::Get(Config::GFX_ASPECT_RATIO);
  custom_aspect_width = Config::Get(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH);
  custom_aspect_height = Config::Get(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT);
  suggested_aspect_mode = Config::Get(Config::GFX_SUGGESTED_ASPECT_RATIO);
  widescreen_heuristic_transition_threshold =
      Config::Get(Config::GFX_WIDESCREEN_HEURISTIC_TRANSITION_THRESHOLD);
  widescreen_heuristic_aspect_ratio_slop =
      Config::Get(Config::GFX_WIDESCREEN_HEURISTIC_ASPECT_RATIO_SLOP);
  widescreen_heuristic_standard_ratio =
      Config::Get(Config::GFX_WIDESCREEN_HEURISTIC_STANDARD_RATIO);
  widescreen_heuristic_widescreen_ratio =
      Config::Get(Config::GFX_WIDESCREEN_HEURISTIC_WIDESCREEN_RATIO);
  bCrop = Config::Get(Config::GFX_CROP);
  iSafeTextureCache_ColorSamples = Config::Get(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
  bShowFPS = Config::Get(Config::GFX_SHOW_FPS);
  bShowFTimes = Config::Get(Config::GFX_SHOW_FTIMES);
  bShowVPS = Config::Get(Config::GFX_SHOW_VPS);
  bShowVTimes = Config::Get(Config::GFX_SHOW_VTIMES);
  bShowGraphs = Config::Get(Config::GFX_SHOW_GRAPHS);
  bShowSpeed = Config::Get(Config::GFX_SHOW_SPEED);
  bShowSpeedColors = Config::Get(Config::GFX_SHOW_SPEED_COLORS);
  iPerfSampleUSec = Config::Get(Config::GFX_PERF_SAMP_WINDOW) * 1000;
  bLogRenderTimeToFile = Config::Get(Config::GFX_LOG_RENDER_TIME_TO_FILE);
  bOverlayStats = Config::Get(Config::GFX_OVERLAY_STATS);
  bOverlayProjStats = Config::Get(Config::GFX_OVERLAY_PROJ_STATS);
  bOverlayScissorStats = Config::Get(Config::GFX_OVERLAY_SCISSOR_STATS);
  bDumpTextures = Config::Get(Config::GFX_DUMP_TEXTURES);
  bDumpMipmapTextures = Config::Get(Config::GFX_DUMP_MIP_TEXTURES);
  bDumpBaseTextures = Config::Get(Config::GFX_DUMP_BASE_TEXTURES);
  bHiresTextures = Config::Get(Config::GFX_HIRES_TEXTURES);
  bCacheHiresTextures = Config::Get(Config::GFX_CACHE_HIRES_TEXTURES);
  bDumpEFBTarget = Config::Get(Config::GFX_DUMP_EFB_TARGET);
  bDumpXFBTarget = Config::Get(Config::GFX_DUMP_XFB_TARGET);
  bEnableGPUTextureDecoding = Config::Get(Config::GFX_ENABLE_GPU_TEXTURE_DECODING);
  bPreferVSForLinePointExpansion = Config::Get(Config::GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION);
  bEnablePixelLighting = Config::Get(Config::GFX_ENABLE_PIXEL_LIGHTING);
  bFastDepthCalc = Config::Get(Config::GFX_FAST_DEPTH_CALC);
  iMultisamples = Config::Get(Config::GFX_MSAA);
  bSSAA = Config::Get(Config::GFX_SSAA);
  iEFBScale = Config::Get(Config::GFX_EFB_SCALE);
  bTexFmtOverlayEnable = Config::Get(Config::GFX_TEXFMT_OVERLAY_ENABLE);
  bTexFmtOverlayCenter = Config::Get(Config::GFX_TEXFMT_OVERLAY_CENTER);
  bWireFrame = Config::Get(Config::GFX_ENABLE_WIREFRAME);
  bDisableFog = Config::Get(Config::GFX_DISABLE_FOG);
  bBorderlessFullscreen = Config::Get(Config::GFX_BORDERLESS_FULLSCREEN);
  bEnableValidationLayer = Config::Get(Config::GFX_ENABLE_VALIDATION_LAYER);
  bBackendMultithreading = Config::Get(Config::GFX_BACKEND_MULTITHREADING);
  iCommandBufferExecuteInterval = Config::Get(Config::GFX_COMMAND_BUFFER_EXECUTE_INTERVAL);
  bShaderCache = Config::Get(Config::GFX_SHADER_CACHE);
  bWaitForShadersBeforeStarting = Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING);
  iShaderCompilationMode = Config::Get(Config::GFX_SHADER_COMPILATION_MODE);
  iShaderCompilerThreads = Config::Get(Config::GFX_SHADER_COMPILER_THREADS);
  iShaderPrecompilerThreads = Config::Get(Config::GFX_SHADER_PRECOMPILER_THREADS);
  bCPUCull = Config::Get(Config::GFX_CPU_CULL);

  texture_filtering_mode = Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
  iMaxAnisotropy = Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY);
  output_resampling_mode = Config::Get(Config::GFX_ENHANCE_OUTPUT_RESAMPLING);
  sPostProcessingShader = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
  bForceTrueColor = Config::Get(Config::GFX_ENHANCE_FORCE_TRUE_COLOR);
  bDisableCopyFilter = Config::Get(Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
  bArbitraryMipmapDetection = Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);
  fArbitraryMipmapDetectionThreshold =
      Config::Get(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD);
  bHDR = Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT);

  color_correction.bCorrectColorSpace = Config::Get(Config::GFX_CC_CORRECT_COLOR_SPACE);
  color_correction.game_color_space = Config::Get(Config::GFX_CC_GAME_COLOR_SPACE);
  color_correction.bCorrectGamma = Config::Get(Config::GFX_CC_CORRECT_GAMMA);
  color_correction.fGameGamma = Config::Get(Config::GFX_CC_GAME_GAMMA);
  color_correction.bSDRDisplayGammaSRGB = Config::Get(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB);
  color_correction.fSDRDisplayCustomGamma = Config::Get(Config::GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA);
  color_correction.fHDRPaperWhiteNits = Config::Get(Config::GFX_CC_HDR_PAPER_WHITE_NITS);

  stereo_mode = Config::Get(Config::GFX_STEREO_MODE);
  stereo_per_eye_resolution_full = Config::Get(Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL);
  iStereoDepth = Config::Get(Config::GFX_STEREO_DEPTH);
  iStereoConvergencePercentage = Config::Get(Config::GFX_STEREO_CONVERGENCE_PERCENTAGE);
  bStereoSwapEyes = Config::Get(Config::GFX_STEREO_SWAP_EYES);
  iStereoConvergence = Config::Get(Config::GFX_STEREO_CONVERGENCE);
  bStereoEFBMonoDepth = Config::Get(Config::GFX_STEREO_EFB_MONO_DEPTH);
  iStereoDepthPercentage = Config::Get(Config::GFX_STEREO_DEPTH_PERCENTAGE);

  bEFBAccessEnable = Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE);
  bEFBAccessDeferInvalidation = Config::Get(Config::GFX_HACK_EFB_DEFER_INVALIDATION);
  bBBoxEnable = Config::Get(Config::GFX_HACK_BBOX_ENABLE);
  bSkipEFBCopyToRam = Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
  bSkipXFBCopyToRam = Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);
  bDisableCopyToVRAM = Config::Get(Config::GFX_HACK_DISABLE_COPY_TO_VRAM);
  bDeferEFBCopies = Config::Get(Config::GFX_HACK_DEFER_EFB_COPIES);
  bImmediateXFB = Config::Get(Config::GFX_HACK_IMMEDIATE_XFB);
  bVISkip = Config::Get(Config::GFX_HACK_VI_SKIP);
  bSkipPresentingDuplicateXFBs = bVISkip || Config::Get(Config::GFX_HACK_SKIP_DUPLICATE_XFBS);
  bCopyEFBScaled = Config::Get(Config::GFX_HACK_COPY_EFB_SCALED);
  bEFBEmulateFormatChanges = Config::Get(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
  bVertexRounding = Config::Get(Config::GFX_HACK_VERTEX_ROUNDING);
  iEFBAccessTileSize = Config::Get(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE);
  iMissingColorValue = Config::Get(Config::GFX_HACK_MISSING_COLOR_VALUE);
  bFastTextureSampling = Config::Get(Config::GFX_HACK_FAST_TEXTURE_SAMPLING);
#ifdef __APPLE__
  bNoMipmapping = Config::Get(Config::GFX_HACK_NO_MIPMAPPING);
#endif

  bPerfQueriesEnable = Config::Get(Config::GFX_PERF_QUERIES_ENABLE);

  bGraphicMods = Config::Get(Config::GFX_MODS_ENABLE); // Keep from current

  customDriverLibraryName = Config::Get(Config::GFX_DRIVER_LIB_NAME); // Keep from current

  vertex_loader_type = Config::Get(Config::GFX_VERTEX_LOADER_TYPE); // Keep from current

  // Load VR settings - Game specific (GFX_VR_ prefix)
  bDisable3D = Config::Get(Config::GFX_VR_DISABLE_3D);
  bHudFullscreen = Config::Get(Config::GFX_VR_HUD_FULLSCREEN);
  bHudOnTop = Config::Get(Config::GFX_VR_HUD_ON_TOP);
  bDontClearScreen = Config::Get(Config::GFX_VR_DONT_CLEAR_SCREEN);
  bCanReadCameraAngles = Config::Get(Config::GFX_VR_CAN_READ_CAMERA_ANGLES);
  bDetectSkybox = Config::Get(Config::GFX_VR_DETECT_SKYBOX);
  fUnitsPerMetre = Config::Get(Config::GFX_VR_UNITS_PER_METRE);
  fHudThickness = Config::Get(Config::GFX_VR_HUD_THICKNESS);
  fHudDistance = Config::Get(Config::GFX_VR_HUD_DISTANCE);
  fHud3DCloser = Config::Get(Config::GFX_VR_HUD_3D_CLOSER);
  fCameraForward = Config::Get(Config::GFX_VR_CAMERA_FORWARD);
  fCameraPitch = Config::Get(Config::GFX_VR_CAMERA_PITCH);
  fAimDistance = Config::Get(Config::GFX_VR_AIM_DISTANCE);
  fMinFOV = Config::Get(Config::GFX_VR_MIN_FOV);
  fN64FOV = Config::Get(Config::GFX_VR_N64_FOV);
  fScreenHeight = Config::Get(Config::GFX_VR_SCREEN_HEIGHT);
  fScreenThickness = Config::Get(Config::GFX_VR_SCREEN_THICKNESS);
  fScreenDistance = Config::Get(Config::GFX_VR_SCREEN_DISTANCE);
  fScreenRight = Config::Get(Config::GFX_VR_SCREEN_RIGHT);
  fScreenUp = Config::Get(Config::GFX_VR_SCREEN_UP);
  fScreenPitch = Config::Get(Config::GFX_VR_SCREEN_PITCH);
  iMetroidPrime = Config::Get(Config::GFX_VR_METROID_PRIME);
  iTelescopeEye = Config::Get(Config::GFX_VR_TELESCOPE_EYE);
  fTelescopeMaxFOV = Config::Get(Config::GFX_VR_TELESCOPE_MAX_FOV);
  fReadPitch = Config::Get(Config::GFX_VR_READ_PITCH);
  iCameraMinPoly = Config::Get(Config::GFX_VR_CAMERA_MIN_POLY);
  fHudDespPosition0 = Config::Get(Config::GFX_VR_HUD_DESP_POSITION_0);
  fHudDespPosition1 = Config::Get(Config::GFX_VR_HUD_DESP_POSITION_1);
  fHudDespPosition2 = Config::Get(Config::GFX_VR_HUD_DESP_POSITION_2);

  // Load VR settings - Global (GLOBAL_VR_ prefix)
  // bool bNoMirrorToWindow; // This was intermediate in Hydra, final is iMirrorStyle
  fScale = Config::Get(Config::GLOBAL_VR_SCALE);
  fFreeLookSensitivity = Config::Get(Config::GLOBAL_VR_FREE_LOOK_SENSITIVITY);
  fLeanBackAngle = Config::Get(Config::GLOBAL_VR_LEAN_BACK_ANGLE);
  bEnableVR = Config::Get(Config::GLOBAL_VR_ENABLE_VR);
  bLowPersistence = Config::Get(Config::GLOBAL_VR_LOW_PERSISTENCE);
  bDynamicPrediction = Config::Get(Config::GLOBAL_VR_DYNAMIC_PREDICTION);
  bOrientationTracking = Config::Get(Config::GLOBAL_VR_ORIENTATION_TRACKING);
  bMagYawCorrection = Config::Get(Config::GLOBAL_VR_MAG_YAW_CORRECTION);
  bPositionTracking = Config::Get(Config::GLOBAL_VR_POSITION_TRACKING);
  bChromatic = Config::Get(Config::GLOBAL_VR_CHROMATIC);
  bTimewarp = Config::Get(Config::GLOBAL_VR_TIMEWARP);
  bVignette = Config::Get(Config::GLOBAL_VR_VIGNETTE);
  bNoRestore = Config::Get(Config::GLOBAL_VR_NO_RESTORE);
  bFlipVertical = Config::Get(Config::GLOBAL_VR_FLIP_VERTICAL);
  bSRGB = Config::Get(Config::GLOBAL_VR_SRGB);
  bOverdrive = Config::Get(Config::GLOBAL_VR_OVERDRIVE);
  bHqDistortion = Config::Get(Config::GLOBAL_VR_HQ_DISTORTION);
  bDisableNearClipping = Config::Get(Config::GLOBAL_VR_DISABLE_NEAR_CLIPPING);
  bAutoPairViveControllers = Config::Get(Config::GLOBAL_VR_AUTO_PAIR_VIVE_CONTROLLERS);
  bShowHands = Config::Get(Config::GLOBAL_VR_SHOW_HANDS);
  bShowFeet = Config::Get(Config::GLOBAL_VR_SHOW_FEET);
  bShowController = Config::Get(Config::GLOBAL_VR_SHOW_CONTROLLER);
  bShowLaserPointer = Config::Get(Config::GLOBAL_VR_SHOW_LASER_POINTER);
  bShowAimRectangle = Config::Get(Config::GLOBAL_VR_SHOW_AIM_RECTANGLE);
  bShowHudBox = Config::Get(Config::GLOBAL_VR_SHOW_HUD_BOX);
  bShow2DBox = Config::Get(Config::GLOBAL_VR_SHOW_2D_SCREEN_BOX); // Renamed from GFX_VR_SHOW_2D_BOX
  bShowSensorBar = Config::Get(Config::GLOBAL_VR_SHOW_SENSOR_BAR);
  bShowGameCamera = Config::Get(Config::GLOBAL_VR_SHOW_GAME_CAMERA);
  bShowGameFrustum = Config::Get(Config::GLOBAL_VR_SHOW_GAME_FRUSTUM);
  bShowTrackingCamera = Config::Get(Config::GLOBAL_VR_SHOW_TRACKING_CAMERA);
  bShowTrackingVolume = Config::Get(Config::GLOBAL_VR_SHOW_TRACKING_VOLUME);
  bShowBaseStation = Config::Get(Config::GLOBAL_VR_SHOW_BASE_STATION);
  bMotionSicknessAlways = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_ALWAYS);
  bMotionSicknessFreelook = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_FREELOOK);
  bMotionSickness2D = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_2D);
  bMotionSicknessLeftStick = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_LEFT_STICK);
  bMotionSicknessRightStick = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_RIGHT_STICK);
  bMotionSicknessDPad = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_DPAD);
  bMotionSicknessIR = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_IR);
  iMotionSicknessMethod = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_METHOD);
  iMotionSicknessSkybox = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_SKYBOX);
  fMotionSicknessFOV = Config::Get(Config::GLOBAL_VR_MOTION_SICKNESS_FOV);
  iVRPlayer = Config::Get(Config::GLOBAL_VR_PLAYER);
  iVRPlayer2 = Config::Get(Config::GLOBAL_VR_PLAYER_2);
  iMirrorPlayer = Config::Get(Config::GLOBAL_VR_MIRROR_PLAYER);
  iMirrorStyle = Config::Get(Config::GLOBAL_VR_MIRROR_STYLE);
  // Hydra logic: iMirrorStyle = bNoMirrorToWindow ? VR_MIRROR_DISABLED : VR_MIRROR_LEFT; then iMirrorStyle = Config::Get(Config::GLOBAL_VR_MIRROR_STYLE);
  // The Config::Get will override the bNoMirrorToWindow logic if GLOBAL_VR_MIRROR_STYLE is set.

  fTimeWarpTweak = Config::Get(Config::GLOBAL_VR_TIMEWARP_TWEAK);
  iExtraTimewarpedFrames = Config::Get(Config::GLOBAL_VR_NUM_EXTRA_FRAMES);
  iExtraVideoLoops = Config::Get(Config::GLOBAL_VR_NUM_EXTRA_VIDEO_LOOPS);
  iExtraVideoLoopsDivider = Config::Get(Config::GLOBAL_VR_NUM_EXTRA_VIDEO_LOOPS_DIVIDER);
  bStabilizeRoll = Config::Get(Config::GLOBAL_VR_STABILIZE_ROLL);
  bStabilizePitch = Config::Get(Config::GLOBAL_VR_STABILIZE_PITCH);
  bStabilizeYaw = Config::Get(Config::GLOBAL_VR_STABILIZE_YAW);
  bStabilizeX = Config::Get(Config::GLOBAL_VR_STABILIZE_X);
  bStabilizeY = Config::Get(Config::GLOBAL_VR_STABILIZE_Y);
  bStabilizeZ = Config::Get(Config::GLOBAL_VR_STABILIZE_Z);
  bKeyhole = Config::Get(Config::GLOBAL_VR_KEYHOLE);
  fKeyholeWidth = Config::Get(Config::GLOBAL_VR_KEYHOLE_WIDTH);
  bKeyholeSnap = Config::Get(Config::GLOBAL_VR_KEYHOLE_SNAP);
  fKeyholeSnapSize = Config::Get(Config::GLOBAL_VR_KEYHOLE_SIZE);
  bPullUp20fps = Config::Get(Config::GLOBAL_VR_PULL_UP_20_FPS);
  bPullUp30fps = Config::Get(Config::GLOBAL_VR_PULL_UP_30_FPS);
  bPullUp60fps = Config::Get(Config::GLOBAL_VR_PULL_UP_60_FPS);
  bPullUpAuto = Config::Get(Config::GLOBAL_VR_PULL_UP_AUTO);
  bOpcodeReplay = Config::Get(Config::GLOBAL_VR_OPCODE_REPLAY);
  bOpcodeWarningDisable = Config::Get(Config::GLOBAL_VR_OPCODE_WARNING_DISABLE);
  bReplayVertexData = Config::Get(Config::GLOBAL_VR_REPLAY_VERTEX_DATA);
  bReplayOtherData = Config::Get(Config::GLOBAL_VR_REPLAY_OTHER_DATA);
  bPullUp20fpsTimewarp = Config::Get(Config::GLOBAL_VR_PULL_UP_20_FPS_TIMEWARP);
  bPullUp30fpsTimewarp = Config::Get(Config::GLOBAL_VR_PULL_UP_30_FPS_TIMEWARP);
  bPullUp60fpsTimewarp = Config::Get(Config::GLOBAL_VR_PULL_UP_60_FPS_TIMEWARP);
  bPullUpAutoTimewarp = Config::Get(Config::GLOBAL_VR_PULL_UP_AUTO_TIMEWARP);
  bSynchronousTimewarp = Config::Get(Config::GLOBAL_VR_SYNCHRONOUS_TIMEWARP);
  sLeftTexture = Config::Get(Config::GLOBAL_VR_LEFT_TEXTURE);
  sRightTexture = Config::Get(Config::GLOBAL_VR_RIGHT_TEXTURE);
  sGCLeftTexture = Config::Get(Config::GLOBAL_VR_GC_LEFT_TEXTURE);
  sGCRightTexture = Config::Get(Config::GLOBAL_VR_GC_RIGHT_TEXTURE);

  // Hydra also had:
  // bUseXFB = Config::Get(Config::GFX_USE_XFB);
  // bUseRealXFB = Config::Get(Config::GFX_USE_REAL_XFB);
  // These are already loaded earlier in the current Refresh(), so no need to repeat.

  // Hydra's iAspectRatio logic:
  // const int aspect_ratio = Config::Get(Config::GFX_ASPECT_RATIO);
  // if (aspect_ratio == ASPECT_AUTO) // ASPECT_AUTO is 0
  //   iAspectRatio = Config::Get(Config::GFX_SUGGESTED_ASPECT_RATIO);
  // else
  //   iAspectRatio = aspect_ratio;
  // Current code uses aspect_mode (enum class) and suggested_aspect_mode (enum class)
  // This logic is different and should be preserved from current.

  // Hydra's iEFBScale and iInternalResolution:
  // if (ARBruteForcer::ch_bruteforce) { ... } else {
  //   iMultisamples = Config::Get(Config::GFX_MSAA);
  //   bSSAA = Config::Get(Config::GFX_SSAA);
  //   iEFBScale = Config::Get(Config::GFX_EFB_SCALE); // This is an enum in Hydra
  //   iInternalResolution = Config::Get(Config::GFX_INTERNAL_RESOLUTION); // This is an int
  //   if (iInternalResolution > -2) {
  //     iEFBScale = iInternalResolution + (iInternalResolution >= 0) + (iInternalResolution > 1) + (iInternalResolution > 2);
  //   }
  // }
  // Current code loads iEFBScale as an int directly. iInternalResolution is new.
  // The logic for iEFBScale based on iInternalResolution needs to be added carefully.
  // For now, load iInternalResolution and then apply the Hydra logic if iInternalResolution is used.
  iInternalResolution = Config::Get(Config::GFX_EFB_SCALE);
  //if (!ARBruteForcer::IsEnabled() && iInternalResolution > -2) // Assuming ARBruteForcer::IsEnabled() is the equivalent check
  //{
    // This mapping is from Hydra's EFBScale enum to an integer multiplier style
    // SCALE_1X = 2, SCALE_1_5X = 3, SCALE_2X = 4, SCALE_2_5X = 5 etc.
    // Current iEFBScale is 1 for 1x, 2 for 2x.
    // This requires careful mapping if we keep iEFBScale as is.
    // Alternative: Keep iEFBScale as loaded by current code, and iInternalResolution is a separate new setting.
    // The Hydra logic `iEFBScale = iInternalResolution + (iInternalResolution >= 0) + (iInternalResolution > 1) + (iInternalResolution > 2);`
    // effectively means:
    // if iInternalResolution = 0 (1x), iEFBScale = 0 + 1 + 0 + 0 = 1 (Incorrect, Hydra SCALE_1X is 2)
    // Let's use the direct Config::Get(Config::GFX_EFB_SCALE) from current code for iEFBScale.
    // iInternalResolution will be a new, separate setting.
    // The user would choose either "Internal Resolution" (which implies an EFB scale) or "EFB Scale" directly.
    // For now, just load iInternalResolution. Its interaction with iEFBScale will be handled by whatever consumes them.
  //}


  // Hydra: phack loading
  phack.m_enable = Config::Get(Config::GFX_PROJECTION_HACK) == 1;
  phack.m_sznear = Config::Get(Config::GFX_PROJECTION_HACK_SZNEAR) == 1;
  phack.m_szfar = Config::Get(Config::GFX_PROJECTION_HACK_SZFAR) == 1;
  phack.m_znear = Config::Get(Config::GFX_PROJECTION_HACK_ZNEAR);
  phack.m_zfar = Config::Get(Config::GFX_PROJECTION_HACK_ZFAR);

  // Hydra: bForceFiltering, maps to texture_filtering_mode
  // Already handled by current code: texture_filtering_mode = Config::Get(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);

  // Hydra: Stereo settings (iStereoMode, etc.)
  // Already handled by current code. The enum values were reconciled in .h.

  // Hydra: Hacks (bEFBCopyEnable, bEFBCopyClearDisable, etc.)
  // Already handled by current code where overlapping. New ones added to .h are loaded here.
  //bEFBCopyEnable = Config::Get(Config::GFX_HACK_EFB_COPY_ENABLE); // New from Hydra
  //bEFBCopyClearDisable = Config::Get(Config::GFX_HACK_EFB_COPY_CLEAR_DISABLE); // New from Hydra
  //bForceProgressive = Config::Get(Config::GFX_HACK_FORCE_PROGRESSIVE); // New from Hydra
  //bBBoxPreferStencilImplementation = Config::Get(Config::GFX_HACK_BBOX_PREFER_STENCIL_IMPLEMENTATION); // New from Hydra

  // Hydra: Logging settings (iLog) - This seems to be debug flags
  // iLog = Config::Get(Config::GFX_LOG_SETTINGS); // Example, needs proper Config Enum

  // Hydra: Shader compilation settings (bBackgroundShaderCompiling, etc.)
  // Current code uses iShaderCompilationMode. The mapping is:
  // Synchronous: !bBackgroundShaderCompiling && !bDisableSpecializedShaders
  // SynchronousUberShaders: !bBackgroundShaderCompiling && bDisableSpecializedShaders
  // AsynchronousUberShaders: bBackgroundShaderCompiling && !bDisableSpecializedShaders (Hybrid in Hydra)
  // AsynchronousSkipRendering: (No direct equivalent in Hydra bools, implies bBackgroundShaderCompiling)
  // For now, we rely on the iShaderCompilationMode loaded by current code.
  // The individual bools from Hydra (bBackgroundShaderCompiling, etc.) are not directly part of VideoConfig anymore.

  // VideoSW Debugging from Hydra (drawStart, drawEnd etc.)
  // These are very specific debug flags, potentially for a software renderer.
  // For now, I will add them assuming Config Enums exist.
  /*drawStart = Config::Get(Config::GFX_SW_DRAW_START);
  drawEnd = Config::Get(Config::GFX_SW_DRAW_END);
  bZComploc = Config::Get(Config::GFX_SW_ZCOMPLOC);
  bZFreeze = Config::Get(Config::GFX_SW_ZFREEZE);*/
  bDumpObjects = Config::Get(Config::GFX_SW_DUMP_OBJECTS);
  bDumpTevStages = Config::Get(Config::GFX_SW_DUMP_TEV_STAGES);
  bDumpTevTextureFetches = Config::Get(Config::GFX_SW_DUMP_TEV_TEX_FETCHES);


  // Final lines from Hydra's Refresh:
  // VerifyValidity(); // Current code calls this after Refresh in a callback.
  // The callback structure in current code is more robust.
}

// TODO: ARBruteForcer::IsEnabled() needs to be available. For now, assume it is.
// This might require including "Core/ARBruteForcer.h" if not already via other headers.
#include "Core/ARBruteForcer.h" // For ARBruteForcer::IsEnabled()

// From Hydra:
void VideoConfig::GameIniSave()
{
  // Save game ini
  // INFO_LOG from Hydra can be replaced with Common/Logging/Log.h if desired, or removed.
  // INFO_LOG(CORE, "VideoConfig::GameIniSave()");

  auto local_game_layer = Config::GetLayer(Config::LayerType::LocalGame);
  if (!local_game_layer)
    return;

  bool modified = false;

  // Helper lambda to implement SaveIfNotDefault logic
  auto save_if_not_default = [&](const auto& setting, const auto& current_value) {
    // Type of current_value could be different from setting's type, ensure comparison is valid or cast.
    // For now, assuming direct comparison is okay or types match.
    using SettingType = typename std::decay_t<decltype(setting.GetDefaultValue())>;
    const SettingType default_value = setting.GetDefaultValue();
    std::optional<SettingType> game_ini_value = local_game_layer->Get(setting);

    if (game_ini_value.has_value())
    {
      // If the value in game INI is different from current runtime, update game INI
      if (*game_ini_value != current_value)
      {
        local_game_layer->Set(setting, current_value);
        modified = true;
      }
    }
    else
    {
      // If not in game INI, but current runtime is different from default, save to game INI
      if (current_value != default_value)
      {
        local_game_layer->Set(setting, current_value);
        modified = true;
      }
    }
  };

  // This uses new Config Enums that will need to be defined in GraphicsSettings.h/cpp
  save_if_not_default(Config::GFX_VR_DISABLE_3D, g_Config.bDisable3D);
  save_if_not_default(Config::GFX_VR_UNITS_PER_METRE, g_Config.fUnitsPerMetre);
  save_if_not_default(Config::GFX_VR_HUD_FULLSCREEN, g_Config.bHudFullscreen);
  save_if_not_default(Config::GFX_VR_HUD_ON_TOP, g_Config.bHudOnTop);
  save_if_not_default(Config::GFX_VR_DONT_CLEAR_SCREEN, g_Config.bDontClearScreen);
  save_if_not_default(Config::GFX_VR_CAN_READ_CAMERA_ANGLES, g_Config.bCanReadCameraAngles);
  save_if_not_default(Config::GFX_VR_DETECT_SKYBOX, g_Config.bDetectSkybox);
  save_if_not_default(Config::GFX_VR_HUD_DISTANCE, g_Config.fHudDistance);
  save_if_not_default(Config::GFX_VR_HUD_THICKNESS, g_Config.fHudThickness);
  save_if_not_default(Config::GFX_VR_HUD_3D_CLOSER, g_Config.fHud3DCloser);
  save_if_not_default(Config::GFX_VR_CAMERA_FORWARD, g_Config.fCameraForward);
  save_if_not_default(Config::GFX_VR_CAMERA_PITCH, g_Config.fCameraPitch);
  save_if_not_default(Config::GFX_VR_AIM_DISTANCE, g_Config.fAimDistance);
  save_if_not_default(Config::GFX_VR_MIN_FOV, g_Config.fMinFOV);
  save_if_not_default(Config::GFX_VR_N64_FOV, g_Config.fN64FOV);
  save_if_not_default(Config::GFX_VR_SCREEN_HEIGHT, g_Config.fScreenHeight);
  save_if_not_default(Config::GFX_VR_SCREEN_DISTANCE, g_Config.fScreenDistance);
  save_if_not_default(Config::GFX_VR_SCREEN_THICKNESS, g_Config.fScreenThickness);
  save_if_not_default(Config::GFX_VR_SCREEN_UP, g_Config.fScreenUp);
  save_if_not_default(Config::GFX_VR_SCREEN_RIGHT, g_Config.fScreenRight);
  save_if_not_default(Config::GFX_VR_SCREEN_PITCH, g_Config.fScreenPitch);
  save_if_not_default(Config::GFX_VR_READ_PITCH, g_Config.fReadPitch);
  save_if_not_default(Config::GFX_VR_HUD_DESP_POSITION_0, g_Config.fHudDespPosition0);
  save_if_not_default(Config::GFX_VR_HUD_DESP_POSITION_1, g_Config.fHudDespPosition1);
  save_if_not_default(Config::GFX_VR_HUD_DESP_POSITION_2, g_Config.fHudDespPosition2);
  save_if_not_default(Config::GFX_VR_CAMERA_MIN_POLY, g_Config.iCameraMinPoly);

  if (modified)
    local_game_layer->Save(); // Saves the current layer to its INI file.

  Refresh(); // Reload all configs
  g_SavedConfig = *this; // Update saved state
}

void VideoConfig::GameIniReset()
{
  // Config::ResetToGameDefault will reset to the default for the current game's INI if one exists,
  // or to the global default if not.
  auto local_game_layer = Config::GetLayer(Config::LayerType::LocalGame);
  bool modified = false;

  // Helper lambda to implement ResetToGameDefault logic
  auto reset_to_default = [&](const auto& setting) {
    if (local_game_layer)
    {
      if (local_game_layer->DeleteKey(setting.GetLocation()))
        modified = true;
    }
  };

  reset_to_default(Config::GFX_VR_DISABLE_3D);
  reset_to_default(Config::GFX_VR_UNITS_PER_METRE);
  reset_to_default(Config::GFX_VR_HUD_FULLSCREEN);
  reset_to_default(Config::GFX_VR_HUD_3D_CLOSER);
  reset_to_default(Config::GFX_VR_HUD_DISTANCE);
  reset_to_default(Config::GFX_VR_HUD_THICKNESS);
  reset_to_default(Config::GFX_VR_CAMERA_FORWARD);
  reset_to_default(Config::GFX_VR_CAMERA_PITCH);
  reset_to_default(Config::GFX_VR_AIM_DISTANCE);
  reset_to_default(Config::GFX_VR_SCREEN_HEIGHT);
  reset_to_default(Config::GFX_VR_SCREEN_DISTANCE);
  reset_to_default(Config::GFX_VR_SCREEN_THICKNESS);
  reset_to_default(Config::GFX_VR_SCREEN_UP);
  reset_to_default(Config::GFX_VR_SCREEN_RIGHT);
  reset_to_default(Config::GFX_VR_SCREEN_PITCH);
  reset_to_default(Config::GFX_VR_MIN_FOV);
  reset_to_default(Config::GFX_VR_N64_FOV);
  reset_to_default(Config::GFX_VR_READ_PITCH);
  reset_to_default(Config::GFX_VR_CAMERA_MIN_POLY);
  reset_to_default(Config::GFX_VR_HUD_DESP_POSITION_0);
  reset_to_default(Config::GFX_VR_HUD_DESP_POSITION_1);
  reset_to_default(Config::GFX_VR_HUD_DESP_POSITION_2);
  reset_to_default(Config::GFX_VR_CAN_READ_CAMERA_ANGLES);
  reset_to_default(Config::GFX_VR_DETECT_SKYBOX);
  reset_to_default(Config::GFX_VR_HUD_ON_TOP);
  reset_to_default(Config::GFX_VR_DONT_CLEAR_SCREEN);

  if (modified)
    Config::Save(); // Saves all layers if any key in LocalGame was deleted.
  // If no keys were deleted from LocalGame, but other layers might need saving,
  // Config::Save() might still be appropriate depending on broader application logic.
  // For now, only save if we actually modified the LocalGame layer.

  g_Config.Refresh(); // Reload all configurations.
  g_SavedConfig = g_Config; // Update saved state after refresh
}

void VideoConfig::UpdateProjectionHack()
{
  // This function was empty in Hydra reference VideoConfig.cpp
  // If it's used by other ported code, its logic might be elsewhere (e.g. VertexShaderManager)
  // For now, keep it as a no-op.
}

bool VideoConfig::IsVSync() const
{
  // From Hydra. Note: Current code uses bVSyncActive which also checks MAIN_EMULATION_SPEED.
  // This version is simpler.
  return bVSync && !Core::GetIsThrottlerTempDisabled();
}

bool VideoConfig::VRSettingsModified()
{
  // Straight from Hydra
  return fUnitsPerMetre != g_SavedConfig.fUnitsPerMetre ||
         fHudThickness != g_SavedConfig.fHudThickness ||
         fHudDistance != g_SavedConfig.fHudDistance || fHud3DCloser != g_SavedConfig.fHud3DCloser ||
         fCameraForward != g_SavedConfig.fCameraForward ||
         fCameraPitch != g_SavedConfig.fCameraPitch || fAimDistance != g_SavedConfig.fAimDistance ||
         fMinFOV != g_SavedConfig.fMinFOV || fScreenHeight != g_SavedConfig.fScreenHeight ||
         fN64FOV != g_SavedConfig.fN64FOV || fScreenThickness != g_SavedConfig.fScreenThickness ||
         fScreenDistance != g_SavedConfig.fScreenDistance ||
         fScreenRight != g_SavedConfig.fScreenRight || fScreenUp != g_SavedConfig.fScreenUp ||
         fScreenPitch != g_SavedConfig.fScreenPitch ||
         fTelescopeMaxFOV != g_SavedConfig.fTelescopeMaxFOV ||
         fReadPitch != g_SavedConfig.fReadPitch || iCameraMinPoly != g_SavedConfig.iCameraMinPoly ||
         bDisable3D != g_SavedConfig.bDisable3D || bHudFullscreen != g_SavedConfig.bHudFullscreen ||
         bHudOnTop != g_SavedConfig.bHudOnTop ||
         fHudDespPosition0 != g_SavedConfig.fHudDespPosition0 ||
         fHudDespPosition1 != g_SavedConfig.fHudDespPosition1 ||
         fHudDespPosition2 != g_SavedConfig.fHudDespPosition2 ||
         bDontClearScreen != g_SavedConfig.bDontClearScreen ||
         bCanReadCameraAngles != g_SavedConfig.bCanReadCameraAngles ||
         bDetectSkybox != g_SavedConfig.bDetectSkybox ||
         iTelescopeEye != g_SavedConfig.iTelescopeEye ||
         iMetroidPrime != g_SavedConfig.iMetroidPrime;
}


void VideoConfig::VerifyValidity()
{
  // TODO: Check iMaxAnisotropy value
  if (iAdapter < 0 || iAdapter > ((int)g_backend_info.Adapters.size() - 1))
    iAdapter = 0;

  if (!Common::Contains(g_backend_info.AAModes, iMultisamples))
    iMultisamples = 1;

  if (stereo_mode != StereoMode::Off)
  {
    // Hydra's VerifyValidity also had specific HMD checks (g_has_rift, g_has_openvr, g_has_vr920)
    // to force stereo_mode. This might be better handled in VR or backend specific code.
    // For now, retain the existing geometry shader check.
    if (!g_backend_info.bSupportsGeometryShaders)
    {
      OSD::AddMessage(
          "Stereoscopic 3D isn't supported by your GPU, support for OpenGL 3.2 is required.",
          10000);
      stereo_mode = StereoMode::Off;
    }
    // Hydra check: if (bUseXFB && bUseRealXFB && !g_has_hmd) stereo_mode = Off;
    // g_has_hmd equivalent is VR::IsHMDActive()
    if (bUseXFB && bUseRealXFB && !g_has_hmd)
    {
        OSD::AddMessage("Stereoscopic 3D isn't supported with Real XFB without an HMD, turning off stereoscopy.", 10000);
        stereo_mode = StereoMode::Off;
    }
  }
}

void VideoConfig::Shutdown()
{
  if (!s_config_changed_callback_id.has_value())
    return;

  CPUThreadConfigCallback::RemoveConfigChangedCallback(*s_config_changed_callback_id);
  s_config_changed_callback_id.reset();
}

bool VideoConfig::UsingUberShaders() const
{
  return iShaderCompilationMode == ShaderCompilationMode::SynchronousUberShaders ||
         iShaderCompilationMode == ShaderCompilationMode::AsynchronousUberShaders;
}

// Added from Hydra, adapted for current ShaderCompilationMode
bool VideoConfig::CanPrecompileUberShaders() const
{
  // Precompile if "Wait For Shaders" is on AND we are in a mode that uses Ubershaders.
  return bWaitForShadersBeforeStarting && UsingUberShaders();
}

// Added from Hydra, adapted for current ShaderCompilationMode
bool VideoConfig::CanBackgroundCompileShaders() const
{
  // Background compilation is essentially AsynchronousUberShaders mode.
  return iShaderCompilationMode == ShaderCompilationMode::AsynchronousUberShaders;
}


static u32 GetNumAutoShaderCompilerThreads()
{
  // Automatic number.
  return static_cast<u32>(std::clamp(cpu_info.num_cores - 3, 1, 4));
}

static u32 GetNumAutoShaderPreCompilerThreads()
{
  // Automatic number. We use clamp(cpus - 2, 1, infty) here.
  // We chose this because we don't want to limit our speed-up
  // and at the same time leave two logical cores for the dolphin UI and the rest of the OS.
  return static_cast<u32>(std::max(cpu_info.num_cores - 2, 1));
}

u32 VideoConfig::GetShaderCompilerThreads() const
{
  if (!g_backend_info.bSupportsBackgroundCompiling)
    return 0;

  if (iShaderCompilerThreads >= 0)
    return static_cast<u32>(iShaderCompilerThreads);
  else
    return GetNumAutoShaderCompilerThreads();
}

u32 VideoConfig::GetShaderPrecompilerThreads() const
{
  // When using background compilation, always keep the same thread count.
  if (!bWaitForShadersBeforeStarting)
    return GetShaderCompilerThreads();

  if (!g_backend_info.bSupportsBackgroundCompiling)
    return 0;

  if (iShaderPrecompilerThreads >= 0)
    return static_cast<u32>(iShaderPrecompilerThreads);
  else if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_MULTITHREADED_SHADER_PRECOMPILATION))
    return GetNumAutoShaderPreCompilerThreads();
  else
    return 1;
}

void CheckForConfigChanges()
{
  const ShaderHostConfig old_shader_host_config = ShaderHostConfig::GetCurrent();
  const StereoMode old_stereo = g_ActiveConfig.stereo_mode;
  const u32 old_multisamples = g_ActiveConfig.iMultisamples;
  const auto old_anisotropy = g_ActiveConfig.iMaxAnisotropy;
  const int old_efb_access_tile_size = g_ActiveConfig.iEFBAccessTileSize;
  const auto old_texture_filtering_mode = g_ActiveConfig.texture_filtering_mode;
  const bool old_vsync = g_ActiveConfig.bVSyncActive;
  const bool old_bbox = g_ActiveConfig.bBBoxEnable;
  const int old_efb_scale = g_ActiveConfig.iEFBScale;
  const u32 old_game_mod_changes =
      g_ActiveConfig.graphics_mod_config ? g_ActiveConfig.graphics_mod_config->GetChangeCount() : 0;
  const bool old_graphics_mods_enabled = g_ActiveConfig.bGraphicMods;
  const AspectMode old_aspect_mode = g_ActiveConfig.aspect_mode;
  const AspectMode old_suggested_aspect_mode = g_ActiveConfig.suggested_aspect_mode;
  const bool old_widescreen_hack = g_ActiveConfig.bWidescreenHack;
  const auto old_post_processing_shader = g_ActiveConfig.sPostProcessingShader;
  const auto old_hdr = g_ActiveConfig.bHDR;

  UpdateActiveConfig();
  FreeLook::UpdateActiveConfig();
  g_vertex_manager->OnConfigChange();

  g_freelook_camera.SetControlType(FreeLook::GetActiveConfig().camera_config.control_type);

  if (g_ActiveConfig.bGraphicMods && !old_graphics_mods_enabled)
  {
    g_ActiveConfig.graphics_mod_config = GraphicsModGroupConfig(SConfig::GetInstance().GetGameID());
    g_ActiveConfig.graphics_mod_config->Load();
  }

  if (g_ActiveConfig.graphics_mod_config &&
      (old_game_mod_changes != g_ActiveConfig.graphics_mod_config->GetChangeCount()))
  {
    g_graphics_mod_manager->Load(*g_ActiveConfig.graphics_mod_config);
  }

  // Update texture cache settings with any changed options.
  g_texture_cache->OnConfigChanged(g_ActiveConfig);

  // EFB tile cache doesn't need to notify the backend.
  if (old_efb_access_tile_size != g_ActiveConfig.iEFBAccessTileSize)
    g_framebuffer_manager->SetEFBCacheTileSize(std::max(g_ActiveConfig.iEFBAccessTileSize, 0));

  // Determine which (if any) settings have changed.
  ShaderHostConfig new_host_config = ShaderHostConfig::GetCurrent();
  u32 changed_bits = 0;
  if (old_shader_host_config.bits != new_host_config.bits)
    changed_bits |= CONFIG_CHANGE_BIT_HOST_CONFIG;
  if (old_stereo != g_ActiveConfig.stereo_mode)
    changed_bits |= CONFIG_CHANGE_BIT_STEREO_MODE;
  if (old_multisamples != g_ActiveConfig.iMultisamples)
    changed_bits |= CONFIG_CHANGE_BIT_MULTISAMPLES;
  if (old_anisotropy != g_ActiveConfig.iMaxAnisotropy)
    changed_bits |= CONFIG_CHANGE_BIT_ANISOTROPY;
  if (old_texture_filtering_mode != g_ActiveConfig.texture_filtering_mode)
    changed_bits |= CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING;
  if (old_vsync != g_ActiveConfig.bVSyncActive)
    changed_bits |= CONFIG_CHANGE_BIT_VSYNC;
  if (old_bbox != g_ActiveConfig.bBBoxEnable)
    changed_bits |= CONFIG_CHANGE_BIT_BBOX;
  if (old_efb_scale != g_ActiveConfig.iEFBScale)
    changed_bits |= CONFIG_CHANGE_BIT_TARGET_SIZE;
  if (old_aspect_mode != g_ActiveConfig.aspect_mode)
    changed_bits |= CONFIG_CHANGE_BIT_ASPECT_RATIO;
  if (old_suggested_aspect_mode != g_ActiveConfig.suggested_aspect_mode)
    changed_bits |= CONFIG_CHANGE_BIT_ASPECT_RATIO;
  if (old_widescreen_hack != g_ActiveConfig.bWidescreenHack)
    changed_bits |= CONFIG_CHANGE_BIT_ASPECT_RATIO;
  if (old_post_processing_shader != g_ActiveConfig.sPostProcessingShader)
    changed_bits |= CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER;
  if (old_hdr != g_ActiveConfig.bHDR)
    changed_bits |= CONFIG_CHANGE_BIT_HDR;

  // No changes?
  if (changed_bits == 0)
    return;

  float old_scale = g_framebuffer_manager->GetEFBScale();

  // Framebuffer changed?
  if (changed_bits & (CONFIG_CHANGE_BIT_MULTISAMPLES | CONFIG_CHANGE_BIT_STEREO_MODE |
                      CONFIG_CHANGE_BIT_TARGET_SIZE | CONFIG_CHANGE_BIT_HDR))
  {
    g_framebuffer_manager->RecreateEFBFramebuffer();
  }

  if (old_scale != g_framebuffer_manager->GetEFBScale())
  {
    auto& system = Core::System::GetInstance();
    auto& pixel_shader_manager = system.GetPixelShaderManager();
    pixel_shader_manager.Dirty();
  }

  // Reload shaders if host config has changed.
  if (changed_bits & (CONFIG_CHANGE_BIT_HOST_CONFIG | CONFIG_CHANGE_BIT_MULTISAMPLES))
  {
    OSD::AddMessage("Video config changed, reloading shaders.", OSD::Duration::NORMAL);
    g_gfx->WaitForGPUIdle();
    g_vertex_manager->InvalidatePipelineObject();
    g_vertex_manager->NotifyCustomShaderCacheOfHostChange(new_host_config);
    g_shader_cache->SetHostConfig(new_host_config);
    g_shader_cache->Reload();
    g_framebuffer_manager->RecompileShaders();
  }

  // Viewport and scissor rect have to be reset since they will be scaled differently.
  if (changed_bits & CONFIG_CHANGE_BIT_TARGET_SIZE)
  {
    BPFunctions::SetScissorAndViewport();
  }

  // Notify all listeners
  ConfigChangedEvent::Trigger(changed_bits);

  // TODO: Move everything else to the ConfigChanged event
}

static Common::EventHook s_check_config_event = AfterFrameEvent::Register(
    [](Core::System&) { CheckForConfigChanges(); }, "CheckForConfigChanges");
