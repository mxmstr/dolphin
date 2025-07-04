// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GeometryShaderGen.h"

#include <cmath>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

constexpr Common::EnumMap<const char*, PrimitiveType::TriangleStrip> primitives_ogl{
    "points",
    "lines",
    "triangles",
    "triangles",
};
constexpr Common::EnumMap<const char*, PrimitiveType::TriangleStrip> primitives_d3d{
    "point",
    "line",
    "triangle",
    "triangle",
};

constexpr Common::EnumMap<u32, PrimitiveType::TriangleStrip> vertex_in_map{1u, 2u, 3u, 3u};
constexpr Common::EnumMap<u32, PrimitiveType::TriangleStrip> vertex_out_map{4u, 4u, 4u, 3u};

bool geometry_shader_uid_data::IsPassthrough() const
{
  const bool stereo = g_ActiveConfig.stereo_mode != StereoMode::Off;
  const bool wireframe = g_ActiveConfig.bWireFrame;
  return primitive_type >= static_cast<u32>(PrimitiveType::Triangles) && !stereo && !wireframe;
}

GeometryShaderUid GetGeometryShaderUid(PrimitiveType primitive_type)
{
  GeometryShaderUid out;

  geometry_shader_uid_data* const uid_data = out.GetUidData();
  uid_data->primitive_type = static_cast<u32>(primitive_type);
  uid_data->numTexGens = xfmem.numTexGen.numTexGens;

  return out;
}

static void EmitVertex(ShaderCode& out, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data* uid_data, const char* vertex,
                       APIType api_type, bool wireframe, bool stereo, bool first_vertex = false);
static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType api_type, bool wireframe,
                         bool stereo);

