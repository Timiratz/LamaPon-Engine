#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
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

    struct PrimitiveDirectionalLight final
    {
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        float intensity{ 1.0f };
    };

    struct PrimitivePointLight final
    {
        DirectX::XMFLOAT3 position{};
        float range{ 10.0f };
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        float intensity{ 1.0f };
    };

    // directionは光の進む向き、cone cosineはSpotLightDataと同じ値です。
    struct PrimitiveSpotLight final
    {
        DirectX::XMFLOAT3 position{};
        float range{ 10.0f };
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
        float innerConeCosine{ 0.9238795f };
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        float intensity{ 1.0f };
        float outerConeCosine{ 0.8191520f };
    };

    struct PrimitiveDirectionalShadow final
    {
        std::array<DirectX::XMFLOAT4X4, 4>
            lightViewProjections{};
        std::array<float, 4> cascadeSplits{};
        GraphicsViewHandle texture;
        std::size_t lightIndex{};
        std::size_t cascadeCount{};
        float bias{ 0.0015f };
        float normalBias{ 0.0025f };
        float strength{ 0.85f };
        float inverseResolution{};
        bool enabled{};
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
        float roughness{ 0.5f };
        float metallic{};
        float normalStrength{ 1.0f };
        float occlusionStrength{ 1.0f };
        DirectX::XMFLOAT3 emissiveFactor{};
        DirectX::XMFLOAT3 ambientColor{ 0.65f, 0.72f, 0.85f };
        float ambientIntensity{ 0.35f };
        std::array<PrimitiveDirectionalLight, 4> directionalLights{};
        std::size_t directionalLightCount{};
        // D3D11の定数buffer経路と同じ上限です（Point 16灯、Spot 8灯）。
        std::array<PrimitivePointLight, 16> pointLights{};
        std::size_t pointLightCount{};
        std::array<PrimitiveSpotLight, 8> spotLights{};
        std::size_t spotLightCount{};
        PrimitiveDirectionalShadow directionalShadow;
        GraphicsViewHandle albedo;
        GraphicsViewHandle normalTexture;
        GraphicsViewHandle roughnessTexture;
        GraphicsViewHandle metallicTexture;
        GraphicsViewHandle occlusionTexture;
        GraphicsViewHandle emissiveTexture;
        GraphicsViewHandle fallbackTexture;
        bool alphaBlend{};
        bool depthTest{ true };
        bool depthWrite{ true };
        // ShadowMapなど、色を書かず深度だけを生成する描画要求です。
        // Backendは現在bind済みの深度描画先を維持し、pixel shaderや
        // material textureを使わずに描画します。
        bool depthOnly{};
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
