// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/GraphicsSettings.h"

#include <string>

#include "Common/Config/Config.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR.h" // For VR default values, will be created later

namespace Config
{
// Configuration Information

// Graphics.Hardware

const Info<bool> GFX_VSYNC{{System::GFX, "Hardware", "VSync"}, false};
const Info<int> GFX_ADAPTER{{System::GFX, "Hardware", "Adapter"}, 0};

// Graphics.Settings

const Info<bool> GFX_WIDESCREEN_HACK{{System::GFX, "Settings", "wideScreenHack"}, false};
const Info<AspectMode> GFX_ASPECT_RATIO{{System::GFX, "Settings", "AspectRatio"}, AspectMode::Auto};
const Info<int> GFX_CUSTOM_ASPECT_RATIO_WIDTH{{System::GFX, "Settings", "CustomAspectRatioWidth"},
                                              1};
const Info<int> GFX_CUSTOM_ASPECT_RATIO_HEIGHT{{System::GFX, "Settings", "CustomAspectRatioHeight"},
                                               1};
const Info<AspectMode> GFX_SUGGESTED_ASPECT_RATIO{{System::GFX, "Settings", "SuggestedAspectRatio"},
                                                  AspectMode::Auto};
const Info<u32> GFX_WIDESCREEN_HEURISTIC_TRANSITION_THRESHOLD{
    {System::GFX, "Settings", "WidescreenHeuristicTransitionThreshold"}, 3};
const Info<float> GFX_WIDESCREEN_HEURISTIC_ASPECT_RATIO_SLOP{
    {System::GFX, "Settings", "WidescreenHeuristicAspectRatioSlop"}, 0.11f};
const Info<float> GFX_WIDESCREEN_HEURISTIC_STANDARD_RATIO{
    {System::GFX, "Settings", "WidescreenHeuristicStandardRatio"}, 1.f};
const Info<float> GFX_WIDESCREEN_HEURISTIC_WIDESCREEN_RATIO{
    {System::GFX, "Settings", "WidescreenHeuristicWidescreenRatio"}, (16 / 9.f) / (4 / 3.f)};
const Info<bool> GFX_CROP{{System::GFX, "Settings", "Crop"}, false};
const Info<int> GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES{
    {System::GFX, "Settings", "SafeTextureCacheColorSamples"}, 128};
const Info<bool> GFX_SHOW_FPS{{System::GFX, "Settings", "ShowFPS"}, false};
const Info<bool> GFX_SHOW_FTIMES{{System::GFX, "Settings", "ShowFTimes"}, false};
const Info<bool> GFX_SHOW_VPS{{System::GFX, "Settings", "ShowVPS"}, false};
const Info<bool> GFX_SHOW_VTIMES{{System::GFX, "Settings", "ShowVTimes"}, false};
const Info<bool> GFX_SHOW_GRAPHS{{System::GFX, "Settings", "ShowGraphs"}, false};
const Info<bool> GFX_SHOW_SPEED{{System::GFX, "Settings", "ShowSpeed"}, false};
const Info<bool> GFX_SHOW_SPEED_COLORS{{System::GFX, "Settings", "ShowSpeedColors"}, true};
const Info<bool> GFX_MOVABLE_PERFORMANCE_METRICS{
    {System::GFX, "Settings", "MovablePerformanceMetrics"}, false};
const Info<int> GFX_PERF_SAMP_WINDOW{{System::GFX, "Settings", "PerfSampWindowMS"}, 1000};
const Info<bool> GFX_SHOW_NETPLAY_PING{{System::GFX, "Settings", "ShowNetPlayPing"}, false};
const Info<bool> GFX_SHOW_NETPLAY_MESSAGES{{System::GFX, "Settings", "ShowNetPlayMessages"}, false};
const Info<bool> GFX_LOG_RENDER_TIME_TO_FILE{{System::GFX, "Settings", "LogRenderTimeToFile"},
                                             false};
const Info<bool> GFX_OVERLAY_STATS{{System::GFX, "Settings", "OverlayStats"}, false};
const Info<bool> GFX_OVERLAY_PROJ_STATS{{System::GFX, "Settings", "OverlayProjStats"}, false};
const Info<bool> GFX_OVERLAY_SCISSOR_STATS{{System::GFX, "Settings", "OverlayScissorStats"}, false};
const Info<bool> GFX_DUMP_TEXTURES{{System::GFX, "Settings", "DumpTextures"}, false};
const Info<bool> GFX_DUMP_MIP_TEXTURES{{System::GFX, "Settings", "DumpMipTextures"}, true};
const Info<bool> GFX_DUMP_BASE_TEXTURES{{System::GFX, "Settings", "DumpBaseTextures"}, true};
const Info<int> GFX_TEXTURE_PNG_COMPRESSION_LEVEL{
    {System::GFX, "Settings", "TexturePNGCompressionLevel"}, 6};
const Info<bool> GFX_HIRES_TEXTURES{{System::GFX, "Settings", "HiresTextures"}, false};
const Info<bool> GFX_CACHE_HIRES_TEXTURES{{System::GFX, "Settings", "CacheHiresTextures"}, false};
const Info<bool> GFX_DUMP_EFB_TARGET{{System::GFX, "Settings", "DumpEFBTarget"}, false};
const Info<bool> GFX_DUMP_XFB_TARGET{{System::GFX, "Settings", "DumpXFBTarget"}, false};
const Info<bool> GFX_DUMP_FRAMES_AS_IMAGES{{System::GFX, "Settings", "DumpFramesAsImages"}, false};
const Info<bool> GFX_USE_LOSSLESS{{System::GFX, "Settings", "UseLossless"}, false};
const Info<std::string> GFX_DUMP_FORMAT{{System::GFX, "Settings", "DumpFormat"}, "avi"};
const Info<std::string> GFX_DUMP_CODEC{{System::GFX, "Settings", "DumpCodec"}, ""};
const Info<std::string> GFX_DUMP_PIXEL_FORMAT{{System::GFX, "Settings", "DumpPixelFormat"}, ""};
const Info<std::string> GFX_DUMP_ENCODER{{System::GFX, "Settings", "DumpEncoder"}, ""};
const Info<std::string> GFX_DUMP_PATH{{System::GFX, "Settings", "DumpPath"}, ""};
const Info<int> GFX_BITRATE_KBPS{{System::GFX, "Settings", "BitrateKbps"}, 25000};
const Info<FrameDumpResolutionType> GFX_FRAME_DUMPS_RESOLUTION_TYPE{
    {System::GFX, "Settings", "FrameDumpsResolutionType"},
    FrameDumpResolutionType::XFBAspectRatioCorrectedResolution};
const Info<int> GFX_PNG_COMPRESSION_LEVEL{{System::GFX, "Settings", "PNGCompressionLevel"}, 6};
const Info<bool> GFX_ENABLE_GPU_TEXTURE_DECODING{
    {System::GFX, "Settings", "EnableGPUTextureDecoding"}, false};
const Info<bool> GFX_ENABLE_PIXEL_LIGHTING{{System::GFX, "Settings", "EnablePixelLighting"}, false};
const Info<bool> GFX_FAST_DEPTH_CALC{{System::GFX, "Settings", "FastDepthCalc"}, true};
const Info<u32> GFX_MSAA{{System::GFX, "Settings", "MSAA"}, 1};
const Info<bool> GFX_SSAA{{System::GFX, "Settings", "SSAA"}, false};
const Info<int> GFX_EFB_SCALE{{System::GFX, "Settings", "InternalResolution"}, 1};
const Info<int> GFX_MAX_EFB_SCALE{{System::GFX, "Settings", "MaxInternalResolution"}, 12};
const Info<bool> GFX_TEXFMT_OVERLAY_ENABLE{{System::GFX, "Settings", "TexFmtOverlayEnable"}, false};
const Info<bool> GFX_TEXFMT_OVERLAY_CENTER{{System::GFX, "Settings", "TexFmtOverlayCenter"}, false};
const Info<bool> GFX_ENABLE_WIREFRAME{{System::GFX, "Settings", "WireFrame"}, false};
const Info<bool> GFX_DISABLE_FOG{{System::GFX, "Settings", "DisableFog"}, false};
const Info<bool> GFX_BORDERLESS_FULLSCREEN{{System::GFX, "Settings", "BorderlessFullscreen"},
                                           false};
const Info<bool> GFX_ENABLE_VALIDATION_LAYER{{System::GFX, "Settings", "EnableValidationLayer"},
                                             false};

const Info<bool> GFX_BACKEND_MULTITHREADING{{System::GFX, "Settings", "BackendMultithreading"},
                                            true};
const Info<int> GFX_COMMAND_BUFFER_EXECUTE_INTERVAL{
    {System::GFX, "Settings", "CommandBufferExecuteInterval"}, 100};

const Info<bool> GFX_SHADER_CACHE{{System::GFX, "Settings", "ShaderCache"}, true};
const Info<bool> GFX_WAIT_FOR_SHADERS_BEFORE_STARTING{
    {System::GFX, "Settings", "WaitForShadersBeforeStarting"}, false};
const Info<ShaderCompilationMode> GFX_SHADER_COMPILATION_MODE{
    {System::GFX, "Settings", "ShaderCompilationMode"}, ShaderCompilationMode::Synchronous};
const Info<int> GFX_SHADER_COMPILER_THREADS{{System::GFX, "Settings", "ShaderCompilerThreads"}, 1};
const Info<int> GFX_SHADER_PRECOMPILER_THREADS{
    {System::GFX, "Settings", "ShaderPrecompilerThreads"}, -1};
const Info<bool> GFX_SAVE_TEXTURE_CACHE_TO_STATE{
    {System::GFX, "Settings", "SaveTextureCacheToState"}, true};
const Info<bool> GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION{
    {System::GFX, "Settings", "PreferVSForLinePointExpansion"}, false};
const Info<bool> GFX_CPU_CULL{{System::GFX, "Settings", "CPUCull"}, false};

const Info<TriState> GFX_MTL_MANUALLY_UPLOAD_BUFFERS{
    {System::GFX, "Settings", "ManuallyUploadBuffers"}, TriState::Auto};
const Info<TriState> GFX_MTL_USE_PRESENT_DRAWABLE{
    {System::GFX, "Settings", "MTLUsePresentDrawable"}, TriState::Auto};

const Info<bool> GFX_SW_DUMP_OBJECTS{{System::GFX, "Settings", "SWDumpObjects"}, false};
const Info<bool> GFX_SW_DUMP_TEV_STAGES{{System::GFX, "Settings", "SWDumpTevStages"}, false};
const Info<bool> GFX_SW_DUMP_TEV_TEX_FETCHES{{System::GFX, "Settings", "SWDumpTevTexFetches"},
                                             false};

const Info<bool> GFX_PREFER_GLES{{System::GFX, "Settings", "PreferGLES"}, false};

const Info<bool> GFX_MODS_ENABLE{{System::GFX, "Settings", "EnableMods"}, false};

const Info<std::string> GFX_DRIVER_LIB_NAME{{System::GFX, "Settings", "DriverLibName"}, ""};

const Info<VertexLoaderType> GFX_VERTEX_LOADER_TYPE{{System::GFX, "Settings", "VertexLoaderType"},
                                                    VertexLoaderType::Native};

// Graphics.Enhancements

const Info<TextureFilteringMode> GFX_ENHANCE_FORCE_TEXTURE_FILTERING{
    {System::GFX, "Enhancements", "ForceTextureFiltering"}, TextureFilteringMode::Default};
const Info<AnisotropicFilteringMode> GFX_ENHANCE_MAX_ANISOTROPY{
    {System::GFX, "Enhancements", "MaxAnisotropy"}, AnisotropicFilteringMode::Default};
const Info<OutputResamplingMode> GFX_ENHANCE_OUTPUT_RESAMPLING{
    {System::GFX, "Enhancements", "OutputResampling"}, OutputResamplingMode::Default};
const Info<std::string> GFX_ENHANCE_POST_SHADER{
    {System::GFX, "Enhancements", "PostProcessingShader"}, ""};
const Info<bool> GFX_ENHANCE_FORCE_TRUE_COLOR{{System::GFX, "Enhancements", "ForceTrueColor"},
                                              true};
const Info<bool> GFX_ENHANCE_DISABLE_COPY_FILTER{{System::GFX, "Enhancements", "DisableCopyFilter"},
                                                 true};
const Info<bool> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION{
    {System::GFX, "Enhancements", "ArbitraryMipmapDetection"}, false};
const Info<float> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD{
    {System::GFX, "Enhancements", "ArbitraryMipmapDetectionThreshold"}, 14.0f};
const Info<bool> GFX_ENHANCE_HDR_OUTPUT{{System::GFX, "Enhancements", "HDROutput"}, false};

// Color.Correction

const Info<bool> GFX_CC_CORRECT_COLOR_SPACE{{System::GFX, "ColorCorrection", "CorrectColorSpace"},
                                            false};
const Info<ColorCorrectionRegion> GFX_CC_GAME_COLOR_SPACE{
    {System::GFX, "ColorCorrection", "GameColorSpace"}, ColorCorrectionRegion::SMPTE_NTSCM};
const Info<bool> GFX_CC_CORRECT_GAMMA{{System::GFX, "ColorCorrection", "CorrectGamma"}, false};
const Info<float> GFX_CC_GAME_GAMMA{{System::GFX, "ColorCorrection", "GameGamma"}, 2.35f};
const Info<bool> GFX_CC_SDR_DISPLAY_GAMMA_SRGB{
    {System::GFX, "ColorCorrection", "SDRDisplayGammaSRGB"}, true};
const Info<float> GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA{
    {System::GFX, "ColorCorrection", "SDRDisplayCustomGamma"}, 2.2f};
const Info<float> GFX_CC_HDR_PAPER_WHITE_NITS{{System::GFX, "ColorCorrection", "HDRPaperWhiteNits"},
                                              203.f};

// Graphics.Stereoscopy

const Info<StereoMode> GFX_STEREO_MODE{{System::GFX, "Stereoscopy", "StereoMode"}, StereoMode::Off};
const Info<bool> GFX_STEREO_PER_EYE_RESOLUTION_FULL{
    {System::GFX, "Stereoscopy", "StereoPerEyeResolutionFull"}, false};
const Info<int> GFX_STEREO_DEPTH{{System::GFX, "Stereoscopy", "StereoDepth"}, 20};
const Info<int> GFX_STEREO_CONVERGENCE_PERCENTAGE{
    {System::GFX, "Stereoscopy", "StereoConvergencePercentage"}, 100};
const Info<bool> GFX_STEREO_SWAP_EYES{{System::GFX, "Stereoscopy", "StereoSwapEyes"}, false};
const Info<int> GFX_STEREO_CONVERGENCE{{System::GFX, "Stereoscopy", "StereoConvergence"}, 20};
const Info<bool> GFX_STEREO_EFB_MONO_DEPTH{{System::GFX, "Stereoscopy", "StereoEFBMonoDepth"},
                                           false};
const Info<int> GFX_STEREO_DEPTH_PERCENTAGE{{System::GFX, "Stereoscopy", "StereoDepthPercentage"},
                                            100};

// Graphics.Hacks

const Info<bool> GFX_HACK_EFB_ACCESS_ENABLE{{System::GFX, "Hacks", "EFBAccessEnable"}, false};
const Info<bool> GFX_HACK_EFB_DEFER_INVALIDATION{
    {System::GFX, "Hacks", "EFBAccessDeferInvalidation"}, false};
const Info<int> GFX_HACK_EFB_ACCESS_TILE_SIZE{{System::GFX, "Hacks", "EFBAccessTileSize"}, 64};
const Info<bool> GFX_HACK_BBOX_ENABLE{{System::GFX, "Hacks", "BBoxEnable"}, false};
const Info<bool> GFX_HACK_FORCE_PROGRESSIVE{{System::GFX, "Hacks", "ForceProgressive"}, true};
const Info<bool> GFX_HACK_SKIP_EFB_COPY_TO_RAM{{System::GFX, "Hacks", "EFBToTextureEnable"}, true};
const Info<bool> GFX_HACK_SKIP_XFB_COPY_TO_RAM{{System::GFX, "Hacks", "XFBToTextureEnable"}, true};
const Info<bool> GFX_HACK_DISABLE_COPY_TO_VRAM{{System::GFX, "Hacks", "DisableCopyToVRAM"}, false};
const Info<bool> GFX_HACK_DEFER_EFB_COPIES{{System::GFX, "Hacks", "DeferEFBCopies"}, true};
const Info<bool> GFX_HACK_IMMEDIATE_XFB{{System::GFX, "Hacks", "ImmediateXFBEnable"}, false};
const Info<bool> GFX_HACK_SKIP_DUPLICATE_XFBS{{System::GFX, "Hacks", "SkipDuplicateXFBs"}, true};
const Info<bool> GFX_HACK_EARLY_XFB_OUTPUT{{System::GFX, "Hacks", "EarlyXFBOutput"}, true};
const Info<bool> GFX_HACK_COPY_EFB_SCALED{{System::GFX, "Hacks", "EFBScaledCopy"}, true};
const Info<bool> GFX_HACK_EFB_EMULATE_FORMAT_CHANGES{
    {System::GFX, "Hacks", "EFBEmulateFormatChanges"}, false};
const Info<bool> GFX_HACK_VERTEX_ROUNDING{{System::GFX, "Hacks", "VertexRounding"}, false};
const Info<bool> GFX_HACK_VI_SKIP{{System::GFX, "Hacks", "VISkip"}, false};
const Info<u32> GFX_HACK_MISSING_COLOR_VALUE{{System::GFX, "Hacks", "MissingColorValue"},
                                             0xFFFFFFFF};
const Info<bool> GFX_HACK_FAST_TEXTURE_SAMPLING{{System::GFX, "Hacks", "FastTextureSampling"},
                                                true};
#ifdef __APPLE__
const Info<bool> GFX_HACK_NO_MIPMAPPING{{System::GFX, "Hacks", "NoMipmapping"}, false};
#endif

// Graphics.GameSpecific

const Info<bool> GFX_PERF_QUERIES_ENABLE{{System::GFX, "GameSpecific", "PerfQueriesEnable"}, false};

// Graphics.Game Specific VR Settings

const Info<bool> GFX_VR_DISABLE_3D{{System::GFX, "VR", "Disable3D"}, false};
const Info<bool> GFX_VR_HUD_FULLSCREEN{{System::GFX, "VR", "HudFullscreen"}, false};
const Info<bool> GFX_VR_HUD_ON_TOP{{System::GFX, "VR", "HudOnTop"}, false};
const Info<bool> GFX_VR_DONT_CLEAR_SCREEN{{System::GFX, "VR", "DontClearScreen"}, false};
const Info<bool> GFX_VR_CAN_READ_CAMERA_ANGLES{{System::GFX, "VR", "CanReadCameraAngles"},
                                                     false};
const Info<bool> GFX_VR_DETECT_SKYBOX{{System::GFX, "VR", "DetectSkybox"}, false};
const Info<float> GFX_VR_UNITS_PER_METRE{{System::GFX, "VR", "UnitsPerMetre"},
                                               DEFAULT_VR_UNITS_PER_METRE};
const Info<float> GFX_VR_HUD_THICKNESS{{System::GFX, "VR", "HudThickness"},
                                             DEFAULT_VR_HUD_THICKNESS};
const Info<float> GFX_VR_HUD_DISTANCE{{System::GFX, "VR", "HudDistance"},
                                            DEFAULT_VR_HUD_DISTANCE};
const Info<float> GFX_VR_HUD_3D_CLOSER{{System::GFX, "VR", "Hud3DCloser"},
                                             DEFAULT_VR_HUD_3D_CLOSER};
const Info<float> GFX_VR_CAMERA_FORWARD{{System::GFX, "VR", "CameraForward"},
                                              DEFAULT_VR_CAMERA_FORWARD};
const Info<float> GFX_VR_CAMERA_PITCH{{System::GFX, "VR", "CameraPitch"},
                                            DEFAULT_VR_CAMERA_PITCH};
const Info<float> GFX_VR_AIM_DISTANCE{{System::GFX, "VR", "AimDistance"},
                                            DEFAULT_VR_AIM_DISTANCE};
const Info<float> GFX_VR_MIN_FOV{{System::GFX, "VR", "MinFOV"}, DEFAULT_VR_MIN_FOV};
const Info<float> GFX_VR_N64_FOV{{System::GFX, "VR", "N64FOV"}, DEFAULT_VR_N64_FOV};
const Info<float> GFX_VR_SCREEN_HEIGHT{{System::GFX, "VR", "ScreenHeight"},
                                             DEFAULT_VR_SCREEN_HEIGHT};
const Info<float> GFX_VR_SCREEN_THICKNESS{{System::GFX, "VR", "ScreenThickness"},
                                                DEFAULT_VR_HUD_THICKNESS};
const Info<float> GFX_VR_SCREEN_DISTANCE{{System::GFX, "VR", "ScreenDistance"},
                                               DEFAULT_VR_SCREEN_DISTANCE};
const Info<float> GFX_VR_SCREEN_RIGHT{{System::GFX, "VR", "ScreenRight"},
                                            DEFAULT_VR_SCREEN_RIGHT};
const Info<float> GFX_VR_SCREEN_UP{{System::GFX, "VR", "ScreenUp"}, DEFAULT_VR_SCREEN_UP};
const Info<float> GFX_VR_SCREEN_PITCH{{System::GFX, "VR", "ScreenPitch"},
                                            DEFAULT_VR_SCREEN_PITCH};
const Info<int> GFX_VR_METROID_PRIME{{System::GFX, "VR", "MetroidPrime"}, 0};
const Info<int> GFX_VR_TELESCOPE_EYE{{System::GFX, "VR", "TelescopeEye"}, 0};
const Info<float> GFX_VR_TELESCOPE_MAX_FOV{{System::GFX, "VR", "TelescopeFOV"}, 0.0f};
const Info<float> GFX_VR_READ_PITCH{{System::GFX, "VR", "ReadPitch"}, 0.0f};
const Info<u32> GFX_VR_CAMERA_MIN_POLY{{System::GFX, "VR", "CameraMinPoly"}, 0};
const Info<float> GFX_VR_HUD_DESP_POSITION_0{{System::GFX, "VR", "HudDespPosition0"}, 0.0f};
const Info<float> GFX_VR_HUD_DESP_POSITION_1{{System::GFX, "VR", "HudDespPosition1"}, 0.0f};
const Info<float> GFX_VR_HUD_DESP_POSITION_2{{System::GFX, "VR", "HudDespPosition2"}, 0.0f};

// Global VR Settings

const Info<float> GLOBAL_VR_SCALE{{System::Main, "VR", "Scale"}, 1.0f};
const Info<float> GLOBAL_VR_FREE_LOOK_SENSITIVITY{{System::Main, "VR", "FreeLookSensitivity"},
                                                        DEFAULT_VR_FREE_LOOK_SENSITIVITY};
const Info<float> GLOBAL_VR_LEAN_BACK_ANGLE{{System::Main, "VR", "LeanBackAngle"}, 0.0f};
const Info<bool> GLOBAL_VR_ENABLE_VR{{System::Main, "VR", "EnableVR"}, true};
const Info<bool> GLOBAL_VR_LOW_PERSISTENCE{{System::Main, "VR", "LowPersistence"}, true};
const Info<bool> GLOBAL_VR_DYNAMIC_PREDICTION{{System::Main, "VR", "DynamicPrediction"},
                                                    true};
const Info<bool> GLOBAL_VR_NO_MIRROR_TO_WINDOW{{System::Main, "VR", "NoMirrorToWindow"},
                                                     false};
const Info<bool> GLOBAL_VR_ORIENTATION_TRACKING{{System::Main, "VR", "OrientationTracking"},
                                                      true};
const Info<bool> GLOBAL_VR_MAG_YAW_CORRECTION{{System::Main, "VR", "MagYawCorrection"}, true};
const Info<bool> GLOBAL_VR_POSITION_TRACKING{{System::Main, "VR", "PositionTracking"}, true};
const Info<bool> GLOBAL_VR_CHROMATIC{{System::Main, "VR", "Chromatic"}, true};
const Info<bool> GLOBAL_VR_TIMEWARP{{System::Main, "VR", "Timewarp"}, true};
const Info<bool> GLOBAL_VR_VIGNETTE{{System::Main, "VR", "Vignette"}, false};
const Info<bool> GLOBAL_VR_NO_RESTORE{{System::Main, "VR", "NoRestore"}, false};
const Info<bool> GLOBAL_VR_FLIP_VERTICAL{{System::Main, "VR", "FlipVertical"}, false};
const Info<bool> GLOBAL_VR_SRGB{{System::Main, "VR", "sRGB"}, false};
const Info<bool> GLOBAL_VR_OVERDRIVE{{System::Main, "VR", "Overdrive"}, true};
const Info<bool> GLOBAL_VR_HQ_DISTORTION{{System::Main, "VR", "HQDistortion"}, false};
const Info<bool> GLOBAL_VR_DISABLE_NEAR_CLIPPING{{System::Main, "VR", "DisableNearClipping"},
                                                       true};
const Info<bool> GLOBAL_VR_AUTO_PAIR_VIVE_CONTROLLERS{
    {System::Main, "VR", "AutoPairViveControllers"}, false};
const Info<bool> GLOBAL_VR_SHOW_HANDS{{System::Main, "VR", "ShowHands"}, false};
const Info<bool> GLOBAL_VR_SHOW_FEET{{System::Main, "VR", "ShowFeet"}, false};
const Info<bool> GLOBAL_VR_SHOW_CONTROLLER{{System::Main, "VR", "ShowController"}, false};
const Info<bool> GLOBAL_VR_SHOW_LASER_POINTER{{System::Main, "VR", "ShowLaserPointer"},
                                                    false};
const Info<bool> GLOBAL_VR_SHOW_AIM_RECTANGLE{{System::Main, "VR", "ShowAimRectangle"},
                                                    false};
const Info<bool> GLOBAL_VR_SHOW_HUD_BOX{{System::Main, "VR", "ShowHudBox"}, false};
const Info<bool> GLOBAL_VR_SHOW_2D_SCREEN_BOX{{System::Main, "VR", "Show2DScreenBox"}, false};
const Info<bool> GLOBAL_VR_SHOW_SENSOR_BAR{{System::Main, "VR", "ShowSensorBar"}, false};
const Info<bool> GLOBAL_VR_SHOW_GAME_CAMERA{{System::Main, "VR", "ShowGameCamera"}, false};
const Info<bool> GLOBAL_VR_SHOW_GAME_FRUSTUM{{System::Main, "VR", "ShowGameFrustum"}, false};
const Info<bool> GLOBAL_VR_SHOW_TRACKING_CAMERA{{System::Main, "VR", "ShowTrackingCamera"},
                                                      false};
const Info<bool> GLOBAL_VR_SHOW_TRACKING_VOLUME{{System::Main, "VR", "ShowTrackingVolume"},
                                                      false};
const Info<bool> GLOBAL_VR_SHOW_BASE_STATION{{System::Main, "VR", "ShowBaseStation"}, false};
const Info<bool> GLOBAL_VR_MOTION_SICKNESS_ALWAYS{
    {System::Main, "VR", "MotionSicknessAlways"}, false};
const Info<bool> GLOBAL_VR_MOTION_SICKNESS_FREELOOK{
    {System::Main, "VR", "MotionSicknessFreelook"}, false};
const Info<bool> GLOBAL_VR_MOTION_SICKNESS_2D{{System::Main, "VR", "MotionSickness2D"},
                                                    false};
const Info<bool> GLOBAL_VR_MOTION_SICKNESS_LEFT_STICK{
    {System::Main, "VR", "MotionSicknessLeftStick"}, false};
const Info<bool> GLOBAL_VR_MOTION_SICKNESS_RIGHT_STICK{
    {System::Main, "VR", "MotionSicknessRightStick"}, false};
const Info<bool> GLOBAL_VR_MOTION_SICKNESS_DPAD{{System::Main, "VR", "MotionSicknessDPad"},
                                                      false};
const Info<bool> GLOBAL_VR_MOTION_SICKNESS_IR{{System::Main, "VR", "MotionSicknessIR"},
                                                    false};
const Info<int> GLOBAL_VR_MOTION_SICKNESS_METHOD{{System::Main, "VR", "MotionSicknessMethod"},
                                                       0};
const Info<int> GLOBAL_VR_MOTION_SICKNESS_SKYBOX{{System::Main, "VR", "MotionSicknessSkybox"},
                                                       0};
const Info<float> GLOBAL_VR_MOTION_SICKNESS_FOV{{System::Main, "VR", "MotionSicknessFOV"}, 0};
const Info<int> GLOBAL_VR_PLAYER{{System::Main, "VR", "Player"}, 0};
const Info<int> GLOBAL_VR_PLAYER_2{{System::Main, "VR", "Player2"}, 1};
const Info<int> GLOBAL_VR_MIRROR_PLAYER{{System::Main, "VR", "MirrorPlayer"},
                                              VR_PLAYER_DEFAULT};
const Info<int> GLOBAL_VR_MIRROR_STYLE{{System::Main, "VR", "MirrorStyle"}, VR_MIRROR_LEFT};
const Info<float> GLOBAL_VR_TIMEWARP_TWEAK{{System::Main, "VR", "TimewarpTweak"},
                                                 DEFAULT_VR_TIMEWARP_TWEAK};
const Info<u32> GLOBAL_VR_NUM_EXTRA_FRAMES{{System::Main, "VR", "NumExtraFrames"},
                                                 DEFAULT_VR_EXTRA_FRAMES};
const Info<u32> GLOBAL_VR_NUM_EXTRA_VIDEO_LOOPS{{System::Main, "VR", "NumExtraVideoLoops"},
                                                      DEFAULT_VR_EXTRA_VIDEO_LOOPS};
const Info<u32> GLOBAL_VR_NUM_EXTRA_VIDEO_LOOPS_DIVIDER{
    {System::Main, "VR", "NumExtraVideoLoopsDivider"}, DEFAULT_VR_EXTRA_VIDEO_LOOPS_DIVIDER};
const Info<bool> GLOBAL_VR_STABILIZE_ROLL{{System::Main, "VR", "StabilizeRoll"}, true};
const Info<bool> GLOBAL_VR_STABILIZE_PITCH{{System::Main, "VR", "StabilizePitch"}, true};
const Info<bool> GLOBAL_VR_STABILIZE_YAW{{System::Main, "VR", "StabilizeYaw"}, false};
const Info<bool> GLOBAL_VR_STABILIZE_X{{System::Main, "VR", "StabilizeX"}, false};
const Info<bool> GLOBAL_VR_STABILIZE_Y{{System::Main, "VR", "StabilizeY"}, false};
const Info<bool> GLOBAL_VR_STABILIZE_Z{{System::Main, "VR", "StabilizeZ"}, false};
const Info<bool> GLOBAL_VR_KEYHOLE{{System::Main, "VR", "Keyhole"}, false};
const Info<float> GLOBAL_VR_KEYHOLE_WIDTH{{System::Main, "VR", "KeyholeWidth"}, 45.0f};
const Info<bool> GLOBAL_VR_KEYHOLE_SNAP{{System::Main, "VR", "KeyholeSnap"}, false};
const Info<float> GLOBAL_VR_KEYHOLE_SIZE{{System::Main, "VR", "KeyholeSnapSize"}, 30.0f};
const Info<bool> GLOBAL_VR_PULL_UP_20_FPS{{System::Main, "VR", "PullUp20fps"}, false};
const Info<bool> GLOBAL_VR_PULL_UP_30_FPS{{System::Main, "VR", "PullUp30fps"}, false};
const Info<bool> GLOBAL_VR_PULL_UP_60_FPS{{System::Main, "VR", "PullUp60fps"}, false};
const Info<bool> GLOBAL_VR_PULL_UP_AUTO{{System::Main, "VR", "PullUpAuto"}, false};
const Info<bool> GLOBAL_VR_OPCODE_REPLAY{{System::Main, "VR", "OpcodeReplay"}, false};
const Info<bool> GLOBAL_VR_OPCODE_WARNING_DISABLE{
    {System::Main, "VR", "OpcodeWarningDisable"}, false};
const Info<bool> GLOBAL_VR_REPLAY_VERTEX_DATA{{System::Main, "VR", "ReplayVertexData"},
                                                    false};
const Info<bool> GLOBAL_VR_REPLAY_OTHER_DATA{{System::Main, "VR", "ReplayOtherData"}, false};
const Info<bool> GLOBAL_VR_PULL_UP_20_FPS_TIMEWARP{
    {System::Main, "VR", "PullUp20fpsTimewarp"}, false};
const Info<bool> GLOBAL_VR_PULL_UP_30_FPS_TIMEWARP{
    {System::Main, "VR", "PullUp30fpsTimewarp"}, false};
const Info<bool> GLOBAL_VR_PULL_UP_60_FPS_TIMEWARP{
    {System::Main, "VR", "PullUp60fpsTimewarp"}, false};
const Info<bool> GLOBAL_VR_PULL_UP_AUTO_TIMEWARP{{System::Main, "VR", "PullUpAutoTimewarp"},
                                                       false};
const Info<bool> GLOBAL_VR_SYNCHRONOUS_TIMEWARP{{System::Main, "VR", "SynchronousTimewarp"},
                                                      false};

const Info<std::string> GLOBAL_VR_LEFT_TEXTURE{{System::Main, "VR", "LeftTexture"}, ""};
const Info<std::string> GLOBAL_VR_RIGHT_TEXTURE{{System::Main, "VR", "RightTexture"}, ""};
const Info<std::string> GLOBAL_VR_GC_LEFT_TEXTURE{{System::Main, "VR", "GCLeftTexture"}, ""};
const Info<std::string> GLOBAL_VR_GC_RIGHT_TEXTURE{{System::Main, "VR", "GCRightTexture"},
                                                         ""};

}  // namespace Config
