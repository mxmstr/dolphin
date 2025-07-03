// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VideoConfig.h"

#include <algorithm>
#include <optional>

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

#include "VideoCommon/VR.h" // For DEFAULT_VR_... constants

VideoConfig g_Config;
VideoConfig g_ActiveConfig;
BackendInfo g_backend_info;
static std::optional<CPUThreadConfigCallback::ConfigChangedCallbackID>
    s_config_changed_callback_id = std::nullopt;

// VR settings save tracking
static VideoConfig g_SavedConfig; // Ported from VR-Hydra

static bool IsVSyncActive(bool enabled)
{
  // Vsync is disabled when the throttler is disabled by the tab key.
  return enabled && !Core::GetIsThrottlerTempDisabled() &&
         Config::Get(Config::MAIN_EMULATION_SPEED) == 1.0;
}

void UpdateActiveConfig()
{
  g_ActiveConfig = g_Config;
  g_ActiveConfig.bVSyncActive = IsVSyncActive(g_ActiveConfig.bVSync);
  if (g_has_hmd && g_ActiveConfig.bEnableVR) // Ported from VR-Hydra
    g_ActiveConfig.bUseRealXFB = false; // RealXFB is generally not compatible with VR rendering paths
}


VideoConfig::VideoConfig() // Constructor to initialize defaults
{
  // Default existing non-VR settings (most are default-constructed by their types or set in Refresh)
  fAspectRatioHackW = 1.0f; // From VR-Hydra's constructor
  fAspectRatioHackH = 1.0f; // From VR-Hydra's constructor

  // Initialize VR settings to their defaults (ported from VR-Hydra VideoConfig constructor)
  fScale = 1.0f;
  fLeanBackAngle = 0.0f;
  bStabilizePitch = true;
  bStabilizeRoll = true;
  bStabilizeYaw = false;
  bStabilizeX = false;
  bStabilizeY = false;
  bStabilizeZ = false;
  bKeyhole = false;
  fKeyholeWidth = 45.0f;
  bKeyholeSnap = false;
  fKeyholeSnapSize = 30.0f;
  bPullUp20fps = false;
  bPullUp30fps = false;
  bPullUp60fps = false;
  bPullUpAuto = false;
  bSynchronousTimewarp = false;
  bOpcodeWarningDisable = false;
  bReplayVertexData = false;
  bReplayOtherData = false;
  bPullUp20fpsTimewarp = false;
  bPullUp30fpsTimewarp = false;
  bPullUp60fpsTimewarp = false;
  bPullUpAutoTimewarp = false;
  bOpcodeReplay = false;
  bEnableVR = true;
  // bAsynchronousTimewarp = false; // This was in Hydra's ctor but not in its .h, likely Oculus SDK specific, omitting for now
  bLowPersistence = true;
  bDynamicPrediction = true;
  bOrientationTracking = true;
  bMagYawCorrection = true;
  bPositionTracking = true;
  bChromatic = true;
  bTimewarp = true;
  bVignette = false;
  bNoRestore = false;
  bFlipVertical = false;
  bSRGB = false;
  bOverdrive = true;
  bHqDistortion = false;
  bDisableNearClipping = true;
  bAutoPairViveControllers = false;
  bShowHands = false;
  bShowFeet = false;
  bShowController = true; // Defaulted to true in Hydra
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
  bMotionSicknessIR = false; // Added this, was missing in Hydra constructor but present in header/logic
  iMotionSicknessMethod = 0;
  iMotionSicknessSkybox = 0;
  fMotionSicknessFOV = 45.0f; // Default from Hydra

  iVRPlayer = VR_PLAYER1; // VR_PLAYER1 will be defined in VR.h
  iVRPlayer2 = VR_PLAYER2; // VR_PLAYER2 will be defined in VR.h
  iMirrorPlayer = VR_PLAYER_DEFAULT; // VR_PLAYER_DEFAULT will be defined in VR.h
  iMirrorStyle = VR_MIRROR_LEFT;   // VR_MIRROR_LEFT will be defined in VR.h
  fTimeWarpTweak = DEFAULT_VR_TIMEWARP_TWEAK;
  iExtraTimewarpedFrames = DEFAULT_VR_EXTRA_FRAMES;
  iExtraVideoLoops = DEFAULT_VR_EXTRA_VIDEO_LOOPS;
  iExtraVideoLoopsDivider = DEFAULT_VR_EXTRA_VIDEO_LOOPS_DIVIDER;

  sLeftTexture = "";
  sRightTexture = "";
  sGCLeftTexture = "";
  sGCRightTexture = "";

  // VR per-game defaults
  fUnitsPerMetre = DEFAULT_VR_UNITS_PER_METRE;
  fFreeLookSensitivity = DEFAULT_VR_FREE_LOOK_SENSITIVITY;
  fHudThickness = DEFAULT_VR_HUD_THICKNESS;
  fHudDistance = DEFAULT_VR_HUD_DISTANCE;
  fHud3DCloser = DEFAULT_VR_HUD_3D_CLOSER;
  fCameraForward = DEFAULT_VR_CAMERA_FORWARD;
  fCameraPitch = DEFAULT_VR_CAMERA_PITCH;
  fAimDistance = DEFAULT_VR_AIM_DISTANCE;
  fMinFOV = DEFAULT_VR_MIN_FOV;
  fN64FOV = DEFAULT_VR_N64_FOV;
  fScreenHeight = DEFAULT_VR_SCREEN_HEIGHT;
  fScreenThickness = DEFAULT_VR_HUD_THICKNESS; // Uses HUD thickness default
  fScreenDistance = DEFAULT_VR_SCREEN_DISTANCE;
  fScreenRight = DEFAULT_VR_SCREEN_RIGHT;
  fScreenUp = DEFAULT_VR_SCREEN_UP;
  fScreenPitch = DEFAULT_VR_SCREEN_PITCH;
  iMetroidPrime = 0;
  iTelescopeEye = 0;
  fTelescopeMaxFOV = 0.0f;
  fReadPitch = 0.0f;
  iCameraMinPoly = 0;
  fHudDespPosition0 = 0.0f;
  fHudDespPosition1 = 0.0f;
  fHudDespPosition2 = 0.0f;
  // matrixHudrot is skipped as it's complex for config

  // VR layer debugging
  iSelectedLayer = -2; // Default from Hydra
  iFlashState = 0;     // Default from Hydra
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

  bGraphicMods = Config::Get(Config::GFX_MODS_ENABLE);

  customDriverLibraryName = Config::Get(Config::GFX_DRIVER_LIB_NAME);

  vertex_loader_type = Config::Get(Config::GFX_VERTEX_LOADER_TYPE);

  // Load VR Per-Game Settings
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

  // Load Global VR Settings
  fScale = Config::Get(Config::GLOBAL_VR_SCALE);
  fFreeLookSensitivity = Config::Get(Config::GLOBAL_VR_FREE_LOOK_SENSITIVITY);
  fLeanBackAngle = Config::Get(Config::GLOBAL_VR_LEAN_BACK_ANGLE);
  bEnableVR = Config::Get(Config::GLOBAL_VR_ENABLE_VR);
  bLowPersistence = Config::Get(Config::GLOBAL_VR_LOW_PERSISTENCE);
  bDynamicPrediction = Config::Get(Config::GLOBAL_VR_DYNAMIC_PREDICTION);
  bool bNoMirrorToWindow = Config::Get(Config::GLOBAL_VR_NO_MIRROR_TO_WINDOW); // Temp variable for logic below
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
  bShow2DBox = Config::Get(Config::GLOBAL_VR_SHOW_2D_SCREEN_BOX);
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
  //if (bNoMirrorToWindow && iMirrorStyle != VR_MIRROR_DISABLED_POWER_SAVING) // VR-Hydra logic for default
  //    iMirrorStyle = VR_MIRROR_DISABLED;

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

  // EFB Scale handling from VR-Hydra (iInternalResolution was GFX_INTERNAL_RESOLUTION)
  // This needs to be coordinated with how GFX_INTERNAL_RESOLUTION is handled in GraphicsSettings.
  // For now, this specific logic is commented out as VR-Reloaded-4 uses iEFBScale directly.
  // if (Config::Get(Config::GFX_INTERNAL_RESOLUTION) > -2) // Placeholder for actual GFX_INTERNAL_RESOLUTION access
  // {
  //    int internalResolution = Config::Get(Config::GFX_INTERNAL_RESOLUTION); // Placeholder
  //    iEFBScale = internalResolution + (internalResolution >= 0) + (internalResolution > 1) + (internalResolution > 2);
  // }
  // if (iEFBScale == SCALE_FORCE_INTEGRAL) // SCALE_FORCE_INTEGRAL needs to be defined
  // {
  //     // Round down to multiple of native IR (logic from VR-Hydra)
  //     const auto base_efb_scale = Config::GetBase(Config::GFX_EFB_SCALE); // This function needs to exist or be adapted
  //     if (base_efb_scale == EFBScale::SCALE_AUTO) iEFBScale = EFB_SCALE_AUTO_INTEGRAL; // Needs EFBScale enum
  //     else if (base_efb_scale == EFBScale::SCALE_1_5X) iEFBScale = 1; // Assuming iEFBScale is int factor
  //     else if (base_efb_scale == EFBScale::SCALE_2_5X) iEFBScale = 2;
  //     else iEFBScale = base_efb_scale; // Needs mapping if EFBScale enum values are not direct int factors
  // }
}

