#include "LamaPon/Scene/Scene.h"

#include "LamaPon/Core/JobSystem.h"

#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/LODGroupComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Scene/GameObject.h"

#include <DirectXCollision.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace
{
    bool IsRenderer(
        const LamaPon::GameObject& object) noexcept
    {
        const auto* mesh =
            object.GetComponent<
                LamaPon::MeshRendererComponent>();
        const auto* model =
            object.GetComponent<
                LamaPon::ModelRendererComponent>();
        return object.IsActiveInHierarchy()
            && ((mesh != nullptr
                    && mesh->IsEnabled())
                || (model != nullptr
                    && model->IsEnabled()));
    }

    LamaPon::Bounds3D RenderBounds(
        const LamaPon::GameObject& object) noexcept
    {
        if (const auto* box =
                object.GetComponent<
                    LamaPon::BoxCollider3DComponent>();
            box != nullptr && box->IsEnabled())
        {
            return box->WorldBounds();
        }
        if (const auto* capsule =
                object.GetComponent<
                    LamaPon::CapsuleCollider3DComponent>();
            capsule != nullptr
                && capsule->IsEnabled())
        {
            return capsule->WorldBounds();
        }
        if (const auto* sphere =
                object.GetComponent<
                    LamaPon::SphereCollider3DComponent>();
            sphere != nullptr
                && sphere->IsEnabled())
        {
            return sphere->WorldBounds();
        }
        if (const auto* hull =
                object.GetComponent<
                    LamaPon::ConvexHullCollider3DComponent>();
            hull != nullptr
                && hull->IsEnabled())
        {
            return hull->WorldBounds();
        }

        LamaPon::Bounds3D localBounds{
            { -0.5f, -0.5f, -0.5f },
            { 0.5f, 0.5f, 0.5f }
        };
        bool hasRendererBounds = false;
        if (const auto* mesh =
                object.GetComponent<
                    LamaPon::MeshRendererComponent>();
            mesh != nullptr)
        {
            hasRendererBounds =
                mesh->TryGetLocalBounds(localBounds);
        }
        if (!hasRendererBounds)
        {
            if (const auto* model =
                object.GetComponent<
                    LamaPon::ModelRendererComponent>();
                model != nullptr)
            {
                static_cast<void>(
                    model->TryGetLocalBounds(localBounds));
            }
        }

        LamaPon::Bounds3D result{
            {
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()
            },
            {
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest()
            }
        };
        const auto world = object.WorldMatrix();
        for (int x = -1; x <= 1; x += 2)
        {
            for (int y = -1; y <= 1; y += 2)
            {
                for (int z = -1; z <= 1; z += 2)
                {
                    DirectX::XMFLOAT3 point{};
                    DirectX::XMStoreFloat3(
                        &point,
                        DirectX::XMVector3TransformCoord(
                            DirectX::XMVectorSet(
                                x < 0
                                    ? localBounds.minimum.x
                                    : localBounds.maximum.x,
                                y < 0
                                    ? localBounds.minimum.y
                                    : localBounds.maximum.y,
                                z < 0
                                    ? localBounds.minimum.z
                                    : localBounds.maximum.z,
                                1.0f),
                            world));
                    result.minimum.x =
                        std::min(
                            result.minimum.x,
                            point.x);
                    result.minimum.y =
                        std::min(
                            result.minimum.y,
                            point.y);
                    result.minimum.z =
                        std::min(
                            result.minimum.z,
                            point.z);
                    result.maximum.x =
                        std::max(
                            result.maximum.x,
                            point.x);
                    result.maximum.y =
                        std::max(
                            result.maximum.y,
                            point.y);
                    result.maximum.z =
                        std::max(
                            result.maximum.z,
                            point.z);
                }
            }
        }
        return result;
    }

    LamaPon::Bounds3D ExpandBounds(
        const LamaPon::Bounds3D& bounds,
        const float margin) noexcept
    {
        if (margin <= 0.0f)
        {
            return bounds;
        }
        return {
            {
                bounds.minimum.x - margin,
                bounds.minimum.y - margin,
                bounds.minimum.z - margin
            },
            {
                bounds.maximum.x + margin,
                bounds.maximum.y + margin,
                bounds.maximum.z + margin
            }
        };
    }

    DirectX::BoundingBox ToBoundingBox(
        const LamaPon::Bounds3D& bounds) noexcept
    {
        return DirectX::BoundingBox{
            {
                (bounds.minimum.x
                    + bounds.maximum.x) * 0.5f,
                (bounds.minimum.y
                    + bounds.maximum.y) * 0.5f,
                (bounds.minimum.z
                    + bounds.maximum.z) * 0.5f
            },
            {
                std::abs(
                    bounds.maximum.x
                    - bounds.minimum.x) * 0.5f,
                std::abs(
                    bounds.maximum.y
                    - bounds.minimum.y) * 0.5f,
                std::abs(
                    bounds.maximum.z
                    - bounds.minimum.z) * 0.5f
            }
        };
    }

    std::array<DirectX::XMFLOAT3, 8> Corners(
        const LamaPon::Bounds3D& bounds) noexcept
    {
        return {
            DirectX::XMFLOAT3{
                bounds.minimum.x,
                bounds.minimum.y,
                bounds.minimum.z },
            DirectX::XMFLOAT3{
                bounds.maximum.x,
                bounds.minimum.y,
                bounds.minimum.z },
            DirectX::XMFLOAT3{
                bounds.minimum.x,
                bounds.maximum.y,
                bounds.minimum.z },
            DirectX::XMFLOAT3{
                bounds.maximum.x,
                bounds.maximum.y,
                bounds.minimum.z },
            DirectX::XMFLOAT3{
                bounds.minimum.x,
                bounds.minimum.y,
                bounds.maximum.z },
            DirectX::XMFLOAT3{
                bounds.maximum.x,
                bounds.minimum.y,
                bounds.maximum.z },
            DirectX::XMFLOAT3{
                bounds.minimum.x,
                bounds.maximum.y,
                bounds.maximum.z },
            DirectX::XMFLOAT3{
                bounds.maximum.x,
                bounds.maximum.y,
                bounds.maximum.z }
        };
    }

    bool IsOutsideClipFrustum(
        const LamaPon::Bounds3D& bounds,
        DirectX::FXMMATRIX viewProjection) noexcept
    {
        std::array<bool, 6> allOutside{
            true, true, true, true, true, true
        };
        for (const auto& corner : Corners(bounds))
        {
            DirectX::XMFLOAT4 clip{};
            DirectX::XMStoreFloat4(
                &clip,
                DirectX::XMVector4Transform(
                    DirectX::XMVectorSet(
                        corner.x,
                        corner.y,
                        corner.z,
                        1.0f),
                    viewProjection));
            if (!std::isfinite(clip.x)
                || !std::isfinite(clip.y)
                || !std::isfinite(clip.z)
                || !std::isfinite(clip.w))
            {
                return false;
            }
            const std::array distances{
                clip.x + clip.w,
                clip.w - clip.x,
                clip.y + clip.w,
                clip.w - clip.y,
                clip.z,
                clip.w - clip.z
            };
            for (std::size_t plane = 0;
                plane < distances.size();
                ++plane)
            {
                if (distances[plane] >= 0.0f)
                {
                    allOutside[plane] = false;
                }
            }
        }
        return std::ranges::any_of(
            allOutside,
            [](const bool outside)
            {
                return outside;
            });
    }

    struct ScreenBounds final
    {
        float left{};
        float top{};
        float right{};
        float bottom{};
        float nearDepth{ 1.0f };
        float farDepth{};
        bool valid{};
    };

    ScreenBounds ProjectBounds(
        const LamaPon::Bounds3D& bounds,
        DirectX::FXMMATRIX viewProjection) noexcept
    {
        ScreenBounds result{
            1.0f,
            1.0f,
            -1.0f,
            -1.0f,
            1.0f,
            0.0f,
            false
        };
        for (const auto& corner : Corners(bounds))
        {
            DirectX::XMFLOAT4 clip{};
            DirectX::XMStoreFloat4(
                &clip,
                DirectX::XMVector4Transform(
                    DirectX::XMVectorSet(
                        corner.x,
                        corner.y,
                        corner.z,
                        1.0f),
                    viewProjection));
            if (clip.w <= 0.00001f)
            {
                return {};
            }
            const float x = clip.x / clip.w;
            const float y = clip.y / clip.w;
            const float depth = clip.z / clip.w;
            result.left =
                std::min(result.left, x);
            result.right =
                std::max(result.right, x);
            result.top =
                std::min(result.top, y);
            result.bottom =
                std::max(result.bottom, y);
            result.nearDepth =
                std::min(result.nearDepth, depth);
            result.farDepth =
                std::max(result.farDepth, depth);
            result.valid = true;
        }
        result.left =
            std::clamp(result.left, -1.0f, 1.0f);
        result.right =
            std::clamp(result.right, -1.0f, 1.0f);
        result.top =
            std::clamp(result.top, -1.0f, 1.0f);
        result.bottom =
            std::clamp(result.bottom, -1.0f, 1.0f);
        result.valid = result.valid
            && result.left < result.right
            && result.top < result.bottom;
        return result;
    }
}

