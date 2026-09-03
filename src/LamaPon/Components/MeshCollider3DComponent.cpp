#include "LamaPon/Components/MeshCollider3DComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/CollisionMeshImporter.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Physics/CollisionMesh.h"
#include "LamaPon/Physics/Raycast.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
    using DirectX::XMFLOAT3;

    // メッシュ未設定でも選択やブロードフェーズが破綻しないよう、
    // 原点周りの小さなAABBを返します。
    constexpr float FallbackHalfExtent = 0.5f;

    LamaPon::Bounds3D TransformBoundsByMatrix(
        const LamaPon::Bounds3D& bounds,
        const DirectX::XMMATRIX matrix) noexcept
    {
        const XMFLOAT3 corners[8]{
            { bounds.minimum.x, bounds.minimum.y, bounds.minimum.z },
            { bounds.maximum.x, bounds.minimum.y, bounds.minimum.z },
            { bounds.minimum.x, bounds.maximum.y, bounds.minimum.z },
            { bounds.maximum.x, bounds.maximum.y, bounds.minimum.z },
            { bounds.minimum.x, bounds.minimum.y, bounds.maximum.z },
            { bounds.maximum.x, bounds.minimum.y, bounds.maximum.z },
            { bounds.minimum.x, bounds.maximum.y, bounds.maximum.z },
            { bounds.maximum.x, bounds.maximum.y, bounds.maximum.z }
        };
        XMFLOAT3 minimum{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max() };
        XMFLOAT3 maximum{
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max() };
        for (const auto& corner : corners)
        {
            XMFLOAT3 transformed{};
            DirectX::XMStoreFloat3(
                &transformed,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&corner),
                    matrix));
            minimum.x = std::min(minimum.x, transformed.x);
            minimum.y = std::min(minimum.y, transformed.y);
            minimum.z = std::min(minimum.z, transformed.z);
            maximum.x = std::max(maximum.x, transformed.x);
            maximum.y = std::max(maximum.y, transformed.y);
            maximum.z = std::max(maximum.z, transformed.z);
        }
        return { minimum, maximum };
    }
}