void VideoConfig::GameIniSave()
{
  // Save game ini for VR settings
  // OSD::AddMessage("Saving VR GameINI", 5000); // For debugging
  if (!Config::LayerExists(Config::LayerType::LocalGame))
    return;

  Config::SaveIfNotDefault(Config::GFX_VR_DISABLE_3D);
  Config::SaveIfNotDefault(Config::GFX_VR_UNITS_PER_METRE);
  Config::SaveIfNotDefault(Config::GFX_VR_HUD_FULLSCREEN);
  Config::SaveIfNotDefault(Config::GFX_VR_HUD_ON_TOP);
  Config::SaveIfNotDefault(Config::GFX_VR_DONT_CLEAR_SCREEN);
  Config::SaveIfNotDefault(Config::GFX_VR_CAN_READ_CAMERA_ANGLES);
  Config::SaveIfNotDefault(Config::GFX_VR_DETECT_SKYBOX);
  Config::SaveIfNotDefault(Config::GFX_VR_HUD_DISTANCE);
  Config::SaveIfNotDefault(Config::GFX_VR_HUD_THICKNESS);
  Config::SaveIfNotDefault(Config::GFX_VR_HUD_3D_CLOSER);
  Config::SaveIfNotDefault(Config::GFX_VR_CAMERA_FORWARD);
  Config::SaveIfNotDefault(Config::GFX_VR_CAMERA_PITCH);
  Config::SaveIfNotDefault(Config::GFX_VR_AIM_DISTANCE);
  Config::SaveIfNotDefault(Config::GFX_VR_MIN_FOV);
  Config::SaveIfNotDefault(Config::GFX_VR_N64_FOV);
  Config::SaveIfNotDefault(Config::GFX_VR_SCREEN_HEIGHT);
  Config::SaveIfNotDefault(Config::GFX_VR_SCREEN_DISTANCE);
  Config::SaveIfNotDefault(Config::GFX_VR_SCREEN_THICKNESS);
  Config::SaveIfNotDefault(Config::GFX_VR_SCREEN_UP);
  Config::SaveIfNotDefault(Config::GFX_VR_SCREEN_RIGHT);
  Config::SaveIfNotDefault(Config::GFX_VR_SCREEN_PITCH);
  Config::SaveIfNotDefault(Config::GFX_VR_READ_PITCH);
  Config::SaveIfNotDefault(Config::GFX_VR_HUD_DESP_POSITION_0);
  Config::SaveIfNotDefault(Config::GFX_VR_HUD_DESP_POSITION_1);
  Config::SaveIfNotDefault(Config::GFX_VR_HUD_DESP_POSITION_2);
  Config::SaveIfNotDefault(Config::GFX_VR_CAMERA_MIN_POLY);
  Config::SaveIfNotDefault(Config::GFX_VR_METROID_PRIME); // Save game specific type
  Config::SaveIfNotDefault(Config::GFX_VR_TELESCOPE_EYE);
  Config::SaveIfNotDefault(Config::GFX_VR_TELESCOPE_MAX_FOV);

  Config::Layer* local = Config::GetLayer(Config::LayerType::LocalGame);
  if (local)
    local->Save();
  Refresh(); // Refresh g_Config
  g_SavedConfig = g_Config; // Update g_SavedConfig with the newly saved values
}

