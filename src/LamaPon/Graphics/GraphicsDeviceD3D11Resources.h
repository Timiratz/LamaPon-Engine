#pragma once

// DirectX 11固有のGraphicsDevice資源です。Runtime内部だけで使用し、
// Game Module SDKにはインストールしません。
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
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

        // Backend、Effect、Shader cache、Sprite stateを一つのD3D11世代
        // として所有します。GraphicsDevice本体からはbaseだけが見えます。
        struct GraphicsDeviceD3D11Resources final
            : GraphicsDeviceApiResources
        {
            GraphicsDeviceD3D11Resources(
                ID3D11Device* device,
                ID3D11DeviceContext* context);
            ~GraphicsDeviceD3D11Resources() override;

            GraphicsDeviceD3D11Resources(
                const GraphicsDeviceD3D11Resources&) = delete;
            GraphicsDeviceD3D11Resources& operator=(
                const GraphicsDeviceD3D11Resources&) = delete;

            [[nodiscard]] RenderingApi Api() const noexcept override
            {
                return RenderingApi::DirectX11;
            }
            void QuiesceResourceWork() noexcept override;
            void ResetHighLevelResources() noexcept override;
            void Reset() noexcept override;
            [[nodiscard]] GraphicsRenderServices*
                TryRenderServices() noexcept override;
            void RecreateShadowMaps(
                GraphicsBackend& backend,
                const GraphicsSettings& settings) override;
            [[nodiscard]] ShadowMap*
                TryDirectionalShadowMap() const noexcept override;
            [[nodiscard]] ShadowMap*
                TrySpotShadowMap() const noexcept override;
            [[nodiscard]] ShadowMap*
                TryPointShadowMap() const noexcept override;

            mutable std::unique_ptr<EnvironmentRenderer>
                environmentRenderer;
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

            std::unique_ptr<GraphicsRenderServices> renderServices;
            std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
            std::vector<std::shared_ptr<
                const TextureResourceSnapshot>> spriteTexturePins;
            std::vector<GraphicsViewHandle> spriteViewPins;
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
