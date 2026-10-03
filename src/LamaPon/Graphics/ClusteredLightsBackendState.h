#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <memory>

namespace LamaPon
{
    class ClusteredLights;
}

namespace LamaPon::Detail
{
    // 全ビューを同じバックエンド世代で作成してから公開する出力状態。
    struct ClusteredLightsBackendState
    {
        // 派生側を含むバックエンド資源を解放する。
        virtual ~ClusteredLightsBackendState() noexcept = default;

        // ライト情報の読込ビュー
        GraphicsViewHandle m_lightView;
        // クラスタ別ライト索引のビュー
        GraphicsViewHandle m_indexListView;
        // クラスタ別ライト数のビュー
        GraphicsViewHandle m_countView;
        // 資源初期化の完了有無
        bool m_initialized{};
    };

    // バックエンド状態の参照・公開用の内部窓口。
    struct ClusteredLightsBackendAccess final
    {
        // 所有する状態を借用し、未公開ならヌルを返す(clusteredLights: 参照する公開窓口)。
        [[nodiscard]] static ClusteredLightsBackendState*
            Get(ClusteredLights& clusteredLights) noexcept;
        // 所有する状態を読取専用で借用し、未公開ならヌルを返す(clusteredLights: 参照する公開窓口)。
        [[nodiscard]] static const ClusteredLightsBackendState*
            Get(const ClusteredLights& clusteredLights) noexcept;
        // 状態の所有権を公開窓口へ移し、既存状態を解放する(clusteredLights: 公開する窓口, state: 完成したバックエンド状態)。
        static void Publish(
            ClusteredLights& clusteredLights,
            std::unique_ptr<ClusteredLightsBackendState> state) noexcept;
    };
}
