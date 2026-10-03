#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <cstdint>
#include <memory>

namespace LamaPon
{
    class ShadowMap;
}

namespace LamaPon::Detail
{
    // 全資源と読込ビューを完成させてから公開する出力状態。
    struct ShadowMapBackendState
    {
        // 派生側を含むバックエンド資源を解放する。
        virtual ~ShadowMapBackendState() noexcept = default;

        // 影マップの読込ビュー
        GraphicsViewHandle m_view;
        // 影マップ一辺の解像度
        std::uint32_t m_resolution{};
        // カスケードの個数
        std::uint32_t m_cascadeCount{};
        // 資源初期化の完了有無
        bool m_initialized{};
        // 影マップ描画の実行中有無
        bool m_rendering{};
    };

    // バックエンド状態の参照・公開用の内部窓口。
    struct ShadowMapBackendAccess final
    {
        // 所有する状態を借用し、未公開ならヌルを返す(shadowMap: 参照する影マップ)。
        [[nodiscard]] static ShadowMapBackendState*
            Get(ShadowMap& shadowMap) noexcept;
        // 所有する状態を読取専用で借用し、未公開ならヌルを返す(shadowMap: 参照する影マップ)。
        [[nodiscard]] static const ShadowMapBackendState*
            Get(const ShadowMap& shadowMap) noexcept;
        // 状態の所有権を影マップへ移し、既存状態を解放する(shadowMap: 公開先の影マップ, state: 完成したバックエンド状態)。
        static void Publish(
            ShadowMap& shadowMap,
            std::unique_ptr<ShadowMapBackendState> state) noexcept;
    };
}
