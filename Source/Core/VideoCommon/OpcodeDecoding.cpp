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
#include "VideoCommon/VR.h"          // Added for VR globals
#include "VideoCommon/VideoConfig.h" // Added for g_ActiveConfig

// VR Opcode Replay Globals (ported from Hydra)
// These should ideally be declared in VR.h and defined here, or be static within this file.
// For now, defining them here directly.
bool g_opcode_replay_enabled = false;
bool g_opcode_replay_frame = false;     // True if this is a replay frame from the timewarp buffer
bool g_opcode_replay_log_frame = false; // True if we should log opcodes to the timewarp buffer this frame
int skipped_opcode_replay_count = 0; // Counter for iExtraVideoLoopsDivider
std::vector<TimewarpLogEntry> timewarp_logentries; // Log buffer

namespace OpcodeDecoder
{
bool g_record_fifo_data = false;
static bool s_bFifoErrorSeen = false; // Ported from Hydra

void Init() // Added from Hydra
{
  // Use g_has_hmd from VideoCommon/VR.h
  g_opcode_replay_enabled = g_ActiveConfig.bOpcodeReplay &&
                            g_has_hmd; // Using the existing global from VR.h
  g_opcode_replay_frame = false;
  g_opcode_replay_log_frame = false;
  skipped_opcode_replay_count = 0; // Reset or set to meet initial condition for logging
  s_bFifoErrorSeen = false;
}

void Shutdown() // Added from Hydra
{
  g_opcode_replay_frame = false;
  g_opcode_replay_log_frame = false;
  timewarp_logentries.clear();
  // timewarp_logentries.shrink_to_fit(); // Optional: if memory is critical
}


template <bool is_preprocess>
class RunCallback final : public Callback
{
public:
  OPCODE_CALLBACK(void OnXF(u16 address, u8 count, const u8* data))
  {
    m_cycles += 18 + 6 * count;

    if constexpr (!is_preprocess)
    {
      LoadXFReg(address, count, data);

      INCSTAT(g_stats.this_frame.num_xf_loads);
    }
  }
  OPCODE_CALLBACK(void OnCP(u8 command, u32 value))
  {
    m_cycles += 12;
    const u8 sub_command = command & CP_COMMAND_MASK;
    if constexpr (!is_preprocess)
    {
      if (sub_command == MATINDEX_A)
      {
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
        auto& system = Core::System::GetInstance();
        system.GetXFStateManager().SetTexMatrixChangedA(value);
      }
      else if (sub_command == MATINDEX_B)
      {
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
        auto& system = Core::System::GetInstance();
        system.GetXFStateManager().SetTexMatrixChangedB(value);
      }
      else if (sub_command == VCD_LO || sub_command == VCD_HI)
      {
        VertexLoaderManager::g_main_vat_dirty = BitSet8::AllTrue(CP_NUM_VAT_REG);
        VertexLoaderManager::g_bases_dirty = true;
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      }
      else if (sub_command == CP_VAT_REG_A || sub_command == CP_VAT_REG_B ||
               sub_command == CP_VAT_REG_C)
      {
        VertexLoaderManager::g_main_vat_dirty[command & CP_VAT_MASK] = true;
        VertexLoaderManager::g_needs_cp_xf_consistency_check = true;
      }
      else if (sub_command == ARRAY_BASE)
      {
        VertexLoaderManager::g_bases_dirty = true;
      }

      INCSTAT(g_stats.this_frame.num_cp_loads);
    }
    else if constexpr (is_preprocess)
    {
      if (sub_command == VCD_LO || sub_command == VCD_HI)
      {
        VertexLoaderManager::g_preprocess_vat_dirty = BitSet8::AllTrue(CP_NUM_VAT_REG);
      }
      else if (sub_command == CP_VAT_REG_A || sub_command == CP_VAT_REG_B ||
               sub_command == CP_VAT_REG_C)
      {
        VertexLoaderManager::g_preprocess_vat_dirty[command & CP_VAT_MASK] = true;
      }
    }
    GetCPState().LoadCPReg(command, value);
  }
  OPCODE_CALLBACK(void OnBP(u8 command, u32 value))
  {
    m_cycles += 12;

    if constexpr (is_preprocess)
    {
      LoadBPRegPreprocess(command, value, m_cycles);
    }
    else
    {
      LoadBPReg(command, value, m_cycles);
      INCSTAT(g_stats.this_frame.num_bp_loads);
    }
  }
  OPCODE_CALLBACK(void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size))
  {
    m_cycles += 6;

    if constexpr (is_preprocess)
      PreprocessIndexedXF(array, index, address, size);
    else
      LoadIndexedXF(array, index, address, size);
  }
  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          u32 vertex_size, u16 num_vertices, const u8* vertex_data))
  {
    // load vertices
    const u32 size = vertex_size * num_vertices;

    const u32 bytes =
        VertexLoaderManager::RunVertices<is_preprocess>(vat, primitive, num_vertices, vertex_data);

    ASSERT(bytes == size);

    // 4 GPU ticks per vertex, 3 CPU ticks per GPU tick
    m_cycles += num_vertices * 4 * 3 + 6;
  }
  // This can't be inlined since it calls Run, which makes it recursive
  // m_in_display_list prevents it from actually recursing infinitely, but there's no real benefit
  // to inlining Run for the display list directly.
  OPCODE_CALLBACK_NOINLINE(void OnDisplayList(u32 address, u32 size))
  {
    m_cycles += 6;

    if (m_in_display_list)
    {
      WARN_LOG_FMT(VIDEO, "recursive display list detected");
    }
    else
    {
      m_in_display_list = true;

      auto& system = Core::System::GetInstance();

      if constexpr (is_preprocess)
      {
        auto& memory = system.GetMemory();
        const u8* const start_address = memory.GetPointerForRange(address, size);

        system.GetFifo().PushFifoAuxBuffer(start_address, size);

        if (start_address != nullptr)
        {
          Run(start_address, size, *this);
        }
      }
      else
      {
        const u8* start_address;

        auto& fifo = system.GetFifo();
        if (fifo.UseDeterministicGPUThread())
        {
          start_address = static_cast<u8*>(fifo.PopFifoAuxBuffer(size));
        }
        else
        {
          auto& memory = system.GetMemory();
          start_address = memory.GetPointerForRange(address, size);
        }

        // Avoid the crash if memory.GetPointerForRange failed ..
        if (start_address != nullptr)
        {
          // temporarily swap dl and non-dl (small "hack" for the stats)
          g_stats.SwapDL();

          Run(start_address, size, *this);
          INCSTAT(g_stats.this_frame.num_dlists_called);

          // un-swap
          g_stats.SwapDL();
        }
      }

      m_in_display_list = false;
    }
  }
  OPCODE_CALLBACK(void OnNop(u32 count))
  {
    m_cycles += 6 * count;  // Hm, this means that we scan over nop streams pretty slowly...
  }
  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8* data))
  {
    if (static_cast<Opcode>(opcode) == Opcode::GX_CMD_UNKNOWN_METRICS)
    {
      // 'Zelda Four Swords' calls it and checks the metrics registers after that
      m_cycles += 6;
      DEBUG_LOG_FMT(VIDEO, "GX 0x44");
    }
    else if (static_cast<Opcode>(opcode) == Opcode::GX_CMD_INVL_VC)
    {
      // Invalidate Vertex Cache
      m_cycles += 6;
      DEBUG_LOG_FMT(VIDEO, "Invalidate (vertex cache?)");
    }
    else
    {
      auto& system = Core::System::GetInstance();
      // In Hydra: CommandProcessor::HandleUnknownOpcode(cmd_byte, opcodeStart, is_preprocess, g_opcode_replay_frame, in_display_list, recursive_call);
      // Reloaded: system.GetCommandProcessor().HandleUnknownOpcode(opcode, data, is_preprocess);
      // Adding the extra VR parameters to HandleUnknownOpcode would be a change in CommandProcessor.h/cpp
      // For now, call the existing one. The VR parameters were for more detailed error reporting.
      if (!s_bFifoErrorSeen && !g_ActiveConfig.bOpcodeWarningDisable) // from Hydra
          system.GetCommandProcessor().HandleUnknownOpcode(opcode, data, is_preprocess);

      ERROR_LOG_FMT(VIDEO, "FIFO: Unknown Opcode(0x{:02x} @ {}, preprocessing = {})", opcode, fmt::ptr(data), is_preprocess); // from Hydra
      s_bFifoErrorSeen = true; // from Hydra
      m_cycles += 1;
    }
  }

  OPCODE_CALLBACK(void OnCommand(const u8* data, u32 size))
  {
    ASSERT(size >= 1);
    if constexpr (!is_preprocess)
    {
      // Display lists get added directly into the FIFO stream since this same callback is used to
      // process them.
      if (g_record_fifo_data && static_cast<Opcode>(data[0]) != Opcode::GX_CMD_CALL_DL)
      {
        Core::System::GetInstance().GetFifoRecorder().WriteGPCommand(data, size);
      }
    }
  }

  OPCODE_CALLBACK(CPState& GetCPState())
  {
    if constexpr (is_preprocess)
      return g_preprocess_cp_state;
    else
      return g_main_cp_state;
  }

  OPCODE_CALLBACK(u32 GetVertexSize(u8 vat))
  {
    VertexLoaderBase* loader = VertexLoaderManager::RefreshLoader<is_preprocess>(vat);
    return loader->m_vertex_size;
  }

  u32 m_cycles = 0;
  bool m_in_display_list = false;
};

