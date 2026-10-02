#pragma once

#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LamaPon
{
    // ワールドXY平面の凸多角形で、衝突には周回順の3頂点以上を指定します。
    struct Polygon2D final
    {
        // 周回順のワールドXY凸頂点列
        std::vector<DirectX::XMFLOAT2> vertices;
    };

    // 形状は凸性を検証しないため、周回順の凸頂点列と有限座標を指定します。
    // noexceptの頂点操作・形状生成で領域確保に失敗すると終了します。
    class PolygonCollider2DComponent final : public Component
    {
    public:
        // 衝突形状とフィルターを設定します(vertices: 周回順のローカル凸頂点列で空は既定三角形, offset: ローカル中心位置, isTrigger: 通知だけにする指定, layer: 32で剰余を取る所属層, collisionMask: 相手の許可層ビット, material: 物理材質)。
        explicit PolygonCollider2DComponent(
            std::vector<DirectX::XMFLOAT2> vertices = DefaultVertices(),
            DirectX::XMFLOAT2 offset = { 0.0f, 0.0f },
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {}) noexcept;

        // 周回順のローカル頂点列を返します。
        [[nodiscard]] const std::vector<DirectX::XMFLOAT2>&
            Vertices() const noexcept
        {
            return m_vertices;
        }
        // 頂点列を検証せず置換します(vertices: 周回順のローカル凸頂点列)。
        void SetVertices(std::vector<DirectX::XMFLOAT2> vertices) noexcept;
        // 範囲内の頂点だけ変更します(index: 頂点番号, value: ローカルXY位置)。
        void SetVertex(
            std::size_t index,
            const DirectX::XMFLOAT2& value) noexcept;
        // 頂点を末尾に追加します(value: ローカルXY位置)。
        void AddVertex(
            const DirectX::XMFLOAT2& value = { 0.0f, 0.0f }) noexcept;
        // 範囲内の頂点だけ削除します(index: 頂点番号)。
        void RemoveVertex(std::size_t index) noexcept;

        // ローカル中心位置を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& Offset() const noexcept
        {
            return m_offset;
        }
        // ローカル中心位置を設定します(offset: 中心位置)。
        void SetOffset(const DirectX::XMFLOAT2& offset) noexcept
        {
            m_offset = offset;
        }
        // 衝突応答を行わず通知だけにする指定を返します。
        [[nodiscard]] bool IsTrigger() const noexcept { return m_isTrigger; }
        // 通知だけにする指定を設定します(trigger: 衝突応答を省く指定)。
        void SetTrigger(const bool trigger) noexcept { m_isTrigger = trigger; }
        // 所属層の番号を返します。
        [[nodiscard]] std::uint32_t Layer() const noexcept { return m_layer; }
        // 所属層を32の剰余で設定します(layer: 層番号)。
        void SetLayer(const std::uint32_t layer) noexcept { m_layer = layer % 32u; }
        // 衝突を許可する相手層のビット集合を返します。
        [[nodiscard]] std::uint32_t CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        // 相手層の許可ビットを設定します(mask: 層ビット集合)。
        void SetCollisionMask(const std::uint32_t mask) noexcept
        {
            m_collisionMask = mask;
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

        // 頂点に中心位置を足してワールド変換したXY頂点列を返します。
        [[nodiscard]] Polygon2D WorldPolygon() const noexcept;
        // 変換した頂点を囲むXY境界を返し空なら原点のゼロ境界を返します。
        [[nodiscard]] Bounds2D WorldBounds() const noexcept;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "PolygonCollider2D";
        }

    protected:
        // 衝突形状の輪郭を描画します(graphics: 描画装置, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // 原点付近の既定三角形の頂点列を返します。
        [[nodiscard]] static std::vector<DirectX::XMFLOAT2> DefaultVertices();

        // 周回順のローカル凸頂点列
        std::vector<DirectX::XMFLOAT2> m_vertices;
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
