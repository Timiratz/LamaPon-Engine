#include "LamaPon/Graphics/GraphicsDeviceD3D12Resources.h"

#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/D3D12RenderServices.h"
#include "LamaPon/Graphics/D3D12SpriteRenderer.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <algorithm>
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
        const GraphicsSettings& settings)
    {
        if (backend.Api() != RenderingApi::DirectX12Experimental
            || !backend.IsInitialized())
        {
            throw std::logic_error(
                "DirectX 12 API resources require an initialized DirectX "
                "12 backend.");
        }

        auto directionalShadowMap = std::make_unique<ShadowMap>();
        auto spotShadowMap = std::make_unique<ShadowMap>();
        auto pointShadowMap = std::make_unique<ShadowMap>();
        if (settings.shadowsEnabled)
        {
            backend.InitializeShadowMap(
                *directionalShadowMap,
                settings.shadowResolution,
                settings.shadowCascadeLimit,
                false);
            const std::uint32_t localShadowResolution = std::max(
                settings.shadowResolution / 2u,
                256u);
            backend.InitializeShadowMap(
                *spotShadowMap,
                localShadowResolution,
                static_cast<std::uint32_t>(MaximumSpotShadows),
                false);
            backend.InitializeShadowMap(
                *pointShadowMap,
                localShadowResolution,
                6u,
                true);
        }

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