namespace LamaPon
{
    MeshCollider3DComponent::MeshCollider3DComponent(
        std::filesystem::path modelPath,
        const DirectX::XMFLOAT3 offset,
        const bool isTrigger,
        const std::uint32_t layer,
        const std::uint32_t collisionMask,
        const PhysicsMaterial material)
        : m_modelPath(std::move(modelPath))
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(std::min(layer, 31u))
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
    }

    void MeshCollider3DComponent::SetModelPath(
        std::filesystem::path path)
    {
        m_modelPath = std::move(path);
        ReloadMesh();
    }

    void MeshCollider3DComponent::SetMesh(
        std::vector<DirectX::XMFLOAT3> vertices,
        std::vector<std::uint32_t> indices)
    {
        auto mesh = std::make_shared<CollisionMesh>();
        mesh->Build(
            std::move(vertices),
            std::move(indices));
        m_mesh = std::move(mesh);
        m_modelPath.clear();
        m_lastError.clear();
    }

    void MeshCollider3DComponent::SetLayer(
        const std::uint32_t layer) noexcept
    {
        m_layer = std::min(layer, 31u);
    }

    bool MeshCollider3DComponent::HasMesh() const noexcept
    {
        return m_mesh != nullptr && !m_mesh->IsEmpty();
    }

    std::size_t
        MeshCollider3DComponent::TriangleCount()
            const noexcept
    {
        return m_mesh != nullptr
            ? m_mesh->TriangleCount()
            : 0;
    }

    DirectX::XMMATRIX
        MeshCollider3DComponent::WorldMatrixWithOffset()
            const noexcept
    {
        return DirectX::XMMatrixTranslation(
                m_offset.x,
                m_offset.y,
                m_offset.z)
            * Owner().WorldMatrix();
    }

    Bounds3D
        MeshCollider3DComponent::WorldBounds()
            const noexcept
    {
        const Bounds3D localBounds = HasMesh()
            ? m_mesh->LocalBounds()
            : Bounds3D{
                {
                    -FallbackHalfExtent,
                    -FallbackHalfExtent,
                    -FallbackHalfExtent
                },
                {
                    FallbackHalfExtent,
                    FallbackHalfExtent,
                    FallbackHalfExtent
                } };
        return TransformBoundsByMatrix(
            localBounds,
            WorldMatrixWithOffset());
    }

    void MeshCollider3DComponent::CollectTriangles(
        const Bounds3D& worldBounds,
        std::vector<MeshColliderTriangle>& results) const
    {
        if (!HasMesh())
        {
            return;
        }
        const auto worldMatrix = WorldMatrixWithOffset();
        DirectX::XMVECTOR determinant{};
        const auto inverseWorld = DirectX::XMMatrixInverse(
            &determinant,
            worldMatrix);
        const Bounds3D localQuery =
            TransformBoundsByMatrix(
                worldBounds,
                inverseWorld);

        std::vector<std::uint32_t> triangles;
        m_mesh->QueryOverlaps(localQuery, triangles);
        results.reserve(results.size() + triangles.size());
        for (const auto triangle : triangles)
        {
            XMFLOAT3 a{};
            XMFLOAT3 b{};
            XMFLOAT3 c{};
            m_mesh->GetTriangle(triangle, a, b, c);
            MeshColliderTriangle world{};
            DirectX::XMStoreFloat3(
                &world.a,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&a),
                    worldMatrix));
            DirectX::XMStoreFloat3(
                &world.b,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&b),
                    worldMatrix));
            DirectX::XMStoreFloat3(
                &world.c,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&c),
                    worldMatrix));
            results.push_back(world);
        }
    }

    bool MeshCollider3DComponent::OverlapsBounds(
        const Bounds3D& worldBounds) const
    {
        if (!HasMesh())
        {
            return false;
        }
        const auto worldMatrix = WorldMatrixWithOffset();
        DirectX::XMVECTOR determinant{};
        const auto inverseWorld = DirectX::XMMatrixInverse(
            &determinant,
            worldMatrix);
        const Bounds3D localQuery =
            TransformBoundsByMatrix(
                worldBounds,
                inverseWorld);
        std::vector<std::uint32_t> triangles;
        m_mesh->QueryOverlaps(localQuery, triangles);
        return !triangles.empty();
    }

    bool MeshCollider3DComponent::Raycast(
        const Ray& worldRay,
        const float maximumDistance,
        MeshColliderRaycastHit& hit) const
    {
        if (!HasMesh() || maximumDistance <= 0.0f)
        {
            return false;
        }
        const auto worldMatrix = WorldMatrixWithOffset();
        DirectX::XMVECTOR determinant{};
        const auto inverseWorld = DirectX::XMMatrixInverse(
            &determinant,
            worldMatrix);

        // 端点をローカルへ変換して、スケール込みの距離を保ちます。
        const auto worldOrigin = DirectX::XMVectorSet(
            worldRay.origin.x,
            worldRay.origin.y,
            worldRay.origin.z,
            1.0f);
        const auto worldDirection = DirectX::XMVectorSet(
            worldRay.direction.x,
            worldRay.direction.y,
            worldRay.direction.z,
            0.0f);
        const auto worldEnd = DirectX::XMVectorAdd(
            worldOrigin,
            DirectX::XMVectorScale(
                DirectX::XMVector3Normalize(
                    worldDirection),
                maximumDistance));
        const auto localOrigin =
            DirectX::XMVector3TransformCoord(
                worldOrigin,
                inverseWorld);
        const auto localEnd =
            DirectX::XMVector3TransformCoord(
                worldEnd,
                inverseWorld);
        const auto localDelta = DirectX::XMVectorSubtract(
            localEnd,
            localOrigin);
        const float localLength =
            DirectX::XMVectorGetX(
                DirectX::XMVector3Length(localDelta));
        if (localLength <= 1e-8f)
        {
            return false;
        }
        XMFLOAT3 origin{};
        XMFLOAT3 direction{};
        DirectX::XMStoreFloat3(&origin, localOrigin);
        DirectX::XMStoreFloat3(
            &direction,
            DirectX::XMVectorScale(
                localDelta,
                1.0f / localLength));

        CollisionMeshHit localHit{};
        if (!m_mesh->Raycast(
                origin,
                direction,
                localLength,
                localHit))
        {
            return false;
        }

        const auto worldPoint =
            DirectX::XMVector3TransformCoord(
                DirectX::XMVectorSet(
                    localHit.point.x,
                    localHit.point.y,
                    localHit.point.z,
                    1.0f),
                worldMatrix);
        DirectX::XMStoreFloat3(&hit.point, worldPoint);
        hit.distance = DirectX::XMVectorGetX(
            DirectX::XMVector3Length(
                DirectX::XMVectorSubtract(
                    worldPoint,
                    worldOrigin)));
        // 法線は逆転置行列で変換します（非一様スケール対応）。
        const auto normalMatrix =
            DirectX::XMMatrixTranspose(inverseWorld);
        DirectX::XMStoreFloat3(
            &hit.normal,
            DirectX::XMVector3Normalize(
                DirectX::XMVector3TransformNormal(
                    DirectX::XMVectorSet(
                        localHit.normal.x,
                        localHit.normal.y,
                        localHit.normal.z,
                        0.0f),
                    normalMatrix)));
        return true;
    }

    void MeshCollider3DComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_assets = &graphics.Assets();
        // パス未指定なら同じGameObjectのModelRendererから借ります。
        if (m_modelPath.empty() && !HasMesh())
        {
            if (const auto* renderer =
                Owner().GetComponent<
                    ModelRendererComponent>())
            {
                m_modelPath = renderer->ModelPath();
            }
        }
        ReloadMesh();
    }

    void MeshCollider3DComponent::ReloadMesh()
    {
        if (m_assets == nullptr || m_modelPath.empty())
        {
            return;
        }
        try
        {
            m_mesh = CollisionMeshImporter::Load(
                *m_assets,
                m_modelPath);
            m_lastError.clear();
        }
        catch (const std::exception& exception)
        {
            m_mesh.reset();
            m_lastError = exception.what();
        }
    }

    void MeshCollider3DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        const auto bounds = WorldBounds();
        const auto color = m_isTrigger
            ? DirectX::XMVectorSet(
                1.0f, 0.75f, 0.1f, 1.0f)
            : DirectX::XMVectorSet(
                0.95f, 0.45f, 0.2f, 1.0f);
        graphics.Debug().DrawBounds(
            bounds,
            color,
            view,
            projection);
        if (!HasMesh())
        {
            return;
        }

        // 三角形が多すぎる場合はワイヤ描画を間引きます。
        constexpr std::size_t MaximumDebugTriangles =
            2048;
        const std::size_t triangleCount =
            m_mesh->TriangleCount();
        const std::size_t step = std::max<std::size_t>(
            1,
            triangleCount / MaximumDebugTriangles);
        const auto worldMatrix = WorldMatrixWithOffset();
        std::vector<DirectX::XMFLOAT3> lines;
        lines.reserve(
            std::min(
                triangleCount,
                MaximumDebugTriangles) * 6);
        for (std::size_t triangle = 0;
            triangle < triangleCount;
            triangle += step)
        {
            XMFLOAT3 a{};
            XMFLOAT3 b{};
            XMFLOAT3 c{};
            m_mesh->GetTriangle(
                static_cast<std::uint32_t>(triangle),
                a,
                b,
                c);
            XMFLOAT3 worldA{};
            XMFLOAT3 worldB{};
            XMFLOAT3 worldC{};
            DirectX::XMStoreFloat3(
                &worldA,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&a),
                    worldMatrix));
            DirectX::XMStoreFloat3(
                &worldB,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&b),
                    worldMatrix));
            DirectX::XMStoreFloat3(
                &worldC,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&c),
                    worldMatrix));
            lines.push_back(worldA);
            lines.push_back(worldB);
            lines.push_back(worldB);
            lines.push_back(worldC);
            lines.push_back(worldC);
            lines.push_back(worldA);
        }
        graphics.Debug().DrawLines(
            lines,
            color,
            view,
            projection);
    }
}
