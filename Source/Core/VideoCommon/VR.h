// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

// Distances are in metres, angles are in degrees.
const float DEFAULT_VR_UNITS_PER_METRE = 1.0f, DEFAULT_VR_FREE_LOOK_SENSITIVITY = 1.0f,
            DEFAULT_VR_HUD_DISTANCE = 1.5f, DEFAULT_VR_HUD_THICKNESS = 0.5f,
            DEFAULT_VR_HUD_3D_CLOSER = 0.5f, DEFAULT_VR_CAMERA_FORWARD = 0.0f,
            DEFAULT_VR_CAMERA_PITCH = 0.0f, DEFAULT_VR_AIM_DISTANCE = 7.0f,
            DEFAULT_VR_SCREEN_HEIGHT = 2.0f, DEFAULT_VR_SCREEN_DISTANCE = 1.5f,
            DEFAULT_VR_SCREEN_THICKNESS = 0.5f, DEFAULT_VR_SCREEN_UP = 0.0f,
            DEFAULT_VR_SCREEN_RIGHT = 0.0f, DEFAULT_VR_SCREEN_PITCH = 0.0f,
            DEFAULT_VR_TIMEWARP_TWEAK = 0, DEFAULT_VR_MIN_FOV = 10.0f, DEFAULT_VR_N64_FOV = 90.0f,
            DEFAULT_VR_MOTION_SICKNESS_FOV = 45.0f;
const int DEFAULT_VR_EXTRA_FRAMES = 0;
const int DEFAULT_VR_EXTRA_VIDEO_LOOPS = 0;
const int DEFAULT_VR_EXTRA_VIDEO_LOOPS_DIVIDER = 0;

#define VR_PLAYER1 0
#define VR_PLAYER2 1
#define VR_PLAYER3 2
#define VR_PLAYER4 3
#define VR_PLAYER_NONE 4
#define VR_PLAYER_DEFAULT 5
#define VR_PLAYER_OTHERS 6
#define VR_PLAYER_ALL 7
#define VR_MIRROR_LEFT 0
#define VR_MIRROR_RIGHT 1
#define VR_MIRROR_DISABLED 2
#define VR_MIRROR_WARPED 3
#define VR_MIRROR_BOTH 4

#define OPENVR_BUTTON_LEFT_SYSTEM 0x01
#define OPENVR_BUTTON_LEFT_MENU 0x02
#define OPENVR_BUTTON_LEFT_GRIP 0x04
#define OPENVR_BUTTON_LEFT_LEFT 0x08
#define OPENVR_BUTTON_LEFT_UP 0x10
#define OPENVR_BUTTON_LEFT_RIGHT 0x20
#define OPENVR_BUTTON_LEFT_DOWN 0x40
#define OPENVR_BUTTON_LEFT_A 0x80
#define OPENVR_BUTTON_LEFT_TOUCHPAD 0x0100
#define OPENVR_BUTTON_LEFT_TRIGGER 0x0200
#define OPENVR_BUTTON_RIGHT_SYSTEM 0x010000
#define OPENVR_BUTTON_RIGHT_MENU 0x020000
#define OPENVR_BUTTON_RIGHT_GRIP 0x040000
#define OPENVR_BUTTON_RIGHT_LEFT 0x080000
#define OPENVR_BUTTON_RIGHT_UP 0x100000
#define OPENVR_BUTTON_RIGHT_RIGHT 0x200000
#define OPENVR_BUTTON_RIGHT_DOWN 0x400000
#define OPENVR_BUTTON_RIGHT_A 0x800000
#define OPENVR_BUTTON_RIGHT_TOUCHPAD 0x01000000
#define OPENVR_BUTTON_RIGHT_TRIGGER 0x02000000

#define OPENVR_SPECIAL_DPAD_UP 0x1
#define OPENVR_SPECIAL_DPAD_DOWN 0x2
#define OPENVR_SPECIAL_DPAD_LEFT 0x4
#define OPENVR_SPECIAL_DPAD_RIGHT 0x8
#define OPENVR_SPECIAL_DPAD_MIDDLE 0x10
#define OPENVR_SPECIAL_GC_A 0x20
#define OPENVR_SPECIAL_GC_B 0x40
#define OPENVR_SPECIAL_GC_X 0x80
#define OPENVR_SPECIAL_GC_Y 0x100
#define OPENVR_SPECIAL_GC_EMPTY 0x200
#define OPENVR_SPECIAL_SIX_A 0x400
#define OPENVR_SPECIAL_SIX_B 0x800
#define OPENVR_SPECIAL_SIX_C 0x1000
#define OPENVR_SPECIAL_SIX_X 0x2000
#define OPENVR_SPECIAL_SIX_Y 0x4000
#define OPENVR_SPECIAL_SIX_Z 0x8000
#define OPENVR_SPECIAL_TOPLEFT 0x10000
#define OPENVR_SPECIAL_TOPRIGHT 0x20000
#define OPENVR_SPECIAL_BOTTOMLEFT 0x40000
#define OPENVR_SPECIAL_BOTTOMRIGHT 0x80000

