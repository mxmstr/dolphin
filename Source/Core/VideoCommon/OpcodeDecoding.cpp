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

namespace OpcodeDecoder
{
bool g_record_fifo_data = false;

// Opcode Replay global variables (mirroring VR-Hydra-Reference/VR.h and OpcodeDecoding.cpp)
// These should ideally be declared in VR.h and externed here, or accessed via VR::Get...()
std::vector<TimewarpLogEntry> timewarp_logentries;
bool g_opcode_replay_enabled = false;
bool g_opcode_replay_frame = false;     // True if the current frame is being replayed from log
bool g_opcode_replay_log_frame = false; // True if the current original frame should be logged
int skipped_opcode_replay_count = 0;    // Counter for frame skipping logic with replay
bool s_bFifoErrorSeen = false; // For unknown opcode warnings


//template <bool is_preprocess>
//u8* RunFifo(DataReader src, u32* cycles)
//{
//  using CallbackT = RunCallback<is_preprocess>;
//  auto callback = CallbackT{};
//  // This is the outermost call for a FIFO stream, so is_outer_call = true
//  u32 size = Run<is_preprocess>(src.GetPointer(), static_cast<u32>(src.size()), callback, true);
//
//  if (cycles != nullptr)
//    *cycles = callback.m_cycles;
//
//  src.Skip(size);
//  return src.GetPointer();
//}
//
//template u8* RunFifo<true>(DataReader src, u32* cycles);
//template u8* RunFifo<false>(DataReader src, u32* cycles);

}  // namespace OpcodeDecoder
