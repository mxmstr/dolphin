
#include "Common/Matrix.h"

#include "VideoCommon/DebugSpriteManager.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/TextureUtils.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "FramebufferManager.h"
#include "OnScreenDisplay.h"
#include "VR.h"
#include "VideoConfig.h"

namespace VideoCommon
{
// Simple shaders for drawing textured quads
// It must handle multiview for VR.
static const char* s_vertex_shader = R"(
    UBO_BINDING(std140, 1) uniform SpriteUniforms
    {
        mat4 projection[2];
        mat4 model;
        vec4 color;
        int eye;
    };

    ATTRIBUTE_LOCATION(0) in vec2 rawpos;
    ATTRIBUTE_LOCATION(1) in vec2 rawuv;

    VARYING_LOCATION(0) centroid smooth out vec2 v_uv;
    VARYING_LOCATION(1) centroid smooth out vec4 v_color;
    VARYING_LOCATION(2) flat out int v_layer_index; // <-- ADD THIS

    void main()
    {
        v_uv = rawuv;
        v_color = color;
        v_layer_index = gl_ViewIndex; // <-- ADD THIS

        if (eye != -1 && eye != gl_ViewIndex)
        {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // Off-screen
            return;
        }

        vec2 centered_pos = rawpos - vec2(0.5, 0.5);
        gl_Position = projection[gl_ViewIndex] * model * vec4(centered_pos, 0.0, 1.0);

        //gl_Position.y = -gl_Position.y;
    }
)";

static const char* s_pixel_shader = R"(
    SAMPLER_BINDING(0) uniform sampler2DArray sprite_texture;

    VARYING_LOCATION(0) centroid smooth in vec2 v_uv;
    VARYING_LOCATION(1) centroid smooth in vec4 v_color;
    VARYING_LOCATION(2) flat in int v_layer_index; // <-- ADD THIS

    FRAGMENT_OUTPUT_LOCATION(0) out vec4 ocol0;

    void main()
    {
        // Use the layer index from the vertex shader
        ocol0 = texture(sprite_texture, vec3(v_uv, float(v_layer_index))) * v_color; // <-- CHANGE THIS
        //ocol0 = vec4(1.0, 0.0, 0.0, 1.0); 
    }
)";

DebugSpriteManager::DebugSpriteManager() = default;
DebugSpriteManager::~DebugSpriteManager() = default;

DebugSpriteManager* DebugSpriteManager::GetInstance()
{
    static std::unique_ptr<DebugSpriteManager> s_debug_sprite_manager;

    if (!s_debug_sprite_manager)
      s_debug_sprite_manager = std::make_unique<DebugSpriteManager>();

    return s_debug_sprite_manager.get();
}

bool DebugSpriteManager::Initialize()
{
    CreateResources();
    return m_resources_created;
}

void DebugSpriteManager::Shutdown()
{
    DestroyResources();
}

void DebugSpriteManager::AddSprite(const SpriteDrawInfo& info)
{
    m_sprites.push_back(info);
}

void DebugSpriteManager::OnFrameEnd()
{
    m_sprites.clear();
}

void DebugSpriteManager::CreateResources()
{
    if (m_resources_created)
        return;

    AbstractFramebuffer* current_fb = g_gfx->GetCurrentFramebuffer();
    if (!current_fb)
        return; // Can't create resources without a target framebuffer

    // 1. Vertex Format
    PortableVertexDeclaration vdecl = {};
    vdecl.stride = sizeof(SpriteVertex);
    vdecl.position.enable = true;
    vdecl.position.components = 2;
    vdecl.position.type = ComponentFormat::Float;
    vdecl.position.offset = offsetof(SpriteVertex, pos);
    vdecl.texcoords[0].enable = true;
    vdecl.texcoords[0].components = 2;
    vdecl.texcoords[0].type = ComponentFormat::Float;
    vdecl.texcoords[0].offset = offsetof(SpriteVertex, uv);
    m_vertex_format = g_gfx->CreateNativeVertexFormat(vdecl);

    // 2. Shaders and Pipeline
    std::string vs_source = s_vertex_shader;
    std::string ps_source = s_pixel_shader;

    auto vs = g_gfx->CreateShaderFromSource(ShaderStage::Vertex, vs_source, "DebugSpriteVS");
    auto ps = g_gfx->CreateShaderFromSource(ShaderStage::Pixel, ps_source, "DebugSpritePS");

    if (!vs || !ps)
    {
        PanicAlertFmt("Failed to create debug sprite shaders.");
        return;
    }

    AbstractPipelineConfig config = {};
    config.vertex_format = m_vertex_format.get();
    config.vertex_shader = vs.get();
    config.pixel_shader = ps.get();
    config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::TriangleStrip);
    config.depth_state = RenderState::GetNoDepthTestingDepthState();
    config.blending_state = RenderState::GetAlphaBlendBlendState();

    config.framebuffer_state = RenderState::GetColorFramebufferState(m_current_fb_format);
    config.framebuffer_state.samples = current_fb->GetSamples();

    config.geometry_shader = nullptr;

    config.usage = AbstractPipelineUsage::Utility;
    
    m_pipeline = g_gfx->CreatePipeline(config);

    if (!m_pipeline)
    {
        PanicAlertFmt("Failed to create debug sprite pipeline.");
        return;
    }
    
    m_resources_created = true;
}

