#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/D3D11ShadowMapState.h"
#include "LamaPon/Graphics/ShadowMapBackendState.h"

#include <utility>

namespace
{
    [[nodiscard]] const LamaPon::Detail::D3D11ShadowMapState*
        AsD3D11State(
            const LamaPon::Detail::ShadowMapBackendState* const state)
        noexcept
    {
        return dynamic_cast<
            const LamaPon::Detail::D3D11ShadowMapState*>(state);
    }
}

namespace LamaPon
{
    Detail::ShadowMapBackendState*
        Detail::ShadowMapBackendAccess::Get(
            ShadowMap& shadowMap) noexcept
    {
        return shadowMap.m_backendState.get();
    }

    const Detail::ShadowMapBackendState*
        Detail::ShadowMapBackendAccess::Get(
            const ShadowMap& shadowMap) noexcept
    {
        return shadowMap.m_backendState.get();
    }

    void Detail::ShadowMapBackendAccess::Publish(
        ShadowMap& shadowMap,
        std::unique_ptr<ShadowMapBackendState> state) noexcept
    {
        shadowMap.m_backendState = std::move(state);
    }

    ShadowMap::ShadowMap() noexcept = default;

    ShadowMap::~ShadowMap() noexcept = default;

    GraphicsViewHandle ShadowMap::ViewHandle() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_view
            : GraphicsViewHandle{};
    }

    std::uint32_t ShadowMap::Resolution() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_resolution
            : 0u;
    }

    std::uint32_t ShadowMap::CascadeCount() const noexcept
    {
        return m_backendState != nullptr
            ? m_backendState->m_cascadeCount
            : 0u;
    }

    bool ShadowMap::IsValid() const noexcept
    {
        return m_backendState != nullptr
            && m_backendState->m_initialized
            && m_backendState->m_view
            && m_backendState->m_resolution > 0
            && m_backendState->m_cascadeCount > 0;
    }

    void* ShadowMap::LegacyNativeView() const noexcept
    {
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->ShaderResourceView()
            : nullptr;
    }
}
