#pragma once

#include "LamaPon/Scene/Component.h"

namespace LamaPon
{
    // 有効な部品の設定で物体ごとの視錐台・遮蔽カリングを調整します。
    class RenderCullingComponent final : public Component
    {
    public:
        // 物体のカリング設定を作ります(alwaysVisible: 視錐台・遮蔽判定の除外指定, cullingMargin: 境界の拡張幅)。
        explicit RenderCullingComponent(
            const bool alwaysVisible = false,
            const float cullingMargin = 0.0f) noexcept
            : m_alwaysVisible(alwaysVisible)
        {
            SetCullingMargin(cullingMargin);
        }

        // 視錐台・遮蔽判定の除外を設定します(enabled: 除外する指定)。
        // LOD判定はこの指定でも適用されます。
        void SetAlwaysVisible(const bool enabled) noexcept
        {
            m_alwaysVisible = enabled;
        }
        // 視錐台・遮蔽判定からの除外指定を返します。
        [[nodiscard]] bool AlwaysVisible() const noexcept
        {
            return m_alwaysVisible;
        }

        // 境界を広げる幅を設定します(margin: ワールド単位の幅で0以下とNaNは0)。
        void SetCullingMargin(const float margin) noexcept
        {
            m_cullingMargin = margin > 0.0f
                ? margin
                : 0.0f;
        }
        // 境界を広げる幅をワールド単位で返します。
        [[nodiscard]] float CullingMargin() const noexcept
        {
            return m_cullingMargin;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "RenderCulling";
        }

    private:
        // 視錐台・遮蔽判定の除外指定
        bool m_alwaysVisible{};
        // ワールド境界の拡張幅
        float m_cullingMargin{};
    };
}
