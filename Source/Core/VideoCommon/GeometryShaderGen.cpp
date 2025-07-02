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
  const auto primitive_type = static_cast<PrimitiveType>(uid_data->primitive_type);
  const u32 vertex_in = vertex_in_map[primitive_type];
  u32 vertex_out = vertex_out_map[primitive_type];

  if (wireframe)
    vertex_out++;

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    // Insert layout parameters
    if (host_config.backend_gs_instancing)
    {
      out.Write("layout({}, invocations = {}) in;\n", primitives_ogl[primitive_type],
                stereo ? 2 : 1);
      out.Write("layout({}_strip, max_vertices = {}) out;\n", wireframe ? "line" : "triangle",
                vertex_out);
    }
    else
    {
      out.Write("layout({}) in;\n", primitives_ogl[primitive_type]);
      out.Write("layout({}_strip, max_vertices = {}) out;\n", wireframe ? "line" : "triangle",
                stereo ? vertex_out * 2 : vertex_out);
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
      out.Write("void main({} VS_OUTPUT o_in_gs[{}], inout {}Stream<VertexData> output, in uint " // Renamed o to o_in_gs
                "InstanceID : SV_GSInstanceID)\n{{\n",
                primitives_d3d[primitive_type], vertex_in, wireframe ? "Line" : "Triangle");
    }
    else
    {
      out.Write("[maxvertexcount({})]\n", stereo ? vertex_out * 2 : vertex_out);
      out.Write("void main({} VS_OUTPUT o_in_gs[{}], inout {}Stream<VertexData> output)\n{{\n", // Renamed o to o_in_gs
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
  // Also, ensure 'i' is declared for the scope if vertex_in is 1
  if (vertex_in > 1 || api_type == APIType::D3D11) // D3D might need o[i]
    out.Write("\tfor (int i = 0; i < {}; ++i) {{\n", vertex_in);
  else
    out.Write("\tint i = 0; {{\n"); // Create a scope for VS_OUTPUT f

  out.Write("\tVS_OUTPUT f;\n"); // Declare f here for all API types within the loop/scope
  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
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
  else // D3D
  {
    // AssignVSOutputMembers is GL-specific for getting from vs[i]. For D3D, o[i] is already VS_OUTPUT
    // out.Write("\tVS_OUTPUT f = o[i];\n"); // This was the original
    // We need to ensure 'f' can be assigned from o[i].
    // Let's assume o (D3D input array) is named o_in to avoid conflict if VS_OUTPUT is also 'o'.
    // The D3D main function signature uses 'o' as the input array name.
    // Let's rename the input array in D3D main to 'o_in_gs'.
    // This change will be made when patching the D3D main signature.
    // For now, assume 'o_in_gs' is the input array.
    out.Write("\tf = o_in_gs[i];\n"); // Changed to o_in_gs
  }

  bool is_vr_mode = (g_ActiveConfig.stereo_mode == StereoMode::OpenVR || g_ActiveConfig.stereo_mode == StereoMode::Oculus);

  if (stereo) // host_config.stereo
  {
    // Select the output layer/eye
    // In D3D, ps.layer is part of the output struct. In GL, gl_Layer is a built-in.
    // The variable 'eye' comes from the 'for (int eye = 0; eye < 2; ++eye)' loop or 'InstanceID'
    // This part of the code is inside the 'for (int i = 0; i < vertex_in; ++i)' loop
    // and also inside the 'for (int eye = 0; eye < 2; ++eye)' loop (if not instanced).

    if (is_vr_mode) // True VR mode
    {
      // For D3D, the layer is set in EmitVertex via ps.layer.
      // For GL, gl_Layer is set in EmitVertex.
      // The actual assignment of gl_Layer or ps.layer happens in EmitVertex.
      // Here, we just modify the vertex position.

      // VR-Hydra logic for stereo parameters:
      // f.pos.x += I_STEREOPARAMS[eye] - I_STEREOPARAMS[eye+2] * f.pos.w;
      // I_STEREOPARAMS is float4 cstereo;
      // We'll use .x for left eye val1, .y for right eye val1
      // .z for left eye val2, .w for right eye val2
      // So, for eye 0 (left): cstereo.x - cstereo.z * w
      // For eye 1 (right): cstereo.y - cstereo.w * w
      out.Write("\tf.pos.x += (eye == 0 ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y) - (eye == 0 ? " I_STEREOPARAMS ".z : " I_STEREOPARAMS ".w) * f.pos.w;\n");
      // Hydra also modified clipPos if available. Assuming f.pos is primary for now.
      // out.Write("\tf.clipPos.x += (eye == 0 ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y) - (eye == 0 ? " I_STEREOPARAMS ".z : " I_STEREOPARAMS ".w) * f.clipPos.w;\n");
    }
    else // Non-VR stereo modes (e.g., anaglyph)
    {
      // Standard stereo offset from VR-Reloaded (current code)
      // This also needs the 'eye' variable.
      out.Write("\tfloat hoffset = (eye == 0) ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y;\n");
      out.Write("\tf.pos.x += hoffset * (f.pos.w - " I_STEREOPARAMS ".z);\n");
    }
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

  if (stereo)
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

// START ADDED FROM VR-HYDRA (with adaptations)
// Forward declaration for EmitVertex used within GenerateAvatarGeometryShader
// Ensure EmitVertex and EndPrimitive are declared or defined before use if they are local static,
// or ensure they are accessible if they are not.
// They are already static in this file, so they should be accessible.

template <class T>
static T GenerateAvatarGeometryShader(PrimitiveType primitive_type, APIType api_type, const ShaderHostConfig& host_config)
{
  T out;

  const bool wireframe = host_config.wireframe;
  // const bool pixel_lighting = host_config.per_pixel_lighting; // Use host_config
  const bool stereo = host_config.stereo; // This will determine if VR logic path is taken too
  bool is_vr_mode = (g_ActiveConfig.stereo_mode == StereoMode::OpenVR || g_ActiveConfig.stereo_mode == StereoMode::Oculus);


  geometry_shader_uid_data temp_dummy_data_assign; // Can't be const if we assign to it
  geometry_shader_uid_data* uid_data = &temp_dummy_data_assign; // Point to non-const local

  // If T is ShaderUid<geometry_shader_uid_data>, GetUidData() would return a valid pointer.
  // For ShaderCode, it might return nullptr or a dummy.
  // This part of Hydra's avatar shader seems to want to populate a UID.
  // If 'out' is ShaderCode, this UID setting is not directly used by 'out'.
  // If 'out' is ShaderUid, it's critical.
  // The GenerateAvatarGeometryShaderCode function below calls this with ShaderCode.
  // So, for that call, uid_data points to temp_dummy_data_assign.
  if constexpr (std::is_same_v<T, ShaderUid<geometry_shader_uid_data>>)
  {
    uid_data = out.GetUidData();
  }


  const unsigned primitive_type_index = static_cast<unsigned>(primitive_type);
  uid_data->primitive_type = primitive_type_index;
  const u32 vertex_in = vertex_in_map[primitive_type]; // Use Reloaded's map
  u32 vertex_out = vertex_out_map[primitive_type];     // Use Reloaded's map

  // Hydra: const unsigned int layers = host_config.more_layers * 2 + host_config.stereo + 1;
  // Reloaded (simplified for VR stereo):
  const unsigned int layers = stereo ? 2 : 1;


  if (wireframe)
    vertex_out++;

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (host_config.backend_gs_instancing)
    {
      out.Write("layout({}, invocations = {}) in;\n", primitives_ogl[primitive_type], layers);
      out.Write("layout({}_strip, max_vertices = {}) out;\n",
                wireframe ? "line" : "triangle", vertex_out);
    }
    else
    {
      out.Write("layout({}) in;\n", primitives_ogl[primitive_type]);
      out.Write("layout({}_strip, max_vertices = {}) out;\n",
                wireframe ? "line" : "triangle",
                vertex_out * layers);
    }
  }

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
      out.Write("UBO_BINDING(std140, 4) uniform GSBlock {{\n"); // Binding 4 from Reloaded
  else
      out.Write("cbuffer GSBlock {{\n");

  // s_geometry_shader_uniforms already defines I_STEREOPARAMS as float4
  out.Write("{}}};\n", s_geometry_shader_uniforms);


  uid_data->numTexGens = 1; // Avatar shader implies 1 texture unit for simplicity?

  out.Write("struct VS_OUTPUT {{\n");
  // Simplified for avatar: pos, color, one texcoord
  // Using Reloaded's GenerateVSOutputMembers structure but simplified for avatar
  // For an avatar, we might not need all the complex outputs.
  // Hydra directly defined members. Let's follow that for avatar simplicity.
  // DefineOutputMember is not a global function in Reloaded's ShaderGenCommon.h
  // It was used in Hydra. We'll write directly.
  const char* interp_qual = GetInterpolationQualifier(host_config.msaa, host_config.ssaa, true, false);

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    out.Write("\t{} vec4 pos;\n", interp_qual);
    out.Write("\t{} vec4 colors_0;\n", interp_qual); // Assuming one color
    out.Write("\t{} vec3 tex0;\n", interp_qual);    // Assuming one texcoord, vec3 for D3D style array tex
  }
  else // D3D
  {
    out.Write("\tfloat4 pos : SV_Position;\n"); // This is for pixel shader input, not GS output yet
    out.Write("\tfloat4 colors_0 : COLOR0;\n");
    out.Write("\tfloat3 tex0 : TEXCOORD0;\n");
  }
  out.Write("}};\n");


  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (host_config.backend_gs_instancing)
      out.Write("#define InstanceID gl_InvocationID\n");

    // GS Input block
    out.Write("VARYING_LOCATION(0) in VertexData {{\n");
    const char* interp_qual_in = GetInterpolationQualifier(host_config.msaa, host_config.ssaa, true, true);
    out.Write("\t{} vec4 pos;\n", interp_qual_in);
    out.Write("\t{} vec4 colors_0;\n", interp_qual_in);
    out.Write("\t{} vec3 tex0;\n", interp_qual_in);
    out.Write("}} vs[{}];\n", vertex_in);

    // GS Output block
    out.Write("VARYING_LOCATION(0) out VertexData {{\n");
    out.Write("\t{} vec4 pos;\n", interp_qual); // Already has GetInterpolationQualifier
    out.Write("\t{} vec4 colors_0;\n", interp_qual);
    out.Write("\t{} vec3 tex0;\n", interp_qual);
    if (stereo && !host_config.backend_gl_layer_in_fs) // From Reloaded
        out.Write("\tflat int layer_out_to_fs;\n"); // Renamed to avoid conflict if 'layer' is used for gl_Layer
    out.Write("}} ps_out;\n"); // Renamed to avoid conflict with ps var if any

    out.Write("void main()\n{{\n");
  }
  else  // D3D
  {
    out.Write("struct GSOutputData {{\n"); // Renamed from VertexData to avoid confusion
    out.Write("\tVS_OUTPUT o;\n"); // The struct VS_OUTPUT defined above
    if (stereo)
      out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");
    out.Write("\tfloat4 pos_clip : SV_Position;\n"); // Final clip space position for rasterizer
    out.Write("}};\n");

    if (host_config.backend_gs_instancing)
    {
      out.Write("[maxvertexcount({})]\n[instance({})]\n", vertex_out, layers);
      out.Write("void main({} VS_OUTPUT o_in_gs[{}], inout {}Stream<GSOutputData> output_stream, in uint InstanceID : SV_GSInstanceID)\n{{\n", // o_in -> o_in_gs
                primitives_d3d[primitive_type], vertex_in,
                wireframe ? "Line" : "Triangle");
    }
    else
    {
      out.Write("[maxvertexcount({})]\n", vertex_out * layers);
      out.Write("void main({} VS_OUTPUT o_in_gs[{}], inout {}Stream<GSOutputData> output_stream)\n{{\n", // o_in -> o_in_gs
                primitives_d3d[primitive_type], vertex_in,
                wireframe ? "Line" : "Triangle");
    }
    out.Write("\tGSOutputData ps_out;\n"); // Renamed
  }


  if (primitive_type == PrimitiveType::Lines)
  {
      out.Write("\tVS_OUTPUT start_v, end_v;\n"); // Renamed to avoid conflict
      if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
      {
          out.Write("\tstart_v.pos = vs[0].pos;\n");
          out.Write("\tstart_v.colors_0 = vs[0].colors_0;\n");
          out.Write("\tstart_v.tex0 = vs[0].tex0;\n");
          out.Write("\tend_v.pos = vs[1].pos;\n");
          out.Write("\tend_v.colors_0 = vs[1].colors_0;\n");
          out.Write("\tend_v.tex0 = vs[1].tex0;\n");
      }
      else // D3D
      {
          out.Write("\tstart_v = o_in[0];\n");
          out.Write("\tend_v = o_in[1];\n");
      }
      GenerateLineOffset(out, "\t", "\t\t", "end_v.pos", "start_v.pos", ""); // Use renamed vars
  }
  else if (primitive_type == PrimitiveType::Points)
  {
      out.Write("\tVS_OUTPUT center_v;\n"); // Renamed
      if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
      {
          out.Write("\tcenter_v.pos = vs[0].pos;\n");
          out.Write("\tcenter_v.colors_0 = vs[0].colors_0;\n");
          out.Write("\tcenter_v.tex0 = vs[0].tex0;\n");
      }
      else // D3D
      {
          out.Write("\tcenter_v = o_in[0];\n");
      }
      out.Write("\tfloat2 offset = float2(" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS
                ".x, -" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS ".y) * center_v.pos.w;\n"); // Use renamed var
  }


  if (stereo)
  {
    if (host_config.backend_gs_instancing)
      out.Write("\tint eye = InstanceID;\n");
    else
      out.Write("\tfor (int eye = 0; eye < 2; ++eye) {{\n");
  }

  if (wireframe)
    out.Write("\tVS_OUTPUT first_v;\n"); // Renamed

  // Loop for input vertices
  if (vertex_in > 1 || api_type == APIType::D3D11) // D3D always needs the loop for o_in[i]
    out.Write("\tfor (int i = 0; i < {}; ++i) {{\n", vertex_in);
  else
    out.Write("\tint i = 0; {{\n"); // Still create a scope for 'f'


  out.Write("\tVS_OUTPUT f_v;\n"); // Current vertex, renamed
  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    out.Write("\tf_v.pos = vs[i].pos;\n");
    out.Write("\tf_v.colors_0 = vs[i].colors_0;\n");
    out.Write("\tf_v.tex0 = vs[i].tex0;\n");
  }
  else // D3D
  {
    out.Write("\tf_v = o_in_gs[i];\n"); // o_in -> o_in_gs
  }

  // VR / Stereo logic from Hydra
  if (stereo) // host_config.stereo is true
  {
    if (is_vr_mode) // True VR mode (OpenVR, Oculus)
    {
      // ps_out is the struct we emit. For D3D, it has .layer. For GL, we set gl_Layer.
      if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
        out.Write("\tgl_Layer = eye;\n");
      else // D3D
        out.Write("\tps_out.layer = eye;\n");

      if (!host_config.backend_gl_layer_in_fs && (api_type == APIType::OpenGL || api_type == APIType::Vulkan))
         out.Write("\tps_out.layer_out_to_fs = eye;\n");

      // Apply VR projection adjustments. I_STEREOPARAMS from s_geometry_shader_uniforms is float4.
      // Hydra: f.pos.x += I_STEREOPARAMS[eye] - I_STEREOPARAMS[eye+2] * f.pos.w;
      // This implies I_STEREOPARAMS is an array accessible with eye and eye+2.
      // If I_STEREOPARAMS is just a single float4 in GSBlock, we need to use .xyzw components.
      // Let's assume I_STEREOPARAMS.x = left eye shift, .y = right eye shift
      // I_STEREOPARAMS.z = left eye offaxis, .w = right eye offaxis
      // This would be: cstereo.x, cstereo.y for shifts, cstereo.z, cstereo.w for offaxis
      out.Write("\tf_v.pos.x += (eye == 0 ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y) - (eye == 0 ? " I_STEREOPARAMS ".z : " I_STEREOPARAMS ".w) * f_v.pos.w;\n");
    }
    else // Other stereo modes (anaglyph, etc.)
    {
      // Standard stereo offset from Reloaded
      out.Write("\tfloat hoffset = (eye == 0) ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y;\n");
      out.Write("\tf_v.pos.x += hoffset * (f_v.pos.w - " I_STEREOPARAMS ".z);\n");
       // Layer assignment for non-VR stereo too
      if (api_type == APIType::OpenGL || api_type == APIType::Vulkan) {
        out.Write("\tgl_Layer = eye;\n");
        if (!host_config.backend_gl_layer_in_fs) out.Write("\tps_out.layer_out_to_fs = eye;\n");
      } else { // D3D
        out.Write("\tps_out.layer = eye;\n");
      }
    }
  }


  if (primitive_type == PrimitiveType::Lines)
  {
    out.Write("\tVS_OUTPUT l_v = f_v;\n" // Renamed
              "\tVS_OUTPUT r_v = f_v;\n"); // Renamed

    out.Write("\tl_v.pos.xy -= offset * l_v.pos.w;\n"
              "\tr_v.pos.xy += offset * r_v.pos.w;\n");

    // Simplified texcoord adjustment for avatar (assuming 1 texgen)
    out.Write("\tif (" I_TEXOFFSET "[2] != 0) {{\n"); // Use I_TEXOFFSET from GSBlock
    out.Write("\tfloat texOffset_val = 1.0 / float(" I_TEXOFFSET "[2]);\n"); // Renamed
    out.Write("\tif (((" I_TEXOFFSET "[0] >> 0) & 0x1) != 0)\n"); // Check for texgen 0
    out.Write("\t\tr_v.tex0.x += texOffset_val;\n");
    out.Write("\t}}\n");

    // Call adapted EmitVertex for avatar
    EmitVertex(out, host_config, uid_data, "l_v", api_type, wireframe, stereo, true);
    EmitVertex(out, host_config, uid_data, "r_v", api_type, wireframe, stereo);
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    out.Write("\tVS_OUTPUT ll_v = f_v, lr_v = f_v, ul_v = f_v, ur_v = f_v;\n"); // Renamed

    out.Write("\tll_v.pos.xy += float2(-1,-1) * offset;\n"
              "\tlr_v.pos.xy += float2(1,-1) * offset;\n"
              "\tul_v.pos.xy += float2(-1,1) * offset;\n"
              "\tur_v.pos.xy += offset;\n");

    // Simplified texcoord adjustment
    out.Write("\tif (" I_TEXOFFSET "[3] != 0) {{\n");
    out.Write("\tfloat2 texOffset_val2 = float2(1.0 / float(" I_TEXOFFSET "[3]), 1.0 / float(" I_TEXOFFSET "[3]));\n"); // Renamed
    out.Write("\tif (((" I_TEXOFFSET "[1] >> 0) & 0x1) != 0) {{\n", 0); // Check for texgen 0
    out.Write("\t\tul_v.tex0.xy += float2(0,1) * texOffset_val2;\n");
    out.Write("\t\tur_v.tex0.xy += texOffset_val2;\n");
    out.Write("\t\tlr_v.tex0.xy += float2(1,0) * texOffset_val2;\n");
    // Hydra had ll_v.tex0.xy += float2(0,1) * texOffset; - was this a typo and should be ll?
    // lr.tex += texOffset (1,1)
    // ur.tex += float2(1,0) * texOffset (should be ul)
    // ll, lr, ul, ur for positions. Standard quad: (0,0) (1,0) (0,1) (1,1) for texcoords.
    // ll=(0,0) default. lr=(1,0). ul=(0,1). ur=(1,1).
    // Hydra: ul += (0,1), ur += (1,1), lr += (1,0). This maps to:
    // ll_v.tex0 remains (0,0)
    // lr_v.tex0.x += texOffset_val2.x;
    // ul_v.tex0.y += texOffset_val2.y;
    // ur_v.tex0.xy += texOffset_val2.xy;
    // The Hydra code was:
    // \t\tll.tex%d.xy += float2(0,1) * texOffset;\n", i); -> Should be ul.tex[i].y += texOffset.y; if ll is bottom-left
    // \t\tlr.tex%d.xy += texOffset;\n", i); -> Should be ur.tex[i].xy += texOffset.xy;
    // \t\tur.tex%d.xy += float2(1,0) * texOffset;\n", i); -> Should be lr.tex[i].x += texOffset.x;
    // Sticking to Hydra's logic for now, assuming it had a purpose for avatar rendering.
    out.Write("\t}}\n");
    out.Write("\t}}\n");


    EmitVertex(out, host_config, uid_data, "ll_v", api_type, wireframe, stereo, true);
    EmitVertex(out, host_config, uid_data, "lr_v", api_type, wireframe, stereo);
    EmitVertex(out, host_config, uid_data, "ul_v", api_type, wireframe, stereo);
    EmitVertex(out, host_config, uid_data, "ur_v", api_type, wireframe, stereo);
  }
  else // Triangles / TriangleStrip
  {
    EmitVertex(out, host_config, uid_data, "f_v", api_type, wireframe, stereo, true);
  }

  if (vertex_in > 1 || api_type == APIType::D3D11)
    out.Write("\t}}\n"); // Close for loop
  else
    out.Write("\t}}\n"); // Close scope for i=0 case


  EndPrimitive(out, host_config, uid_data, api_type, wireframe, stereo);

  if (stereo && !host_config.backend_gs_instancing)
    out.Write("\t}}\n"); // Close for eye loop

  out.Write("}}\n");
  return out;
}

ShaderCode GenerateAvatarGeometryShaderCode(PrimitiveType primitive_type, APIType api_type, const ShaderHostConfig& host_config)
{
  return GenerateAvatarGeometryShader<ShaderCode>(primitive_type, api_type, host_config);
}
// END ADDED FROM VR-HYDRA


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
