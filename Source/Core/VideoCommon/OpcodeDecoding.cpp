// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DL facts:
//  Ikaruga uses (nearly) NO display lists!
//  Zelda WW uses TONS of display lists
//  Zelda TP uses almost 100% display lists except menus (we like this!)
//  Super Mario Galaxy has nearly all geometry and more than half of the state in DLs (great!)

// Note that it IS NOT GENERALLY POSSIBLE to precompile display lists! You can compile them as they
// are while interpreting them, and hope that the vertex format doesn't change, though, if you do
// it right when they are called. The reason is that the vertex format affects the sizes of the
// vertices.

#include "VideoCommon/OpcodeDecoding.h"

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"
#include "VideoCommon/XFStructs.h"
#include "VideoCommon/VideoConfig.h" // For g_ActiveConfig for GLOBAL_VR_NUM_EXTRA_VIDEO_LOOPS_DIVIDER

// Define global variables for Opcode Replay
//bool g_opcode_replay_enabled = false;
//bool g_opcode_replay_frame = false;
//bool g_opcode_replay_log_frame = false;
//int skipped_opcode_replay_count = 0;
//std::vector<TimewarpLogEntry> timewarp_logentries;

namespace OpcodeDecoder
{
bool g_record_fifo_data = false;


template <bool is_preprocess>
u8* RunFifo(DataReader src, u32* cycles)
{
  // Opcode Replay Logging Logic (from VR-Hydra OpcodeDecoder::Run)
  // Note: recursive_call check in VR-Hydra was to prevent logging display lists themselves.
  // Here, RunFifo is the entry point for a command stream, so recursive_call is implicitly false.
  // Display list processing happens inside the templated Run via OnDisplayList callback.
  // We need to ensure that the 'src' here is not from within a display list if we want to match Hydra's behavior.
  // However, the callback.m_in_display_list can't be checked here easily.
  // For now, we assume RunFifo is called for top-level command buffers.
  // The original check was: if (g_opcode_replay_log_frame && !g_opcode_replay_frame && !recursive_call && ...)
  // Here, !recursive_call is assumed.
  if (g_opcode_replay_log_frame && !g_opcode_replay_frame &&
      (skipped_opcode_replay_count >= (int)g_ActiveConfig.iExtraVideoLoopsDivider)) // TODO: Ensure g_ActiveConfig is accessible and iExtraVideoLoopsDivider is correct name
  {
    // It's important that TimewarpLogEntry's constructor correctly copies the data from src.
    timewarp_logentries.emplace_back(src, is_preprocess);

    // Original VR-Hydra code had a PanicAlert if in_display_list was true here.
    // This implies that display lists themselves should not be logged directly by this top-level mechanism.
    // The current structure where RunFifo calls Run which then calls OnDisplayList (which calls RunFifo recursively)
    // means this top-level log *could* happen for a display list if called directly into RunFifo.
    // This might require more nuanced handling if display lists are passed directly to RunFifo from external code
    // rather than only via the OnDisplayList callback. For now, proceeding with this simple check.
  }

  using CallbackT = RunCallback<is_preprocess>;
  auto callback = CallbackT{};
  u32 size = Run(src.GetPointer(), static_cast<u32>(src.size()), callback);

  if (cycles != nullptr)
    *cycles = callback.m_cycles;

  src.Skip(size);
  return src.GetPointer();
}

template u8* RunFifo<true>(DataReader src, u32* cycles);
template u8* RunFifo<false>(DataReader src, u32* cycles);

}  // namespace OpcodeDecoder