ShaderCode GenerateGeometryShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                      const geometry_shader_uid_data* uid_data)
{
  ShaderCode out;
  // Non-uid template parameters will write to the dummy data (=> gets optimized out)

  const bool wireframe = host_config.wireframe;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool stereo = host_config.stereo;
  const bool vr = host_config.vr_active; // Added for VR
  const auto primitive_type = static_cast<PrimitiveType>(uid_data->primitive_type);
  const u32 vertex_in = vertex_in_map[primitive_type];
  u32 vertex_out = vertex_out_map[primitive_type];

  if (wireframe)
    vertex_out++;

  // Determine number of layers/invocations
  const unsigned int num_layers = vr ? 2 : (stereo ? 2 : 1);

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    // Insert layout parameters
    if (host_config.backend_gs_instancing)
    {
      out.Write("layout({}, invocations = {}) in;\n", primitives_ogl[primitive_type],
                num_layers);
      out.Write("layout({}_strip, max_vertices = {}) out;\n", wireframe ? "line" : "triangle",
                vertex_out);
    }
    else
    {
      out.Write("layout({}) in;\n", primitives_ogl[primitive_type]);
      out.Write("layout({}_strip, max_vertices = {}) out;\n", wireframe ? "line" : "triangle",
                vertex_out * num_layers);
    }
  }

  out.Write("{}", s_lighting_struct);

  // uniforms
  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("UBO_BINDING(std140, 4) uniform GSBlock {{\n");
  else
    out.Write("cbuffer GSBlock {{\n");

  out.Write("{}", s_geometry_shader_uniforms);
  out.Write("}};\n");

  out.Write("struct VS_OUTPUT {{\n");
  GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config, "",
                          ShaderStage::Geometry);
  out.Write("}};\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (host_config.backend_gs_instancing)
      out.Write("#define InstanceID gl_InvocationID\n");

    out.Write("VARYING_LOCATION(0) in VertexData {{\n");
    GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, true),
                            ShaderStage::Geometry);
    out.Write("}} vs[{}];\n", vertex_in);

    out.Write("VARYING_LOCATION(0) out VertexData {{\n");
    GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, false),
                            ShaderStage::Geometry);

    out.Write("}} ps;\n");
    if (stereo && !host_config.backend_gl_layer_in_fs)
      out.Write("flat out int layer;");

    out.Write("void main()\n{{\n");
  }
  else  // D3D
  {
    out.Write("struct VertexData {{\n");
    out.Write("\tVS_OUTPUT o;\n");

    if (stereo)
    {
      out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");
    }
    out.Write("\tfloat4 posout : SV_Position;\n");

    out.Write("}};\n");

    if (host_config.backend_gs_instancing)
    {
      out.Write("[maxvertexcount({})]\n[instance({})]\n", vertex_out, stereo ? 2 : 1);
      out.Write("void main({} VS_OUTPUT o[{}], inout {}Stream<VertexData> output, in uint "
                "InstanceID : SV_GSInstanceID)\n{{\n",
                primitives_d3d[primitive_type], vertex_in, wireframe ? "Line" : "Triangle");
    }
    else
    {
      out.Write("[maxvertexcount({})]\n", stereo ? vertex_out * 2 : vertex_out);
      out.Write("void main({} VS_OUTPUT o[{}], inout {}Stream<VertexData> output)\n{{\n",
                primitives_d3d[primitive_type], vertex_in, wireframe ? "Line" : "Triangle");
    }

    out.Write("\tVertexData ps;\n");
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    {
      out.Write("\tVS_OUTPUT start, end;\n");
      AssignVSOutputMembers(out, "start", "vs[0]", uid_data->numTexGens, host_config);
      AssignVSOutputMembers(out, "end", "vs[1]", uid_data->numTexGens, host_config);
    }
    else
    {
      out.Write("\tVS_OUTPUT start = o[0];\n"
                "\tVS_OUTPUT end = o[1];\n");
    }

    GenerateLineOffset(out, "\t", "\t\t", "end.pos", "start.pos", "");
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    {
      out.Write("\tVS_OUTPUT center;\n");
      AssignVSOutputMembers(out, "center", "vs[0]", uid_data->numTexGens, host_config);
    }
    else
    {
      out.Write("\tVS_OUTPUT center = o[0];\n");
    }

    // Offset from center to upper right vertex
    // Lerp PointSize/2 from [0,0..VpWidth,VpHeight] to [-1,1..1,-1]
    out.Write("\tfloat2 offset = float2(" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS
              ".x, -" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS ".y) * center.pos.w;\n");
  }

  if (stereo)
  {
    // If the GPU supports invocation we don't need a for loop and can simply use the
    // invocation identifier to determine which layer we're rendering.
    if (host_config.backend_gs_instancing)
      out.Write("\tint eye = InstanceID;\n");
    else
      out.Write("\tfor (int eye = 0; eye < 2; ++eye) {{\n");
  }

  if (wireframe)
    out.Write("\tVS_OUTPUT first;\n");

  // Avoid D3D warning about forced unrolling of single-iteration loop
  if (vertex_in > 1)
    out.Write("\tfor (int i = 0; i < {}; ++i) {{\n", vertex_in);
  else
    out.Write("\tint i = 0;\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    out.Write("\tVS_OUTPUT f;\n");
    AssignVSOutputMembers(out, "f", "vs[i]", uid_data->numTexGens, host_config);

    if (host_config.backend_depth_clamp &&
        DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
    {
      // On certain GPUs we have to consume the clip distance from the vertex shader
      // or else the other vertex shader outputs will get corrupted.
      out.Write("\tf.clipDist0 = gl_in[i].gl_ClipDistance[0];\n"
                "\tf.clipDist1 = gl_in[i].gl_ClipDistance[1];\n");
    }
  }
  else
  {
    out.Write("\tVS_OUTPUT f = o[i];\n");
  }

  if (vr) // VR stereo rendering
  {
    // Select the output layer
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
      out.Write("\tgl_Layer = eye;\n");
    else
      out.Write("\tps.layer = eye;\n");

    // StereoParams[eye] = camera shift in game units * projection[0][0]
    // StereoParams[eye+2] = offaxis shift from VR projection[0][2] (e.g. Oculus)
    // Note: clipPos might not exist or be used in all shader paths, but pos.w is generally clip-space W.
    out.Write("\tf.pos.x += (" I_STEREOPARAMS "[eye] - " I_STEREOPARAMS "[eye+2] * f.pos.w);\n");
    // The VR-Hydra reference also modified f.clipPos.x. If f.clipPos is consistently available and used,
    // it should be modified too. Assuming f.pos is the primary position attribute modified for projection.
    // out.Write("\tf.clipPos.x += (" I_STEREOPARAMS "[eye] - " I_STEREOPARAMS "[eye+2] * f.clipPos.w);\n");
  }
  else if (stereo) // Standard (non-VR) stereo rendering
  {
    // For stereoscopy add a small horizontal offset in Normalized Device Coordinates proportional
    // to the depth of the vertex. We retrieve the depth value from the w-component of the projected
    // vertex which contains the negated z-component of the original vertex.
    // For negative parallax (out-of-screen effects) we subtract a convergence value from
    // the depth value. This results in objects at a distance smaller than the convergence
    // distance to seemingly appear in front of the screen.
    // This formula is based on page 13 of the "Nvidia 3D Vision Automatic, Best Practices Guide"
    out.Write("\tfloat hoffset = (eye == 0) ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y;\n");
    out.Write("\tf.pos.x += hoffset * (f.pos.w - " I_STEREOPARAMS ".z);\n");
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    out.Write("\tVS_OUTPUT l = f;\n"
              "\tVS_OUTPUT r = f;\n");

    out.Write("\tl.pos.xy -= offset * l.pos.w;\n"
              "\tr.pos.xy += offset * r.pos.w;\n");

    out.Write("\tif (" I_TEXOFFSET "[2] != 0) {{\n");
    out.Write("\tfloat texOffset = 1.0 / float(" I_TEXOFFSET "[2]);\n");

    for (u32 i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Write("\tif (((" I_TEXOFFSET "[0] >> {}) & 0x1) != 0)\n", i);
      out.Write("\t\tr.tex{}.x += texOffset;\n", i);
    }
    out.Write("\t}}\n");

    EmitVertex(out, host_config, uid_data, "l", api_type, wireframe, stereo, true);
    EmitVertex(out, host_config, uid_data, "r", api_type, wireframe, stereo);
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    out.Write("\tVS_OUTPUT ll = f;\n"
              "\tVS_OUTPUT lr = f;\n"
              "\tVS_OUTPUT ul = f;\n"
              "\tVS_OUTPUT ur = f;\n");

    out.Write("\tll.pos.xy += float2(-1,-1) * offset;\n"
              "\tlr.pos.xy += float2(1,-1) * offset;\n"
              "\tul.pos.xy += float2(-1,1) * offset;\n"
              "\tur.pos.xy += offset;\n");

    out.Write("\tif (" I_TEXOFFSET "[3] != 0) {{\n");
    out.Write("\tfloat2 texOffset = float2(1.0 / float(" I_TEXOFFSET
              "[3]), 1.0 / float(" I_TEXOFFSET "[3]));\n");

    for (u32 i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Write("\tif (((" I_TEXOFFSET "[1] >> {}) & 0x1) != 0) {{\n", i);
      out.Write("\t\tul.tex{}.xy += float2(0,1) * texOffset;\n", i);
      out.Write("\t\tur.tex{}.xy += texOffset;\n", i);
      out.Write("\t\tlr.tex{}.xy += float2(1,0) * texOffset;\n", i);
      out.Write("\t}}\n");
    }
    out.Write("\t}}\n");

    EmitVertex(out, host_config, uid_data, "ll", api_type, wireframe, stereo, true);
    EmitVertex(out, host_config, uid_data, "lr", api_type, wireframe, stereo);
    EmitVertex(out, host_config, uid_data, "ul", api_type, wireframe, stereo);
    EmitVertex(out, host_config, uid_data, "ur", api_type, wireframe, stereo);
  }
  else
  {
    EmitVertex(out, host_config, uid_data, "f", api_type, wireframe, stereo, true);
  }

  // Only close loop if previous code was in one (See D3D warning above)
  if (vertex_in > 1)
    out.Write("\t}}\n");

  EndPrimitive(out, host_config, uid_data, api_type, wireframe, stereo);

  if (stereo && !host_config.backend_gs_instancing)
    out.Write("\t}}\n");

  out.Write("}}\n");

  return out;
}

