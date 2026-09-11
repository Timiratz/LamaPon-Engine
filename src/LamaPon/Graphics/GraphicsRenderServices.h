#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <functional>
#include <span>
#include <cstdint>

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

    enum class PrimitiveRenderShape : std::uint8_t
    {
        Cube,
        Sphere,
        Cylinder,
        Plane,
        Procedural
    };

    struct PrimitiveRenderVertex final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT2 textureCoordinate{};
    };

    // MeshRendererがAPI固有objectを持たずに送る最小3D描画要求です。
    // spanはDrawPrimitiveの同期呼び出し中だけ有効です。
    struct PrimitiveDrawRequest final
    {
        PrimitiveRenderShape shape{ PrimitiveRenderShape::Cube };
        std::span<const PrimitiveRenderVertex> vertices;
        std::span<const std::uint32_t> indices;
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMFLOAT4X4 view{};
        DirectX::XMFLOAT4X4 projection{};
        DirectX::XMFLOAT4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        GraphicsViewHandle albedo;
        GraphicsViewHandle fallbackTexture;
        bool alphaBlend{};
        bool depthTest{ true };
        bool depthWrite{ true };
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
        // D3D11は従来のComponent描画を維持するため、未対応serviceの
        // 既定値はfalseです。D3D12がこの共通要求を実装します。
        [[nodiscard]] virtual bool DrawPrimitive(
            const PrimitiveDrawRequest&)
        {
            return false;
        }
    };
}
