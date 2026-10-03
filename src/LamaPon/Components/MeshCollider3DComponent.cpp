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

    // 未設定メッシュの境界半幅
    constexpr float FallbackHalfExtent = 0.5f;

    // 8隅を変換して軸平行境界を求めます(bounds: 変換前の境界, matrix: 境界へ適用する行列)。
    LamaPon::Bounds3D TransformBoundsByMatrix(
        const LamaPon::Bounds3D& bounds,
        const DirectX::XMMATRIX matrix) noexcept
    {
        // 変換前の境界の8隅
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
        // 変換後のXYZ最小値
        XMFLOAT3 minimum{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max() };
        // 変換後のXYZ最大値
        XMFLOAT3 maximum{
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max() };
        // 変換する境界の隅
        for (const auto& corner : corners)
        {
            // ワールド変換した隅位置
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
        // 構築する共有衝突メッシュ
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
        // メッシュか代替箱の境界
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
        // 中心位置込みのワールド変換
        const auto worldMatrix = WorldMatrixWithOffset();
        // ワールド変換の行列式
        DirectX::XMVECTOR determinant{};
        // 中心位置込みの逆変換
        const auto inverseWorld = DirectX::XMMatrixInverse(
            &determinant,
            worldMatrix);
        // 逆変換して囲んだ検索境界
        const Bounds3D localQuery =
            TransformBoundsByMatrix(
                worldBounds,
                inverseWorld);

        // 検索境界に重なる三角形番号列
        std::vector<std::uint32_t> triangles;
        m_mesh->QueryOverlaps(localQuery, triangles);
        results.reserve(results.size() + triangles.size());
        // 検出または描画する三角形番号
        for (const auto triangle : triangles)
        {
            // 三角形の第1ローカル頂点
            XMFLOAT3 a{};
            // 三角形の第2ローカル頂点
            XMFLOAT3 b{};
            // 三角形の第3ローカル頂点
            XMFLOAT3 c{};
            m_mesh->GetTriangle(triangle, a, b, c);
            // 三角形のワールド頂点
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
        // 中心位置込みのワールド変換
        const auto worldMatrix = WorldMatrixWithOffset();
        // ワールド変換の行列式
        DirectX::XMVECTOR determinant{};
        // 中心位置込みの逆変換
        const auto inverseWorld = DirectX::XMMatrixInverse(
            &determinant,
            worldMatrix);
        // 逆変換して囲んだ検索境界
        const Bounds3D localQuery =
            TransformBoundsByMatrix(
                worldBounds,
                inverseWorld);
        // 検索境界に重なる三角形番号列
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
        // 中心位置込みのワールド変換
        const auto worldMatrix = WorldMatrixWithOffset();
        // ワールド変換の行列式
        DirectX::XMVECTOR determinant{};
        // 中心位置込みの逆変換
        const auto inverseWorld = DirectX::XMMatrixInverse(
            &determinant,
            worldMatrix);

        // レイ両端を逆変換し、非一様スケールを含む距離上限を保ちます。
        // レイのワールド始点
        const auto worldOrigin = DirectX::XMVectorSet(
            worldRay.origin.x,
            worldRay.origin.y,
            worldRay.origin.z,
            1.0f);
        // レイのワールド方向
        const auto worldDirection = DirectX::XMVectorSet(
            worldRay.direction.x,
            worldRay.direction.y,
            worldRay.direction.z,
            0.0f);
        // 距離上限のワールド終点
        const auto worldEnd = DirectX::XMVectorAdd(
            worldOrigin,
            DirectX::XMVectorScale(
                DirectX::XMVector3Normalize(
                    worldDirection),
                maximumDistance));
        // 逆変換したローカル始点
        const auto localOrigin =
            DirectX::XMVector3TransformCoord(
                worldOrigin,
                inverseWorld);
        // 逆変換したローカル終点
        const auto localEnd =
            DirectX::XMVector3TransformCoord(
                worldEnd,
                inverseWorld);
        // ローカルのレイ変位
        const auto localDelta = DirectX::XMVectorSubtract(
            localEnd,
            localOrigin);
        // ローカルのレイ長さ
        const float localLength =
            DirectX::XMVectorGetX(
                DirectX::XMVector3Length(localDelta));
        if (localLength <= 1e-8f)
        {
            return false;
        }
        // ローカルレイの始点
        XMFLOAT3 origin{};
        // 正規化したローカル方向
        XMFLOAT3 direction{};
        DirectX::XMStoreFloat3(&origin, localOrigin);
        DirectX::XMStoreFloat3(
            &direction,
            DirectX::XMVectorScale(
                localDelta,
                1.0f / localLength));

        // ローカルの最も近い接触
        CollisionMeshHit localHit{};
        if (!m_mesh->Raycast(
                origin,
                direction,
                localLength,
                localHit))
        {
            return false;
        }

        // 接触点のワールド位置
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

        // 法線を変換する逆転置行列
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

        if (m_modelPath.empty() && !HasMesh())
        {
            // 同じ物体のモデル描画部品
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
        // メッシュ読み込みの例外
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
        // ワールド形状の境界
        const auto bounds = WorldBounds();
        // 形状のデバッグRGBA色
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


        // 間引く三角形数の目安
        constexpr std::size_t MaximumDebugTriangles =
            2048;
        // 形状の全三角形数
        const std::size_t triangleCount =
            m_mesh->TriangleCount();
        // 描画する三角形番号の間隔
        const std::size_t step = std::max<std::size_t>(
            1,
            triangleCount / MaximumDebugTriangles);
        // 中心位置込みのワールド変換
        const auto worldMatrix = WorldMatrixWithOffset();
        // 三角形の辺の線分端点列
        std::vector<DirectX::XMFLOAT3> lines;
        lines.reserve(
            std::min(
                triangleCount,
                MaximumDebugTriangles) * 6);
        // 検出または描画する三角形番号
        for (std::size_t triangle = 0;
            // 形状の全三角形数
            triangle < triangleCount;
            triangle += step)
        {
            // 三角形の第1ローカル頂点
            XMFLOAT3 a{};
            // 三角形の第2ローカル頂点
            XMFLOAT3 b{};
            // 三角形の第3ローカル頂点
            XMFLOAT3 c{};
            m_mesh->GetTriangle(
                static_cast<std::uint32_t>(triangle),
                a,
                b,
                c);
            // 三角形の第1ワールド頂点
            XMFLOAT3 worldA{};
            // 三角形の第2ワールド頂点
            XMFLOAT3 worldB{};
            // 三角形の第3ワールド頂点
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
