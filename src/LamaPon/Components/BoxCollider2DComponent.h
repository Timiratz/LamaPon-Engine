#pragma once

#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>
#include <cstdint>

namespace LamaPon
{
    // 形状の数値設定には有限値を指定します。
    class BoxCollider2DComponent final : public Component
    {
    public:
        // 衝突形状とフィルターを設定します(size: ローカルXY全幅, offset: ローカル中心位置, isTrigger: 通知だけにする指定, layer: 32で剰余を取る所属層, collisionMask: 相手の許可層ビット, material: 物理材質)。
        explicit BoxCollider2DComponent(
            DirectX::XMFLOAT2 size = { 1.0f, 1.0f },
            DirectX::XMFLOAT2 offset = { 0.0f, 0.0f },
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        // ローカルの箱の全幅を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& Size() const noexcept { return m_size; }
        // ローカル全幅を設定し境界計算では絶対値を使います(size: 箱の全幅)。
        void SetSize(const DirectX::XMFLOAT2& size) noexcept { m_size = size; }

        // ローカル中心位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& Offset() const noexcept { return m_offset; }
        // ローカル中心位置を設定します(offset: 中心位置)。
        void SetOffset(const DirectX::XMFLOAT2& offset) noexcept { m_offset = offset; }

        // 衝突応答を行わず通知だけにする指定を返します。
        [[nodiscard]] bool IsTrigger() const noexcept { return m_isTrigger; }
        // 通知だけにする指定を設定します(trigger: 衝突応答を省く指定)。
        void SetTrigger(const bool trigger) noexcept { m_isTrigger = trigger; }
        // 所属層の番号を返します。
        [[nodiscard]] std::uint32_t Layer() const noexcept { return m_layer; }
        // 所属層を32の剰余で設定します(layer: 層番号)。
        void SetLayer(const std::uint32_t layer) noexcept { m_layer = layer % 32u; }
        // 衝突を許可する相手層のビット集合を返します。
        [[nodiscard]] std::uint32_t CollisionMask() const noexcept { return m_collisionMask; }
        // 相手層の許可ビットを設定します(mask: 層ビット集合)。
        void SetCollisionMask(const std::uint32_t mask) noexcept { m_collisionMask = mask; }
        // 制限済みの物理材質を返します。
        [[nodiscard]] const PhysicsMaterial& Material() const noexcept
        {
            return m_material;
        }
        // 物理材質を制限して設定します(value: 物理材質)。
        void SetMaterial(PhysicsMaterial value) noexcept
        {
            value.Clamp();
            m_material = value;
        }
        // 双方のマスクとプロジェクト層行列が許可するか返します(other: 相手の箱コライダー)。
        [[nodiscard]] bool CanCollideWith(
            const BoxCollider2DComponent& other) const noexcept;

        // 形状を囲むワールドXYの軸平行境界を返します。
        [[nodiscard]] Bounds2D WorldBounds() const noexcept;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "BoxCollider2D";
        }

    protected:
        // 衝突形状の輪郭を描画します(graphics: 描画装置, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // ローカルの箱の全幅
        DirectX::XMFLOAT2 m_size;
        // ローカル中心位置
        DirectX::XMFLOAT2 m_offset;
        // 衝突通知だけにする指定
        bool m_isTrigger{};
        // 所属層の番号
        std::uint32_t m_layer{};
        // 相手層の許可ビット集合
        std::uint32_t m_collisionMask{ 0xffffffffu };
        // 制限済みの物理材質
        PhysicsMaterial m_material;
    };
}
