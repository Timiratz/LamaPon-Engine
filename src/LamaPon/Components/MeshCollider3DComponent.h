#pragma once

#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    class CollisionMesh;
    class GraphicsDevice;
    struct Bounds3D;
    struct Ray;

    // ワールド空間の三角形1枚（ナローフェーズへ渡す単位）。
    struct MeshColliderTriangle final
    {
        DirectX::XMFLOAT3 a{};
        DirectX::XMFLOAT3 b{};
        DirectX::XMFLOAT3 c{};
    };

    struct MeshColliderRaycastHit final
    {
        DirectX::XMFLOAT3 point{};
        DirectX::XMFLOAT3 normal{};
        float distance{};
    };

    // インポートしたモデルの三角形メッシュをそのまま使う
    // 静的コライダー。Rigidbodyを持たないステージ・地形用です。
    class MeshCollider3DComponent final : public Component
    {
    public:
        explicit MeshCollider3DComponent(
            std::filesystem::path modelPath = {},
            DirectX::XMFLOAT3 offset = {},
            bool isTrigger = false,
            std::uint32_t layer = 0,
            std::uint32_t collisionMask = 0xffffffffu,
            PhysicsMaterial material = {});

        void SetModelPath(std::filesystem::path path);
        // コードから直接三角形を設定します（テスト・手続き生成用）。
        void SetMesh(
            std::vector<DirectX::XMFLOAT3> vertices,
            std::vector<std::uint32_t> indices);
        void SetOffset(
            const DirectX::XMFLOAT3& offset) noexcept
        {
            m_offset = offset;
        }
        void SetTrigger(const bool trigger) noexcept
        {
            m_isTrigger = trigger;
        }
        void SetLayer(std::uint32_t layer) noexcept;
        void SetCollisionMask(
            const std::uint32_t mask) noexcept
        {
            m_collisionMask = mask;
        }
        void SetMaterial(
            const PhysicsMaterial& material) noexcept
        {
            m_material = material;
        }

        [[nodiscard]] const std::filesystem::path&
            ModelPath() const noexcept
        {
            return m_modelPath;
        }
        [[nodiscard]] const DirectX::XMFLOAT3&
            Offset() const noexcept
        {
            return m_offset;
        }
        [[nodiscard]] bool IsTrigger() const noexcept
        {
            return m_isTrigger;
        }
        [[nodiscard]] std::uint32_t Layer() const noexcept
        {
            return m_layer;
        }
        [[nodiscard]] std::uint32_t
            CollisionMask() const noexcept
        {
            return m_collisionMask;
        }
        [[nodiscard]] const PhysicsMaterial&
            Material() const noexcept
        {
            return m_material;
        }
        [[nodiscard]] bool HasMesh() const noexcept;
        [[nodiscard]] std::size_t
            TriangleCount() const noexcept;
        [[nodiscard]] const std::string&
            LastError() const noexcept
        {
            return m_lastError;
        }

        [[nodiscard]] Bounds3D WorldBounds() const noexcept;
        // ワールドAABBに重なる三角形をワールド空間で列挙します。
        void CollectTriangles(
            const Bounds3D& worldBounds,
            std::vector<MeshColliderTriangle>& results)
            const;
        // ワールド空間レイキャスト（最近ヒット）。
        [[nodiscard]] bool Raycast(
            const Ray& worldRay,
            float maximumDistance,
            MeshColliderRaycastHit& hit) const;
        // ワールドAABBと交差する三角形が1つでもあればtrue。
        [[nodiscard]] bool OverlapsBounds(
            const Bounds3D& worldBounds) const;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "MeshCollider3D";
        }

    protected:
        void OnInitialize(
            GraphicsDevice& graphics) override;
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        void ReloadMesh();
        [[nodiscard]] DirectX::XMMATRIX
            WorldMatrixWithOffset() const noexcept;

        std::filesystem::path m_modelPath;
        DirectX::XMFLOAT3 m_offset;
        bool m_isTrigger;
        std::uint32_t m_layer;
        std::uint32_t m_collisionMask;
        PhysicsMaterial m_material;
        std::shared_ptr<const CollisionMesh> m_mesh;
        std::string m_lastError;
        AssetManager* m_assets{};
    };
}
