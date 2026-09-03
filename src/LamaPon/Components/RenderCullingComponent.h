#pragma once

#include "LamaPon/Scene/Component.h"

namespace LamaPon
{
    // 描画カリングの個別調整。付けたGameObjectだけが「カメラ外でも
    // 描画」「カリング余白」を持ちます。
    //
    // 以前は全GameObjectが常時この設定を持っていて、カメラやライト
    // などカリングと無関係なもののインスペクターにも表示されて
    // いました。コンポーネントにすることで、必要なオブジェクト
    // （広い地形・背景・影だけ画面外から落とす物）にだけ付ければ
    // よくなっています。付いていないオブジェクトは通常どおり
    // カリングされます。
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