namespace LamaPon
{
    bool Scene::RefreshRenderSpatialIndex() const
    {
        std::vector<GameObject*> renderers;
        renderers.reserve(m_gameObjects.size());
        for (const auto& object : m_gameObjects)
        {
            if (IsRenderer(*object))
            {
                renderers.push_back(object.get());
            }
        }

        std::vector<RenderSpatialIndex::Entry> next(renderers.size());
        JobSystem::Instance().ParallelFor(
            renderers.size(),
            64,
            [&renderers, &next](
                const std::size_t begin,
                const std::size_t end)
            {
                for (std::size_t index = begin;
                    index < end;
                    ++index)
                {
                    auto* const object = renderers[index];
                    const auto bounds = RenderBounds(*object);
                    next[index] = {
                        object,
                        bounds,
                        ExpandBounds(
                            bounds,
                            object->CullingMargin())
                    };
                }
            });

        return m_renderSpatialIndex.Update(std::move(next));
    }

    Scene::VisibilityResult
        Scene::BuildRenderVisibility(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) const
    {
        VisibilityResult result;
        const bool spatialIndexReused =
            RefreshRenderSpatialIndex();
        std::vector<GameObject*> renderers;
        renderers.reserve(m_renderSpatialIndex.Entries().size());
        for (const auto& entry : m_renderSpatialIndex.Entries())
        {
            renderers.push_back(entry.object);
        }
        result.stats.rendererCount =
            renderers.size();
        result.stats.spatialNodeCount =
            m_renderSpatialIndex.NodeCount();
        result.stats.spatialIndexReused =
            spatialIndexReused;

        DirectX::XMVECTOR determinant{};
        const auto inverseView =
            DirectX::XMMatrixInverse(
                &determinant,
                view);
        DirectX::XMFLOAT3 cameraPosition{};
        DirectX::XMStoreFloat3(
            &cameraPosition,
            inverseView.r[3]);

        std::unordered_map<GameObjectId, bool>
            lodVisibility;
        for (const auto& object : m_gameObjects)
        {
            const auto* group =
                object->GetComponent<
                    LODGroupComponent>();
            if (!object->IsActiveInHierarchy()
                || group == nullptr
                || !group->IsEnabled())
            {
                continue;
            }
            ++result.stats.lodGroupCount;
            DirectX::XMFLOAT3 groupPosition{};
            DirectX::XMStoreFloat3(
                &groupPosition,
                object->WorldMatrix().r[3]);
            const float deltaX =
                groupPosition.x
                - cameraPosition.x;
            const float deltaY =
                groupPosition.y
                - cameraPosition.y;
            const float deltaZ =
                groupPosition.z
                - cameraPosition.z;
            const float distance = std::sqrt(
                deltaX * deltaX
                + deltaY * deltaY
                + deltaZ * deltaZ);
            const auto selected =
                group->SelectTarget(distance);
            for (const auto& level :
                group->Levels())
            {
                const bool visible =
                    level.targetId != 0
                    && level.targetId == selected;
                if (const auto existing =
                        lodVisibility.find(
                            level.targetId);
                    existing != lodVisibility.end())
                {
                    existing->second =
                        existing->second
                        && visible;
                }
                else if (level.targetId != 0)
                {
                    lodVisibility.emplace(
                        level.targetId,
                        visible);
                }
            }
        }
        for (const auto& [id, visible] :
            lodVisibility)
        {
            if (!visible)
            {
                result.lodHidden.insert(id);
                result.renderHidden.insert(id);
                if (const auto* object =
                        FindGameObject(id);
                    object != nullptr
                        && IsRenderer(*object))
                {
                    ++result.stats.lodCulledCount;
                }
            }
        }

        DirectX::BoundingFrustum worldFrustum;
        if (m_frustumCullingEnabled)
        {
            DirectX::BoundingFrustum
                projectionFrustum;
            DirectX::BoundingFrustum::
                CreateFromMatrix(
                    projectionFrustum,
                    projection,
                    true);
            projectionFrustum.Transform(
                worldFrustum,
                inverseView);
        }

        std::vector<unsigned char> spatialVisible;
        if (m_frustumCullingEnabled)
        {
            auto query = m_renderSpatialIndex.Query(
                [&worldFrustum](const Bounds3D& bounds)
                {
                    return worldFrustum.Contains(ToBoundingBox(bounds)) != DirectX::DISJOINT;
                });
            spatialVisible = std::move(query.candidates);
            result.stats.spatialNodeTestCount = query.nodeTests;
        }
        else
        {
            spatialVisible.assign(m_renderSpatialIndex.Entries().size(), 1u);
        }

        struct Candidate final
        {
            GameObject* object{};
            Bounds3D bounds{};
            ScreenBounds screen{};
            bool occluder{};
        };
        std::vector<Candidate> candidates;
        const auto viewProjection =
            view * projection;

        // バウンズ計算・フラスタム判定・スクリーン投影は
        // 要素ごとに独立した読み取り処理なので並列化します。
        // 結果はスロットへ書き、逐次でマージして統計と
        // candidatesの順序を安定させます。
        struct CandidateSlot final
        {
            Candidate candidate{};
            bool frustumCulled{};
            bool skipped{};
        };
        std::vector<CandidateSlot> slots(
            renderers.size());
        JobSystem::Instance().ParallelFor(
            renderers.size(),
            64,
            [this,
                &renderers,
                &slots,
                &result,
                &worldFrustum,
                &spatialVisible,
                &viewProjection](
                const std::size_t begin,
                const std::size_t end)
            {
                for (std::size_t index = begin;
                    index < end;
                    ++index)
                {
                    auto* object = renderers[index];
                    auto& slot = slots[index];
                    // renderHiddenはこの並列区間中は読み取り
                    // 専用です（LOD非表示の照会のみ）。
                    if (result.renderHidden.contains(
                            object->Id()))
                    {
                        slot.skipped = true;
                        continue;
                    }
                    // 地形や背景など、常時描画を指定したオブジェクトは
                    // 視錐台・遮蔽カリングのどちらにも渡しません。
                    if (object->IsAlwaysVisible())
                    {
                        slot.skipped = true;
                        continue;
                    }
                    const auto& bounds =
                        m_renderSpatialIndex.Entries()[index].bounds;
                    const auto& cullingBounds =
                        m_renderSpatialIndex.Entries()[index].cullingBounds;
                    if (m_frustumCullingEnabled
                        && (spatialVisible[index] == 0u
                            || worldFrustum.Contains(
                                ToBoundingBox(cullingBounds))
                                == DirectX::DISJOINT))
                    {
                        slot.frustumCulled = true;
                        continue;
                    }
                    const auto* model =
                        object->GetComponent<
                            ModelRendererComponent>();
                    slot.candidate = {
                        object,
                        bounds,
                        ProjectBounds(
                            bounds,
                            viewProjection),
                        model == nullptr
                            || !model->IsWireframe()
                    };
                }
            });
        candidates.reserve(renderers.size());
        for (std::size_t index = 0;
            index < renderers.size();
            ++index)
        {
            const auto& slot = slots[index];
            if (slot.skipped)
            {
                continue;
            }
            if (slot.frustumCulled)
            {
                result.renderHidden.insert(
                    renderers[index]->Id());
                ++result.stats.frustumCulledCount;
                continue;
            }
            candidates.push_back(slot.candidate);
        }

        if (m_occlusionCullingEnabled)
        {
            std::ranges::sort(
                candidates,
                [](const Candidate& left,
                    const Candidate& right)
                {
                    return left.screen.nearDepth
                        < right.screen.nearDepth;
                });
            constexpr int gridWidth = 32;
            constexpr int gridHeight = 18;
            std::array<
                float,
                gridWidth * gridHeight> depth;
            depth.fill(
                std::numeric_limits<float>::infinity());

            for (const auto& candidate :
                candidates)
            {
                if (!candidate.screen.valid)
                {
                    continue;
                }
                const float left =
                    (candidate.screen.left
                        + 1.0f)
                    * 0.5f * gridWidth;
                const float right =
                    (candidate.screen.right
                        + 1.0f)
                    * 0.5f * gridWidth;
                const float top =
                    (1.0f
                        - candidate.screen.bottom)
                    * 0.5f * gridHeight;
                const float bottom =
                    (1.0f
                        - candidate.screen.top)
                    * 0.5f * gridHeight;
                const int minimumX = std::clamp(
                    static_cast<int>(
                        std::floor(left)),
                    0,
                    gridWidth - 1);
                const int maximumX = std::clamp(
                    static_cast<int>(
                        std::ceil(right)) - 1,
                    0,
                    gridWidth - 1);
                const int minimumY = std::clamp(
                    static_cast<int>(
                        std::floor(top)),
                    0,
                    gridHeight - 1);
                const int maximumY = std::clamp(
                    static_cast<int>(
                        std::ceil(bottom)) - 1,
                    0,
                    gridHeight - 1);

                bool covered = true;
                bool tested{};
                for (int y = minimumY;
                    y <= maximumY;
                    ++y)
                {
                    for (int x = minimumX;
                        x <= maximumX;
                        ++x)
                    {
                        tested = true;
                        if (depth[
                                y * gridWidth + x]
                            >= candidate.screen.
                                nearDepth - 0.001f)
                        {
                            covered = false;
                        }
                    }
                }
                if (tested && covered)
                {
                    result.renderHidden.insert(
                        candidate.object->Id());
                    ++result.stats.
                        occlusionCulledCount;
                    continue;
                }

                if (!candidate.occluder)
                {
                    continue;
                }
                for (int y = minimumY;
                    y <= maximumY;
                    ++y)
                {
                    for (int x = minimumX;
                        x <= maximumX;
                        ++x)
                    {
                        if (static_cast<float>(x)
                                + 0.001f >= left
                            && static_cast<float>(
                                x + 1)
                                - 0.001f <= right
                            && static_cast<float>(y)
                                + 0.001f >= top
                            && static_cast<float>(
                                y + 1)
                                - 0.001f <= bottom)
                        {
                            auto& cell = depth[
                                y * gridWidth + x];
                            cell = std::min(
                                cell,
                                candidate.screen.
                                    farDepth);
                        }
                    }
                }
            }
        }

        result.stats.visibleRendererCount =
            std::ranges::count_if(
                renderers,
                [&result](
                    const GameObject* object)
                {
                    return !result.renderHidden.
                        contains(object->Id());
                });
        for (const auto* object : renderers)
        {
            if (result.renderHidden.contains(object->Id()))
            {
                continue;
            }
            const auto* model = object->GetComponent<
                ModelRendererComponent>();
            if (model == nullptr)
            {
                continue;
            }
            const auto lodLevel = model->AutomaticLodLevel(
                view,
                projection);
            if (lodLevel == 0)
            {
                continue;
            }
            const auto fullTriangles =
                model->TriangleCount(0);
            const auto selectedTriangles =
                model->TriangleCount(lodLevel);
            if (selectedTriangles < fullTriangles)
            {
                ++result.stats.automaticLodRendererCount;
                result.stats.automaticLodTrianglesSaved +=
                    fullTriangles - selectedTriangles;
            }
        }
        return result;
    }

