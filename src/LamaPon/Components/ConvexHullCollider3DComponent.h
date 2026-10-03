#pragma once

#include "LamaPon/Physics/Collision3D.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace LamaPon
{
    // 形状の数値設定には有限値を指定します。
    // noexceptの点群操作・形状生成で領域確保に失敗すると終了します。
    class ConvexHullCollider3DComponent final : public Component
    {
    public:
        // 衝突形状とフィルターを設定します(points: ローカル点群で空は既定八面体, offset: ローカル中心位置, isTrigger: 通知だけにする指定, layer: 32で剰余を取る所属層, collisionMask: 相手の許可層ビット, material: 物理材質)。
        explicit ConvexHullCollider3DComponent(
            std::vector<DirectX::XMFLOAT3> points = DefaultPoints(),
            DirectX::XMFLOAT3 offset = {},
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        // 凸包を作るローカル点群を返します。
        [[nodiscard]] const std::vector<DirectX::XMFLOAT3>&
            Points() const noexcept
        {
            return m_points;
        }
        // 点群を検証せず置換します(points: 凸包を作るローカル点群)。
        void SetPoints(std::vector<DirectX::XMFLOAT3> points) noexcept;
        // 範囲内の点だけ変更します(index: 点番号, value: ローカルXYZ位置)。
        void SetPoint(
            std::size_t index,
            const DirectX::XMFLOAT3& value) noexcept;
        // 点を末尾に追加します(value: ローカルXYZ位置)。
        void AddPoint(
            const DirectX::XMFLOAT3& value =
                { 0.0f, 0.0f, 0.0f }) noexcept;
        // 範囲内の点だけ削除します(index: 点番号)。
        void RemovePoint(std::size_t index) noexcept;

        // ローカル中心位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT3&
            Offset() const noexcept
        {
            return m_offset;
        }
        // ローカル中心位置を設定します(value: 中心位置)。
        void SetOffset(
            const DirectX::XMFLOAT3& value) noexcept
        {
            m_offset = value;
        }
        // 衝突応答を行わず通知だけにする指定を返します。
        [[nodiscard]] bool IsTrigger() const noexcept
        {
            return m_isTrigger;
        }
        // 通知だけにする指定を設定します(value: 衝突応答を省く指定)。
        void SetTrigger(bool value) noexcept
        {
            m_isTrigger = value;
        }
        // 所属層の番号を返します。
        [[nodiscard]] std::uint32_t Layer() const noexcept
        {
            return m_layer;
        }
        // 所属層を32の剰余で設定します(value: 層番号)。
        void SetLayer(std::uint32_t value) noexcept
        {
            m_layer = value % 32u;
        }
        // 衝突を許可する相手層のビット集合を返します。
        [[nodiscard]] std::uint32_t
            CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        // 相手層の許可ビットを設定します(value: 層ビット集合)。
        void SetCollisionMask(std::uint32_t value) noexcept
        {
            m_collisionMask = value;
        }
        // 制限済みの物理材質を返します。
        [[nodiscard]] const PhysicsMaterial&
            Material() const noexcept
        {
            return m_material;
        }
        // 物理材質を制限して設定します(value: 物理材質)。
        void SetMaterial(PhysicsMaterial value) noexcept
        {
            value.Clamp();
            m_material = value;
        }

        // 点群に中心位置を足してワールド変換した凸包の点群を返します。
        [[nodiscard]] ConvexHull3D WorldHull() const noexcept;
        // ワールド形状を囲むXYZ軸平行境界を返します。
        [[nodiscard]] Bounds3D WorldBounds() const noexcept;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "ConvexHullCollider3D";
        }

    protected:
        // 軸平行境界と点群の目印を描画します(graphics: 描画装置, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // 原点中心でXYZ各軸に半幅0.5の既定八面体の点群を返します。
        [[nodiscard]] static std::vector<DirectX::XMFLOAT3>
            DefaultPoints();

        // 凸包を作るローカル点群
        std::vector<DirectX::XMFLOAT3> m_points;
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
