#pragma once

#include "LamaPon/Scene/Component.h"

namespace LamaPon
{
    // 描画カリングをGameObjectごとに調整します。広い地形や背景など、
    // カメラ外でも描画するものや余白が必要なものにだけ追加します。
    // 追加していないGameObjectには通常のカリングを適用します。
    class RenderCullingComponent final : public Component
    {
    public:
        explicit RenderCullingComponent(
            const bool alwaysVisible = false,
            const float cullingMargin = 0.0f) noexcept
            : m_alwaysVisible(alwaysVisible)
        {
            SetCullingMargin(cullingMargin);
        }

        // 視錐台・遮蔽カリングの対象から外し、常に描画します。
        void SetAlwaysVisible(const bool enabled) noexcept
        {
            m_alwaysVisible = enabled;
        }
        [[nodiscard]] bool AlwaysVisible() const noexcept
        {
            return m_alwaysVisible;
        }

        // 視錐台カリングの境界をワールド単位で外側へ広げます。
        // 頂点シェーダーで揺らす草木など、バウンディングより
        // 大きく見えるものの消え際を防ぎます。
        void SetCullingMargin(const float margin) noexcept
        {
            m_cullingMargin = margin > 0.0f
                ? margin
                : 0.0f;
        }
        [[nodiscard]] float CullingMargin() const noexcept
        {
            return m_cullingMargin;
        }

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "RenderCulling";
        }

    private:
        bool m_alwaysVisible{};
        float m_cullingMargin{};
    };
}
