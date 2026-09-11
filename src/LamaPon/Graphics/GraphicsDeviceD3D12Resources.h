#pragma once

// DirectX 12 ExperimentalのGraphicsDevice資源です。現段階ではSprite描画の
// 既定pipelineと、影を持たない安全なShadowMap facadeを所有します。
// D3D11 native objectは持たず、SDKにもインストールしません。
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"

#include <memory>

namespace LamaPon
{
    class D3D12Backend;
}

namespace LamaPon::Detail
{
    class D3D12SpriteRenderer;

    class GraphicsDeviceD3D12Resources final
        : public GraphicsDeviceApiResources
    {
    public:
        explicit GraphicsDeviceD3D12Resources(D3D12Backend& backend);
        ~GraphicsDeviceD3D12Resources() noexcept override;

        GraphicsDeviceD3D12Resources(
            const GraphicsDeviceD3D12Resources&) = delete;
        GraphicsDeviceD3D12Resources& operator=(
            const GraphicsDeviceD3D12Resources&) = delete;

        [[nodiscard]] RenderingApi Api() const noexcept override
        {
            return RenderingApi::DirectX12Experimental;
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

        // SpriteRenderPassのD3D12 driverです。資源解放後はnullptrです。
        [[nodiscard]] D3D12SpriteRenderer* TrySpriteRenderer() noexcept;

    private:
        std::unique_ptr<D3D12SpriteRenderer> m_spriteRenderer;
        // Sceneの影判定はShadowMap accessorを常に取得できることを前提に
        // するため、Experimental段階でも空のfacadeを保持します。
        std::unique_ptr<ShadowMap> m_directionalShadowMap;
        std::unique_ptr<ShadowMap> m_spotShadowMap;
        std::unique_ptr<ShadowMap> m_pointShadowMap;
    };

    [[nodiscard]] std::unique_ptr<GraphicsDeviceApiResources>
        CreateD3D12GraphicsDeviceApiResources(
            GraphicsBackend& backend);
}
