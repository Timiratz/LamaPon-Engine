#pragma once

// DirectX 12 ExperimentalのGraphicsDevice資源です。現段階では
// 高水準rendererを実装せず、D3D11 native objectを持たない起動用の
// 安全なplaceholderだけを所有します。SDKにはインストールしません。
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"

#include <memory>

namespace LamaPon::Detail
{
    class GraphicsDeviceD3D12Resources final
        : public GraphicsDeviceApiResources
    {
    public:
        GraphicsDeviceD3D12Resources();
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

    private:
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
