#pragma once

#include "LamaPon/Scene/Component.h"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace LamaPon
{
    struct LODLevel final
    {
        // 対象を選ぶワールド距離上限
        float maximumDistance{ 25.0f };
        // 描画対象IDで0は対象なし
        std::uint64_t targetId{};
    };

    // 距離には有限値を指定し、編集後は距離順の並べ替えで段階番号が変わります。
    class LODGroupComponent final : public Component
    {
    public:
        // 距離に応じる描画対象を設定します(levels: 距離上限と対象IDの列, cullDistance: 非表示にする距離)。
        explicit LODGroupComponent(
            std::vector<LODLevel> levels = {},
            float cullDistance = 200.0f);

        // 距離順の段階設定を返します。
        [[nodiscard]] const std::vector<LODLevel>&
            Levels() const noexcept
        {
            return m_levels;
        }
        // 先頭8件を残して非負距離の昇順に設定します(levels: 段階設定の列)。
        void SetLevels(std::vector<LODLevel> levels);
        // 8件未満なら追加して距離順に整えます(level: 追加する段階設定)。
        void AddLevel(LODLevel level);
        // 範囲内の段階だけ削除します(index: 現在の段階番号)。
        void RemoveLevel(std::size_t index);
        // 範囲内の段階を置換して距離順に整えます(index: 現在の段階番号, level: 新しい段階設定)。
        void SetLevel(std::size_t index, LODLevel level);

        // 対象を非表示にする距離を返します。
        [[nodiscard]] float CullDistance() const noexcept
        {
            return m_cullDistance;
        }
        // 非表示距離を非負かつ最遠段階以上に設定します(value: ワールド距離)。
        void SetCullDistance(float value) noexcept;

        // 距離上限に合う対象を選び空設定・非表示距離超過なら0を返します(distance: ワールド距離)。
        // 最遠段階から非表示距離までは最終対象を使い、非表示距離ちょうどでは表示します。
        [[nodiscard]] std::uint64_t
            SelectTarget(float distance) const noexcept;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "LODGroup";
        }

    private:
        // 先頭8件を残し非負の距離順に並べて非表示距離を調整します。
        void Normalize();

        // 距離順の描画対象の段階列
        std::vector<LODLevel> m_levels;
        // 非表示にするワールド距離
        float m_cullDistance{ 200.0f };
    };
}