template <bool is_preprocess>
u8* RunFifo(DataReader src, u32* cycles)
{
  using CallbackT = RunCallback<is_preprocess>;
  auto callback = CallbackT{};

  // VR Opcode Replay Logging (ported from Hydra)
  // Log the DataReader src if conditions are met.
  // The 'recursive_call' equivalent is !callback.m_in_display_list for top-level calls to RunFifo.
  // However, RunFifo itself is usually the top-level entry point for a FIFO stream segment.
  // The check !in_display_list in Hydra's Run was to ensure it's not logging segments of an already-called Display List.
  // Since RunFifo is processing a segment, we assume it's a "top-level" segment for logging purposes here.
  // The m_in_display_list in the callback will be false for the first command in this src,
  // unless this entire src is itself a Display List's content being processed.
  if (g_opcode_replay_log_frame && !g_opcode_replay_frame &&
      (skipped_opcode_replay_count >= static_cast<int>(g_ActiveConfig.iExtraVideoLoopsDivider)))
  {
    // Check if this segment is part of a display list being executed.
    // This requires knowing the context from which RunFifo is called.
    // If CommandProcessor::ProcessCommands calls RunFifo, m_in_display_list would be false for direct FIFO data.
    // If OpcodeDecoder::Run (for a GX_CMD_CALL_DL) calls RunFifo, then m_in_display_list might be true.
    // Hydra's check was: !recursive_call. Here, recursive_call is implicit.
    // Let's assume for now that if RunFifo is called, it's a loggable block unless it's inside a DL *handled by the callback*.
    // The callback's m_in_display_list is set *during* DL processing.
    // A simple check: if the callback starts as not in a DL, this whole block is a candidate.
    bool is_top_level_segment = !callback.m_in_display_list; // Check initial state

    if (is_top_level_segment) // Only log top-level FIFO segments, not the content of called DLs already captured.
    {
      timewarp_logentries.push_back(TimewarpLogEntry{src, is_preprocess});
      // The PanicAlert for DL in replay buffer would be more complex to replicate here,
      // as 'src' could *start* with a DL call. The original check was on the specific cmd_byte.
    }
  }

  u32 size = Run(src.GetPointer(), static_cast<u32>(src.size()), callback);

  if (cycles != nullptr)
    *cycles = callback.m_cycles;

  src.Skip(size);
  return src.GetPointer();
}

template u8* RunFifo<true>(DataReader src, u32* cycles);
template u8* RunFifo<false>(DataReader src, u32* cycles);

}  // namespace OpcodeDecoder