static void EmitVertex(ShaderCode& out, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data* uid_data, const char* vertex,
                       APIType api_type, bool wireframe, bool stereo, bool first_vertex)
{
  if (wireframe && first_vertex)
    out.Write("\tif (i == 0) first = {};\n", vertex);

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    // Vulkan NDC space has Y pointing down (right-handed NDC space).
    if (api_type == APIType::Vulkan)
      out.Write("\tgl_Position = float4({0}.pos.x, -{0}.pos.y, {0}.pos.z, {0}.pos.w);\n", vertex);
    else
      out.Write("\tgl_Position = {}.pos;\n", vertex);

    if (host_config.backend_depth_clamp)
    {
      out.Write("\tgl_ClipDistance[0] = {}.clipDist0;\n", vertex);
      out.Write("\tgl_ClipDistance[1] = {}.clipDist1;\n", vertex);
    }
    AssignVSOutputMembers(out, "ps", vertex, uid_data->numTexGens, host_config);
  }
  else
  {
    out.Write("\tps.o = {};\n", vertex);
    out.Write("\tps.posout = {}.pos;\n", vertex);
  }

  // If VR is active, layer is set in the main loop.
  // Only set layer here for standard (non-VR) stereo.
  if (stereo && !host_config.vr_active)
  {
    // Select the output layer
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
      out.Write("\tgl_Layer = eye;\n");
    else
    {
      out.Write("\tps.layer = eye;\n");
    }
    if (!host_config.backend_gl_layer_in_fs)
      out.Write("\tlayer = eye;\n");
  }

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("\tEmitVertex();\n");
  else
    out.Write("\toutput.Append(ps);\n");
}

