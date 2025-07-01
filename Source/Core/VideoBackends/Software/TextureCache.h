#pragma once

#include <memory>
#include "VideoBackends/Software/SWTexture.h"
#include "VideoBackends/Software/TextureEncoder.h"
#include "VideoCommon/TextureCacheBase.h"

namespace SW
{
class TextureCache : public TextureCacheBase
{
protected:
  void CopyEFB(AbstractStagingTexture* dst, const EFBCopyParams& params, u32 native_width,
               u32 bytes_per_row, u32 num_blocks_y, u32 memory_stride,
               const MathUtil::Rectangle<int>& game_src_rect, // Added
               const MathUtil::Rectangle<int>& our_src_rect, // Renamed
               bool scale_by_half, bool linear_filter, float y_scale, float gamma,
               bool clamp_top, bool clamp_bottom,
               const std::array<u32, 3>& filter_coefficients) override
  {
    // Pass our_src_rect to TextureEncoder::Encode
    TextureEncoder::Encode(dst, params, native_width, bytes_per_row, num_blocks_y, memory_stride,
                           game_src_rect, our_src_rect, scale_by_half, y_scale, gamma);
  }
  void CopyEFBToCacheEntry(RcTcacheEntry& entry, bool is_depth_copy,
                           const MathUtil::Rectangle<int>& game_src_rect, // Added
                           const MathUtil::Rectangle<int>& our_src_rect, // Renamed
                           bool scale_by_half, bool linear_filter, EFBCopyFormat dst_format,
                           bool is_intensity, float gamma, bool clamp_top, bool clamp_bottom,
                           const std::array<u32, 3>& filter_coefficients) override
  {
    // TODO: If we ever want to "fake" vram textures, we would need to implement this
    // If implemented, it should use our_src_rect for actual EFB pixel source.
  }
};

}  // namespace SW
