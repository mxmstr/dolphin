// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <type_traits>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Common/Inline.h"
#include "Common/Swap.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/DataReader.h" // For DataReader in TimewarpLogEntry
#include "VideoCommon/VR.h"         // For TimewarpLogEntry, g_opcode_replay_log_frame etc.
#include "VideoCommon/VideoConfig.h"// For g_ActiveConfig
#include "VideoCommon/VertexLoaderBase.h"


struct CPState;
// class DataReader; // Now included via DataReader.h

namespace OpcodeDecoder
{
// Global flag to signal if FifoRecorder is active.
extern bool g_record_fifo_data;

enum class Opcode
{
  GX_NOP = 0x00,

  GX_LOAD_BP_REG = 0x61,
  GX_LOAD_CP_REG = 0x08,
  GX_LOAD_XF_REG = 0x10,
  GX_LOAD_INDX_A = 0x20,
  GX_LOAD_INDX_B = 0x28,
  GX_LOAD_INDX_C = 0x30,
  GX_LOAD_INDX_D = 0x38,

  GX_CMD_CALL_DL = 0x40,
  GX_CMD_UNKNOWN_METRICS = 0x44,
  GX_CMD_INVL_VC = 0x48,

  GX_PRIMITIVE_START = 0x80,
  GX_PRIMITIVE_END = 0xbf,
};

constexpr u8 GX_PRIMITIVE_MASK = 0x78;
constexpr u32 GX_PRIMITIVE_SHIFT = 3;
constexpr u8 GX_VAT_MASK = 0x07;

// These values are the values extracted using GX_PRIMITIVE_MASK
// and GX_PRIMITIVE_SHIFT.
// GX_DRAW_QUADS_2 behaves the same way as GX_DRAW_QUADS.
enum class Primitive : u8
{
  GX_DRAW_QUADS = 0x0,           // 0x80
  GX_DRAW_QUADS_2 = 0x1,         // 0x88
  GX_DRAW_TRIANGLES = 0x2,       // 0x90
  GX_DRAW_TRIANGLE_STRIP = 0x3,  // 0x98
  GX_DRAW_TRIANGLE_FAN = 0x4,    // 0xA0
  GX_DRAW_LINES = 0x5,           // 0xA8
  GX_DRAW_LINE_STRIP = 0x6,      // 0xB0
  GX_DRAW_POINTS = 0x7           // 0xB8
};

// Interface for the Run and RunCommand functions below.
// The functions themselves are templates so that the compiler generates separate versions for each
// callback (with the callback functions inlined), so the callback doesn't actually need to be
// publicly inherited.
// Compilers don't generate warnings for failed inlining with virtual functions, so this define
// allows disabling the use of virtual functions to generate those warnings.  However, this means
// that missing functions will generate errors on their use in RunCommand, instead of in the
// subclass, which can be confusing.
#define OPCODE_CALLBACK_USE_INHERITANCE

#ifdef OPCODE_CALLBACK_USE_INHERITANCE
#define OPCODE_CALLBACK(sig) DOLPHIN_FORCE_INLINE sig override
#define OPCODE_CALLBACK_NOINLINE(sig) sig override
#else
#define OPCODE_CALLBACK(sig) DOLPHIN_FORCE_INLINE sig
#define OPCODE_CALLBACK_NOINLINE(sig) sig
#endif
class Callback
{
#ifdef OPCODE_CALLBACK_USE_INHERITANCE
public:
  virtual ~Callback() = default;

  // Called on any XF command.
  virtual void OnXF(u16 address, u8 count, const u8* data) = 0;
  // Called on any CP command.
  // Subclasses should update the CP state with GetCPState().LoadCPReg(command, value) so that
  // primitive commands decode properly.
  virtual void OnCP(u8 command, u32 value) = 0;
  // Called on any BP command.
  virtual void OnBP(u8 command, u32 value) = 0;
  // Called on any indexed XF load command.
  virtual void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size) = 0;
  // Called on any primitive command.
  virtual void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat, u32 vertex_size,
                                  u16 num_vertices, const u8* vertex_data) = 0;
  // Called on a display list.
  virtual void OnDisplayList(u32 address, u32 size) = 0;
  // Called on any NOP commands (which are all merged into a single call).
  virtual void OnNop(u32 count) = 0;
  // Called on an unknown opcode, or an opcode that is known but not implemented.
  // data[0] is opcode.
  virtual void OnUnknown(u8 opcode, const u8* data) = 0;

  // Called on ANY command.  The first byte of data is the opcode.  Size will be at least 1.
  // This function is called after one of the above functions is called.
  virtual void OnCommand(const u8* data, u32 size) = 0;

  // Get the current CP state.  Needed for vertex decoding; will also be mutated for CP commands.
  virtual CPState& GetCPState() = 0;

  virtual u32 GetVertexSize(u8 vat) = 0;
