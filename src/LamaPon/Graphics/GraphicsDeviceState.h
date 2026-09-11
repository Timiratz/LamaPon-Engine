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
        // Shader workerを含むAPI固有資源より長く生存させます。Stateの
        // 自動破棄でもAssetManagerがworkerより先に消えない宣言順です。
        std::unique_ptr<RuntimeServices> m_services;
        // SpriteBatchや固定機能state等のAPI固有資源です。
        std::unique_ptr<Detail::GraphicsDeviceApiResources>
            m_apiResources;
        GraphicsTextureHandle m_whiteTexture;
        GraphicsViewHandle m_whiteTextureView;
        GraphicsBufferHandle m_instanceBuffer;
        DepthPassKind m_depthPass{ DepthPassKind::None };
        GpuProfiler m_gpuProfiler;

        std::unique_ptr<DebugRenderer> m_debugRenderer;
        mutable std::unique_ptr<ClusteredLights> m_clusteredLights;
        mutable BuiltInFailure m_clustersFailure;
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
        mutable std::unordered_map<
            std::filesystem::path,
            ShaderVariantDeclaration>
            m_shaderVariants;
        bool m_asyncShaderCompilation{ true };
        mutable std::uint64_t m_materialShaderGeneration{};
        mutable std::uint64_t m_spriteShaderGeneration{};
        mutable std::uint64_t m_screenShaderGeneration{};
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