void VideoConfig::GameIniReset()
{
  // OSD::AddMessage("Resetting VR GameINI", 5000); // For debugging
  Config::ResetToGameDefault(Config::GFX_VR_DISABLE_3D);
  Config::ResetToGameDefault(Config::GFX_VR_UNITS_PER_METRE);
  Config::ResetToGameDefault(Config::GFX_VR_HUD_FULLSCREEN);
  Config::ResetToGameDefault(Config::GFX_VR_HUD_ON_TOP);
  Config::ResetToGameDefault(Config::GFX_VR_DONT_CLEAR_SCREEN);
  Config::ResetToGameDefault(Config::GFX_VR_CAN_READ_CAMERA_ANGLES);
  Config::ResetToGameDefault(Config::GFX_VR_DETECT_SKYBOX);
  Config::ResetToGameDefault(Config::GFX_VR_HUD_DISTANCE);
  Config::ResetToGameDefault(Config::GFX_VR_HUD_THICKNESS);
  Config::ResetToGameDefault(Config::GFX_VR_HUD_3D_CLOSER);
  Config::ResetToGameDefault(Config::GFX_VR_CAMERA_FORWARD);
  Config::ResetToGameDefault(Config::GFX_VR_CAMERA_PITCH);
  Config::ResetToGameDefault(Config::GFX_VR_AIM_DISTANCE);
  Config::ResetToGameDefault(Config::GFX_VR_MIN_FOV);
  Config::ResetToGameDefault(Config::GFX_VR_N64_FOV);
  Config::ResetToGameDefault(Config::GFX_VR_SCREEN_HEIGHT);
  Config::ResetToGameDefault(Config::GFX_VR_SCREEN_DISTANCE);
  Config::ResetToGameDefault(Config::GFX_VR_SCREEN_THICKNESS);
  Config::ResetToGameDefault(Config::GFX_VR_SCREEN_UP);
  Config::ResetToGameDefault(Config::GFX_VR_SCREEN_RIGHT);
  Config::ResetToGameDefault(Config::GFX_VR_SCREEN_PITCH);
  Config::ResetToGameDefault(Config::GFX_VR_READ_PITCH);
  Config::ResetToGameDefault(Config::GFX_VR_HUD_DESP_POSITION_0);
  Config::ResetToGameDefault(Config::GFX_VR_HUD_DESP_POSITION_1);
  Config::ResetToGameDefault(Config::GFX_VR_HUD_DESP_POSITION_2);
  Config::ResetToGameDefault(Config::GFX_VR_CAMERA_MIN_POLY);
  Config::ResetToGameDefault(Config::GFX_VR_METROID_PRIME);
  Config::ResetToGameDefault(Config::GFX_VR_TELESCOPE_EYE);
  Config::ResetToGameDefault(Config::GFX_VR_TELESCOPE_MAX_FOV);

  Config::Save(); // Save the overall config after resetting game specific to their defaults
  Refresh(); // Refresh g_Config
  g_SavedConfig = g_Config; // Update g_SavedConfig
}