extern const char* scm_vr_sdk_str;

#include <atomic>
#include <mutex>

#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "VideoCommon/DataReader.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define RADIANS_TO_DEGREES(rad) ((float)rad * (float)(180.0 / M_PI))
#define DEGREES_TO_RADIANS(deg) ((float)deg * (float)(M_PI / 180.0))
//#define RECURSIVE_OPCODE
#define INLINE_OPCODE

#include "VideoCommon/VideoCommon.h"

#define HAVE_OPENVR

extern MathUtil::Rectangle<int> g_final_screen_region;

enum ViewportType
{
  VIEW_FULLSCREEN = 0,
  VIEW_LETTERBOXED,
  VIEW_HUD_ELEMENT,
  VIEW_SKYBOX,
  VIEW_PLAYER_1,
  VIEW_PLAYER_2,
  VIEW_PLAYER_3,
  VIEW_PLAYER_4,
  VIEW_OFFSCREEN,
  VIEW_RENDER_TO_TEXTURE,
};
extern enum ViewportType g_viewport_type, g_old_viewport_type;

typedef enum {
  CS_HYDRA_LEFT,
  CS_HYDRA_RIGHT,
  CS_WIIMOTE,
  CS_WIIMOTE_IR,
  CS_NUNCHUK,
  CS_NUNCHUK_UNREAD,
  CS_WIIMOTE_LEFT,
  CS_WIIMOTE_RIGHT,
  CS_CLASSIC_LEFT,
  CS_CLASSIC_RIGHT,
  CS_GC_LEFT,
  CS_GC_RIGHT,
  CS_N64_LEFT,
  CS_N64_RIGHT,
  CS_SNES_LEFT,
  CS_SNES_RIGHT,
  CS_SNES_NTSC_RIGHT,
  CS_NES_LEFT,
  CS_NES_RIGHT,
  CS_FAMICON_LEFT,
  CS_FAMICON_RIGHT,
  CS_SEGA_LEFT,
  CS_SEGA_RIGHT,
  CS_GENESIS_LEFT,
  CS_GENESIS_RIGHT,
  CS_TURBOGRAFX_LEFT,
  CS_TURBOGRAFX_RIGHT,
  CS_PCENGINE_LEFT,
  CS_PCENGINE_RIGHT,
  CS_ARCADE_LEFT,
  CS_ARCADE_RIGHT
} ControllerStyle;

// Main emulator interface
void VR_Init();
void VR_StopRendering();
void VR_Shutdown();
void VR_RecenterHMD();
void VR_NewVRFrame();
void VR_SetGame(bool is_wii, bool is_nand, std::string id);
void VR_CheckStatus(bool* ShouldRecenter, bool* ShouldQuit);

// Used for VR rendering
void VR_ConfigureHMDTracking();
void VR_ConfigureHMDPrediction();
void VR_BeginFrame();
void VR_GetEyePoses();
void ReadHmdOrientation(float* roll, float* pitch, float* yaw, float* x, float* y, float* z);
void VR_UpdateHeadTrackingIfNeeded();
void VR_GetProjectionHalfTan(float& hmd_halftan);
void VR_GetProjectionMatrices(Common::Matrix44& left_eye, Common::Matrix44& right_eye, float znear, float zfar);
void VR_GetEyePos(float* posLeft, float* posRight);
void VR_GetFovTextureSize(int* width, int* height);
void VR_GetRecommendedRenderTargetSize(u32* width, u32* height);
void VR_GetEyeToHeadTransforms(Common::Matrix44* left, Common::Matrix44* right);

std::wstring VR_GetAudioDeviceId();

