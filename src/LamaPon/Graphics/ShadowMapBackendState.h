#pragma once

// ShadowMapの公開レイアウトからBackend stateを隠すための
// Runtime内部ヘッダーです。SDKにはインストールしません。
#include "LamaPon/Graphics/GraphicsResource.h"

#include <cstdint>
#include <memory>

namespace LamaPon
{
    class ShadowMap;
}

namespace LamaPon::Detail
{
    // 描画APIに依存しない公開状態です。具象stateはnative資源を全て
    // 作成してneutral viewを登録した後にだけShadowMapへ公開されます。
    struct ShadowMapBackendState
    {
        virtual ~ShadowMapBackendState() noexcept = default;

        GraphicsViewHandle m_view;
        std::uint32_t m_resolution{};
        std::uint32_t m_cascadeCount{};
        bool m_initialized{};
        bool m_rendering{};
    };

    // Backend実装だけがopaque stateを参照・公開するための内部bridgeです。
    // 公開ShadowMapは具象Backendをfriendにせず、API-neutralなまま保ちます。
    struct ShadowMapBackendAccess final
    {
        [[nodiscard]] static ShadowMapBackendState*
            Get(ShadowMap& shadowMap) noexcept;
        [[nodiscard]] static const ShadowMapBackendState*
            Get(const ShadowMap& shadowMap) noexcept;
        static void Publish(
            ShadowMap& shadowMap,
            std::unique_ptr<ShadowMapBackendState> state) noexcept;
    };
}
