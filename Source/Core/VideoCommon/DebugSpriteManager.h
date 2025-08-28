#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "VideoCommon/TextureConfig.h"

class AbstractPipeline;
class AbstractTexture;
class NativeVertexFormat;

namespace VideoCommon
{

class DebugSpriteManager
{
public:
    struct SpriteDrawInfo
    {
        std::string texture_path;
        float x = 0.5f, y = 0.5f;
        float width = 0.1f, height = 0.1f;
        float rotation_deg = 0.0f;
        std::array<float, 4> color{{1.0f, 1.0f, 1.0f, 1.0f}};
        int eye = -1; // -1 for both, 0 for left, 1 for right
    };

    DebugSpriteManager();
    ~DebugSpriteManager();

    static DebugSpriteManager* GetInstance();

    bool Initialize();
    void Shutdown();

    void AddSprite(const SpriteDrawInfo& info);
    void Render();
    void OnFrameEnd();

private:
    struct SpriteVertex
    {
        float pos[2];
        float uv[2];
    };
    
    AbstractTexture* GetTexture(const std::string& path);
    void CreateResources();
    void DestroyResources();

    std::vector<SpriteDrawInfo> m_sprites;
    std::unordered_map<std::string, std::unique_ptr<AbstractTexture>> m_texture_cache;

    std::unique_ptr<NativeVertexFormat> m_vertex_format;
    std::unique_ptr<AbstractPipeline> m_pipeline;

    bool m_resources_created = false;
    AbstractTextureFormat m_current_fb_format = AbstractTextureFormat::Undefined;
};

} // namespace VideoCommon