#endif
};

namespace detail
{
// Main logic; split so that the main RunCommand can call OnCommand with the returned size.
template <typename T, typename = std::enable_if_t<std::is_base_of_v<Callback, T>>>
static DOLPHIN_FORCE_INLINE u32 RunCommand(const u8* data, u32 available, T& callback)
{
  if (available < 1)
    return 0;

  const Opcode cmd = static_cast<Opcode>(data[0]);

  switch (cmd)
  {
  case Opcode::GX_NOP:
  {
    u32 count = 1;
    while (count < available && static_cast<Opcode>(data[count]) == Opcode::GX_NOP)
      count++;
    callback.OnNop(count);
    return count;
  }

  case Opcode::GX_LOAD_CP_REG:
  {
    if (available < 6)
      return 0;

    const u8 cmd2 = data[1];
    const u32 value = Common::swap32(&data[2]);

    callback.OnCP(cmd2, value);

    return 6;
  }

  case Opcode::GX_LOAD_XF_REG:
  {
    if (available < 5)
      return 0;

    const u32 cmd2 = Common::swap32(&data[1]);
    const u16 base_address = cmd2 & 0xffff;

    const u16 stream_size_temp = cmd2 >> 16;
    ASSERT_MSG(VIDEO, stream_size_temp < 16, "cmd2 = 0x{:08X}", cmd2);
    const u8 stream_size = (stream_size_temp & 0xf) + 1;

    if (available < u32(5 + stream_size * 4))
      return 0;

    callback.OnXF(base_address, stream_size, &data[5]);

    return 5 + stream_size * 4;
  }

  case Opcode::GX_LOAD_INDX_A:  // Used for position matrices
  case Opcode::GX_LOAD_INDX_B:  // Used for normal matrices
  case Opcode::GX_LOAD_INDX_C:  // Used for postmatrices
  case Opcode::GX_LOAD_INDX_D:  // Used for lights
  {
    if (available < 5)
      return 0;

    const u32 value = Common::swap32(&data[1]);

    const u32 index = value >> 16;
    const u16 address = value & 0xFFF;  // TODO: check mask
    const u8 size = ((value >> 12) & 0xF) + 1;

    // Map the command byte to its ref array.
    // GX_LOAD_INDX_A (32 = 8*4) . CPArray::XF_A (4+8 = 12)
    // GX_LOAD_INDX_B (40 = 8*5) . CPArray::XF_B (5+8 = 13)
    // GX_LOAD_INDX_C (48 = 8*6) . CPArray::XF_C (6+8 = 14)
    // GX_LOAD_INDX_D (56 = 8*7) . CPArray::XF_D (7+8 = 15)
    const auto ref_array = static_cast<CPArray>((static_cast<u8>(cmd) / 8) + 8);

    callback.OnIndexedLoad(ref_array, index, address, size);
    return 5;
  }

  case Opcode::GX_CMD_CALL_DL:
  {
    if (available < 9)
      return 0;

    const u32 address = Common::swap32(&data[1]);
    const u32 size = Common::swap32(&data[5]);

    // Force 32-byte alignment for both the address and the size.
    callback.OnDisplayList(address & ~31, size & ~31);
    return 9;
  }

  case Opcode::GX_LOAD_BP_REG:
  {
    if (available < 5)
      return 0;

    const u8 cmd2 = data[1];
    const u32 value = Common::swap24(&data[2]);

    callback.OnBP(cmd2, value);

    return 5;
  }

  default:
    if (cmd >= Opcode::GX_PRIMITIVE_START && cmd <= Opcode::GX_PRIMITIVE_END)
    {
      if (available < 3)
        return 0;

      const u8 cmdbyte = static_cast<u8>(cmd);
      const OpcodeDecoder::Primitive primitive = static_cast<OpcodeDecoder::Primitive>(
          (cmdbyte & OpcodeDecoder::GX_PRIMITIVE_MASK) >> OpcodeDecoder::GX_PRIMITIVE_SHIFT);
      const u8 vat = cmdbyte & OpcodeDecoder::GX_VAT_MASK;

      const u32 vertex_size = callback.GetVertexSize(vat);
      const u16 num_vertices = Common::swap16(&data[1]);

      if (available < 3 + num_vertices * vertex_size)
        return 0;

      callback.OnPrimitiveCommand(primitive, vat, vertex_size, num_vertices, &data[3]);

      return 3 + num_vertices * vertex_size;
    }
  }

  callback.OnUnknown(static_cast<u8>(cmd), data);
  return 1;
}
}  // namespace detail

