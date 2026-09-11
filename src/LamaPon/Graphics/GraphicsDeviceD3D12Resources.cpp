#include "LamaPon/Graphics/GraphicsDeviceD3D12Resources.h"

#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <memory>
#include <stdexcept>
#include <utility>

namespace LamaPon::Detail
{
    GraphicsDeviceD3D12Resources::GraphicsDeviceD3D12Resources() = default;

    GraphicsDeviceD3D12Resources::~GraphicsDeviceD3D12Resources() noexcept
    {
        Reset();
    }

    void GraphicsDeviceD3D12Resources::QuiesceResourceWork() noexcept
    {
        // D3D12 Experimentalにはまだshader workerやrender serviceがなく、
        // Backendを借用する非同期作業も開始しません。
    }

    void GraphicsDeviceD3D12Resources::ResetHighLevelResources() noexcept
    {
        QuiesceResourceWork();
        m_directionalShadowMap.reset();
        m_spotShadowMap.reset();
        m_pointShadowMap.reset();
    }

    void GraphicsDeviceD3D12Resources::Reset() noexcept
    {
        ResetHighLevelResources();
    }

    GraphicsRenderServices*
        GraphicsDeviceD3D12Resources::TryRenderServices() noexcept
    {
        // ParticleなどのD3D11-only rendererは呼び出し側がfalseへ倒します。
        return nullptr;
    }

    void GraphicsDeviceD3D12Resources::RecreateShadowMaps(
        GraphicsBackend& backend,
        const GraphicsSettings&)
    {
        if (backend.Api() != RenderingApi::DirectX12Experimental
            || !backend.IsInitialized())
        {
            throw std::logic_error(
                "DirectX 12 API resources require an initialized DirectX "
                "12 backend.");
        }

        // ShadowMapを空のまま公開します。D3D12 shadow resourcesと
        // shadow passが完成するまで、SceneはIsValid()==falseとして
        // 影描画を安全にスキップします。
        auto directionalShadowMap = std::make_unique<ShadowMap>();
        auto spotShadowMap = std::make_unique<ShadowMap>();
        auto pointShadowMap = std::make_unique<ShadowMap>();

        // allocationがすべて成功するまで現在のfacadeを変更しません。
        m_directionalShadowMap = std::move(directionalShadowMap);
        m_spotShadowMap = std::move(spotShadowMap);
        m_pointShadowMap = std::move(pointShadowMap);
    }

    ShadowMap* GraphicsDeviceD3D12Resources::
        TryDirectionalShadowMap() const noexcept
    {
        return m_directionalShadowMap.get();
    }

    ShadowMap* GraphicsDeviceD3D12Resources::
        TrySpotShadowMap() const noexcept
    {
        return m_spotShadowMap.get();
    }

    ShadowMap* GraphicsDeviceD3D12Resources::
        TryPointShadowMap() const noexcept
    {
        return m_pointShadowMap.get();
    }

    std::unique_ptr<GraphicsDeviceApiResources>
        CreateD3D12GraphicsDeviceApiResources(
            GraphicsBackend& backend)
    {
        if (backend.Api() != RenderingApi::DirectX12Experimental
            || !backend.IsInitialized())
        {
            throw std::invalid_argument(
                "DirectX 12 API resources require an initialized DirectX "
                "12 backend.");
        }
        return std::make_unique<GraphicsDeviceD3D12Resources>();
    }
}
