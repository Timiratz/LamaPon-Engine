#include "LamaPon/Scene/Scene.h"

#include "LamaPon/Core/JobSystem.h"
#include "LamaPon/Core/Profiler.h"

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
    // 階層と描画コンポーネントが有効か返します(object: 調べるオブジェクト)。
    bool IsRenderer(
        const LamaPon::GameObject& object) noexcept
    {
        // メッシュ描画コンポーネント
        const auto* mesh =
            object.GetComponent<
                LamaPon::MeshRendererComponent>();
        // モデル描画コンポーネント
        const auto* model =
            object.GetComponent<
                LamaPon::ModelRendererComponent>();
        return object.IsActiveInHierarchy()
            && ((mesh != nullptr
                    && mesh->IsEnabled())
                || (model != nullptr
                    && model->IsEnabled()));
    }

    // 有効なコライダーを優先してワールド境界を求めます(object: 描画対象)。
    // コライダーがなければ描画境界を使い、取得できなければローカル単位立方体を使います。
    LamaPon::Bounds3D RenderBounds(
        const LamaPon::GameObject& object) noexcept
    {
        // 箱形状のコライダー
        if (const auto* box =
                object.GetComponent<
                    LamaPon::BoxCollider3DComponent>();
            box != nullptr && box->IsEnabled())
        {
            return box->WorldBounds();
        }
        // カプセル形状のコライダー
        if (const auto* capsule =
                object.GetComponent<
                    LamaPon::CapsuleCollider3DComponent>();
            capsule != nullptr
                && capsule->IsEnabled())
        {
            return capsule->WorldBounds();
        }
        // 球形状のコライダー
        if (const auto* sphere =
                object.GetComponent<
                    LamaPon::SphereCollider3DComponent>();
            sphere != nullptr
                && sphere->IsEnabled())
        {
            return sphere->WorldBounds();
        }
        // 凸形状のコライダー
        if (const auto* hull =
                object.GetComponent<
                    LamaPon::ConvexHullCollider3DComponent>();
            hull != nullptr
                && hull->IsEnabled())
        {
            return hull->WorldBounds();
        }

        // 描画対象のローカル境界
        LamaPon::Bounds3D localBounds{
            { -0.5f, -0.5f, -0.5f },
            { 0.5f, 0.5f, 0.5f }
        };
        // メッシュの境界を取得した状態
        bool hasRendererBounds = false;
        // メッシュ描画コンポーネント
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
            // モデル描画コンポーネント
            if (const auto* model =
                object.GetComponent<
                    LamaPon::ModelRendererComponent>();
                model != nullptr)
            {
                static_cast<void>(
                    model->TryGetLocalBounds(localBounds));
            }
        }

        // 全頂点を囲むワールド境界
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
        // 対象のワールド行列
        const auto world = object.WorldMatrix();
        // 境界のX端を選ぶ符号
        for (int x = -1; x <= 1; x += 2)
        {
            // 境界のY端を選ぶ符号
            for (int y = -1; y <= 1; y += 2)
            {
                // 境界のZ端を選ぶ符号
                for (int z = -1; z <= 1; z += 2)
                {
                    // ワールドへ変換した境界頂点
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

    // 正の余白だけ各方向へ境界を広げます(bounds: 元の境界, margin: 境界に加える余白)。
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

    // 最小・最大座標の境界を中心と半径で表す箱へ変換します(bounds: 変換する境界)。
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

    // 境界を構成する八つの頂点を返します(bounds: 頂点を求める境界)。
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

    // 全頂点が同じクリップ面の外ならtrueを返します(bounds: ワールド境界, viewProjection: ビュー投影行列)。
    // 非有限な射影結果は可視候補として残します。
    bool IsOutsideClipFrustum(
        const LamaPon::Bounds3D& bounds,
        DirectX::FXMMATRIX viewProjection) noexcept
    {
        // 全頂点が各クリップ面外の状態
        std::array<bool, 6> allOutside{
            true, true, true, true, true, true
        };
        // 射影する境界の頂点
        for (const auto& corner : Corners(bounds))
        {
            // 射影後の同次座標
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
            // 各クリップ面の内外を表す距離
            const std::array distances{
                clip.x + clip.w,
                clip.w - clip.x,
                clip.y + clip.w,
                clip.w - clip.y,
                clip.z,
                clip.w - clip.z
            };
            // 内外を判定するクリップ面番号
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
        // 全頂点が外となった面の有無を返します(outside: 一つの面で全頂点が外の状態)。
        return std::ranges::any_of(
            allOutside,
            [](const bool outside)
            {
                return outside;
            });
    }

    struct ScreenBounds final
    {
        // 正規化画面の最小X座標
        float left{};
        // 正規化画面の最小Y座標
        float top{};
        // 正規化画面の最大X座標
        float right{};
        // 正規化画面の最大Y座標
        float bottom{};
        // 境界頂点の最小射影深度
        float nearDepth{ 1.0f };
        // 境界頂点の最大射影深度
        float farDepth{};
        // 射影した画面矩形の有効状態
        bool valid{};
    };

    // ワールド境界を正規化画面の矩形と深度へ射影します(bounds: ワールド境界, viewProjection: ビュー投影行列)。
    // 頂点がカメラの後ろか直近にある場合は無効な結果を返します。
    ScreenBounds ProjectBounds(
        const LamaPon::Bounds3D& bounds,
        DirectX::FXMMATRIX viewProjection) noexcept
    {
        // 境界の射影先となる画面矩形
        ScreenBounds result{
            1.0f,
            1.0f,
            -1.0f,
            -1.0f,
            1.0f,
            0.0f,
            false
        };
        // 射影する境界の頂点
        for (const auto& corner : Corners(bounds))
        {
            // 射影後の同次座標
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
            // 射影後の正規化画面X座標
            const float x = clip.x / clip.w;
            // 射影後の正規化画面Y座標
            const float y = clip.y / clip.w;
            // 正規化した射影深度
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
    // 有効な描画対象の境界を並列に求めて索引を更新し、再利用したか返します。
    bool Scene::RefreshRenderSpatialIndex() const
    {
        // 有効な描画対象の一覧
        std::vector<GameObject*> renderers;
        renderers.reserve(m_gameObjects.size());
        // 処理するオブジェクト
        for (const auto& object : m_gameObjects)
        {
            if (IsRenderer(*object))
            {
                renderers.push_back(object.get());
            }
        }

        // 次の索引に登録する描画候補
        std::vector<RenderSpatialIndex::Entry> next(renderers.size());
        // 候補区間の境界と判定結果を計算します(begin: 開始添字, end: 終端の次の添字)。
        JobSystem::Instance().ParallelFor(
            renderers.size(),
            64,
            [&renderers, &next](
                const std::size_t begin,
                const std::size_t end)
            {
                // 処理する描画対象の添字
                for (std::size_t index = begin;
                    index < end;
                    ++index)
                {
                    // 処理するオブジェクト
                    auto* const object = renderers[index];
                    // 対象のワールド境界
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

    // LOD・視錐台・遮蔽による非表示対象と統計を求めます(view: 可逆なビュー行列, projection: 投影行列)。
        // 遮蔽判定は画面境界の粗い深度格子で近似し、実際の形状の隙間までは判定しません。
    Scene::VisibilityResult
        Scene::BuildRenderVisibility(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) const
    {
        LAMAPON_PROFILE_SCOPE("Visibility");
        // カリングで非表示にする対象と統計
        VisibilityResult result;
        // 描画候補の索引を再利用した状態
        const bool spatialIndexReused =
            RefreshRenderSpatialIndex();
        // 有効な描画対象の一覧
        std::vector<GameObject*> renderers;
        renderers.reserve(m_renderSpatialIndex.Entries().size());
        // 索引に登録された描画候補
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

        // ビュー行列の行列式
        DirectX::XMVECTOR determinant{};
        // ビュー行列の逆行列
        const auto inverseView =
            DirectX::XMMatrixInverse(
                &determinant,
                view);
        // カメラのワールド位置
        DirectX::XMFLOAT3 cameraPosition{};
        DirectX::XMStoreFloat3(
            &cameraPosition,
            inverseView.r[3]);

        // 対象番号ごとのLOD可視設定
        std::unordered_map<GameObjectId, bool>
            lodVisibility;
        // 処理するオブジェクト
        for (const auto& object : m_gameObjects)
        {
            // LOD選択コンポーネント
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
            // LODグループのワールド位置
            DirectX::XMFLOAT3 groupPosition{};
            DirectX::XMStoreFloat3(
                &groupPosition,
                object->WorldMatrix().r[3]);
            // カメラからグループへのX差分
            const float deltaX =
                groupPosition.x
                - cameraPosition.x;
            // カメラからグループへのY差分
            const float deltaY =
                groupPosition.y
                - cameraPosition.y;
            // カメラからグループへのZ差分
            const float deltaZ =
                groupPosition.z
                - cameraPosition.z;
            // カメラとLODグループの距離
            const float distance = std::sqrt(
                deltaX * deltaX
                + deltaY * deltaY
                + deltaZ * deltaZ);
            // 距離から選んだLOD対象番号
            const auto selected =
                group->SelectTarget(distance);
            // 照合するLOD段階の設定
            for (const auto& level :
                group->Levels())
            {
                // 段階の対象が選択された状態
                const bool visible =
                    level.targetId != 0
                    && level.targetId == selected;
                // 対象の既存LOD可視設定
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
        // LOD可視設定(id: 対象番号, visible: 全グループが表示する状態)。
        for (const auto& [id, visible] :
            lodVisibility)
        {
            if (!visible)
            {
                result.lodHidden.insert(id);
                result.renderHidden.insert(id);
                // 処理するオブジェクト
                if (const auto* object =
                        FindGameObject(id);
                    object != nullptr
                        && IsRenderer(*object))
                {
                    ++result.stats.lodCulledCount;
                }
            }
        }

        // ワールド座標の視錐台
        DirectX::BoundingFrustum worldFrustum;
        if (m_frustumCullingEnabled)
        {
            // ビュー座標の投影視錐台
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

        // 索引順の視錐台候補フラグ
        std::vector<unsigned char> spatialVisible;
        if (m_frustumCullingEnabled)
        {
            // 境界がワールド視錐台に触れるか返します(bounds: ノードのワールド境界)。
            // 空間索引で絞った候補と統計
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
            // 非所有の描画対象
            GameObject* object{};
            // 描画対象のワールド境界
            Bounds3D bounds{};
            // 射影した画面矩形と深度
            ScreenBounds screen{};
            // 他の候補を遮蔽する指定
            bool occluder{};
        };
        // 遮蔽を判定する描画候補
        std::vector<Candidate> candidates;
        // ビューと投影を合成した行列
        const auto viewProjection =
            view * projection;

        // 並列区間は別々の結果スロットへ書き、元の順序で統計と候補を集約します。
        struct CandidateSlot final
        {
            // 並列区間で求めた描画候補
            Candidate candidate{};
            // 視錐台の外による非表示
            bool frustumCulled{};
            // LODか常時表示による除外
            bool skipped{};
        };
        // 対象ごとの並列計算結果
        std::vector<CandidateSlot> slots(
            renderers.size());
        // 候補区間の境界と判定結果を計算します(begin: 開始添字, end: 終端の次の添字)。
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
                // 処理する描画対象の添字
                for (std::size_t index = begin;
                    index < end;
                    ++index)
                {
                    // 処理するオブジェクト
                    auto* object = renderers[index];
                    // 並列区間の描画判定結果
                    auto& slot = slots[index];
                    // この並列区間ではrenderHiddenを変更せずLOD可視設定を照会します。
                    if (result.renderHidden.contains(
                            object->Id()))
                    {
                        slot.skipped = true;
                        continue;
                    }
                    // 常時表示を指定した対象は視錐台と遮蔽の判定を省きます。
                    if (object->IsAlwaysVisible())
                    {
                        slot.skipped = true;
                        continue;
                    }
                    // 対象のワールド境界
                    const auto& bounds =
                        m_renderSpatialIndex.Entries()[index].bounds;
                    // 余白を加えたカリング境界
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
                    // モデル描画コンポーネント
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
        // 処理する描画対象の添字
        for (std::size_t index = 0;
            index < renderers.size();
            ++index)
        {
            // 並列区間の描画判定結果
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
            // 画面境界の手前深度を比較します(left: 一方の候補, right: もう一方の候補)。
            std::ranges::sort(
                candidates,
                [](const Candidate& left,
                    const Candidate& right)
                {
                    return left.screen.nearDepth
                        < right.screen.nearDepth;
                });
            // 遮蔽格子の横セル数
            constexpr int gridWidth = 32;
            // 遮蔽格子の縦セル数
            constexpr int gridHeight = 18;
            // 遮蔽に使う各セルの奥側深度
            std::array<
                float,
                gridWidth * gridHeight> depth;
            depth.fill(
                std::numeric_limits<float>::infinity());

            // 遮蔽を判定する描画候補
            for (const auto& candidate :
                candidates)
            {
                if (!candidate.screen.valid)
                {
                    continue;
                }
                // 遮蔽格子上の矩形の左端
                const float left =
                    (candidate.screen.left
                        + 1.0f)
                    * 0.5f * gridWidth;
                // 遮蔽格子上の矩形の右端
                const float right =
                    (candidate.screen.right
                        + 1.0f)
                    * 0.5f * gridWidth;
                // 遮蔽格子上の矩形の上端
                const float top =
                    (1.0f
                        - candidate.screen.bottom)
                    * 0.5f * gridHeight;
                // 遮蔽格子上の矩形の下端
                const float bottom =
                    (1.0f
                        - candidate.screen.top)
                    * 0.5f * gridHeight;
                // 判定矩形の最小Xセル番号
                const int minimumX = std::clamp(
                    static_cast<int>(
                        std::floor(left)),
                    0,
                    gridWidth - 1);
                // 判定矩形の最大Xセル番号
                const int maximumX = std::clamp(
                    static_cast<int>(
                        std::ceil(right)) - 1,
                    0,
                    gridWidth - 1);
                // 判定矩形の最小Yセル番号
                const int minimumY = std::clamp(
                    static_cast<int>(
                        std::floor(top)),
                    0,
                    gridHeight - 1);
                // 判定矩形の最大Yセル番号
                const int maximumY = std::clamp(
                    static_cast<int>(
                        std::ceil(bottom)) - 1,
                    0,
                    gridHeight - 1);

                // 矩形全体が深度格子で遮蔽する状態
                bool covered = true;
                // 遮蔽格子を一つ以上判定した状態
                bool tested{};
                // 遮蔽格子のYセル番号
                for (int y = minimumY;
                    y <= maximumY;
                    ++y)
                {
                    // 遮蔽格子のXセル番号
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
                // 遮蔽格子のYセル番号
                for (int y = minimumY;
                    y <= maximumY;
                    ++y)
                {
                    // 遮蔽格子のXセル番号
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
                            // 更新する格子セルの遮蔽深度
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

        // 非表示でない描画対象を数えます(object: 数える描画対象)。
        result.stats.visibleRendererCount =
            std::ranges::count_if(
                renderers,
                [&result](
                    const GameObject* object)
                {
                    return !result.renderHidden.
                        contains(object->Id());
                });
        // 処理するオブジェクト
        for (const auto* object : renderers)
        {
            if (result.renderHidden.contains(object->Id()))
            {
                continue;
            }
            // モデル描画コンポーネント
            const auto* model = object->GetComponent<
                ModelRendererComponent>();
            if (model == nullptr)
            {
                continue;
            }
            // モデルが自動選択したLOD段階
            const auto lodLevel = model->AutomaticLodLevel(
                view,
                projection);
            if (lodLevel == 0)
            {
                continue;
            }
            // 最高詳細段階の三角形数
            const auto fullTriangles =
                model->TriangleCount(0);
            // 選択したLOD段階の三角形数
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

    // 影の視錐台から外れる描画対象を収集します(view: 影のビュー行列, projection: 影の投影行列, alreadyHidden: 既に非表示の対象)。
    std::unordered_set<GameObjectId>
        Scene::BuildShadowFrustumHidden(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const std::unordered_set<GameObjectId>&
                alreadyHidden) const
    {
        // 影の視錐台外のオブジェクト番号
        std::unordered_set<GameObjectId> hidden;
        if (!m_frustumCullingEnabled)
        {
            return hidden;
        }

        static_cast<void>(RefreshRenderSpatialIndex());
        // ビューと投影を合成した行列
        const auto viewProjection = view * projection;
        // 境界が影のクリップ範囲と交わるか返します(bounds: ノードのワールド境界)。
        // 空間索引で絞った候補と統計
        const auto query = m_renderSpatialIndex.Query(
            [&viewProjection](const Bounds3D& bounds)
            {
                return !IsOutsideClipFrustum(bounds, viewProjection);
            });
        // 索引順の影の視錐台候補
        const auto& potentiallyVisible = query.candidates;
        hidden.reserve(m_renderSpatialIndex.Entries().size());
        // 処理する描画対象の添字
        for (std::size_t index = 0;
            index < m_renderSpatialIndex.Entries().size();
            ++index)
        {
            // 索引に登録された描画候補
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

    // 描画対象の可視判定を実行し最新の統計を保存して返します(view: 可逆なビュー行列, projection: 投影行列)。
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
