#pragma once

// GraphicsDeviceの公開layoutを描画APIや実装cacheから切り離すRuntime内部
// headerです。Game Module SDKにはインストールしません。
#include "LamaPon/Graphics/GraphicsDevice.h"

namespace LamaPon
{
    struct GraphicsDevice::State final
    {
        explicit State(GraphicsDevice* owner);
        ~State();

        State(const State&) = delete;
        State& operator=(const State&) = delete;

        // lease stateは最後に破棄します。GraphicsDeviceの明示的な
        // ReleaseResourcesも、GPU資源からBackendの順序を維持します。
        std::shared_ptr<
            Detail::GraphicsDeviceResourceLeaseState>
            m_resourceLeaseState;

        // Device / Context / SwapChainとバックバッファ資源の所有者です。
        std::unique_ptr<GraphicsBackend> m_backend;
        // SpriteBatchや固定機能state等のAPI固有資源です。
        std::unique_ptr<Detail::GraphicsDeviceApiResources>
            m_apiResources;
        GraphicsTextureHandle m_whiteTexture;
        GraphicsViewHandle m_whiteTextureView;
        GraphicsBufferHandle m_instanceBuffer;
        DepthPassKind m_depthPass{ DepthPassKind::None };
        GpuProfiler m_gpuProfiler;

        // 既存のAssets/Audio/Input APIを保つ高レベル所有者です。
        std::unique_ptr<RuntimeServices> m_services;
        std::unique_ptr<DebugRenderer> m_debugRenderer;
        mutable std::unique_ptr<EnvironmentRenderer>
            m_environmentRenderer;
        mutable std::unique_ptr<ClusteredLights>
            m_clusteredLights;
        std::unique_ptr<RenderTarget> m_sceneCompositionTarget;
        DirectX::XMFLOAT4X4 m_sceneProjection{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        std::unordered_map<
            std::string,
            std::unique_ptr<RenderTarget>>
            m_renderTextures;
        mutable std::unique_ptr<LitEffect> m_litEffect;
        mutable std::unique_ptr<LitEffect> m_skinnedLitEffect;
        mutable std::unique_ptr<LitEffect> m_errorEffect;
        mutable std::unique_ptr<LitEffect> m_skinnedErrorEffect;
        mutable std::unique_ptr<SpriteEffect> m_spriteErrorEffect;
        mutable bool m_errorEffectUnavailable{};
        mutable bool m_skinnedErrorEffectUnavailable{};
        mutable bool m_spriteErrorEffectUnavailable{};
        mutable BuiltInFailure m_litFailure;
        mutable BuiltInFailure m_skinnedLitFailure;
        mutable BuiltInFailure m_environmentFailure;
        mutable BuiltInFailure m_clustersFailure;
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<MaterialShaderEntry>>
            m_materialShaders;
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<MaterialShaderEntry>>
            m_skinnedMaterialShaders;
        mutable std::unordered_map<
            std::filesystem::path,
            ShaderVariantDeclaration>
            m_shaderVariants;
        bool m_asyncShaderCompilation{ true };
        mutable std::uint64_t m_materialShaderGeneration{};
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<SpriteShaderEntry>>
            m_spriteShaders;
        mutable std::uint64_t m_spriteShaderGeneration{};
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<ScreenShaderEntry>>
            m_screenShaders;
        mutable std::uint64_t m_screenShaderGeneration{};
        std::vector<QueuedScreenEffect> m_queuedScreenEffects;
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<ComputeShaderEntry>>
            m_computeShaders;
        std::unique_ptr<ShadowMap> m_shadowMap;
        std::unique_ptr<ShadowMap> m_spotShadowMap;
        std::unique_ptr<ShadowMap> m_pointShadowMap;
        LightingState m_lightingState;
        GraphicsSettings m_graphicsSettings =
            GraphicsSettingsForPreset(GraphicsQualityPreset::High);
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        std::uint32_t m_uiWidth{};
        std::uint32_t m_uiHeight{};
        DirectX::XMFLOAT2 m_sprite2DOffset{};
        mutable FrameStatistics m_frameStatistics;
        GraphicsMemoryStatistics m_memoryStatistics;
        std::chrono::steady_clock::time_point
            m_lastMemoryStatisticsSample{};
        RenderingApi m_startupRenderingApi{
            RenderingApi::DirectX11 };
        RenderingApiFallbackReason m_renderingApiFallbackReason{
            RenderingApiFallbackReason::None };
    };
}