bool VideoConfig::VRSettingsModified()
{
    // Compare current g_Config (which reflects UI changes) with g_SavedConfig (last explicitly saved game ini state)
    // This is slightly different from Hydra which compared g_ActiveConfig.
    // Using g_Config here makes more sense if we want to prompt before game INI save.
    return fUnitsPerMetre != g_SavedConfig.fUnitsPerMetre ||
           fHudThickness != g_SavedConfig.fHudThickness ||
           fHudDistance != g_SavedConfig.fHudDistance ||
           fHud3DCloser != g_SavedConfig.fHud3DCloser ||
           fCameraForward != g_SavedConfig.fCameraForward ||
           fCameraPitch != g_SavedConfig.fCameraPitch ||
           fAimDistance != g_SavedConfig.fAimDistance ||
           fMinFOV != g_SavedConfig.fMinFOV ||
           fScreenHeight != g_SavedConfig.fScreenHeight ||
           fN64FOV != g_SavedConfig.fN64FOV ||
           fScreenThickness != g_SavedConfig.fScreenThickness ||
           fScreenDistance != g_SavedConfig.fScreenDistance ||
           fScreenRight != g_SavedConfig.fScreenRight ||
           fScreenUp != g_SavedConfig.fScreenUp ||
           fScreenPitch != g_SavedConfig.fScreenPitch ||
           fTelescopeMaxFOV != g_SavedConfig.fTelescopeMaxFOV ||
           fReadPitch != g_SavedConfig.fReadPitch ||
           iCameraMinPoly != g_SavedConfig.iCameraMinPoly ||
           bDisable3D != g_SavedConfig.bDisable3D ||
           bHudFullscreen != g_SavedConfig.bHudFullscreen ||
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

  // VR-Hydra stereo mode logic
  if (g_has_hmd && bEnableVR) // g_has_hmd will be from VR.h
  {
    // TODO: This assumes STEREO_OCULUS is the desired mode for all HMDs.
    // VR-Hydra had specific checks for g_has_rift, g_has_openvr, g_has_vr920.
    // For now, if any HMD is present and VR enabled, try to set a VR stereo mode.
    // The actual stereo_mode used by the backend might depend on VR.cpp initialization.
    stereo_mode = StereoMode::QuadBuffer; // Or another suitable default VR stereo mode, QuadBuffer is often used for OpenVR/OpenGL
                                          // VR-Hydra used STEREO_OCULUS enum value.
                                          // The actual active VR SDK might override this later in VR specific code.
  }
   else if (stereo_mode == StereoMode::QuadBuffer) // If VR disabled or no HMD, turn off VR stereo modes
  {
      stereo_mode = StereoMode::Off;
  }


  if (stereo_mode != StereoMode::Off)
  {
    if (!g_backend_info.bSupportsGeometryShaders && stereo_mode != StereoMode::Anaglyph) // Anaglyph might not need GS
    {
      OSD::AddMessage(
          "Stereoscopic 3D (excluding Anaglyph) isn't supported by your GPU, support for Geometry Shaders is required.",
          10000);
      stereo_mode = StereoMode::Off;
    }
    // VR-Hydra check: if (bUseXFB && bUseRealXFB && !g_has_hmd)
    // This needs to be adapted. RealXFB is generally incompatible with advanced rendering / VR.
    // Current g_ActiveConfig.bUseRealXFB is already set to false if HMD is present.
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
