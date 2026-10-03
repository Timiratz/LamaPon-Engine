#pragma once

#include "LamaPon/Physics/Collision3D.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>
#include <cstdint>

namespace LamaPon
{
    // 形状の数値設定には有限値を指定します。
    class CapsuleCollider3DComponent final : public Component
    {
    public:
        // 衝突形状とフィルターを設定します(radius: ローカル半径, height: 半球を含む全高, offset: ローカル中心位置, isTrigger: 通知だけにする指定, layer: 32で剰余を取る所属層, collisionMask: 相手の許可層ビット, material: 物理材質)。
        explicit CapsuleCollider3DComponent(
            float radius = 0.5f,
            float height = 2.0f,
            DirectX::XMFLOAT3 offset = {},
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        // ローカル半径を返します。
        [[nodiscard]] float Radius() const noexcept { return m_radius; }
        // 半径を0.01以上に制限し全高も直径以上に広げます(value: 有限のローカル半径)。
        void SetRadius(float value) noexcept;
        // 半球を含むローカル全高を返します。
        [[nodiscard]] float Height() const noexcept { return m_height; }
        // 全高を直径以上に制限して設定します(value: 有限のローカル全高)。
        void SetHeight(float value) noexcept;
        // ローカル中心位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Offset() const noexcept
        {
            return m_offset;
        }
        // ローカル中心位置を設定します(value: 中心位置)。
        void SetOffset(const DirectX::XMFLOAT3& value) noexcept
        {
            m_offset = value;
        }
        // 衝突応答を行わず通知だけにする指定を返します。
        [[nodiscard]] bool IsTrigger() const noexcept { return m_isTrigger; }
        // 通知だけにする指定を設定します(value: 衝突応答を省く指定)。
        void SetTrigger(bool value) noexcept { m_isTrigger = value; }
        // 所属層の番号を返します。
        [[nodiscard]] std::uint32_t Layer() const noexcept { return m_layer; }
        // 所属層を32の剰余で設定します(value: 層番号)。
        void SetLayer(std::uint32_t value) noexcept { m_layer = value % 32u; }
        // 衝突を許可する相手層のビット集合を返します。
        [[nodiscard]] std::uint32_t CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        // 相手層の許可ビットを設定します(value: 層ビット集合)。
        void SetCollisionMask(std::uint32_t value) noexcept
        {
            m_collisionMask = value;
        }
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
        // 中心線をY倍率で変換し半径をXZ最大倍率で近似したカプセルを返します。
        [[nodiscard]] Capsule3D WorldCapsule() const noexcept;
        // ワールド形状を囲むXYZ軸平行境界を返します。
        [[nodiscard]] Bounds3D WorldBounds() const noexcept;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "CapsuleCollider3D";
        }

    protected:
        // 衝突形状の輪郭を描画します(graphics: 描画装置, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // ローカル半径
        float m_radius{ 0.5f };
        // 半球を含むローカル全高
        float m_height{ 2.0f };
        // ローカル中心位置
        DirectX::XMFLOAT3 m_offset{};
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
