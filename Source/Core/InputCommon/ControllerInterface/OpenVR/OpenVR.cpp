// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "InputCommon/ControllerInterface/OpenVR/OpenVR.h"
#include "InputCommon/ControllerInterface/InputBackend.h"
#include "VideoCommon/VR.h"
#include "Common/BitSet.h"

namespace ciface
{
namespace OpenVR
{
class Backend : public InputBackend
{
public:
  explicit Backend(ControllerInterface* ciface) : InputBackend(ciface) {}
  void PopulateDevices() override { OpenVR::PopulateDevices(); }
};

std::unique_ptr<InputBackend> CreateInputBackend(ControllerInterface* ciface)
{
  return std::make_unique<Backend>(ciface);
}

static const struct
{
  const char* const name;
  const u64 bitmask;
} openvr_buttons[] =
    {
        {"LTouchpad", OPENVR_BUTTON_LEFT_TOUCHPAD},
        {"LMenu", OPENVR_BUTTON_LEFT_MENU},
        {"LGrip", OPENVR_BUTTON_LEFT_GRIP},
        {"RTouchpad", OPENVR_BUTTON_RIGHT_TOUCHPAD},
        {"RMenu", OPENVR_BUTTON_RIGHT_MENU},
        {"RGrip", OPENVR_BUTTON_RIGHT_GRIP},
        {"LSystem", OPENVR_BUTTON_LEFT_SYSTEM},
        {"RSystem", OPENVR_BUTTON_RIGHT_SYSTEM},
        {"LA", OPENVR_BUTTON_LEFT_A},
        {"LDPadUp", OPENVR_BUTTON_LEFT_UP},
        {"LDPadDown", OPENVR_BUTTON_LEFT_DOWN},
        {"LDPadLeft", OPENVR_BUTTON_LEFT_LEFT},
        {"LDPadRight", OPENVR_BUTTON_LEFT_RIGHT},
        {"RA", OPENVR_BUTTON_RIGHT_A},
        {"RDPadUp", OPENVR_BUTTON_RIGHT_UP},
        {"RDPadDown", OPENVR_BUTTON_RIGHT_DOWN},
        {"RDPadLeft", OPENVR_BUTTON_RIGHT_LEFT},
        {"RDPadRight", OPENVR_BUTTON_RIGHT_RIGHT},
},
  openvr_specials[] =
      {
          {"LClickUp", OPENVR_SPECIAL_DPAD_UP},
          {"LClickDown", OPENVR_SPECIAL_DPAD_DOWN},
          {"LClickLeft", OPENVR_SPECIAL_DPAD_LEFT},
          {"LClickRight", OPENVR_SPECIAL_DPAD_RIGHT},
          {"LClickMiddle", OPENVR_SPECIAL_DPAD_MIDDLE},
          {"RClickUp", (u64)OPENVR_SPECIAL_DPAD_UP << 32},
          {"RClickDown", (u64)OPENVR_SPECIAL_DPAD_DOWN << 32},
          {"RClickLeft", (u64)OPENVR_SPECIAL_DPAD_LEFT << 32},
          {"RClickRight", (u64)OPENVR_SPECIAL_DPAD_RIGHT << 32},
          {"RClickMiddle", (u64)OPENVR_SPECIAL_DPAD_MIDDLE << 32},
          {"RGameCubeA", (u64)OPENVR_SPECIAL_GC_A << 32},
          {"RGameCubeB", (u64)OPENVR_SPECIAL_GC_B << 32},
          {"RGameCubeX", (u64)OPENVR_SPECIAL_GC_X << 32},
          {"RGameCubeY", (u64)OPENVR_SPECIAL_GC_Y << 32},
          {"RGameCubeEmpty", (u64)OPENVR_SPECIAL_GC_EMPTY << 32},
          {"RSixA", (u64)OPENVR_SPECIAL_SIX_A << 32},
          {"RSixB", (u64)OPENVR_SPECIAL_SIX_B << 32},
          {"RSixC", (u64)OPENVR_SPECIAL_SIX_C << 32},
          {"RSixX", (u64)OPENVR_SPECIAL_SIX_X << 32},
          {"RSixY", (u64)OPENVR_SPECIAL_SIX_Y << 32},
          {"RSixZ", (u64)OPENVR_SPECIAL_SIX_Z << 32},
          {"RTopLeft", (u64)OPENVR_SPECIAL_TOPLEFT << 32},
          {"RTopRight", (u64)OPENVR_SPECIAL_TOPRIGHT << 32},
          {"RBottomLeft", (u64)OPENVR_SPECIAL_BOTTOMLEFT << 32},
          {"RBottomRight", (u64)OPENVR_SPECIAL_BOTTOMRIGHT << 32},
},
  openvr_touches[] = {
      {"TouchLTouchpad", OPENVR_BUTTON_LEFT_TOUCHPAD},
      {"TouchRTouchpad", OPENVR_BUTTON_RIGHT_TOUCHPAD},
      {"TouchLTrigger", OPENVR_BUTTON_LEFT_TRIGGER},
      {"TouchLMenu", OPENVR_BUTTON_LEFT_MENU},
      {"TouchLGrip", OPENVR_BUTTON_LEFT_GRIP},
      {"TouchRTrigger", OPENVR_BUTTON_RIGHT_TRIGGER},
      {"TouchRMenu", OPENVR_BUTTON_RIGHT_MENU},
      {"TouchRGrip", OPENVR_BUTTON_RIGHT_GRIP},
      {"TouchLSystem", OPENVR_BUTTON_LEFT_SYSTEM},
      {"TouchLA", OPENVR_BUTTON_LEFT_A},
      {"TouchLDPadUp", OPENVR_BUTTON_LEFT_UP},
      {"TouchLDPadDown", OPENVR_BUTTON_LEFT_DOWN},
      {"TouchLDPadLeft", OPENVR_BUTTON_LEFT_LEFT},
      {"TouchLDPadRight", OPENVR_BUTTON_LEFT_RIGHT},
      {"TouchRA", OPENVR_BUTTON_RIGHT_A},
      {"TouchRSystem", OPENVR_BUTTON_RIGHT_SYSTEM},
      {"TouchRDPadUp", OPENVR_BUTTON_RIGHT_UP},
      {"TouchRDPadDown", OPENVR_BUTTON_RIGHT_DOWN},
      {"TouchRDPadLeft", OPENVR_BUTTON_RIGHT_LEFT},
      {"TouchRDPadRight", OPENVR_BUTTON_RIGHT_RIGHT},
};

static const char* const named_triggers[] = {
    "LTrigger", "RTrigger",
};

static const char* const named_axes[] = {"LAnalogX", "LAnalogY", "RAnalogX", "RAnalogY",
                                         "LTouchX",  "LTouchY",  "RTouchX",  "RTouchY"};

static const char* const named_motors[] = {
    "LHaptic", "RHaptic",
};

void Init()
{
}

void PopulateDevices()
{
  g_controller_interface.AddDevice(std::make_shared<OpenVRController>());
}

void DeInit()
{
}

OpenVRController::OpenVRController()
{
  for (size_t i = 0; i < 6; ++i)
  {
    AddInput(new Button(i, m_buttons));
  }
  for (size_t i = 0; i < 2; ++i)
  {
    AddInput(new Trigger(i, m_triggers));
  }
  for (size_t i = 0; i != sizeof(openvr_specials) / sizeof(*openvr_specials); ++i)
  {
    AddInput(new Special(i, m_specials));
  }
  for (size_t i = 0; i < 8; ++i)
  {
    AddInput(new Axis(i, -1, m_axes));
    AddInput(new Axis(i, 1, m_axes));
  }
  for (size_t i = 0; i < 2; ++i)
  {
    AddInput(new Touch(i, m_touches));
  }
  for (size_t i = 6; i != sizeof(openvr_buttons) / sizeof(*openvr_buttons); ++i)
  {
    AddInput(new Button(i, m_buttons));
  }
  for (size_t i = 2; i < sizeof(openvr_touches) / sizeof(*openvr_touches); ++i)
  {
    AddInput(new Touch(i, m_touches));
  }

  for (size_t i = 0; i != sizeof(named_motors) / sizeof(*named_motors); ++i)
  {
    AddOutput(new Motor(i, this, m_motors));
  }
}

std::string OpenVRController::GetName() const
{
  return "OpenVR";
}

std::string OpenVRController::GetSource() const
{
  return "VR";
}

// Update I/O

Core::DeviceRemoval OpenVRController::UpdateInput()
{
  VR_GetOpenVRButtons(&m_buttons, &m_touches, &m_specials, m_triggers, m_axes);
  UpdateMotors();
  return Core::DeviceRemoval::Keep;
}

void OpenVRController::UpdateMotors()
{
  if (m_motors)
    VR_OpenVRHapticPulse(m_motors, 3999);
}

// GET name/source/id

std::string OpenVRController::Button::GetName() const
{
  return openvr_buttons[m_index].name;
}

// GET / SET STATES

ControlState OpenVRController::Button::GetState() const
{
  return (m_buttons & openvr_buttons[m_index].bitmask) > 0;
}

// GET name/source/id

std::string OpenVRController::Special::GetName() const
{
  return openvr_specials[m_index].name;
}

// GET / SET STATES

ControlState OpenVRController::Special::GetState() const
{
  return (m_buttons & openvr_specials[m_index].bitmask) > 0;
}

// GET name/source/id

std::string OpenVRController::Touch::GetName() const
{
  return openvr_touches[m_index].name;
}

std::string OpenVRController::Axis::GetName() const
{
  return std::string(named_axes[m_index]) + (m_range < 0 ? '-' : '+');
}

std::string OpenVRController::Trigger::GetName() const
{
  return named_triggers[m_index];
}

std::string OpenVRController::Motor::GetName() const
{
  return named_motors[m_index];
}

// GET / SET STATES

ControlState OpenVRController::Touch::GetState() const
{
  return (m_touches & openvr_touches[m_index].bitmask) > 0;
}

ControlState OpenVRController::Trigger::GetState() const
{
  return ControlState(m_triggers[m_index]);
}

ControlState OpenVRController::Axis::GetState() const
{
  return std::max(0.0, ControlState(m_axes[m_index] * m_range));
}

void OpenVRController::Motor::SetState(ControlState state)
{
  if (state > 0)
    m_motors |= (1 << m_index);
  else
    m_motors &= ~(1 << m_index);
  m_parent->UpdateMotors();
}
}
}