template <bool is_preprocess_param, typename T, typename = std::enable_if_t<std::is_base_of_v<Callback, T>>>
DOLPHIN_FORCE_INLINE u32 RunCommand(const u8* data, u32 available, T& callback, bool /*is_outer_call_for_logging_context -- not used here anymore */)
{
  // 'is_preprocess_param' is now available here.
  const u32 size = detail::RunCommand(data, available, callback);
  if (size > 0)
  {
    // Logging decision is now primarily in OpcodeDecoder::Run, not per-command.
    // This RunCommand is now mostly a dispatcher.
    callback.OnCommand(data, size);
  }
  return size;
}

template <bool is_preprocess_param, typename T, typename = std::enable_if_t<std::is_base_of_v<Callback, T>>>
DOLPHIN_FORCE_INLINE u32 Run(const u8* data, u32 available, T& callback, bool is_outer_call)
{
  // Opcode Logging Logic from Hydra, placed at the start of processing a stream (either FIFO or DL)
  if (is_outer_call && // Only log if it's an outer call (not inside a DL that was itself logged as part of a stream)
      g_opcode_replay_log_frame && !g_opcode_replay_frame &&
      skipped_opcode_replay_count >= static_cast<int>(g_ActiveConfig.iExtraVideoLoopsDivider))
  {
    // Construct a DataReader for the *entire current stream* being processed by this Run call.
    // This matches Hydra's TimewarpLogEntry{src, is_preprocess}
    // where 'src' was the DataReader for the whole fifo/DL chunk.
    DataReader current_stream_reader(data, data + available);
    timewarp_logentries.push_back(TimewarpLogEntry{current_stream_reader, is_preprocess_param});

    // Hydra also had some commented out logging for CP registers and VertexManager pointers here.
    // These can be added if necessary for debugging.
  }

  u32 size = 0;
  while (size < available)
  {
    // Pass is_preprocess_param. The is_outer_call is not strictly needed by RunCommand if logging is only in Run.
    const u32 command_size = RunCommand<is_preprocess_param>(&data[size], available - size, callback, is_outer_call);
    if (command_size == 0)
      break;
    size += command_size;
  }
  return size;
}

template <bool is_preprocess = false>
u8* RunFifo(DataReader src, u32* cycles)
{
  using CallbackT = RunCallback<is_preprocess>;
  auto callback = CallbackT{};
  // Pass 'is_preprocess' to Run
  u32 size_processed = Run<is_preprocess>(src.GetPointer(), static_cast<u32>(src.size()), callback);

  if (cycles != nullptr)
    *cycles = callback.m_cycles;

  // src.Skip(size_processed); // This was how RunFifo updated src before, but Run now returns total size
  return src.GetPointer() + size_processed; // Correctly return pointer after processed data
}

void Init();    // Added from Hydra
void Shutdown(); // Added from Hydra


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
          // Called from DL, so is_outer_call = false
          Run<is_preprocess>(start_address, size, *this, false);
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
          // Called from DL, so is_outer_call = false
          Run<false>(start_address, size, *this, false);
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
      // From Hydra:
      if (!s_bFifoErrorSeen && !g_ActiveConfig.bOpcodeWarningDisable)
      {
        auto& system = Core::System::GetInstance();
        // Current HandleUnknownOpcode doesn't take the extra bools from Hydra.
        // (g_opcode_replay_frame, m_in_display_list, is_outer_call_equivalent?)
        system.GetCommandProcessor().HandleUnknownOpcode(opcode, data, is_preprocess);
      }
      ERROR_LOG(VIDEO, "FIFO: Unknown Opcode(0x%02x @ %p, preprocessing = %s, in_dl = %s)",
                opcode, data, is_preprocess ? "yes" : "no", m_in_display_list ? "yes" : "no");
      s_bFifoErrorSeen = true;
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



}  // namespace OpcodeDecoder

template <>
struct fmt::formatter<OpcodeDecoder::Primitive>
    : EnumFormatter<OpcodeDecoder::Primitive::GX_DRAW_POINTS>
{
  static constexpr array_type names = {
      "GX_DRAW_QUADS",        "GX_DRAW_QUADS_2 (nonstandard)",
      "GX_DRAW_TRIANGLES",    "GX_DRAW_TRIANGLE_STRIP",
      "GX_DRAW_TRIANGLE_FAN", "GX_DRAW_LINES",
      "GX_DRAW_LINE_STRIP",   "GX_DRAW_POINTS",
  };
  constexpr formatter() : EnumFormatter(names) {}
};
