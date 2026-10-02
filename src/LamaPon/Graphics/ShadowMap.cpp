#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/D3D11ShadowMapState.h"
#include "LamaPon/Graphics/ShadowMapBackendState.h"

#include <utility>

namespace
{
    // 状態をD3D11実装として借用し、型が違えばヌルを返す(state: 共通バックエンド状態)。
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
    // 所有する状態を借用し、未公開ならヌルを返す(shadowMap: 参照する影マップ)。
    Detail::ShadowMapBackendState*
        Detail::ShadowMapBackendAccess::Get(
            ShadowMap& shadowMap) noexcept
    {
        return shadowMap.m_backendState.get();
    }

    // 所有する状態を読取専用で借用し、未公開ならヌルを返す(shadowMap: 参照する影マップ)。
    const Detail::ShadowMapBackendState*
        Detail::ShadowMapBackendAccess::Get(
            const ShadowMap& shadowMap) noexcept
    {
        return shadowMap.m_backendState.get();
    }

    // 状態の所有権を影マップへ移し、既存状態を解放する(shadowMap: 公開先の影マップ, state: 完成したバックエンド状態)。
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
        // 旧ABIで参照するD3D11状態
        const auto* const state = AsD3D11State(m_backendState.get());
        return state != nullptr
            ? state->ShaderResourceView()
            : nullptr;
    }
}
