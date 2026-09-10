#pragma once

#include "LamaPon/Graphics/GraphicsDeviceShaderState.h"
#include "LamaPon/Graphics/GraphicsResource.h"
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class CommonStates;
        class SpriteBatch;
    }
}

namespace LamaPon
{
    class GraphicsBackend;
    class GraphicsRenderServices;
    struct TextureResourceSnapshot;

    namespace Detail
    {
        enum class D3D11SpriteBatchOwner : std::uint8_t
        {
            None,
            Legacy,
            Neutral,
            Poisoned
        };

        // GraphicsDeviceの公開レイアウトから隔離するDirectX 11固有状態です。
        // GraphicsDeviceApiResourcesだけが所有し、Backendより先に破棄します。
        struct GraphicsDeviceD3D11Resources final
        {
            GraphicsDeviceD3D11Resources(
                ID3D11Device* device,
                ID3D11DeviceContext* context);
            ~GraphicsDeviceD3D11Resources();

            GraphicsDeviceD3D11Resources(
                const GraphicsDeviceD3D11Resources&) = delete;
            GraphicsDeviceD3D11Resources& operator=(
                const GraphicsDeviceD3D11Resources&) = delete;

            // 部分初期化の巻き戻しと通常終了の両方から安全に呼べます。
            void Reset() noexcept;
            // Shader workerはAssetManagerとDeviceを借用します。両方が
            // 生存している間に明示的に停止します。
            void QuiesceShaderWork() noexcept;
            // Device世代に属する高レベルD3D11資源だけを破棄します。
            // SpriteBatch/CommonStates等の描画serviceはResetが後で破棄します。
            void ResetHighLevelResources() noexcept;

            mutable std::unique_ptr<EnvironmentRenderer>
                environmentRenderer;
            mutable std::unique_ptr<ClusteredLights>
                clusteredLights;
            mutable std::unique_ptr<LitEffect> litEffect;
            mutable std::unique_ptr<LitEffect> skinnedLitEffect;
            mutable std::unique_ptr<LitEffect> errorEffect;
            mutable std::unique_ptr<LitEffect> skinnedErrorEffect;
            mutable std::unique_ptr<SpriteEffect> spriteErrorEffect;
            mutable bool errorEffectUnavailable{};
            mutable bool skinnedErrorEffectUnavailable{};
            mutable bool spriteErrorEffectUnavailable{};
            mutable GraphicsDevice::BuiltInFailure litFailure;
            mutable GraphicsDevice::BuiltInFailure skinnedLitFailure;
            mutable GraphicsDevice::BuiltInFailure environmentFailure;
            mutable GraphicsDevice::BuiltInFailure clustersFailure;
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::MaterialShaderEntry>>
                materialShaders;
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::MaterialShaderEntry>>
                skinnedMaterialShaders;
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::SpriteShaderEntry>>
                spriteShaders;
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::ScreenShaderEntry>>
                screenShaders;
            std::vector<GraphicsDevice::QueuedScreenEffect>
                queuedScreenEffects;
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::ComputeShaderEntry>>
                computeShaders;
            std::unique_ptr<ShadowMap> shadowMap;
            std::unique_ptr<ShadowMap> spotShadowMap;
            std::unique_ptr<ShadowMap> pointShadowMap;

            std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
            std::vector<std::shared_ptr<
                const TextureResourceSnapshot>> spriteTexturePins;
            std::vector<GraphicsViewHandle> spriteViewPins;
            // EnvironmentRendererが所有するSky IBLのraw cacheを、公開
            // LightingState用の世代付きviewとして一度だけ取り込みます。
            GraphicsViewHandle skyPrefilteredSpecular;
            GraphicsViewHandle skyPrefilteredIrradiance;
            float skyPrefilteredMaximumMip{};
            std::unique_ptr<DirectX::CommonStates> commonStates;
            mutable Microsoft::WRL::ComPtr<ID3D11BlendState>
                additiveBlendPreservingAlpha;
            Microsoft::WRL::ComPtr<ID3D11RasterizerState>
                uiScissorRasterizer;
            std::vector<D3D11_RECT> uiScissorStack;
            D3D11SpriteBatchOwner spriteBatchOwner{
                D3D11SpriteBatchOwner::None };
            std::uint64_t spriteBatchToken{};
            std::uint64_t nextSpriteBatchToken{ 1 };
            bool spriteBatchNativeBegun{};
            ID3D11BlendState* spriteBlendState{};
            std::function<void()> spriteShaderCallback;
        };

        // 将来の描画APIごとの状態を同じ所有境界へ追加するための
        // GraphicsDevice側のopaqueコンテナーです。
        class GraphicsDeviceApiResources final
        {
        public:
            GraphicsDeviceApiResources();
            ~GraphicsDeviceApiResources();

            GraphicsDeviceApiResources(
                const GraphicsDeviceApiResources&) = delete;
            GraphicsDeviceApiResources& operator=(
                const GraphicsDeviceApiResources&) = delete;

            void Reset() noexcept;

            std::unique_ptr<GraphicsRenderServices> renderServices;
            std::unique_ptr<GraphicsDeviceD3D11Resources> d3d11;
        };

        [[nodiscard]] std::unique_ptr<GraphicsRenderServices>
            CreateD3D11GraphicsRenderServices(
                ID3D11Device* device,
                ID3D11DeviceContext* context,
                GraphicsBackend& backend);

        [[nodiscard]] std::unique_ptr<GraphicsDeviceApiResources>
            CreateD3D11GraphicsDeviceApiResources(
                ID3D11Device* device,
                ID3D11DeviceContext* context,
                GraphicsBackend& backend);
    }
}
