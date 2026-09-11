#include "LamaPon/Graphics/GraphicsDeviceD3D12Resources.h"

#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/D3D12RenderServices.h"
#include "LamaPon/Graphics/D3D12SpriteRenderer.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <memory>
#include <stdexcept>
#include <utility>

namespace LamaPon::Detail
{
    GraphicsDeviceD3D12Resources::GraphicsDeviceD3D12Resources(
        D3D12Backend& backend)
        : m_spriteRenderer(
            std::make_unique<D3D12SpriteRenderer>(backend))
        , m_renderServices(CreateD3D12GraphicsRenderServices(backend))
    {
    }

    GraphicsDeviceD3D12Resources::~GraphicsDeviceD3D12Resources() noexcept
    {
        Reset();
    }

    void GraphicsDeviceD3D12Resources::QuiesceResourceWork() noexcept
    {
        // D3D12 render serviceは同期描画だけを行い、Backendを借用する
        // 非同期作業は開始しません。
    }

    void GraphicsDeviceD3D12Resources::ResetHighLevelResources() noexcept
    {
        QuiesceResourceWork();
        m_spriteRenderer.reset();
        m_renderServices.reset();
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
        return m_renderServices.get();
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

    D3D12SpriteRenderer*
        GraphicsDeviceD3D12Resources::TrySpriteRenderer() noexcept
    {
        return m_spriteRenderer.get();
    }

    std::unique_ptr<GraphicsDeviceApiResources>
        CreateD3D12GraphicsDeviceApiResources(
            GraphicsBackend& backend)
    {
        auto* const d3d12Backend = dynamic_cast<D3D12Backend*>(&backend);
        if (d3d12Backend == nullptr || !d3d12Backend->IsInitialized())
        {
            throw std::invalid_argument(
                "DirectX 12 API resources require an initialized DirectX "
                "12 backend.");
        }
        return std::make_unique<GraphicsDeviceD3D12Resources>(
            *d3d12Backend);
    }
}