    std::unordered_set<GameObjectId>
        Scene::BuildShadowFrustumHidden(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const std::unordered_set<GameObjectId>&
                alreadyHidden) const
    {
        std::unordered_set<GameObjectId> hidden;
        if (!m_frustumCullingEnabled)
        {
            return hidden;
        }

        static_cast<void>(RefreshRenderSpatialIndex());
        const auto viewProjection = view * projection;
        const auto query = m_renderSpatialIndex.Query(
            [&viewProjection](const Bounds3D& bounds)
            {
                return !IsOutsideClipFrustum(bounds, viewProjection);
            });
        const auto& potentiallyVisible = query.candidates;
        hidden.reserve(m_renderSpatialIndex.Entries().size());
        for (std::size_t index = 0;
            index < m_renderSpatialIndex.Entries().size();
            ++index)
        {
            const auto& entry = m_renderSpatialIndex.Entries()[index];
            if (alreadyHidden.contains(entry.object->Id())
                || entry.object->IsAlwaysVisible())
            {
                continue;
            }
            if (potentiallyVisible[index] == 0u
                || IsOutsideClipFrustum(
                    entry.cullingBounds,
                    viewProjection))
            {
                hidden.insert(entry.object->Id());
            }
        }
        return hidden;
    }

    RenderVisibilityStats
        Scene::EvaluateRenderVisibility(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection)
    {
        m_visibilityStats =
            BuildRenderVisibility(
                view,
                projection).stats;
        return m_visibilityStats;
    }
}