void DebugSpriteManager::DestroyResources()
{
    g_gfx->WaitForGPUIdle();
    m_pipeline.reset();
    m_vertex_format.reset();
    m_texture_cache.clear();
    m_resources_created = false;
}

AbstractTexture* DebugSpriteManager::GetTexture(const std::string& path)
{
    auto it = m_texture_cache.find(path);
    if (it != m_texture_cache.end())
        return it->second.get();
    
    CustomTextureData::ArraySlice::Level level;
    if (!LoadPNGTexture(&level, path))
    {
        OSD::AddMessage(fmt::format("Failed to load debug sprite: {}", path), 5000);
        // Insert nullptr to prevent trying to load again
        m_texture_cache[path] = nullptr;
        return nullptr;
    }

    const u32 num_layers = (g_ActiveConfig.stereo_mode != StereoMode::Off) ? 2 : 1;

    TextureConfig config(level.width, level.height, 1, num_layers, 1, level.format, 0, AbstractTextureType::Texture_2DArray);
    auto texture = g_gfx->CreateTexture(config, path);
    if (texture)
    {
        for (u32 i = 0; i < num_layers; ++i)
        {
            texture->Load(0, config.width, config.height, config.width, level.data.data(), level.data.size(), i);
        }
    }

    auto [new_it, success] = m_texture_cache.emplace(path, std::move(texture));
    return new_it->second.get();
}

void DebugSpriteManager::Render()
{
    if (m_sprites.empty())
        return;

    AbstractFramebuffer* current_fb = g_gfx->GetCurrentFramebuffer();
    if (!current_fb)
        return;

    AbstractTextureFormat fb_format = current_fb->GetColorFormat();
    if (!m_resources_created || m_current_fb_format != fb_format)
    {
        DestroyResources();
        m_current_fb_format = fb_format; // Store the new format
        CreateResources();
    }
    
    if (!m_resources_created) // Check again in case creation failed
        return;

    g_gfx->SetPipeline(m_pipeline.get());
    
    // Simple ortho projection for both eyes
    Common::Matrix44 ortho_proj = Common::Matrix44::Identity();//Common::Matrix44::GenerateOrthoMatrix(0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f);
    
    struct SpriteUniforms
    {
        std::array<float, 32> projection; // 2 mat4
        std::array<float, 16> model;
        std::array<float, 4> color;
        s32 eye;
        s32 _pad[3];
    };

    // Prepare a single quad vertex buffer
    const SpriteVertex quad_vertices[] = {
        {{0.0f, 0.0f}, {0.0f, 0.0f}},
        {{1.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.0f, 1.0f}, {0.0f, 1.0f}},
        {{1.0f, 1.0f}, {1.0f, 1.0f}},
    };
    u32 base_vtx, base_idx;
    g_vertex_manager->UploadUtilityVertices(quad_vertices, sizeof(SpriteVertex), 4, nullptr, 0, &base_vtx, &base_idx);

    for (const auto& sprite : m_sprites)
    {
        AbstractTexture* tex = GetTexture(sprite.texture_path);
        if (!tex)
            continue;
            
        g_gfx->SetTexture(0, tex);
        g_gfx->SetSamplerState(0, RenderState::GetSpriteSamplerState());
        //g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());

        Common::Matrix44 model_matrix = Common::Matrix44::Identity();
        //model_matrix *= Common::Matrix44::Translate({ sprite.x, sprite.y, 0.0f });
        //model_matrix *= Common::Matrix44::RotateZ(DEGREES_TO_RADIANS(sprite.rotation_deg));
        //model_matrix *= Common::Matrix44::Scale({ sprite.width, sprite.height, 1.0f });
        
        SpriteUniforms uniforms;
        memcpy(uniforms.projection.data(), ortho_proj.data.data(), sizeof(float) * 16);
        memcpy(uniforms.projection.data() + 16, ortho_proj.data.data(), sizeof(float) * 16);
        memcpy(uniforms.model.data(), model_matrix.data.data(), sizeof(float) * 16);
        uniforms.color = sprite.color;
        uniforms.eye = sprite.eye;

        g_vertex_manager->UploadUtilityUniforms(&uniforms, sizeof(uniforms));

        g_gfx->Draw(base_vtx, 4);
    }

    m_sprites.clear();
}
} // namespace VideoCommon
