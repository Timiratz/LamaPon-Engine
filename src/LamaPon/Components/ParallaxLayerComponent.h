#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    // 背景／地形／前景のTilemap・SpriteをMain Camera（既定）や任意の
    // GameObjectの動きに対して指定倍率で追従させ、奥行きのある
    // スクロールを作ります。Factor 1.0で参照と同じ速さ、0.5で半分の
    // 速さ（遠い背景）、0で画面に固定、1.0より大きいと参照より速く
    // 動きます（近景）。参照は初回OnUpdate時点の位置を原点として
    // 記録するため、参照未設定またはシーンに存在しない間は動きません。
    class ParallaxLayerComponent final : public Component
    {
    public:
        explicit ParallaxLayerComponent(
            DirectX::XMFLOAT2 factor = { 0.5f, 0.5f },
            std::uint64_t referenceId = 0) noexcept;

        void SetFactor(const DirectX::XMFLOAT2& factor) noexcept
        {
            m_factor = factor;
        }
        [[nodiscard]] const DirectX::XMFLOAT2& Factor() const noexcept
        {
            return m_factor;
        }

        // 0は「Main Cameraを参照する」既定動作です。
        void SetReferenceId(const std::uint64_t referenceId) noexcept
        {
            m_referenceId = referenceId;
            m_initialized = false;
        }
        [[nodiscard]] std::uint64_t ReferenceId() const noexcept
        {
            return m_referenceId;
        }

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "ParallaxLayer";
        }

    protected:
        void OnUpdate(float deltaTime) override;

    private:
        DirectX::XMFLOAT2 m_factor;
        std::uint64_t m_referenceId{};
        bool m_initialized{};
        DirectX::XMFLOAT2 m_referenceOrigin{};
        DirectX::XMFLOAT2 m_ownOrigin{};
    };
}