bool VR_GetOpenVRButtons(u32* buttons, u32* touches, u64* specials, float triggers[], float axes[]);
bool VR_OpenVRHapticPulse(int hands, int microseconds);
bool VR_GetAccel(int index, bool sideways, bool has_extension, float* gx, float* gy, float* gz);
bool VR_GetNunchuckAccel(int index, float* gx, float* gy, float* gz);
bool VR_GetIR(int index, double* irx, double* iry, double* irz);
// called whenever the game reads the wiimote, to let us know which features they are reading
void VR_UpdateWiimoteReportingMode(int index, u8 accel, u8 ir, u8 ext);

bool VR_PairOpenVRControllers();

bool VR_GetLeftControllerPos(float* pos, float* thumbpos, Common::Matrix33* m);
bool VR_GetRightControllerPos(float* pos, float* thumbpos, Common::Matrix33* m);
ControllerStyle VR_GetHydraStyle(int hand);

void OpcodeReplayBuffer();
void OpcodeReplayBufferInline();

// HMD description and capabilities
extern bool g_force_vr; // User option to force VR mode.
extern bool g_has_hmd, g_has_openvr, g_openvr_is_vive; // OpenVR related flags.
extern bool g_is_nes; // This seems unrelated to VR SDK, keeping for now.
// The following flags' default values might need adjustment in VR.cpp for an OpenVR-only setup.
extern bool g_vr_cant_motion_blur, g_vr_must_motion_blur;
extern bool g_vr_needs_endframe, g_vr_needs_DXGIFactory1, g_vr_can_disable_hsw;
extern bool g_vr_has_dynamic_predict, g_vr_has_configure_rendering, g_vr_has_hq_distortion;
extern bool g_vr_has_configure_tracking, g_vr_has_timewarp_tweak, g_vr_has_asynchronous_timewarp;
extern bool g_vr_should_swap_buffers, g_vr_dont_vsync;
extern bool g_new_tracking_frame;
extern bool g_new_frame_tracker_for_efb_skip;
extern u32 skip_objects_count;
extern Common::Matrix44 g_head_tracking_matrix;
extern Common::Vec3 g_head_tracking_position;
extern float g_left_hand_tracking_position[3], g_right_hand_tracking_position[3];
extern int g_hmd_window_width, g_hmd_window_height, g_hmd_window_x, g_hmd_window_y,
    g_hmd_refresh_rate;
extern const char* g_hmd_device_name;
extern bool g_fov_changed, g_vr_black_screen;
extern bool g_vr_had_3D_already;
extern float vr_freelook_speed;
extern float vr_widest_3d_HFOV;
extern float vr_widest_3d_VFOV;
extern float vr_widest_3d_zNear;
extern float vr_widest_3d_zFar;
extern bool g_is_skybox;
extern Common::Vec3 g_game_camera_pos;
extern Common::Matrix44 g_game_camera_rotmat;

extern double g_older_tracking_time, g_old_tracking_time, g_last_tracking_time;

extern float g_current_fps, g_current_speed;

// 4 Wiimotes + 1 Balance Board
extern u8 g_vr_reading_wiimote_accel[5], g_vr_reading_wiimote_ir[5], g_vr_reading_wiimote_ext[5];

extern bool g_vr_has_ir;
extern float g_vr_ir_x, g_vr_ir_y, g_vr_ir_z;

// Opcode Replay Buffer
struct TimewarpLogEntry
{
  DataReader timewarp_log;
  bool is_preprocess_log;
};
extern std::vector<TimewarpLogEntry> timewarp_logentries;
extern bool g_opcode_replay_enabled;
extern bool g_new_frame_just_rendered;
extern bool g_first_pass;
extern bool g_first_pass_vs_constants;
extern bool g_opcode_replay_frame;
extern bool g_opcode_replay_log_frame;
extern int skipped_opcode_replay_count;

// extern std::vector<u8*> s_pCurBufferPointer_log;
// extern std::vector<u8*> s_pBaseBufferPointer_log;
// extern std::vector<u8*> s_pEndBufferPointer_log;

// extern std::vector<u32> CPBase_log;
// extern std::vector<u32> CPEnd_log;
// extern std::vector<u32> CPHiWatermark_log;
// extern std::vector<u32> CPLoWatermark_log;
// extern std::vector<u32> CPReadWriteDistance_log;
// extern std::vector<u32> CPWritePointer_log;
// extern std::vector<u32> CPReadPointer_log;
// extern std::vector<u32> CPBreakpoint_log;

extern std::mutex g_vr_lock;
namespace Core
{
extern std::atomic<u32> g_drawn_vr;
}

extern bool debug_nextScene;