static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType api_type, bool wireframe,
                         bool stereo)
{
  if (wireframe)
    EmitVertex(out, host_config, uid_data, "first", api_type, wireframe, stereo);

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("\tEndPrimitive();\n");
  else
    out.Write("\toutput.RestartStrip();\n");
}

void EnumerateGeometryShaderUids(const std::function<void(const GeometryShaderUid&)>& callback)
{
  GeometryShaderUid uid;

  const std::array<PrimitiveType, 3> primitive_lut = {
      {g_backend_info.bSupportsPrimitiveRestart ? PrimitiveType::TriangleStrip :
                                                  PrimitiveType::Triangles,
       PrimitiveType::Lines, PrimitiveType::Points}};
  for (PrimitiveType primitive : primitive_lut)
  {
    geometry_shader_uid_data* const guid = uid.GetUidData();
    guid->primitive_type = static_cast<u32>(primitive);

    for (u32 texgens = 0; texgens <= 8; texgens++)
    {
      guid->numTexGens = texgens;
      callback(uid);
    }
  }
}

// --- Start of GenerateAvatarGeometryShaderCode ---
// This function is ported from VR-Hydra for rendering VR avatars (controllers, hands)
// It's a simplified version of GenerateGeometryShaderCode, tailored for single texture avatars.
ShaderCode GenerateAvatarGeometryShaderCode(PrimitiveType primitive_type, APIType api_type, const ShaderHostConfig& host_config)
{
  ShaderCode out;

  const bool wireframe = host_config.wireframe;
  const bool stereo = host_config.stereo; // Standard stereo
  const bool vr = host_config.vr_active;   // VR stereo

  const unsigned primitive_type_index = static_cast<unsigned>(primitive_type);
  const unsigned vertex_in = std::min(static_cast<unsigned>(primitive_type_index) + 1, 3u);
  unsigned vertex_out = primitive_type == PrimitiveType::TriangleStrip ? 3 : 4;

  if (wireframe)
    vertex_out++;

  // Determine number of layers/invocations
  const unsigned int num_layers = vr ? 2 : (stereo ? 2 : 1);

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (host_config.backend_gs_instancing)
    {
      out.Write("layout(%s, invocations = %d) in;\n", primitives_ogl.data()[primitive_type_index], num_layers);
      out.Write("layout(%s_strip, max_vertices = %d) out;\n", wireframe ? "line" : "triangle", vertex_out);
    }
    else
    {
      out.Write("layout(%s) in;\n", primitives_ogl.data()[primitive_type_index]);
      out.Write("layout(%s_strip, max_vertices = %d) out;\n", wireframe ? "line" : "triangle", vertex_out * num_layers);
    }
  }

  // Uniforms (GSBlock is already defined with I_STEREOPARAMS etc. via s_geometry_shader_uniforms)
  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("UBO_BINDING(std140, 4) uniform GSBlock {\n");
  else
    out.Write("cbuffer GSBlock {\n");
  out.Write("%s", s_geometry_shader_uniforms); // This now includes I_STEREOPARAMS
  out.Write("};\n");

  // Simplified VS_OUTPUT for avatars (pos, one color, one texcoord)
  out.Write("struct VS_OUTPUT {\n");
  out.Write("\tfloat4 pos;\n");
  out.Write("\tfloat4 colors_0;\n"); // Assuming one color attribute
  out.Write("\tfloat3 tex0;\n");    // Assuming one texture coordinate
  if (host_config.backend_depth_clamp) // Copied from GenerateGeometryShaderCode logic for VS_OUTPUT
  {
    out.Write("\tfloat clipDist0;\n");
    out.Write("\tfloat clipDist1;\n");
  }
  out.Write("};\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (host_config.backend_gs_instancing)
      out.Write("#define InstanceID gl_InvocationID\n");

    out.Write("VARYING_LOCATION(0) in VertexData {\n");
    out.Write("\t%s float4 pos;\n", GetInterpolationQualifier(host_config.msaa, host_config.ssaa, true, true));
    out.Write("\t%s float4 colors_0;\n", GetInterpolationQualifier(host_config.msaa, host_config.ssaa, true, true));
    out.Write("\t%s float3 tex0;\n", GetInterpolationQualifier(host_config.msaa, host_config.ssaa, true, true));
    if (host_config.backend_depth_clamp && DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
    {
       out.Write("\tfloat clipDist0;\n"); // Match VS_OUTPUT members
       out.Write("\tfloat clipDist1;\n");
    }
    out.Write("} vs[%d];\n", vertex_in);

    out.Write("VARYING_LOCATION(0) out VertexData {\n");
    out.Write("\t%s float4 pos;\n", GetInterpolationQualifier(host_config.msaa, host_config.ssaa, true, false));
    out.Write("\t%s float4 colors_0;\n", GetInterpolationQualifier(host_config.msaa, host_config.ssaa, true, false));
    out.Write("\t%s float3 tex0;\n", GetInterpolationQualifier(host_config.msaa, host_config.ssaa, true, false));
     if (host_config.backend_depth_clamp) // Match VS_OUTPUT members
    {
       out.Write("\tfloat clipDist0;\n");
       out.Write("\tfloat clipDist1;\n");
    }
    out.Write("} ps;\n");
    if ((vr || stereo) && !host_config.backend_gl_layer_in_fs)
       out.Write("flat out int layer;\n");

    out.Write("void main()\n{\n");
  }
  else  // D3D
  {
    out.Write("struct VertexData {\n");
    out.Write("\tVS_OUTPUT o;\n");
    if (vr || stereo) // Use num_layers logic here
      out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");
    out.Write("\tfloat4 posout : SV_Position;\n");
    out.Write("};\n");

    if (host_config.backend_gs_instancing)
    {
      out.Write("[maxvertexcount(%d)]\n[instance(%d)]\n", vertex_out, num_layers);
      out.Write("void main(%s VS_OUTPUT o[%d], inout %sStream<VertexData> output, in uint InstanceID : SV_GSInstanceID)\n{\n",
                primitives_d3d.data()[primitive_type_index], vertex_in, wireframe ? "Line" : "Triangle");
    }
    else
    {
      out.Write("[maxvertexcount(%d)]\n", vertex_out * num_layers);
      out.Write("void main(%s VS_OUTPUT o[%d], inout %sStream<VertexData> output)\n{\n",
                primitives_d3d.data()[primitive_type_index], vertex_in, wireframe ? "Line" : "Triangle");
    }
    out.Write("\tVertexData ps;\n");
  }

  // Avatar shader primarily for triangles, line/point expansion can be simpler or omitted
  // if not used for avatars. For now, assume triangles.
  // If line/point needed, simplified logic from GenerateGeometryShaderCode would go here.

  if (vr || stereo)
  {
    if (host_config.backend_gs_instancing)
      out.Write("\tint eye = InstanceID;\n");
    else
      out.Write("\tfor (int eye = 0; eye < %d; ++eye) {\n", num_layers);
  }

  if (wireframe)
    out.Write("\tVS_OUTPUT first;\n");

  if (vertex_in > 1)
    out.Write("\tfor (int i = 0; i < %d; ++i) {\n", vertex_in);
  else
    out.Write("\tint i = 0;\n");

  // Simplified vertex assignment for avatars
  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    out.Write("\tVS_OUTPUT f;\n");
    out.Write("\tf.pos = vs[i].pos;\n");
    out.Write("\tf.colors_0 = vs[i].colors_0;\n");
    out.Write("\tf.tex0 = vs[i].tex0;\n");
    if (host_config.backend_depth_clamp && DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
    {
      out.Write("\tf.clipDist0 = vs[i].clipDist0;\n");
      out.Write("\tf.clipDist1 = vs[i].clipDist1;\n");
    }
  }
  else
  {
    out.Write("\tVS_OUTPUT f = o[i];\n");
  }

  if (vr)
  {
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan) // This was 'OpenGL' only in Hydra, making consistent
      out.Write("\tgl_Layer = eye;\n");
    else // D3D
      out.Write("\tps.layer = eye;\n"); // This was ps.layer in Hydra for D3D
    // Apply VR stereo parameters
    out.Write("\tf.pos.x += (" I_STEREOPARAMS "[eye] - " I_STEREOPARAMS "[eye+2] * f.pos.w);\n");
  }
  else if (stereo) // Standard stereo
  {
     // Standard stereo logic (from original GenerateGeometryShaderCode)
     out.Write("\tfloat hoffset = (eye == 0) ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y;\n");
     out.Write("\tf.pos.x += hoffset * (f.pos.w - " I_STEREOPARAMS ".z);\n");
  }

  // Create a dummy uid_data for EmitVertex as avatar shader doesn't rely on its specific fields like numTexGens for this part
  geometry_shader_uid_data avatar_uid_data = {}; // Zero-initialize
  avatar_uid_data.numTexGens = 1; // Avatars use 1 texture coordinate set

  // Simplified EmitVertex: we directly use 'f' and ps/gl_Position
  if (wireframe && (vertex_in == 0)) // Corrected first_vertex condition
    out.Write("\tif (i == 0) first = f;\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (api_type == APIType::Vulkan)
      out.Write("\tgl_Position = float4(f.pos.x, -f.pos.y, f.pos.z, f.pos.w);\n");
    else
      out.Write("\tgl_Position = f.pos;\n");

    if (host_config.backend_depth_clamp)
    {
      out.Write("\tps.clipDist0 = f.clipDist0;\n"); // Pass through clip distances
      out.Write("\tps.clipDist1 = f.clipDist1;\n");
    }
    out.Write("\tps.pos = gl_Position;\n"); // ps members must match VARYING_LOCATION out VertexData
    out.Write("\tps.colors_0 = f.colors_0;\n");
    out.Write("\tps.tex0 = f.tex0;\n");
    if ((vr || stereo) && !host_config.backend_gl_layer_in_fs)
      out.Write("\tlayer = eye;\n"); // ps.layer is not used directly for gl_Layer here
  }
  else // D3D
  {
    out.Write("\tps.o = f;\n");
    out.Write("\tps.posout = f.pos;\n");
    if (vr || stereo)
        out.Write("\tps.layer = eye;\n");
  }

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("\tEmitVertex();\n");
  else
    out.Write("\toutput.Append(ps);\n");
  // End simplified EmitVertex for avatars

  if (vertex_in > 1)
    out.Write("\t}\n"); // End for (int i = 0; ...

  EndPrimitive(out, host_config, &avatar_uid_data, api_type, wireframe, (stereo || vr));

  if ((vr || stereo) && !host_config.backend_gs_instancing)
    out.Write("\t}\n"); // End for (int eye = 0; ...

  out.Write("}\n"); // End main
  return out;
}
// --- End of GenerateAvatarGeometryShaderCode ---
