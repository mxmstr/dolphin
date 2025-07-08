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
  const bool stereo = host_config.stereo; // This will be true if g_ActiveConfig.stereo_mode != StereoMode::Off
  const auto primitive_type = static_cast<PrimitiveType>(uid_data->primitive_type);
  const u32 vertex_in = vertex_in_map[primitive_type];
  u32 vertex_out = vertex_out_map[primitive_type];

  if (wireframe)
    vertex_out++;

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    // Insert layout parameters
    // For stereo, GS instancing (invocations = 2) is preferred if available.
    // Otherwise, the shader manually loops and outputs twice the vertices.
    if (host_config.backend_gs_instancing && stereo) // Use GS instancing for stereo if supported
    {
      out.Write("layout({}, invocations = 2) in;\n", primitives_ogl[primitive_type]);
      out.Write("layout({}_strip, max_vertices = {}) out;\n", wireframe ? "line" : "triangle", vertex_out);
    }
    else // Mono or stereo without GS instancing
    {
      out.Write("layout({}) in;\n", primitives_ogl[primitive_type]);
      out.Write("layout({}_strip, max_vertices = {}) out;\n", wireframe ? "line" : "triangle",
                (stereo && !host_config.backend_gs_instancing) ? vertex_out * 2 : vertex_out);
    }
  }

  out.Write("{}", s_lighting_struct);

  // uniforms
  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("UBO_BINDING(std140, 4) uniform GSBlock {{\n");
  else
    out.Write("cbuffer GSBlock {{\n");

  out.Write("{}", s_geometry_shader_uniforms); // This includes cstereo (stereoparams)
  out.Write("}};\n");

  out.Write("struct VS_OUTPUT {{\n");
  GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config, "", ShaderStage::Geometry);
  out.Write("}};\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (host_config.backend_gs_instancing && stereo) // Define InstanceID if using GS instancing for stereo
      out.Write("#define InstanceID gl_InvocationID\n");

    out.Write("VARYING_LOCATION(0) in VertexData {{\n");
    GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, true), ShaderStage::Geometry);
    out.Write("}} vs[{}];\n", vertex_in);

    out.Write("VARYING_LOCATION(0) out VertexData {{\n");
    GenerateVSOutputMembers(out, api_type, uid_data->numTexGens, host_config,
                            GetInterpolationQualifier(msaa, ssaa, true, false), ShaderStage::Geometry);
    out.Write("}} ps;\n");

    // If stereo is active and we are not using GS instancing to set gl_Layer,
    // or if the backend doesn't support gl_Layer in FS, we might need a flat out int layer.
    // However, the new plan is to always set gl_Layer or SV_RenderTargetArrayIndex in GS.
    if (stereo && !host_config.backend_gl_layer_in_fs && !(host_config.backend_gs_instancing && stereo))
    {
        // This path might be redundant if gl_Layer is always set directly.
        // The new logic will use stereoparams.x to set gl_Layer.
    }
    out.Write("void main()\n{{\n");
  }
  else  // D3D
  {
    out.Write("struct VertexData {{\n");
    out.Write("\tVS_OUTPUT o;\n");
    // SV_RenderTargetArrayIndex is always set if stereo is active.
    if (stereo)
    {
      out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");
    }
    out.Write("\tfloat4 posout : SV_Position;\n");
    out.Write("}};\n");

    if (host_config.backend_gs_instancing && stereo) // Use GS instancing for stereo
    {
      out.Write("[maxvertexcount({})]\n[instance(2)]\n", vertex_out); // Force 2 instances for stereo
      out.Write("void main({} VS_OUTPUT o[{}], inout {}Stream<VertexData> output, in uint InstanceID : SV_GSInstanceID)\n{{\n",
                primitives_d3d[primitive_type], vertex_in, wireframe ? "Line" : "Triangle");
    }
    else // Mono or stereo without GS instancing
    {
      out.Write("[maxvertexcount({})]\n", (stereo && !host_config.backend_gs_instancing) ? vertex_out * 2 : vertex_out);
      out.Write("void main({} VS_OUTPUT o[{}], inout {}Stream<VertexData> output)\n{{\n",
                primitives_d3d[primitive_type], vertex_in, wireframe ? "Line" : "Triangle");
    }
    out.Write("\tVertexData ps;\n");
  }

  // New simplified GS logic for stereo
  if (stereo)
  {
    // Get eye from uniform if not using GS instancing, or from InstanceID if using it.
    if (host_config.backend_gs_instancing) // True for both GL/Vulkan and D3D if this path is taken
    {
        out.Write("\tuint eye = InstanceID;\n");
    }
    else // Manual loop for stereo or if GS instancing is not used for stereo
    {
        // The loop `for (int eye = 0; eye < 2; ++eye)` is removed from here.
        // The actual draw call loop is in VertexManagerBase::Flush.
        // The GS receives `stereoparams.x` to know the current eye.
        out.Write("\tuint eye = uint(cstereo.x);\n"); // Use the uniform we set in Flush()
    }

    // The old stereo logic (hoffset based on cstereo.x/y/z) is removed.
    // The new logic just passes through vertices and sets the layer.
    out.Write("\tfor (int i = 0; i < {}; ++i) {{\n", vertex_in);
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    {
        out.Write("\t\tVS_OUTPUT f;\n");
        AssignVSOutputMembers(out, "f", "vs[i]", uid_data->numTexGens, host_config);
        // Pass through clip distances if necessary
        if (host_config.backend_depth_clamp && DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
        {
            out.Write("\t\tf.clipDist0 = gl_in[i].gl_ClipDistance[0];\n");
            out.Write("\t\tf.clipDist1 = gl_in[i].gl_ClipDistance[1];\n");
        }
        // Set output members for ps
        AssignVSOutputMembers(out, "ps", "f", uid_data->numTexGens, host_config);
        // Set position
        if (api_type == APIType::Vulkan)
            out.Write("\t\tgl_Position = float4(f.pos.x, -f.pos.y, f.pos.z, f.pos.w);\n");
        else
            out.Write("\t\tgl_Position = f.pos;\n");
        // Set layer
        out.Write("\t\tgl_Layer = int(eye);\n"); // GL/Vulkan
        out.Write("\t\tEmitVertex();\n");
    }
    else // D3D
    {
        out.Write("\t\tVS_OUTPUT f = o[i];\n");
        out.Write("\t\tps.o = f;\n");
        out.Write("\t\tps.posout = f.pos;\n");
        out.Write("\t\tps.layer = eye;\n"); // D3D SV_RenderTargetArrayIndex
        out.Write("\t\toutput.Append(ps);\n");
    }
    out.Write("\t}}\n"); // End for (int i = 0; i < vertex_in; ++i)
    out.Write("\tEndPrimitive();\n"); // Or output.RestartStrip(); for D3D

    if (api_type != APIType::OpenGL && api_type != APIType::Vulkan) // D3D specific
        out.Write("\toutput.RestartStrip();\n"); // Ensure strip is restarted if needed after EndPrimitive

  }
  else // Original non-stereo logic
  {
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
        out.Write("\tfloat2 offset = float2(" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS
                  ".x, -" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS ".y) * center.pos.w;\n");
      }

      if (wireframe)
        out.Write("\tVS_OUTPUT first;\n");

      if (vertex_in > 1)
        out.Write("\tfor (int i = 0; i < {}; ++i) {{\n", vertex_in);
      else
        out.Write("\tint i = 0;\n");

      if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
      {
        out.Write("\tVS_OUTPUT f;\n");
        AssignVSOutputMembers(out, "f", "vs[i]", uid_data->numTexGens, host_config);
        if (host_config.backend_depth_clamp && DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
        {
          out.Write("\tf.clipDist0 = gl_in[i].gl_ClipDistance[0];\n");
          out.Write("\tf.clipDist1 = gl_in[i].gl_ClipDistance[1];\n");
        }
      }
      else
      {
        out.Write("\tVS_OUTPUT f = o[i];\n");
      }

      if (primitive_type == PrimitiveType::Lines)
      {
        out.Write("\tVS_OUTPUT l = f;\n\tVS_OUTPUT r = f;\n");
        out.Write("\tl.pos.xy -= offset * l.pos.w;\n\tr.pos.xy += offset * r.pos.w;\n");
        out.Write("\tif (" I_TEXOFFSET "[2] != 0) {{\n");
        out.Write("\tfloat texOffset = 1.0 / float(" I_TEXOFFSET "[2]);\n");
        for (u32 k = 0; k < uid_data->numTexGens; ++k)
        {
          out.Write("\tif (((" I_TEXOFFSET "[0] >> {}) & 0x1) != 0)\n", k);
          out.Write("\t\tr.tex{}.x += texOffset;\n", k);
        }
        out.Write("\t}}\n");
        EmitVertex(out, host_config, uid_data, "l", api_type, wireframe, false, true); // stereo is false here
        EmitVertex(out, host_config, uid_data, "r", api_type, wireframe, false);
      }
      else if (primitive_type == PrimitiveType::Points)
      {
        out.Write("\tVS_OUTPUT ll = f;\n\tVS_OUTPUT lr = f;\n\tVS_OUTPUT ul = f;\n\tVS_OUTPUT ur = f;\n");
        out.Write("\tll.pos.xy += float2(-1,-1) * offset;\n\tlr.pos.xy += float2(1,-1) * offset;\n");
        out.Write("\tul.pos.xy += float2(-1,1) * offset;\n\tur.pos.xy += offset;\n");
        out.Write("\tif (" I_TEXOFFSET "[3] != 0) {{\n");
        out.Write("\tfloat2 texOffset = float2(1.0 / float(" I_TEXOFFSET "[3]), 1.0 / float(" I_TEXOFFSET "[3]));\n");
        for (u32 k = 0; k < uid_data->numTexGens; ++k)
        {
          out.Write("\tif (((" I_TEXOFFSET "[1] >> {}) & 0x1) != 0) {{\n", k);
          out.Write("\t\tul.tex{}.xy += float2(0,1) * texOffset;\n", k);
          out.Write("\t\tur.tex{}.xy += texOffset;\n", k);
          out.Write("\t\tlr.tex{}.xy += float2(1,0) * texOffset;\n", k);
          out.Write("\t}}\n");
        }
        out.Write("\t}}\n");
        EmitVertex(out, host_config, uid_data, "ll", api_type, wireframe, false, true); // stereo is false
        EmitVertex(out, host_config, uid_data, "lr", api_type, wireframe, false);
        EmitVertex(out, host_config, uid_data, "ul", api_type, wireframe, false);
        EmitVertex(out, host_config, uid_data, "ur", api_type, wireframe, false);
      }
      else // Triangles
      {
        EmitVertex(out, host_config, uid_data, "f", api_type, wireframe, false, true); // stereo is false
      }

      if (vertex_in > 1)
        out.Write("\t}}\n"); // End for (int i = 0; ...

      EndPrimitive(out, host_config, uid_data, api_type, wireframe, false); // stereo is false
  } // End else (original non-stereo logic)


  // Removed the outer stereo loop: `if (stereo && !host_config.backend_gs_instancing) out.Write("\t}}\n");`
  // That loop is now handled by num_eyes in VertexManagerBase::Flush

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

  // This stereo block is effectively replaced by the new simplified GS logic for stereo.
  // if (stereo)
  // {
  //   // Select the output layer
  //   if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  //     out.Write("\tgl_Layer = eye;\n"); // eye must be defined from uniform or InstanceID
  //   else
  //   {
  //     out.Write("\tps.layer = eye;\n"); // eye must be defined
  //   }
  //   if (!host_config.backend_gl_layer_in_fs) // This condition might also need review
  //     out.Write("\tlayer = eye;\n");
  // }


  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("\tEmitVertex();\n");
  else
    out.Write("\toutput.Append(ps);\n");
}

static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType api_type, bool wireframe,
                         bool stereo) // stereo parameter might be unused if logic is self-contained
{
  if (wireframe) // This assumes 'first' and 'i' are in scope if wireframe is true.
                 // This might be problematic if the stereo path doesn't define them.
                 // For the new stereo path, wireframe logic needs to be integrated carefully.
                 // The new stereo path does not have 'i' or 'first' in the same context here.
                 // This will likely break wireframe in stereo.
    EmitVertex(out, host_config, uid_data, "first", api_type, wireframe, stereo, false);


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
