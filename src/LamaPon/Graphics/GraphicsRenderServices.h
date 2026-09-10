#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <functional>
#include <span>

namespace LamaPon
{
    // ParticleSystemが生成した完成済みquadの頂点です。描画API固有の
    // vertex型へ変換する責務はGraphicsRenderServicesが持ちます。
    struct ParticleRenderVertex final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT4 color{};
        DirectX::XMFLOAT2 textureCoordinate{};
    };

    // verticesは4頂点を1quadとする連続列です。spanとcallbackは
    // DrawParticlesの同期呼び出し中だけ有効で、実装側は保持しません。
    struct ParticleDrawRequest final
    {
        std::span<const ParticleRenderVertex> vertices;
        DirectX::XMFLOAT4X4 view{};
        DirectX::XMFLOAT4X4 projection{};
        GraphicsViewHandle texture;
        GraphicsViewHandle auxiliaryTexture;
        GraphicsViewHandle fallbackTexture;
        bool additive{ true };
        // 既存custom HLSL cacheとの過渡的な同期境界です。D3D12実装前に
        // API-neutralなshader/pipeline handleへ置き換えます。
        std::function<bool()> applyCustomPixelShader;
    };

    // 高レベルrendererが生成済みの描画要求を実効APIへ送る同期serviceです。
    // Device / Context / Effectなどの具象型をComponentへ公開しません。
    class GraphicsRenderServices
    {
    public:
        virtual ~GraphicsRenderServices() = default;

        GraphicsRenderServices() = default;
        GraphicsRenderServices(const GraphicsRenderServices&) = delete;
        GraphicsRenderServices& operator=(
            const GraphicsRenderServices&) = delete;

        [[nodiscard]] virtual bool DrawParticles(
            const ParticleDrawRequest& request) = 0;
    };
}
