#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    // ローカルXY移動量に倍率を掛けて追従するため、参照と自身の座標系を合わせます。
    class ParallaxLayerComponent final : public Component
    {
    public:
        // 参照の移動に追従する層を作ります(factor: ローカルXY移動の倍率, referenceId: 参照物体IDで0はMain Camera)。
        explicit ParallaxLayerComponent(
            DirectX::XMFLOAT2 factor = { 0.5f, 0.5f },
            std::uint64_t referenceId = 0) noexcept;

        // 追従倍率を変更します(factor: ローカルXY移動の倍率)。
        void SetFactor(const DirectX::XMFLOAT2& factor) noexcept
        {
            m_factor = factor;
        }
        // ローカルXY移動の追従倍率を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& Factor() const noexcept
        {
            return m_factor;
        }

        // 参照を変更し次回更新で原点を取り直します(referenceId: 参照物体IDで0はMain Camera)。
        void SetReferenceId(const std::uint64_t referenceId) noexcept
        {
            m_referenceId = referenceId;
            m_initialized = false;
        }
        // 参照物体IDを返し0ならMain Cameraを示します。
        [[nodiscard]] std::uint64_t ReferenceId() const noexcept
        {
            return m_referenceId;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "ParallaxLayer";
        }

    protected:
        // 参照の原点からの移動量に追従します(deltaTime: 使用しない経過秒数)。
        // 最初の有効な参照で原点を記録し、参照がない間は位置と原点を保持します。
        void OnUpdate(float deltaTime) override;

    private:
        // ローカルXY移動の倍率
        DirectX::XMFLOAT2 m_factor;
        // 参照物体IDで0は主カメラ
        std::uint64_t m_referenceId{};
        // 追従原点を取得済みの指定
        bool m_initialized{};
        // 参照の初期ローカルXY位置
        DirectX::XMFLOAT2 m_referenceOrigin{};
        // 自身の初期ローカルXY位置
        DirectX::XMFLOAT2 m_ownOrigin{};
    };
}
