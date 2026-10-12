#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Reactive/Reactive.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/CharacterControllerComponent.h"
#include "LamaPon/Components/AudioListenerComponent.h"
#include "LamaPon/Components/AudioSourceComponent.h"
#include "LamaPon/Components/BoxCollider2DComponent.h"
#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/InputMoverComponent.h"
#include "LamaPon/Components/JointComponent.h"
#include "LamaPon/Components/LODGroupComponent.h"
#include "LamaPon/Components/PointLightComponent.h"
#include "LamaPon/Components/SpotLightComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/NavMeshAgentComponent.h"
#include "LamaPon/Components/NavMeshComponent.h"
#include "LamaPon/Components/NativeScriptComponent.h"
#include "LamaPon/Components/NetworkIdentityComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Components/CircleCollider2DComponent.h"
#include "LamaPon/Components/PolygonCollider2DComponent.h"
#include "LamaPon/Components/Light2DComponent.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Components/RenderCullingComponent.h"
#include "LamaPon/Components/SpriteMaskComponent.h"
#include "LamaPon/Components/ParallaxLayerComponent.h"
#include "LamaPon/Components/MeshCollider3DComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Components/UIInputFieldComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/UISliderComponent.h"
#include "LamaPon/Components/UIToggleComponent.h"
#include "LamaPon/Components/BillboardComponent.h"
#include "LamaPon/Components/RotatorComponent.h"
#include "LamaPon/Components/RigidbodyComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Components/SpriteAnimatorComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/Sway2DComponent.h"
#include "LamaPon/Components/Blink2DComponent.h"
#include "LamaPon/Components/SpriteSkin2DComponent.h"
#include "LamaPon/Components/Rig2DComponent.h"
#include "LamaPon/Components/Keyform2DComponent.h"
#include "LamaPon/Components/TextRendererComponent.h"
#include "LamaPon/Components/TilemapComponent.h"
#include "LamaPon/Components/TransformAnimatorComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Graphics/FrameDebugger.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/EnvironmentCache.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/TemporalJitter.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/SpriteEffect.h"
#include "LamaPon/Physics/SpatialHashBroadPhase.h"
#include "LamaPon/Physics/Collision3D.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Physics/PhysicsSettings.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/SceneManager.h"

#include <algorithm>
#include <array>
#include <DirectXPackedVector.h>

#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace
{
    class BooleanStateScope final
    {
    public:
        // 真偽値を変更し復元用に元の値を保存します(target: 借用する真偽値, value: 設定する値)。
        BooleanStateScope(
            bool& target,
            const bool value) noexcept
            : m_target(target)
            , m_previous(target)
        {
            m_target = value;
        }

        // 借用した真偽値を元へ戻します。
        ~BooleanStateScope()
        {
            Restore();
        }

        // 保存した状態を一度だけ戻します。
        void Restore() noexcept
        {
            if (!m_active)
            {
                return;
            }
            m_target = m_previous;
            m_active = false;
        }

        // 借用する復元責務のコピーを禁止します。
        BooleanStateScope(
            const BooleanStateScope&) = delete;
        // 借用する復元責務のコピー代入を禁止します。
        BooleanStateScope& operator=(
            const BooleanStateScope&) = delete;

    private:
        // 借用する変更対象の真偽値
        bool& m_target;
        // 変更前の真偽値
        bool m_previous;
        // 元の値への復元を待つ状態
        bool m_active{ true };
    };

    class GraphicsOutputStateScope final
    {
    public:
        // 描画機器の出力状態を保存します(graphics: 借用する描画機器)。
        explicit GraphicsOutputStateScope(
            LamaPon::GraphicsDevice& graphics)
            : m_graphics(graphics)
            , m_state(graphics.CaptureOutputState())
        {
        }

        // 保存した描画出力状態を戻し、復元失敗の例外を外へ送出しません。
        ~GraphicsOutputStateScope() noexcept
        {
            try
            {
                Restore();
            }
            catch (...)
            {
                // 例外処理中は元の描画失敗を優先します。
            }
        }

        // 保存した描画出力状態を一度だけ戻します。
        void Restore()
        {
            if (!m_state)
            {
                return;
            }
            m_graphics.RestoreOutputState(*m_state);
            m_state.reset();
        }

        // 借用する復元責務のコピーを禁止します。
        GraphicsOutputStateScope(
            const GraphicsOutputStateScope&) = delete;
        // 借用する復元責務のコピー代入を禁止します。
        GraphicsOutputStateScope& operator=(
            const GraphicsOutputStateScope&) = delete;

    private:
        // 借用する描画機器
        LamaPon::GraphicsDevice& m_graphics;
        // 復元する描画出力状態
        std::unique_ptr<LamaPon::GraphicsOutputState> m_state;
    };

    class UIViewportSizeScope final
    {
    public:
        // 描画機器のUI寸法を保存します(graphics: 借用する描画機器)。
        explicit UIViewportSizeScope(
            LamaPon::GraphicsDevice& graphics) noexcept
            : m_graphics(graphics)
            , m_width(graphics.UIWidth())
            , m_height(graphics.UIHeight())
        {
        }

        // 借用した描画機器のUI表示寸法を元へ戻します。
        ~UIViewportSizeScope()
        {
            Restore();
        }

        // 保存した状態を一度だけ戻します。
        void Restore() noexcept
        {
            if (!m_active)
            {
                return;
            }
            m_graphics.SetUIViewportSize(m_width, m_height);
            m_active = false;
        }

        // 借用する復元責務のコピーを禁止します。
        UIViewportSizeScope(const UIViewportSizeScope&) = delete;
        // 借用する復元責務のコピー代入を禁止します。
        UIViewportSizeScope& operator=(
            const UIViewportSizeScope&) = delete;

    private:
        // 借用する描画機器
        LamaPon::GraphicsDevice& m_graphics;
        // 変更前のUI表示幅
        std::uint32_t m_width{};
        // 変更前のUI表示高さ
        std::uint32_t m_height{};
        // 元の寸法への復元を待つ状態
        bool m_active{ true };
    };

    struct Contact final
    {
        // 第1形状へ通知する接触法線
        DirectX::XMFLOAT3 normal;
        // 形状同士のめり込み量
        float penetration;
        // 通知する代表接触点
        DirectX::XMFLOAT3 point{};
        // 最大4個の接触点
        std::array<DirectX::XMFLOAT3, 4> points{};
        // 有効な接触点の数
        std::size_t pointCount{};
    };

    // 2Dの箱の貫入を判定し最大2点の接触を返します(left: 第1境界, right: 第2境界)。
    std::optional<Contact> IntersectBounds(
        const LamaPon::Bounds2D& left,
        const LamaPon::Bounds2D& right)
    {
        // X方向の重なり幅
        const float overlapX = std::min(left.maximum.x, right.maximum.x)
            - std::max(left.minimum.x, right.minimum.x);
        // Y方向の重なり幅
        const float overlapY = std::min(left.maximum.y, right.maximum.y)
            - std::max(left.minimum.y, right.minimum.y);
        if (overlapX <= 0.0f || overlapY <= 0.0f)
        {
            return std::nullopt;
        }

        // 第1境界の中心X座標
        const float centerLeftX = (left.minimum.x + left.maximum.x) * 0.5f;
        // 第1境界の中心Y座標
        const float centerLeftY = (left.minimum.y + left.maximum.y) * 0.5f;
        // 第2境界の中心X座標
        const float centerRightX = (right.minimum.x + right.maximum.x) * 0.5f;
        // 第2境界の中心Y座標
        const float centerRightY = (right.minimum.y + right.maximum.y) * 0.5f;

        if (overlapX < overlapY)
        {
            // 接触線のX座標
            const float contactX =
                (std::max(
                    left.minimum.x,
                    right.minimum.x)
                    + std::min(
                        left.maximum.x,
                        right.maximum.x))
                * 0.5f;
            // 接触線の最小Y座標
            const float minimumY =
                std::max(
                    left.minimum.y,
                    right.minimum.y);
            // 接触線の最大Y座標
            const float maximumY =
                std::min(
                    left.maximum.y,
                    right.maximum.y);
            return Contact{
                {
                    centerLeftX < centerRightX ? -1.0f : 1.0f,
                    0.0f,
                    0.0f
                },
                overlapX,
                {
                    contactX,
                    (minimumY + maximumY) * 0.5f,
                    0.0f
                },
                {
                    DirectX::XMFLOAT3{
                        contactX,
                        minimumY,
                        0.0f },
                    DirectX::XMFLOAT3{
                        contactX,
                        maximumY,
                        0.0f }
                },
                2
            };
        }

        // 接触線のY座標
        const float contactY =
            (std::max(
                left.minimum.y,
                right.minimum.y)
                + std::min(
                    left.maximum.y,
                    right.maximum.y))
            * 0.5f;
        // 接触線の最小X座標
        const float minimumX =
            std::max(
                left.minimum.x,
                right.minimum.x);
        // 接触線の最大X座標
        const float maximumX =
            std::min(
                left.maximum.x,
                right.maximum.x);
        return Contact{
            {
                0.0f,
                centerLeftY < centerRightY ? -1.0f : 1.0f,
                0.0f
            },
            overlapY,
            {
                (minimumX + maximumX) * 0.5f,
                contactY,
                0.0f
            },
            {
                DirectX::XMFLOAT3{
                    minimumX,
                    contactY,
                    0.0f },
                DirectX::XMFLOAT3{
                    maximumX,
                    contactY,
                    0.0f }
            },
            2
        };
    }

    // 円の貫入を判定し第1円側の法線で接触を返します(left: 第1円, right: 第2円)。
    std::optional<Contact> IntersectCircles(
        const LamaPon::Circle2D& left,
        const LamaPon::Circle2D& right)
    {
        // 第2円から第1円へのX差分
        const float deltaX =
            left.center.x - right.center.x;
        // 第2円から第1円へのY差分
        const float deltaY =
            left.center.y - right.center.y;
        // 二つの円の半径の合計
        const float radiusSum =
            left.radius + right.radius;
        // 最近接位置間の距離の二乗
        const float squaredDistance =
            deltaX * deltaX + deltaY * deltaY;
        if (squaredDistance >= radiusSum * radiusSum)
        {
            return std::nullopt;
        }
        // 最近接位置間の距離
        const float distance =
            std::sqrt(squaredDistance);
        // 第1形状側へ向く接触法線
        DirectX::XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        if (distance > 0.0001f)
        {
            normal = {
                deltaX / distance,
                deltaY / distance,
                0.0f };
        }
        // 形状同士のめり込み量
        const float penetration = radiusSum - distance;
        // 通知する代表接触点
        const DirectX::XMFLOAT3 point{
            right.center.x
                + normal.x
                    * (right.radius
                        - penetration * 0.5f),
            right.center.y
                + normal.y
                    * (right.radius
                        - penetration * 0.5f),
            0.0f };
        // 法線と接触点の判定結果
        Contact contact{
            normal,
            penetration,
            point };
        contact.points[0] = point;
        contact.pointCount = 1;
        return contact;
    }

    // 円と箱の貫入を判定し円側の法線で接触を返します(circle: 円形状, bounds: 箱の境界)。
    std::optional<Contact> IntersectCircleBounds(
        const LamaPon::Circle2D& circle,
        const LamaPon::Bounds2D& bounds)
    {
        // 箱から円中心に最も近いX座標
        const float closestX = std::clamp(
            circle.center.x,
            bounds.minimum.x,
            bounds.maximum.x);
        // 箱から円中心に最も近いY座標
        const float closestY = std::clamp(
            circle.center.y,
            bounds.minimum.y,
            bounds.maximum.y);
        // 最近接点から円中心へのX差分
        const float deltaX = circle.center.x - closestX;
        // 最近接点から円中心へのY差分
        const float deltaY = circle.center.y - closestY;
        // 最近接位置間の距離の二乗
        const float squaredDistance =
            deltaX * deltaX + deltaY * deltaY;
        if (squaredDistance
            >= circle.radius * circle.radius)
        {
            return std::nullopt;
        }

        // 第1形状側へ向く接触法線
        DirectX::XMFLOAT3 normal{};
        // 形状同士のめり込み量
        float penetration{};
        if (squaredDistance > 0.0000001f)
        {
            // 円の中心がボックスの外側にあるケース。
            // 最近接位置間の距離
            const float distance =
                std::sqrt(squaredDistance);
            normal = {
                deltaX / distance,
                deltaY / distance,
                0.0f };
            penetration = circle.radius - distance;
        }
        else
        {
            // 中心がボックス内部：最も近い面へ押し出します。
            // 円中心から箱の左面への距離
            const float toLeft =
                circle.center.x - bounds.minimum.x;
            // 円中心から箱の右面への距離
            const float toRight =
                bounds.maximum.x - circle.center.x;
            // 円中心から箱の下面への距離
            const float toBottom =
                circle.center.y - bounds.minimum.y;
            // 円中心から箱の上面への距離
            const float toTop =
                bounds.maximum.y - circle.center.y;
            // 内側の円中心に最も近い面の距離
            const float smallest = std::min(
                { toLeft, toRight, toBottom, toTop });
            if (smallest == toLeft)
            {
                normal = { -1.0f, 0.0f, 0.0f };
            }
            else if (smallest == toRight)
            {
                normal = { 1.0f, 0.0f, 0.0f };
            }
            else if (smallest == toBottom)
            {
                normal = { 0.0f, -1.0f, 0.0f };
            }
            else
            {
                normal = { 0.0f, 1.0f, 0.0f };
            }
            penetration = circle.radius + smallest;
        }
        // 通知する代表接触点
        const DirectX::XMFLOAT3 point{
            closestX,
            closestY,
            0.0f };
        // 法線と接触点の判定結果
        Contact contact{
            normal,
            penetration,
            point };
        contact.points[0] = point;
        contact.pointCount = 1;
        return contact;
    }

    // 頂点の座標平均を返し空の一覧なら原点を返します(vertices: 平均する頂点)。
    DirectX::XMFLOAT2 PolygonCentroid(
        const std::vector<DirectX::XMFLOAT2>& vertices)
    {
        // 全頂点の座標平均
        DirectX::XMFLOAT2 centroid{ 0.0f, 0.0f };
        // 平均へ加える頂点
        for (const auto& vertex : vertices)
        {
            centroid.x += vertex.x;
            centroid.y += vertex.y;
        }
        // 除算に使う1以上の頂点数
        const float count = static_cast<float>(
            std::max<std::size_t>(vertices.size(), 1));
        centroid.x /= count;
        centroid.y /= count;
        return centroid;
    }

    // 参照面と相手頂点の最小符号距離を返します(axis: 参照面の外向き法線, facePoint: 参照面上の点, otherVertices: 相手の頂点)。
    float FaceSeparation(
        const DirectX::XMFLOAT2& axis,
        const DirectX::XMFLOAT2& facePoint,
        const std::vector<DirectX::XMFLOAT2>& otherVertices)
    {
        // 相手頂点の最小符号付き距離
        float minimum = std::numeric_limits<float>::max();
        // 参照面との距離を測る頂点
        for (const auto& vertex : otherVertices)
        {
            // 参照面からの符号付き距離
            const float distance =
                axis.x * (vertex.x - facePoint.x)
                    + axis.y * (vertex.y - facePoint.y);
            minimum = std::min(minimum, distance);
        }
        return minimum;
    }

    // 2Dの箱を反時計回りの四頂点に変換します(bounds: 箱の境界)。
    std::vector<DirectX::XMFLOAT2> BoxBoundsToPolygon(
        const LamaPon::Bounds2D& bounds)
    {
        return {
            { bounds.minimum.x, bounds.minimum.y },
            { bounds.maximum.x, bounds.minimum.y },
            { bounds.maximum.x, bounds.maximum.y },
            { bounds.minimum.x, bounds.maximum.y }
        };
    }

    // 頂点の巻き順によらず、頂点平均から外へ向く辺の法線を持つ分離軸候補です。
    struct PolygonAxis final
    {
        // 辺の外向き単位法線
        DirectX::XMFLOAT2 axis;
        // 辺の始点となる頂点番号
        std::size_t edgeStart;
        // 第1多角形由来の指定
        bool fromLeft;
    };

    // 辺の外向き単位法線を列挙します(vertices: 多角形の頂点, fromLeft: 第1形状由来の指定)。
    std::vector<PolygonAxis> PolygonAxisCandidates(
        const std::vector<DirectX::XMFLOAT2>& vertices,
        const bool fromLeft)
    {
        // 辺から求めた分離軸の候補
        std::vector<PolygonAxis> axes;
        if (vertices.size() < 2)
        {
            return axes;
        }
        // 外向きを判定する頂点平均
        const auto centroid = PolygonCentroid(vertices);
        axes.reserve(vertices.size());
        // 分離軸を求める辺の始点番号
        for (std::size_t index{}; index < vertices.size(); ++index)
        {
            // 辺の始点
            const auto& a = vertices[index];
            // 辺の終点
            const auto& b = vertices[(index + 1) % vertices.size()];
            // 辺のX方向の差分
            const float edgeX = b.x - a.x;
            // 辺のY方向の差分
            const float edgeY = b.y - a.y;
            // 辺の長さ
            const float length =
                std::sqrt(edgeX * edgeX + edgeY * edgeY);
            if (length <= 0.0001f)
            {
                continue;
            }
            // 正規化した辺の外向き法線
            DirectX::XMFLOAT2 normal{ edgeY / length, -edgeX / length };
            // 辺の中点
            const DirectX::XMFLOAT2 midpoint{
                (a.x + b.x) * 0.5f,
                (a.y + b.y) * 0.5f };
            // 法線が頂点平均へ向く内積
            const float towardCentroid =
                (centroid.x - midpoint.x) * normal.x
                    + (centroid.y - midpoint.y) * normal.y;
            if (towardCentroid > 0.0f)
            {
                normal.x = -normal.x;
                normal.y = -normal.y;
            }
            axes.push_back({ normal, index, fromLeft });
        }
        return axes;
    }

    // 半平面内に残る0〜2頂点を出力します(a: 線分始点, b: 線分終点, direction: 半平面の内向き法線, offset: 法線方向の面位置, output: 2頂点の出力先)。
    std::size_t ClipSegment(
        const DirectX::XMFLOAT2& a,
        const DirectX::XMFLOAT2& b,
        const DirectX::XMFLOAT2& direction,
        const float offset,
        std::array<DirectX::XMFLOAT2, 2>& output)
    {
        // クリップ後の頂点数
        std::size_t count{};
        // 始点の半平面からの符号距離
        const float distanceA =
            direction.x * a.x + direction.y * a.y - offset;
        // 終点の半平面からの符号距離
        const float distanceB =
            direction.x * b.x + direction.y * b.y - offset;
        if (distanceA >= 0.0f)
        {
            output[count++] = a;
        }
        if (distanceB >= 0.0f)
        {
            output[count++] = b;
        }
        if (distanceA * distanceB < 0.0f && count < 2)
        {
            // 交点の線分内の補間率
            const float t = distanceA / (distanceA - distanceB);
            output[count++] = {
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t
            };
        }
        return count;
    }

    // 凸多角形のSAT判定と辺のクリップで最大2点の接触を返します(left: 第1多角形の頂点, right: 第2多角形の頂点)。
    // 各形状は3頂点以上で、法線は第1形状側へ向けます。
    std::optional<Contact> IntersectPolygons(
        const std::vector<DirectX::XMFLOAT2>& left,
        const std::vector<DirectX::XMFLOAT2>& right)
    {
        if (left.size() < 3 || right.size() < 3)
        {
            return std::nullopt;
        }

        // 両多角形の分離軸候補
        auto axes = PolygonAxisCandidates(left, true);
        // 第2多角形の分離軸候補
        const auto rightAxes = PolygonAxisCandidates(right, false);
        axes.insert(axes.end(), rightAxes.begin(), rightAxes.end());
        if (axes.empty())
        {
            return std::nullopt;
        }

        // 0以下で最大の符号付き分離量
        float bestSeparation = std::numeric_limits<float>::lowest();
        // 参照面に選ぶ分離軸の添字
        std::size_t bestIndex{};
        // 採用できる分離軸を得た状態
        bool found{};
        // 分離軸または接触点の添字
        for (std::size_t index{}; index < axes.size(); ++index)
        {
            // 比較する分離軸の候補
            const auto& candidate = axes[index];
            // 参照面を持つ多角形
            const auto& facePolygon = candidate.fromLeft ? left : right;
            // 参照面と比較する相手多角形
            const auto& otherPolygon = candidate.fromLeft ? right : left;
            // 参照面からの符号付き分離量
            const float separation = FaceSeparation(
                candidate.axis,
                facePolygon[candidate.edgeStart],
                otherPolygon);
            if (separation > 0.0f)
            {
                // 分離軸が見つかったため接触していません。
                return std::nullopt;
            }
            if (separation > bestSeparation)
            {
                bestSeparation = separation;
                bestIndex = index;
                found = true;
            }
        }
        if (!found)
        {
            return std::nullopt;
        }

        // 参照面に選んだ分離軸
        const auto winner = axes[bestIndex];
        // クリップの参照辺を持つ多角形
        const auto& referencePolygon = winner.fromLeft ? left : right;
        // クリップする入射辺の多角形
        const auto& incidentPolygon = winner.fromLeft ? right : left;

        // 参照辺の始点
        const auto referenceA = referencePolygon[winner.edgeStart];
        // 参照辺の終点
        const auto referenceB = referencePolygon[
            (winner.edgeStart + 1) % referencePolygon.size()];

        // 入射エッジ：分離軸と最も逆向き（内積が最小）の辺を選びます。
        // 入射辺を選ぶ法線候補
        const auto incidentAxes =
            PolygonAxisCandidates(incidentPolygon, winner.fromLeft);
        // 選んだ入射辺の始点番号
        std::size_t incidentStart{};
        // 参照法線と最も逆向きの内積
        float lowestDot = std::numeric_limits<float>::max();
        // 比較する分離軸の候補
        for (const auto& candidate : incidentAxes)
        {
            // 参照法線と候補法線の内積
            const float dot =
                candidate.axis.x * winner.axis.x
                    + candidate.axis.y * winner.axis.y;
            if (dot < lowestDot)
            {
                lowestDot = dot;
                incidentStart = candidate.edgeStart;
            }
        }
        // 入射辺の始点
        const auto incidentA = incidentPolygon[incidentStart];
        // 入射辺の終点
        const auto incidentB = incidentPolygon[
            (incidentStart + 1) % incidentPolygon.size()];

        // 参照辺のX方向の差分
        const float referenceEdgeX = referenceB.x - referenceA.x;
        // 参照辺のY方向の差分
        const float referenceEdgeY = referenceB.y - referenceA.y;
        // 参照辺の長さ
        const float referenceLength = std::sqrt(
            referenceEdgeX * referenceEdgeX
                + referenceEdgeY * referenceEdgeY);
        if (referenceLength <= 0.0001f)
        {
            return std::nullopt;
        }
        // 参照辺の単位接線
        const DirectX::XMFLOAT2 referenceDirection{
            referenceEdgeX / referenceLength,
            referenceEdgeY / referenceLength };
        // 参照辺の逆向き単位接線
        const DirectX::XMFLOAT2 negativeDirection{
            -referenceDirection.x, -referenceDirection.y };

        // 参照辺の始点側で切った頂点
        std::array<DirectX::XMFLOAT2, 2> stage{};
        // 最初のクリップで残った頂点数
        const std::size_t stageCount = ClipSegment(
            incidentA,
            incidentB,
            referenceDirection,
            referenceDirection.x * referenceA.x
                + referenceDirection.y * referenceA.y,
            stage);
        if (stageCount < 2)
        {
            return std::nullopt;
        }
        // 参照辺の終点側でも切った頂点
        std::array<DirectX::XMFLOAT2, 2> clipped{};
        // 両側のクリップで残った頂点数
        const std::size_t clippedCount = ClipSegment(
            stage[0],
            stage[1],
            negativeDirection,
            negativeDirection.x * referenceB.x
                + negativeDirection.y * referenceB.y,
            clipped);
        if (clippedCount < 2)
        {
            return std::nullopt;
        }

        // 参照面の法線方向の位置
        const float referenceOffset =
            winner.axis.x * referenceA.x + winner.axis.y * referenceA.y;

        // 法線と接触点の判定結果
        Contact contact{};
        contact.pointCount = 0;
        // 採用する接触点の貫入量合計
        float totalPenetration{};
        // 採用を調べるクリップ後の頂点
        for (const auto& point : clipped)
        {
            // 参照面からの符号付き分離量
            const float separation =
                winner.axis.x * point.x + winner.axis.y * point.y
                    - referenceOffset;
            if (separation > 0.0f)
            {
                // 参照面より外側（めり込んでいない）は除外します。
                continue;
            }
            contact.points[contact.pointCount++] =
                { point.x, point.y, 0.0f };
            totalPenetration += -separation;
        }
        if (contact.pointCount == 0)
        {
            return std::nullopt;
        }
        contact.penetration = std::max(
            totalPenetration
                / static_cast<float>(contact.pointCount),
            0.0001f);

        // 法線は他の2D関数と同じ規約（left側を向く）へ補正します。
        // 第1多角形の頂点平均
        const auto leftCentroid = PolygonCentroid(left);
        // 第2多角形の頂点平均
        const auto rightCentroid = PolygonCentroid(right);
        // 第1形状側へ向く接触法線
        DirectX::XMFLOAT2 normal = winner.axis;
        // 第1形状へ法線が向く内積
        const float towardLeft =
            normal.x * (leftCentroid.x - rightCentroid.x)
                + normal.y * (leftCentroid.y - rightCentroid.y);
        if (towardLeft < 0.0f)
        {
            normal.x = -normal.x;
            normal.y = -normal.y;
        }
        contact.normal = { normal.x, normal.y, 0.0f };

        // 接触点の座標平均
        DirectX::XMFLOAT3 average{};
        // 分離軸または接触点の添字
        for (std::size_t index{}; index < contact.pointCount; ++index)
        {
            average.x += contact.points[index].x;
            average.y += contact.points[index].y;
        }
        // 平均の除算に使う接触数の逆数
        const float scale =
            1.0f / static_cast<float>(contact.pointCount);
        average.x *= scale;
        average.y *= scale;
        contact.point = average;

        return contact;
    }

    // 凸多角形の辺と円を判定して接触を返します(polygon: 3頂点以上の凸多角形, circle: 円形状)。
    std::optional<Contact> IntersectPolygonCircle(
        const std::vector<DirectX::XMFLOAT2>& polygon,
        const LamaPon::Circle2D& circle)
    {
        if (polygon.size() < 3)
        {
            return std::nullopt;
        }

        // 多角形の辺の外向き法線候補
        const auto axes = PolygonAxisCandidates(polygon, true);
        if (axes.empty())
        {
            return std::nullopt;
        }

        // 円中心と辺の最大符号分離量
        float maxSeparation = std::numeric_limits<float>::lowest();
        // 最も分離する辺の外向き法線
        DirectX::XMFLOAT2 faceNormal{ 0.0f, 1.0f };
        // 最も分離する辺の始点番号
        std::size_t edgeStart{};
        // 比較する辺の外向き法線
        for (const auto& candidate : axes)
        {
            // 比較する辺上の頂点
            const auto& vertex = polygon[candidate.edgeStart];
            // 円中心と辺の符号付き分離量
            const float separation =
                candidate.axis.x * (circle.center.x - vertex.x)
                    + candidate.axis.y * (circle.center.y - vertex.y);
            if (separation > maxSeparation)
            {
                maxSeparation = separation;
                faceNormal = candidate.axis;
                edgeStart = candidate.edgeStart;
            }
        }

        if (maxSeparation > circle.radius)
        {
            return std::nullopt;
        }

        if (maxSeparation <= 0.0001f)
        {
            // 中心が多角形の内部：最も近い面から押し出します。
            // 形状同士のめり込み量
            const float penetration = circle.radius - maxSeparation;
            // 通知する代表接触点
            const DirectX::XMFLOAT3 point{
                circle.center.x - faceNormal.x * maxSeparation,
                circle.center.y - faceNormal.y * maxSeparation,
                0.0f };
            // 法線と接触点の判定結果
            Contact contact{
                { faceNormal.x, faceNormal.y, 0.0f },
                std::max(penetration, 0.0001f),
                point };
            contact.points[0] = point;
            contact.pointCount = 1;
            return contact;
        }

        // 最近接点を求める辺の始点
        const auto& v1 = polygon[edgeStart];
        // 最近接点を求める辺の終点
        const auto& v2 = polygon[(edgeStart + 1) % polygon.size()];
        // 辺のX方向の差分
        const float edgeX = v2.x - v1.x;
        // 辺のY方向の差分
        const float edgeY = v2.y - v1.y;
        // 辺の長さの二乗
        const float lengthSquared = edgeX * edgeX + edgeY * edgeY;
        // 辺の最近接点を表す補間率
        float t = lengthSquared > 0.0001f
            ? ((circle.center.x - v1.x) * edgeX
                + (circle.center.y - v1.y) * edgeY) / lengthSquared
            : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        // 辺上で円中心に最も近い点
        const DirectX::XMFLOAT2 closest{
            v1.x + edgeX * t, v1.y + edgeY * t };

        // 最近接点から円中心へのX差分
        const float deltaX = circle.center.x - closest.x;
        // 最近接点から円中心へのY差分
        const float deltaY = circle.center.y - closest.y;
        // 最近接位置間の距離の二乗
        const float squaredDistance = deltaX * deltaX + deltaY * deltaY;
        if (squaredDistance >= circle.radius * circle.radius)
        {
            return std::nullopt;
        }
        // 最近接位置間の距離
        const float distance = std::sqrt(squaredDistance);
        // 第1形状側へ向く接触法線
        DirectX::XMFLOAT3 normal{ faceNormal.x, faceNormal.y, 0.0f };
        if (distance > 0.0001f)
        {
            normal = { deltaX / distance, deltaY / distance, 0.0f };
        }
        // 形状同士のめり込み量
        const float penetration = circle.radius - distance;
        // 通知する代表接触点
        const DirectX::XMFLOAT3 point{ closest.x, closest.y, 0.0f };
        // 法線と接触点の判定結果
        Contact contact{ normal, penetration, point };
        contact.points[0] = point;
        contact.pointCount = 1;
        return contact;
    }

    // 3Dの箱の貫入を判定し最小貫入軸の接触を返します(left: 第1境界, right: 第2境界)。
    std::optional<Contact> IntersectBounds(
        const LamaPon::Bounds3D& left,
        const LamaPon::Bounds3D& right)
    {
        // X方向の重なり幅
        const float overlapX = std::min(left.maximum.x, right.maximum.x)
            - std::max(left.minimum.x, right.minimum.x);
        // Y方向の重なり幅
        const float overlapY = std::min(left.maximum.y, right.maximum.y)
            - std::max(left.minimum.y, right.minimum.y);
        // Z方向の重なり幅
        const float overlapZ = std::min(left.maximum.z, right.maximum.z)
            - std::max(left.minimum.z, right.minimum.z);
        if (overlapX <= 0.0f || overlapY <= 0.0f || overlapZ <= 0.0f)
        {
            return std::nullopt;
        }

        // 第1境界の中心位置
        const DirectX::XMFLOAT3 leftCenter{
            (left.minimum.x + left.maximum.x) * 0.5f,
            (left.minimum.y + left.maximum.y) * 0.5f,
            (left.minimum.z + left.maximum.z) * 0.5f
        };
        // 第2境界の中心位置
        const DirectX::XMFLOAT3 rightCenter{
            (right.minimum.x + right.maximum.x) * 0.5f,
            (right.minimum.y + right.maximum.y) * 0.5f,
            (right.minimum.z + right.maximum.z) * 0.5f
        };

        if (overlapX <= overlapY && overlapX <= overlapZ)
        {
            return Contact{
                { leftCenter.x < rightCenter.x ? -1.0f : 1.0f, 0.0f, 0.0f },
                overlapX
            };
        }
        if (overlapY <= overlapZ)
        {
            return Contact{
                { 0.0f, leftCenter.y < rightCenter.y ? -1.0f : 1.0f, 0.0f },
                overlapY
            };
        }

        return Contact{
            { 0.0f, 0.0f, leftCenter.z < rightCenter.z ? -1.0f : 1.0f },
            overlapZ
        };
    }

    struct DirectionalShadowCascade final
    {
        // 分割ごとの影ビュー行列
        DirectX::XMMATRIX view;
        // 分割ごとの影投影行列
        DirectX::XMMATRIX projection;
        // カメラから分割終端までの距離
        float splitDistance{};
    };

    struct DirectionalShadowMatrices final
    {
        // 分割ごとの影行列と分割距離
        std::array<
            DirectionalShadowCascade,
            LamaPon::MaximumShadowCascades> cascades;
        // 有効な影分割の数
        std::size_t count{};
    };

    // 視錐台を分割し位置をテクセルへ合わせた影行列を作ります(cameraView: カメラビュー, cameraProjection: カメラ投影, lightDirection: 非零の光進行方向, shadowDistance: 影距離, requestedCascadeCount: 分割数, splitLambda: 対数分割率, shadowResolution: 影解像度)。
    // 入力は有限で、カメラのビューと投影行列は可逆である必要があります。
    DirectionalShadowMatrices BuildDirectionalShadowMatrices(
        DirectX::FXMMATRIX cameraView,
        DirectX::CXMMATRIX cameraProjection,
        const DirectX::XMFLOAT3& lightDirection,
        const float shadowDistance,
        const std::size_t requestedCascadeCount,
        const float splitLambda,
        const std::uint32_t shadowResolution) noexcept
    {
        using namespace DirectX;

        // 逆行列計算時の行列式
        XMVECTOR determinant{};
        // カメラのビュー行列の逆行列
        const XMMATRIX inverseView =
            XMMatrixInverse(&determinant, cameraView);
        // カメラの投影行列の逆行列
        const XMMATRIX inverseProjection =
            XMMatrixInverse(&determinant, cameraProjection);
        // 正規化した平行光の進行方向
        const XMVECTOR direction = XMVector3Normalize(
            XMLoadFloat3(&lightDirection));

        // 光方向と上方向の平行度
        const float verticalAlignment = std::abs(
            XMVectorGetX(XMVector3Dot(
                direction,
                XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f))));
        // 光方向に平行でない上方向
        const XMVECTOR up = verticalAlignment > 0.95f
            ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        // ビュー座標の近平面中心
        const XMVECTOR nearCenter = XMVector3TransformCoord(
            XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
            inverseProjection);
        // ビュー座標の遠平面中心
        const XMVECTOR farCenter = XMVector3TransformCoord(
            XMVectorSet(0.0f, 0.0f, 1.0f, 1.0f),
            inverseProjection);
        // 0.01以上のカメラ近平面距離
        const float cameraNear = std::max(
            -XMVectorGetZ(nearCenter),
            0.01f);
        // 近平面より先の遠平面距離
        const float cameraFar = std::max(
            -XMVectorGetZ(farCenter),
            cameraNear + 0.01f);
        // カメラ内へ制限した影距離
        const float maximumDistance = std::clamp(
            shadowDistance,
            cameraNear + 0.01f,
            cameraFar);
        // 上限内に制限した分割数
        const std::size_t cascadeCount = std::clamp(
            requestedCascadeCount,
            std::size_t{ 1 },
            LamaPon::MaximumShadowCascades);
        // 0〜1に制限した対数分割率
        const float lambda = std::clamp(
            splitLambda,
            0.0f,
            1.0f);

        // 分割ごとの影行列と距離
        DirectionalShadowMatrices result;
        result.count = cascadeCount;
        // 直前の分割境界の距離
        float previousSplit = cameraNear;
        // 構築する影分割の番号
        for (std::size_t cascadeIndex = 0;
            cascadeIndex < cascadeCount;
            ++cascadeIndex)
        {
            // 影範囲を分ける分割率
            const float fraction =
                static_cast<float>(cascadeIndex + 1)
                / static_cast<float>(cascadeCount);
            // 対数配置で求めた分割距離
            const float logarithmicSplit =
                cameraNear * std::pow(
                    maximumDistance / cameraNear,
                    fraction);
            // 等間隔配置の分割距離
            const float uniformSplit =
                cameraNear
                + (maximumDistance - cameraNear)
                    * fraction;
            // 混合後の分割境界距離
            const float splitDistance = std::lerp(
                uniformSplit,
                logarithmicSplit,
                lambda);

            // 分割視錐台のワールド頂点
            std::array<XMVECTOR, 8> corners;
            // 保存する分割視錐台の頂点番号
            std::size_t cornerIndex{};
            // 正規化画面のY端の座標
            for (const float ndcY : { -1.0f, 1.0f })
            {
                // 正規化画面のX端の座標
                for (const float ndcX : { -1.0f, 1.0f })
                {
                    // 逆射影した近平面の頂点
                    const XMVECTOR nearPoint =
                        XMVector3TransformCoord(
                            XMVectorSet(
                                ndcX,
                                ndcY,
                                0.0f,
                                1.0f),
                            inverseProjection);
                    // 逆射影した遠平面の頂点
                    const XMVECTOR farPoint =
                        XMVector3TransformCoord(
                            XMVectorSet(
                                ndcX,
                                ndcY,
                                1.0f,
                                1.0f),
                            inverseProjection);
                    // 近平面から遠平面への差分
                    const XMVECTOR ray =
                        XMVectorSubtract(
                            farPoint,
                            nearPoint);
                    // 遠近平面間のビュー深度差
                    const float rayDepth =
                        XMVectorGetZ(farPoint)
                        - XMVectorGetZ(nearPoint);
                    // 分割始点への深度補間率
                    const float nearFraction =
                        (-previousSplit
                            - XMVectorGetZ(nearPoint))
                        / rayDepth;
                    // 分割終点への深度補間率
                    const float farFraction =
                        (-splitDistance
                            - XMVectorGetZ(nearPoint))
                        / rayDepth;
                    corners[cornerIndex++] =
                        XMVector3TransformCoord(
                            XMVectorMultiplyAdd(
                                XMVectorReplicate(
                                    nearFraction),
                                ray,
                                nearPoint),
                            inverseView);
                    corners[cornerIndex++] =
                        XMVector3TransformCoord(
                            XMVectorMultiplyAdd(
                                XMVectorReplicate(
                                    farFraction),
                                ray,
                                nearPoint),
                            inverseView);
                }
            }

            // 分割視錐台の頂点平均
            XMVECTOR center = XMVectorZero();
            // 範囲を測る分割視錐台の頂点
            for (const auto& corner : corners)
            {
                center = XMVectorAdd(center, corner);
            }
            center = XMVectorScale(
                center,
                1.0f
                    / static_cast<float>(corners.size()));

            // 切り上げた分割視錐台の半径
            float radius{};
            // 範囲を測る分割視錐台の頂点
            for (const auto& corner : corners)
            {
                radius = std::max(
                    radius,
                    XMVectorGetX(XMVector3Length(
                        XMVectorSubtract(
                            corner,
                            center))));
            }
            radius = std::max(
                std::ceil(radius * 16.0f) / 16.0f,
                0.5f);

            // 影範囲の前後に加える余裕
            const float depthPadding = std::max(
                shadowDistance * 0.5f,
                10.0f);
            // 影用カメラのワールド位置
            const XMVECTOR eye = XMVectorMultiplyAdd(
                XMVectorNegate(direction),
                XMVectorReplicate(
                    radius + depthPadding),
                center);
            // 影用のビュー行列
            auto view = XMMatrixLookToRH(
                eye,
                direction,
                up);
            // テクセル位置を合わせる投影行列
            auto projection = XMMatrixOrthographicRH(
                radius * 2.0f,
                radius * 2.0f,
                0.1f,
                radius * 2.0f
                    + depthPadding * 2.0f);

            // 影解像度の半分の値
            const float halfResolution =
                static_cast<float>(
                    std::max(shadowResolution, 1u))
                * 0.5f;
            // テクセル座標へ変換した原点
            const XMVECTOR shadowOrigin =
                XMVectorScale(
                    XMVector3TransformCoord(
                        XMVectorZero(),
                        view * projection),
                    halfResolution);
            // 整数テクセルへ丸めた原点
            const XMVECTOR roundedOrigin =
                XMVectorRound(shadowOrigin);
            // 投影行列へ加える位置補正
            XMVECTOR roundingOffset =
                XMVectorScale(
                    XMVectorSubtract(
                        roundedOrigin,
                        shadowOrigin),
                    1.0f / halfResolution);
            roundingOffset = XMVectorSetZ(
                roundingOffset,
                0.0f);
            roundingOffset = XMVectorSetW(
                roundingOffset,
                0.0f);
            projection.r[3] = XMVectorAdd(
                projection.r[3],
                roundingOffset);

            result.cascades[cascadeIndex] = {
                view,
                projection,
                splitDistance
            };
            previousSplit = splitDistance;
        }

        return result;
    }

    // 最も近い有効な祖先スクロール表示を返します(gameObject: 祖先を探す対象)。
    const LamaPon::UIScrollViewComponent*
        FindAncestorScrollView(
            const LamaPon::GameObject& gameObject)
    {
        // スクロール表示を探す祖先
        for (const auto* ancestor = gameObject.Parent();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
            // 有効性を調べるスクロール表示
            if (const auto* scrollView =
                ancestor->GetComponent<
                    LamaPon::UIScrollViewComponent>();
                scrollView != nullptr
                && scrollView->IsEnabled())
            {
                return scrollView;
            }
        }
        return nullptr;
    }

    struct Light2DEntry final
    {
        // 2DライトのワールドXY位置
        DirectX::XMFLOAT2 position;
        // 2Dライトの影響半径
        float radius;
        // 2Dライトの明るさ
        float intensity;
        // 2DライトのRGB色
        DirectX::XMFLOAT3 color;
        // UIへも加算照明を適用する指定
        bool affectsUI;
    };

    // 上限までライトを選び画面単位の照明定数を作ります(lights: 2Dライト一覧, uiOnly: UIへ影響するライトのみ採用, worldOffset: ワールド描画の画面位置補正)。
    // ワールドXYと画面ピクセルは1対1で対応し、2D照明は加算で適用します。
    LamaPon::Sprite2DLighting BuildSprite2DLighting(
        const std::vector<Light2DEntry>& lights,
        const bool uiOnly,
        const DirectX::XMFLOAT2& worldOffset)
    {
        // 画面単位の2D照明定数
        LamaPon::Sprite2DLighting lighting{};
        // 採用した2Dライト数
        std::uint32_t count{};
        // 採用を調べる2Dライト
        for (const auto& light : lights)
        {
            if (count >= LamaPon::MaximumSprite2DLights)
            {
                break;
            }
            // UIを描いている間は、UIを照らす設定の灯りだけを渡します。
            if (uiOnly && !light.affectsUI)
            {
                continue;
            }
            // 定数バッファへ格納するライト
            auto& destination = lighting.lights[count];
            destination.positionRadiusIntensity = {
                light.position.x
                    + (uiOnly ? 0.0f : worldOffset.x),
                light.position.y
                    + (uiOnly ? 0.0f : worldOffset.y),
                light.radius,
                light.intensity };
            destination.color = {
                light.color.x,
                light.color.y,
                light.color.z,
                0.0f };
            ++count;
        }
        lighting.counts = { count, 0u, 0u, 0u };
        return lighting;
    }

    // 2D加算照明シェーダーのパス
    const std::filesystem::path SpriteLit2DShaderPath{
        L"shaders/LamaPonSpriteLit.hlsl" };

    struct SpriteMaskEntry final
    {
        // 画面補正後のマスク中心位置
        DirectX::XMFLOAT2 position;
        // マスクの形状
        LamaPon::SpriteMaskShape shape;
        // マスクの幅と高さ
        DirectX::XMFLOAT2 size;
    };

    // 2Dマスクシェーダーのパス
    const std::filesystem::path SpriteMaskShaderPath{
        L"shaders/LamaPonSpriteMask.hlsl" };

    // 最も近い一つのマスクと描画定数を設定します(spritePosition: 画面位置, spriteTint: 描画色, spriteDrawRect: 描画矩形, interaction: マスク内外の表示指定, masks: マスク一覧)。
    // 5番と6番の定数に描画色と矩形を格納します。
    std::array<DirectX::XMFLOAT4, 8> BuildSpriteMaskParameters(
        const DirectX::XMFLOAT2& spritePosition,
        const DirectX::XMFLOAT4& spriteTint,
        const DirectX::XMFLOAT4& spriteDrawRect,
        const LamaPon::SpriteMaskInteraction interaction,
        const std::vector<SpriteMaskEntry>& masks)
    {
        // マスク描画用のシェーダー定数
        std::array<DirectX::XMFLOAT4, 8> parameters{};
        parameters[5] = spriteTint;
        parameters[6] = spriteDrawRect;

        // 最も近いマスクの添字
        std::size_t nearestIndex = masks.size();
        // 最近傍マスクまでの距離の二乗
        float nearestDistanceSquared =
            std::numeric_limits<float>::max();
        // 比較するマスクの添字
        for (std::size_t index{}; index < masks.size(); ++index)
        {
            // スプライトからマスクへのX差分
            const float deltaX =
                masks[index].position.x - spritePosition.x;
            // スプライトからマスクへのY差分
            const float deltaY =
                masks[index].position.y - spritePosition.y;
            // マスクまでの距離の二乗
            const float distanceSquared =
                deltaX * deltaX + deltaY * deltaY;
            if (distanceSquared < nearestDistanceSquared)
            {
                nearestDistanceSquared = distanceSquared;
                nearestIndex = index;
            }
        }

        if (nearestIndex < masks.size())
        {
            // 採用した最近傍のマスク
            const auto& mask = masks[nearestIndex];
            parameters[0].x =
                mask.shape == LamaPon::SpriteMaskShape::Rectangle
                    ? 0.0f
                    : 1.0f;
            parameters[0].y =
                interaction
                    == LamaPon::SpriteMaskInteraction::
                        VisibleOutsideMask
                    ? 1.0f
                    : 0.0f;
            parameters[1] = {
                mask.position.x,
                mask.position.y,
                mask.size.x * 0.5f,
                mask.size.y * 0.5f };
        }
        return parameters;
    }

    // 描画順で2DとUIを描き独自パス・マスク・照明を適用します(gameObjects: シーンのオブジェクト一覧, graphics: 描画機器)。
    // 同順序は登録順を保ち、独自シェーダーを優先し、マスクは照明より優先します。
    void RenderSprites2D(
        const std::vector<std::unique_ptr<LamaPon::GameObject>>&
            gameObjects,
        LamaPon::GraphicsDevice& graphics)
    {
        // 標準のスプライト描画パス
        auto defaultPass = graphics.BeginSpritePass();
        // 現在の標準スプライト描画状態
        auto sprites = defaultPass.Context();
        // 描画順序で並べる対象一覧
        std::vector<LamaPon::GameObject*> ordered;
        ordered.reserve(gameObjects.size());
        // 描画や設定収集の対象
        for (const auto& gameObject : gameObjects)
        {
            ordered.push_back(gameObject.get());
        }
        // 有効な2Dライトの一覧
        std::vector<Light2DEntry> lights2D;
        // 描画や設定収集の対象
        for (const auto& gameObject : gameObjects)
        {
            // 設定を収集する2Dライト
            if (const auto* light =
                    gameObject->GetComponent<
                        LamaPon::Light2DComponent>();
                light != nullptr
                && light->IsEnabled()
                && gameObject->IsActiveInHierarchy())
            {
                lights2D.push_back({
                    light->WorldPosition(),
                    light->Radius(),
                    light->Intensity(),
                    light->Color(),
                    light->AffectsUI() });
            }
        }
        // 画面単位で照明一覧を一度作り、UIには指定のあるライトだけを渡します。
        // ワールド描画用の照明定数
        const auto worldLighting =
            BuildSprite2DLighting(
                lights2D,
                false,
                graphics.Sprite2DOffset());
        // UIへ適用する照明定数
        const auto uiLighting =
            BuildSprite2DLighting(
                lights2D,
                true,
                graphics.Sprite2DOffset());
        // UIへ影響するライトの有無
        const bool anyLightAffectsUI =
            uiLighting.counts.x > 0u;
        // 有効な2Dマスクの一覧
        std::vector<SpriteMaskEntry> masks2D;
        // 描画や設定収集の対象
        for (const auto& gameObject : gameObjects)
        {
            // 設定を収集する2Dマスク
            if (const auto* mask =
                    gameObject->GetComponent<
                        LamaPon::SpriteMaskComponent>();
                mask != nullptr
                && mask->IsEnabled()
                && gameObject->IsActiveInHierarchy())
            {
                masks2D.push_back({
                    {
                        mask->WorldPosition().x
                            + graphics.Sprite2DOffset().x,
                        mask->WorldPosition().y
                            + graphics.Sprite2DOffset().y
                    },
                    mask->Shape(),
                    mask->Size() });
            }
        }
        // 2D描画順序を比較します(left: 一方の対象, right: もう一方の対象)。
        std::stable_sort(
            ordered.begin(),
            ordered.end(),
            [](
                const LamaPon::GameObject* left,
                const LamaPon::GameObject* right)
            {
                return left->Render2DSortOrder()
                    < right->Render2DSortOrder();
            });
        // ScrollView配下のオブジェクトは表示領域でクリッピングします。
        // 標準パスで適用中のスクロール表示
        const LamaPon::UIScrollViewComponent*
            activeScrollView{};
        // 描画や設定収集の対象
        for (auto* gameObject : ordered)
        {
            // 独自シェーダーは対象ごとにパスを切り替え、標準パスのスクロールクリップを解除します。
            if (gameObject->IsEnabled())
            {
                // 独自シェーダーを使う描画設定
                if (auto* sprite =
                        gameObject->GetComponent<
                            LamaPon::SpriteRendererComponent>();
                    sprite != nullptr
                    && sprite->IsEnabled()
                    && !sprite->ShaderPath().empty())
                {
                    if (activeScrollView != nullptr)
                    {
                        static_cast<void>(sprites.PopScissor());
                        activeScrollView = nullptr;
                    }
                    defaultPass.End();
                    // 独自シェーダーの描画パス
                    auto customPass =
                        sprite->BeginRenderPass(graphics);
                    gameObject->Render2D(
                        graphics,
                        customPass.Context());
                    customPass.End();
                    defaultPass = graphics.BeginSpritePass();
                    sprites = defaultPass.Context();
                    continue;
                }
            }
            // マスク指定のスプライトだけを別パスで描き、同じ対象の2D照明より優先します。
            if (!masks2D.empty()
                && gameObject->IsEnabled()
                && gameObject->GetComponent<
                    LamaPon::UIRectTransformComponent>()
                    == nullptr)
            {
                // マスクを適用する描画設定
                if (auto* maskedSprite =
                        gameObject->GetComponent<
                            LamaPon::SpriteRendererComponent>();
                    maskedSprite != nullptr
                    && maskedSprite->IsEnabled()
                    && maskedSprite->ShaderPath().empty()
                    && maskedSprite->MaskInteraction()
                        != LamaPon::SpriteMaskInteraction::None)
                {
                    // スプライトのワールド変換
                    DirectX::XMFLOAT4X4 spriteWorld{};
                    DirectX::XMStoreFloat4x4(
                        &spriteWorld,
                        gameObject->WorldMatrix());
                    // ワールド変換のX拡縮量
                    const float worldScaleX = std::sqrt(
                        spriteWorld._11 * spriteWorld._11
                            + spriteWorld._12
                                * spriteWorld._12);
                    // ワールド変換のY拡縮量
                    const float worldScaleY = std::sqrt(
                        spriteWorld._21 * spriteWorld._21
                            + spriteWorld._22
                                * spriteWorld._22);
                    // スプライトの設定寸法
                    const auto& spriteSize =
                        maskedSprite->Size();
                    // 拡縮後の描画幅
                    const float drawWidth = std::max(
                        spriteSize.x * worldScaleX,
                        0.0001f);
                    // 拡縮後の描画高さ
                    const float drawHeight = std::max(
                        spriteSize.y * worldScaleY,
                        0.0001f);
                    // Pivotを動かしているとTransformの位置は左上ではないので、矩形の左上へ戻してから渡します。
                    // 描画矩形の基準点
                    const auto& spritePivot =
                        maskedSprite->Pivot();
                    // 画面上の位置と描画寸法
                    const DirectX::XMFLOAT4 drawRect{
                        spriteWorld._41
                            - spritePivot.x * drawWidth
                            + graphics.Sprite2DOffset().x,
                        spriteWorld._42
                            - spritePivot.y * drawHeight
                            + graphics.Sprite2DOffset().y,
                        drawWidth,
                        drawHeight
                    };
                    // マスク描画のシェーダー定数
                    const auto parameters =
                        BuildSpriteMaskParameters(
                            { spriteWorld._41
                                + graphics.Sprite2DOffset().x,
                                spriteWorld._42
                                + graphics.Sprite2DOffset().y },
                            maskedSprite->Color(),
                            drawRect,
                            maskedSprite->MaskInteraction(),
                            masks2D);

                    if (activeScrollView != nullptr)
                    {
                        static_cast<void>(sprites.PopScissor());
                        activeScrollView = nullptr;
                    }
                    defaultPass.End();
                    // マスク描画パスの設定
                    LamaPon::SpritePassDescription maskDescription;
                    maskDescription.pixelShader =
                        SpriteMaskShaderPath;
                    maskDescription.customParameters = parameters;
                    // マスクを適用する描画パス
                    auto maskPass = graphics.BeginSpritePass(
                        maskDescription);
                    gameObject->Render2D(
                        graphics,
                        maskPass.Context());
                    maskPass.End();
                    defaultPass = graphics.BeginSpritePass();
                    sprites = defaultPass.Context();
                    continue;
                }
            }
            // 独自シェーダーのないスプライトとタイルを照らし、UIは指定されたライトだけを適用します。
            if (!lights2D.empty() && gameObject->IsEnabled())
            {
                // UI座標で描画する対象
                const bool isUI =
                    gameObject->GetComponent<
                        LamaPon::UIRectTransformComponent>()
                        != nullptr;
                // 照明を適用する描画設定
                auto* litSprite =
                    gameObject->GetComponent<
                        LamaPon::SpriteRendererComponent>();
                // 照明を使うスプライトの状態
                const bool spriteIsLit =
                    litSprite != nullptr
                    && litSprite->IsEnabled()
                    && litSprite->ShaderPath().empty();
                // 照明を調べるタイル描画設定
                const auto* tilemap =
                    gameObject->GetComponent<
                        LamaPon::TilemapComponent>();
                // 照明を使うタイルの状態
                const bool tilemapIsLit =
                    !isUI
                    && tilemap != nullptr
                    && tilemap->IsEnabled();
                if ((spriteIsLit || tilemapIsLit)
                    && (!isUI || anyLightAffectsUI))
                {
                    if (activeScrollView != nullptr)
                    {
                        static_cast<void>(sprites.PopScissor());
                        activeScrollView = nullptr;
                    }
                    defaultPass.End();
                    // 色とUVは頂点から受け取り、照明定数はb1へ渡します。
                    // 照明付き描画パスの設定
                    LamaPon::SpritePassDescription litDescription;
                    litDescription.pixelShader =
                        SpriteLit2DShaderPath;
                    litDescription.lighting =
                        isUI ? uiLighting : worldLighting;
                    // 照明を適用する描画パス
                    auto litPass = graphics.BeginSpritePass(
                        litDescription);
                    gameObject->Render2D(
                        graphics,
                        litPass.Context());
                    litPass.End();
                    defaultPass = graphics.BeginSpritePass();
                    sprites = defaultPass.Context();
                    continue;
                }
            }
            // 現在の対象の祖先スクロール表示
            const auto* scrollView =
                FindAncestorScrollView(*gameObject);
            if (scrollView != activeScrollView)
            {
                if (activeScrollView != nullptr)
                {
                    static_cast<void>(sprites.PopScissor());
                }
                activeScrollView = scrollView;
                if (activeScrollView != nullptr)
                {
                    // 画面上のスクロール表示範囲
                    const auto viewRect =
                        activeScrollView->ViewRect(
                            graphics);
                    static_cast<void>(sprites.PushScissor({
                        viewRect.minimum.x,
                        viewRect.minimum.y,
                        viewRect.maximum.x,
                        viewRect.maximum.y }));
                }
            }
            gameObject->Render2D(
                graphics,
                sprites);
        }
        if (activeScrollView != nullptr)
        {
            static_cast<void>(sprites.PopScissor());
        }
        defaultPass.End();
    }
}

namespace LamaPon
{
    Scene::Scene(GraphicsDevice& graphics)
        : m_graphics(graphics)
        , m_graphicsResourceLease(
            graphics.AcquireResourceLease())
        , m_sceneManager(
            std::make_unique<SceneManager>(
                *this,
                graphics))
    {
    }

    Scene::~Scene() = default;

    bool Scene::SetWindowSize(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        return width != 0 && height != 0
            && width <= 16384 && height <= 16384
            && m_windowSizeSetter
            && m_windowSizeSetter(width, height);
    }

    std::pair<std::uint32_t, std::uint32_t>
        Scene::WindowSize() const
    {
        if (m_windowSizeGetter)
        {
            return m_windowSizeGetter();
        }
        return { m_graphics.Width(), m_graphics.Height() };
    }

    void Scene::SetWindowSizeCallbacks(
        std::function<bool(std::uint32_t, std::uint32_t)> setter,
        std::function<std::pair<std::uint32_t, std::uint32_t>()> getter)
    {
        m_windowSizeSetter = std::move(setter);
        m_windowSizeGetter = std::move(getter);
    }

    GameObject& Scene::CreateGameObject(std::string name)
    {
        // 新たに所有するオブジェクト
        auto gameObject = std::make_unique<GameObject>(m_nextId++, std::move(name));
        gameObject->m_scene = this;
        // 追加読み込み中なら、そのシーンの所属にします。
        gameObject->m_sourceScene = m_loadingScene;
        // 生成または複製したルート
        auto* result = gameObject.get();
        m_gameObjects.emplace_back(std::move(gameObject));
        return *result;
    }

    GameObject& Scene::DuplicateGameObject(
        const GameObject& source,
        GameObject* targetParent,
        const bool appendCopySuffix)
    {
        if (targetParent != nullptr
            && FindGameObject(targetParent->Id()) != targetParent)
        {
            throw std::invalid_argument("The duplicate target parent is not in this scene.");
        }

        // 失敗時に破棄する複製ルート
        GameObject* duplicatedRoot{};
        // 複製元と複製先の番号対応表
        std::unordered_map<
            GameObjectId,
            GameObjectId> duplicatedIds;

        // CreateGameObjectの所属を複製元に合わせ、終了時に元へ戻します。
        // 複製前の生成所属シーン番号
        const SceneHandle previousLoadingScene =
            m_loadingScene;
        m_loadingScene = source.SourceScene();
        // 生成所属シーンの復元ガード
        struct LoadingSceneRestore final
        {
            // 借用する複製先のシーン
            Scene& scene;
            // 複製前の生成所属シーン番号
            SceneHandle previous;
            // 複製中だけ変更した生成所属シーン番号を戻します。
            ~LoadingSceneRestore()
            {
                scene.m_loadingScene = previous;
            }
        } restoreLoadingScene{
            *this,
            previousLoadingScene
        };

        // 対象と子孫を複製します(self: 再帰用の自身, sourceObject: 複製元, parent: 複製先の親, isRoot: ルートの指定)。
        const auto clone =
            [this,
                &duplicatedRoot,
                &duplicatedIds,
                appendCopySuffix](
                const auto& self,
                const GameObject& sourceObject,
                GameObject* parent,
                const bool isRoot) -> GameObject&
            {
                // 新たに生成した複製先
                auto& duplicate = CreateGameObject(
                    sourceObject.Name()
                    + (isRoot && appendCopySuffix
                        ? " Copy"
                        : ""));

                if (isRoot)
                {
                    duplicatedRoot = &duplicate;
                }
                duplicatedIds.emplace(
                    sourceObject.Id(),
                    duplicate.Id());

                duplicate.SetEnabled(sourceObject.IsEnabled());
                duplicate.SetTag(sourceObject.Tag());
                duplicate.GetTransform() = sourceObject.GetTransform();
                duplicate.SetPrefabAssetPath(
                    sourceObject.PrefabAssetPath());
                duplicate.SetParent(parent);

                // 複製元が所有する構成要素
                for (const auto& sourceComponent : sourceObject.Components())
                {
                    // 生成した構成要素の非所有参照
                    Component* duplicateComponent{};

                    // 複製元のカメラ
                    if (const auto* camera =
                        dynamic_cast<const CameraComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<CameraComponent>(
                            camera->VerticalFieldOfView(),
                            camera->NearPlane(),
                            camera->FarPlane());
                    }
                    // 複製元の平行光源
                    else if (const auto* directionalLight =
                        dynamic_cast<const DirectionalLightComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                DirectionalLightComponent>(
                                directionalLight->Color(),
                                directionalLight->Intensity(),
                                directionalLight->CastsShadows(),
                                directionalLight->ShadowDistance(),
                                directionalLight->ShadowBias(),
                                directionalLight->ShadowNormalBias(),
                                directionalLight->ShadowStrength(),
                                directionalLight->ShadowCascadeCount(),
                                directionalLight->ShadowSplitLambda());
                    }
                    // 複製元の点光源
                    else if (const auto* pointLight =
                        dynamic_cast<const PointLightComponent*>(
                            sourceComponent.get()))
                    {
                        // 設定を引き継ぐ点光源
                        auto& duplicatePointLight =
                            duplicate.AddComponent<
                                PointLightComponent>(
                                pointLight->Color(),
                                pointLight->Intensity(),
                                pointLight->Range());
                        duplicatePointLight.SetCastsShadows(
                            pointLight->CastsShadows());
                        duplicatePointLight.SetShadowBias(
                            pointLight->ShadowBias());
                        duplicatePointLight
                            .SetShadowStrength(
                                pointLight
                                    ->ShadowStrength());
                        duplicateComponent =
                            &duplicatePointLight;
                    }
                    // 複製元のスポット光源
                    else if (const auto* spotLight =
                        dynamic_cast<const SpotLightComponent*>(
                            sourceComponent.get()))
                    {
                        // 設定を引き継ぐスポット光源
                        auto& duplicateSpotLight =
                            duplicate.AddComponent<
                                SpotLightComponent>(
                                spotLight->Color(),
                                spotLight->Intensity(),
                                spotLight->Range(),
                                spotLight->InnerConeAngle(),
                                spotLight->OuterConeAngle());
                        duplicateSpotLight.SetCastsShadows(
                            spotLight->CastsShadows());
                        duplicateSpotLight.SetShadowBias(
                            spotLight->ShadowBias());
                        duplicateSpotLight
                            .SetShadowNormalBias(
                                spotLight
                                    ->ShadowNormalBias());
                        duplicateSpotLight
                            .SetShadowStrength(
                                spotLight
                                    ->ShadowStrength());
                        duplicateComponent =
                            &duplicateSpotLight;
                    }
                    // 複製元の2Dライト
                    else if (const auto* light2D =
                        dynamic_cast<
                            const Light2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                Light2DComponent>(
                                    light2D->Color(),
                                    light2D->Intensity(),
                                    light2D->Radius());
                    }
                    // 複製元の2D箱コライダー
                    else if (const auto* collider2D =
                        dynamic_cast<const BoxCollider2DComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<BoxCollider2DComponent>(
                            collider2D->Size(),
                            collider2D->Offset(),
                            collider2D->IsTrigger(),
                            collider2D->Layer(),
                            collider2D->CollisionMask(),
                            collider2D->Material());
                    }
                    // 複製元の2D円コライダー
                    else if (const auto* circle2D =
                        dynamic_cast<
                            const CircleCollider2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                CircleCollider2DComponent>(
                                    circle2D->Radius(),
                                    circle2D->Offset(),
                                    circle2D->IsTrigger(),
                                    circle2D->Layer(),
                                    circle2D
                                        ->CollisionMask(),
                                    circle2D->Material());
                    }
                    // 複製元の2D多角形コライダー
                    else if (const auto* polygon2D =
                        dynamic_cast<
                            const PolygonCollider2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                PolygonCollider2DComponent>(
                                    polygon2D->Vertices(),
                                    polygon2D->Offset(),
                                    polygon2D->IsTrigger(),
                                    polygon2D->Layer(),
                                    polygon2D
                                        ->CollisionMask(),
                                    polygon2D->Material());
                    }
                    // 複製元の3D箱コライダー
                    else if (const auto* collider3D =
                        dynamic_cast<const BoxCollider3DComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<BoxCollider3DComponent>(
                            collider3D->Size(),
                            collider3D->Offset(),
                            collider3D->IsTrigger(),
                            collider3D->Layer(),
                            collider3D->CollisionMask(),
                            collider3D->Material());
                    }
                    // 複製元のカプセルコライダー
                    else if (const auto* capsule =
                        dynamic_cast<const CapsuleCollider3DComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                CapsuleCollider3DComponent>(
                                    capsule->Radius(),
                                    capsule->Height(),
                                    capsule->Offset(),
                                    capsule->IsTrigger(),
                                    capsule->Layer(),
                                    capsule->CollisionMask(),
                                    capsule->Material());
                    }
                    // 複製元の球コライダー
                    else if (const auto* sphere =
                        dynamic_cast<
                            const SphereCollider3DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                SphereCollider3DComponent>(
                                    sphere->Radius(),
                                    sphere->Offset(),
                                    sphere->IsTrigger(),
                                    sphere->Layer(),
                                    sphere->CollisionMask(),
                                    sphere->Material());
                    }
                    // 複製元の凸形状コライダー
                    else if (const auto* hull =
                        dynamic_cast<
                            const ConvexHullCollider3DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                ConvexHullCollider3DComponent>(
                                    hull->Points(),
                                    hull->Offset(),
                                    hull->IsTrigger(),
                                    hull->Layer(),
                                    hull->CollisionMask(),
                                    hull->Material());
                    }
                    // 複製元のメッシュコライダー
                    else if (const auto* meshCollider =
                        dynamic_cast<
                            const MeshCollider3DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                MeshCollider3DComponent>(
                                    meshCollider
                                        ->ModelPath(),
                                    meshCollider->Offset(),
                                    meshCollider
                                        ->IsTrigger(),
                                    meshCollider->Layer(),
                                    meshCollider
                                        ->CollisionMask(),
                                    meshCollider
                                        ->Material());
                    }
                    // 複製元のメッシュ描画
                    else if (const auto* mesh =
                        dynamic_cast<const MeshRendererComponent*>(sourceComponent.get()))
                    {
                        // 設定を引き継ぐメッシュ描画
                        auto& duplicateMesh =
                            duplicate.AddComponent<MeshRendererComponent>(
                            mesh->Shape(),
                            mesh->Color(),
                            mesh->AlbedoTexturePath(),
                            mesh->NormalTexturePath(),
                            mesh->Roughness(),
                            mesh->NormalStrength(),
                            mesh->MaterialAssetPath());
                        duplicateMesh.SetMetallic(
                            mesh->Metallic());
                        duplicateMesh.SetRoughnessTexturePath(
                            mesh->RoughnessTexturePath());
                        duplicateMesh.SetMetallicTexturePath(
                            mesh->MetallicTexturePath());
                        duplicateMesh.SetOcclusionTexturePath(
                            mesh->OcclusionTexturePath());
                        duplicateMesh.SetEmissiveTexturePath(
                            mesh->EmissiveTexturePath());
                        duplicateMesh.SetOcclusionStrength(
                            mesh->OcclusionStrength());
                        duplicateMesh.SetEmissiveColor(
                            mesh->EmissiveColor());
                        if (mesh->HasProceduralMesh())
                        {
                            duplicateMesh.SetProceduralMesh(
                                mesh->ProceduralVertices(),
                                mesh->ProceduralIndices());
                        }
                        duplicateComponent =
                            &duplicateMesh;
                    }
                    // 複製元のスプライト描画
                    else if (const auto* sprite =
                        dynamic_cast<const SpriteRendererComponent*>(sourceComponent.get()))
                    {
                        // 設定を引き継ぐスプライト描画
                        auto& duplicateSprite =
                            duplicate.AddComponent<SpriteRendererComponent>(
                                sprite->Size(),
                                sprite->Color(),
                                sprite->TexturePath());
                        duplicateSprite.SetSortOrder(
                            sprite->SortOrder());
                        duplicateSprite.SetPivot(
                            sprite->Pivot());
                        duplicateSprite.SetRenderTexture(
                            sprite->RenderTexture());
                        duplicateSprite.SetSourceRect(
                            sprite->SourceRect());
                        duplicateSprite.SetMeshGrid(
                            sprite->MeshColumns(),
                            sprite->MeshRows());
                        duplicateSprite.SetShaderPath(
                            sprite->ShaderPath());
                        duplicateSprite.SetMaskInteraction(
                            sprite->MaskInteraction());
                        // 引き継ぐシェーダー定数の番号
                        for (std::size_t index = 0;
                            index
                                < SpriteRendererComponent::
                                    CustomParameterCount;
                            ++index)
                        {
                            duplicateSprite.SetCustomParameter(
                                index,
                                sprite->CustomParameter(index));
                        }
                        duplicateComponent = &duplicateSprite;
                    }
                    // 複製元の2Dマスク
                    else if (const auto* spriteMask =
                        dynamic_cast<
                            const SpriteMaskComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                SpriteMaskComponent>(
                                    spriteMask->Shape(),
                                    spriteMask->Size());
                    }
                    // 複製元のカリング設定
                    else if (const auto* renderCulling =
                        dynamic_cast<
                            const RenderCullingComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                RenderCullingComponent>(
                                    renderCulling
                                        ->AlwaysVisible(),
                                    renderCulling
                                        ->CullingMargin());
                    }
                    // 複製元の反射プローブ
                    else if (const auto* reflectionProbe =
                        dynamic_cast<
                            const ReflectionProbeComponent*>(
                                sourceComponent.get()))
                    {
                        // 複製したプローブは新しい位置で自動ベイクするため、結果を引き継ぎません。
                        // 新たにベイクする反射プローブ
                        auto& duplicateProbe =
                            duplicate.AddComponent<
                                ReflectionProbeComponent>(
                                    reflectionProbe
                                        ->Range(),
                                    reflectionProbe
                                        ->Intensity());
                        duplicateProbe.SetBoxExtents(
                            reflectionProbe->BoxExtents());
                        duplicateProbe.SetBlendDistance(
                            reflectionProbe
                                ->BlendDistance());
                        duplicateComponent =
                            &duplicateProbe;
                    }
                    // 複製元のスプライト再生設定
                    else if (const auto* spriteAnimator =
                        dynamic_cast<
                            const SpriteAnimatorComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐスプライト再生
                        auto& duplicateAnimator =
                            duplicate.AddComponent<
                                SpriteAnimatorComponent>(
                                spriteAnimator->Columns(),
                                spriteAnimator->Rows());
                        duplicateAnimator.SetSpeed(
                            spriteAnimator->Speed());
                        duplicateAnimator.SetPlayOnStart(
                            spriteAnimator->PlayOnStart());
                        duplicateAnimator.SetDefaultClip(
                            spriteAnimator->DefaultClip());
                        // 引き継ぐスプライトクリップ
                        for (const auto& clip :
                            spriteAnimator->Clips())
                        {
                            duplicateAnimator.AddClip(
                                clip);
                        }
                        duplicateComponent =
                            &duplicateAnimator;
                    }
                    // 複製元のタイルマップ
                    else if (const auto* tilemap =
                        dynamic_cast<
                            const TilemapComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐタイルマップ
                        auto& duplicateTilemap =
                            duplicate.AddComponent<
                                TilemapComponent>(
                                    tilemap->
                                        TileSize(),
                                    tilemap->
                                        AtlasColumns(),
                                    tilemap->
                                        AtlasRows(),
                                    tilemap->Color(),
                                    tilemap->
                                        TexturePath());
                        // セルの配置(coordinate: 格子のXY番号, tileIndex: アトラスのタイル番号)。
                        for (const auto&
                            [coordinate, tileIndex] :
                            tilemap->Cells())
                        {
                            duplicateTilemap.SetCell(
                                coordinate.first,
                                coordinate.second,
                                tileIndex);
                        }
                        duplicateTilemap.SetSortOrder(
                            tilemap->SortOrder());
                        duplicateComponent =
                            &duplicateTilemap;
                    }
                    // 複製元の視差レイヤー
                    else if (const auto* parallax =
                        dynamic_cast<
                            const ParallaxLayerComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                ParallaxLayerComponent>(
                                    parallax->Factor(),
                                    parallax
                                        ->ReferenceId());
                    }
                    // 複製元のナビメッシュ
                    else if (const auto* navMesh =
                        dynamic_cast<
                            const NavMeshComponent*>(
                                sourceComponent.get()))
                    {
                        // ベイク格子を引き継ぐナビメッシュ
                        auto& duplicateNavMesh =
                            duplicate.AddComponent<
                                NavMeshComponent>(
                                    navMesh->
                                        SurfaceSize(),
                                    navMesh->
                                        CellSize(),
                                    navMesh->
                                        AgentRadius(),
                                    navMesh->
                                        AgentHeight());
                        if (navMesh->IsBaked())
                        {
                            // ベイク済みの通行不可セル一覧
                            std::vector<
                                NavMeshComponent::
                                    CellCoordinate>
                                blockedCells;
                            // 通行不可を調べる格子のZ番号
                            for (std::uint32_t z{};
                                z < navMesh->
                                    GridDepth();
                                ++z)
                            {
                                // 通行不可を調べる格子のX番号
                                for (std::uint32_t x{};
                                    x < navMesh->
                                        GridWidth();
                                    ++x)
                                {
                                    if (navMesh->
                                        IsBlocked(x, z))
                                    {
                                        blockedCells.
                                            emplace_back(
                                                x,
                                                z);
                                    }
                                }
                            }
                            duplicateNavMesh.
                                RestoreBake(
                                    blockedCells);
                        }
                        duplicateComponent =
                            &duplicateNavMesh;
                    }
                    // 複製元の経路移動エージェント
                    else if (const auto* agent =
                        dynamic_cast<
                            const
                                NavMeshAgentComponent*>(
                                    sourceComponent.get()))
                    {
                        // 経路を引き継ぐ移動エージェント
                        auto& duplicateAgent =
                            duplicate.AddComponent<
                                NavMeshAgentComponent>(
                                    agent->Speed(),
                                    agent->
                                        StoppingDistance(),
                                    agent->
                                        RotateToPath());
                        duplicateAgent.SetPath(
                            agent->Destination(),
                            agent->Path());
                        duplicateComponent =
                            &duplicateAgent;
                    }
                    // 複製元の粒子システム
                    else if (const auto* particles =
                        dynamic_cast<
                            const
                                ParticleSystemComponent*>(
                                    sourceComponent.get()))
                    {
                        // 設定を引き継ぐ粒子システム
                        auto& duplicateParticles =
                            duplicate.AddComponent<
                                ParticleSystemComponent>(
                                    particles->
                                        MaxParticles(),
                                    particles->
                                        EmissionRate(),
                                    particles->
                                        Lifetime(),
                                    particles->
                                        StartSpeed(),
                                    particles->
                                        StartSize(),
                                    particles->
                                        StartColor(),
                                    particles->
                                        EndColor(),
                                    particles->
                                        EmitterShape(),
                                    particles->
                                        TexturePath());
                        duplicateParticles.
                            SetEndSizeMultiplier(
                                particles->
                                    EndSizeMultiplier());
                        duplicateParticles.SetRenderMode(
                            particles->RenderMode());
                        duplicateParticles.SetGravity(
                            particles->Gravity());
                        duplicateParticles.
                            SetEmitterSize(
                                particles->
                                    EmitterSize());
                        duplicateParticles.
                            SetConeAngle(
                                particles->
                                    ConeAngle());
                        duplicateParticles.
                            SetDuration(
                                particles->
                                    Duration());
                        duplicateParticles.
                            SetLooping(
                                particles->
                                    Looping());
                        duplicateParticles.
                            SetPlayOnStart(
                                particles->
                                    PlayOnStart());
                        duplicateParticles.
                            SetPreviewInEditor(
                                particles->
                                    PreviewInEditor());
                        duplicateParticles.
                            SetAdditive(
                                particles->
                                    Additive());
                        duplicateParticles.
                            SetShaderPath(
                                particles->
                                    ShaderPath());
                        duplicateParticles.
                            SetAuxiliaryTexturePath(
                                particles->
                                    AuxiliaryTexturePath());
                        // 引き継ぐシェーダー定数の番号
                        for (std::size_t index = 0;
                            index
                                < ParticleSystemComponent::
                                    CustomParameterCount;
                            ++index)
                        {
                            duplicateParticles.
                                SetCustomParameter(
                                    index,
                                    particles->
                                        CustomParameter(index));
                        }
                        duplicateComponent =
                            &duplicateParticles;
                    }
                    // 複製元のUIキャンバス
                    else if (const auto* canvas =
                        dynamic_cast<
                            const UICanvasComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                UICanvasComponent>(
                                    canvas->
                                        ReferenceResolution(),
                                    canvas->
                                        MatchWidthOrHeight());
                    }
                    // 複製元のUI配置設定
                    else if (const auto* uiTransform =
                        dynamic_cast<
                            const
                                UIRectTransformComponent*>(
                                    sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                UIRectTransformComponent>(
                                    uiTransform->AnchorMin(),
                                    uiTransform->AnchorMax(),
                                    uiTransform->Pivot(),
                                    uiTransform->
                                        AnchoredPosition(),
                                    uiTransform->SizeDelta());
                    }
                    // 複製元のUIボタン
                    else if (const auto* button =
                        dynamic_cast<
                            const UIButtonComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐUIボタン
                        auto& duplicateButton =
                            duplicate.AddComponent<
                                UIButtonComponent>(
                                    button->Label(),
                                    button->FallbackSize(),
                                    button->TexturePath());
                        duplicateButton.SetFontFamily(
                            button->FontFamily());
                        duplicateButton.SetFontSize(
                            button->FontSize());
                        duplicateButton.SetNormalColor(
                            button->NormalColor());
                        duplicateButton.SetHoveredColor(
                            button->HoveredColor());
                        duplicateButton.SetPressedColor(
                            button->PressedColor());
                        duplicateButton.SetDisabledColor(
                            button->DisabledColor());
                        duplicateButton.SetTextColor(
                            button->TextColor());
                        duplicateButton.SetInteractable(
                            button->Interactable());
                        duplicateButton.SetTargetScene(
                            button->TargetScene());
                        duplicateButton.
                            SetReloadCurrentScene(
                                button->
                                    ReloadCurrentScene());
                        duplicateButton
                            .SetClickEventName(
                                button
                                    ->ClickEventName());
                        duplicateButton.SetSortOrder(
                            button->SortOrder());
                        duplicateComponent =
                            &duplicateButton;
                    }
                    // 複製元のUI画像
                    else if (const auto* image =
                        dynamic_cast<
                            const UIImageComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐUI画像
                        auto& duplicateImage =
                            duplicate.AddComponent<
                                UIImageComponent>(
                                    image->TexturePath(),
                                    image->Color());
                        duplicateImage.SetBorder(
                            image->Border());
                        duplicateImage.SetFallbackSize(
                            image->FallbackSize());
                        duplicateImage.SetSortOrder(
                            image->SortOrder());
                        duplicateComponent =
                            &duplicateImage;
                    }
                    // 複製元のUIトグル
                    else if (const auto* toggle =
                        dynamic_cast<
                            const UIToggleComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐUIトグル
                        auto& duplicateToggle =
                            duplicate.AddComponent<
                                UIToggleComponent>(
                                    toggle->Label(),
                                    toggle->IsOn());
                        duplicateToggle.SetFontFamily(
                            toggle->FontFamily());
                        duplicateToggle.SetFontSize(
                            toggle->FontSize());
                        duplicateToggle.SetInteractable(
                            toggle->Interactable());
                        duplicateToggle.SetBoxColor(
                            toggle->BoxColor());
                        duplicateToggle.SetCheckColor(
                            toggle->CheckColor());
                        duplicateToggle.SetTextColor(
                            toggle->TextColor());
                        duplicateToggle.SetFallbackSize(
                            toggle->FallbackSize());
                        duplicateToggle.SetSortOrder(
                            toggle->SortOrder());
                        static_cast<void>(
                            duplicateToggle.
                                ConsumeValueChanged());
                        duplicateComponent =
                            &duplicateToggle;
                    }
                    // 複製元のUIスライダー
                    else if (const auto* slider =
                        dynamic_cast<
                            const UISliderComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐUIスライダー
                        auto& duplicateSlider =
                            duplicate.AddComponent<
                                UISliderComponent>(
                                    slider->MinimumValue(),
                                    slider->MaximumValue(),
                                    slider->Value());
                        duplicateSlider.SetWholeNumbers(
                            slider->WholeNumbers());
                        duplicateSlider.SetInteractable(
                            slider->Interactable());
                        duplicateSlider.
                            SetBackgroundColor(
                                slider->
                                    BackgroundColor());
                        duplicateSlider.SetFillColor(
                            slider->FillColor());
                        duplicateSlider.SetHandleColor(
                            slider->HandleColor());
                        duplicateSlider.SetFallbackSize(
                            slider->FallbackSize());
                        duplicateSlider.SetSortOrder(
                            slider->SortOrder());
                        static_cast<void>(
                            duplicateSlider.
                                ConsumeValueChanged());
                        duplicateComponent =
                            &duplicateSlider;
                    }
                    // 複製元のUI入力欄
                    else if (const auto* inputField =
                        dynamic_cast<
                            const UIInputFieldComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐUI入力欄
                        auto& duplicateField =
                            duplicate.AddComponent<
                                UIInputFieldComponent>(
                                    inputField->Text(),
                                    inputField->
                                        Placeholder());
                        duplicateField.SetFontFamily(
                            inputField->FontFamily());
                        duplicateField.SetFontSize(
                            inputField->FontSize());
                        duplicateField.SetMaxLength(
                            inputField->MaxLength());
                        duplicateField.SetInteractable(
                            inputField->Interactable());
                        duplicateField.
                            SetBackgroundColor(
                                inputField->
                                    BackgroundColor());
                        duplicateField.SetFocusedColor(
                            inputField->FocusedColor());
                        duplicateField.SetTextColor(
                            inputField->TextColor());
                        duplicateField.
                            SetPlaceholderColor(
                                inputField->
                                    PlaceholderColor());
                        duplicateField.SetFallbackSize(
                            inputField->FallbackSize());
                        duplicateField.SetSortOrder(
                            inputField->SortOrder());
                        static_cast<void>(
                            duplicateField.
                                ConsumeValueChanged());
                        duplicateComponent =
                            &duplicateField;
                    }
                    // 複製元のUIレイアウト
                    else if (const auto* layoutGroup =
                        dynamic_cast<
                            const UILayoutGroupComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐUIレイアウト
                        auto& duplicateLayout =
                            duplicate.AddComponent<
                                UILayoutGroupComponent>(
                                    layoutGroup->Axis(),
                                    layoutGroup->
                                        Spacing());
                        duplicateLayout.SetPadding(
                            layoutGroup->Padding());
                        duplicateLayout.
                            SetChildAlignment(
                                layoutGroup->
                                    ChildAlignment());
                        duplicateComponent =
                            &duplicateLayout;
                    }
                    // 複製元のスクロール表示
                    else if (const auto* scrollView =
                        dynamic_cast<
                            const UIScrollViewComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐスクロール表示
                        auto& duplicateScroll =
                            duplicate.AddComponent<
                                UIScrollViewComponent>();
                        duplicateScroll.SetScrollSpeed(
                            scrollView->ScrollSpeed());
                        duplicateScroll.SetInteractable(
                            scrollView->Interactable());
                        duplicateScroll.
                            SetBackgroundColor(
                                scrollView->
                                    BackgroundColor());
                        duplicateScroll.
                            SetScrollbarColor(
                                scrollView->
                                    ScrollbarColor());
                        duplicateScroll.SetSortOrder(
                            scrollView->SortOrder());
                        duplicateComponent =
                            &duplicateScroll;
                    }
                    else if (dynamic_cast<
                        const AudioListenerComponent*>(
                            sourceComponent.get()) != nullptr)
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                AudioListenerComponent>();
                    }
                    // 複製元の音源
                    else if (const auto* audio =
                        dynamic_cast<const AudioSourceComponent*>(sourceComponent.get()))
                    {
                        // 設定を引き継ぐ音源
                        auto& duplicateAudio =
                            duplicate.AddComponent<AudioSourceComponent>(
                            audio->AudioPath(),
                            audio->Volume(),
                            audio->Pitch(),
                            audio->Pan(),
                            audio->Loop(),
                            audio->PlayOnStart(),
                            audio->IsSpatial(),
                            audio->MinimumDistance(),
                            audio->MaximumDistance());
                        duplicateAudio.SetBus(audio->Bus());
                        duplicateAudio.SetStreaming(
                            audio->IsStreaming());
                        duplicateComponent =
                            &duplicateAudio;
                    }
                    // 複製元のモデル描画
                    else if (const auto* model =
                        dynamic_cast<const ModelRendererComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<ModelRendererComponent>(
                            model->ModelPath(),
                            model->IsWireframe(),
                            model->IsMaterialOverrideEnabled(),
                            model->Color(),
                            model->AlbedoTexturePath(),
                            model->NormalTexturePath(),
                            model->Roughness(),
                            model->NormalStrength(),
                            model->MaterialAssetPath(),
                            model->AnimationIndex(),
                            model->AnimationSpeed(),
                            model->AnimationLoop(),
                            model->AnimationPlayOnStart(),
                            model->AnimationControllerPath(),
                            model->ApplyRootMotion(),
                            model->RootMotionNode(),
                            model->PreserveEmbeddedMaterialColor());
                        static_cast<ModelRendererComponent*>(
                            duplicateComponent)
                            ->SetMetallic(
                                model->Metallic());
                        // 設定を引き継ぐモデル描画
                        auto* const duplicateModel =
                            static_cast<
                                ModelRendererComponent*>(
                                    duplicateComponent);
                        duplicateModel->SetUseLegacyShading(
                            model->UsesLegacyShading());
                        duplicateModel->SetRoughnessTexturePath(
                            model->RoughnessTexturePath());
                        duplicateModel->SetMetallicTexturePath(
                            model->MetallicTexturePath());
                        duplicateModel->SetOcclusionTexturePath(
                            model->OcclusionTexturePath());
                        duplicateModel->SetEmissiveTexturePath(
                            model->EmissiveTexturePath());
                        duplicateModel->SetOcclusionStrength(
                            model->OcclusionStrength());
                        duplicateModel->SetEmissiveColor(
                            model->EmissiveColor());
                    }
                    // 複製元のテキスト描画
                    else if (const auto* text =
                        dynamic_cast<const TextRendererComponent*>(sourceComponent.get()))
                    {
                        // 設定を引き継ぐテキスト描画
                        auto& duplicateText =
                            duplicate.AddComponent<TextRendererComponent>(
                                text->Text(),
                                text->FontFamily(),
                                text->FontSize(),
                                text->Color(),
                                text->LayoutSize(),
                                text->WordWrap(),
                                text->HorizontalAlignment(),
                                text->VerticalAlignment());
                        duplicateText.SetSortOrder(
                            text->SortOrder());
                        duplicateComponent = &duplicateText;
                    }
                    // 複製元の自動回転設定
                    else if (const auto* rotator =
                        dynamic_cast<const RotatorComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<RotatorComponent>(
                            rotator->AngularVelocity());
                    }
                    // 複製元の2D揺れ物設定
                    else if (const auto* sway =
                        dynamic_cast<
                            const Sway2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                Sway2DComponent>(
                                    sway->Settings());
                    }
                    // 複製元の2Dリグ
                    else if (const auto* rig =
                        dynamic_cast<
                            const Rig2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                Rig2DComponent>(
                                    rig->Parameters());
                    }
                    // 複製元の2Dキーフォーム
                    else if (const auto* keyform =
                        dynamic_cast<
                            const Keyform2DComponent*>(
                                sourceComponent.get()))
                    {
                        // キーと基準姿勢を引き継ぐキーフォーム
                        auto& duplicateKeyform =
                            duplicate.AddComponent<
                                Keyform2DComponent>(
                                    keyform->Channels());
                        if (keyform->HasRestPose())
                        {
                            duplicateKeyform.SetRestPose(
                                keyform->RestPosition(),
                                keyform->RestRotation(),
                                keyform->RestScale(),
                                keyform->RestOpacity());
                        }
                        duplicateComponent = &duplicateKeyform;
                    }
                    // 複製元の2Dスキン
                    else if (const auto* skin =
                        dynamic_cast<
                            const SpriteSkin2DComponent*>(
                                sourceComponent.get()))
                    {
                        // ボーンとバインドを引き継ぐ2Dスキン
                        auto& duplicateSkin =
                            duplicate.AddComponent<
                                SpriteSkin2DComponent>(
                                    skin->Bones());
                        duplicateSkin.SetWeightFalloff(
                            skin->WeightFalloff());
                        if (skin->IsBound())
                        {
                            static_cast<void>(
                                duplicateSkin.RestoreBinding(
                                    skin->BoneBindPoses(),
                                    skin->SpriteBindPose(),
                                    skin->Weights(),
                                    skin->BoundColumns(),
                                    skin->BoundRows()));
                        }
                        duplicateComponent = &duplicateSkin;
                    }
                    // 複製元の2D瞬き設定
                    else if (const auto* blink =
                        dynamic_cast<
                            const Blink2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                Blink2DComponent>(
                                    blink->Settings());
                    }
                    // 複製元のビルボード設定
                    else if (const auto* billboard =
                        dynamic_cast<
                            const BillboardComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐビルボード
                        auto& duplicateBillboard =
                            duplicate.AddComponent<
                                BillboardComponent>(
                                billboard->Mode(),
                                billboard->FacingAxis());
                        duplicateBillboard
                            .SetTargetPosition(
                                billboard
                                    ->TargetPosition());
                        duplicateComponent =
                            &duplicateBillboard;
                    }
                    // 複製元の変換アニメーション
                    else if (const auto* animator =
                        dynamic_cast<
                            const TransformAnimatorComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                TransformAnimatorComponent>(
                                    animator->ClipPath(),
                                    animator->Speed(),
                                    animator->Loop(),
                                    animator->PlayOnStart(),
                                    animator->ControllerPath());
                    }
                    // 複製元の入力移動設定
                    else if (const auto* inputMover =
                        dynamic_cast<const InputMoverComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                InputMoverComponent>(
                                    inputMover->
                                        HorizontalAction(),
                                    inputMover->
                                        VerticalAction(),
                                    inputMover->Speed());
                    }
                    // 複製元のキャラクター制御
                    else if (const auto* controller =
                        dynamic_cast<
                            const CharacterControllerComponent*>(
                                sourceComponent.get()))
                    {
                        // 設定を引き継ぐキャラクター制御
                        auto& duplicateController =
                            duplicate.AddComponent<
                                CharacterControllerComponent>(
                                    controller->Radius(),
                                    controller->Height(),
                                    controller->MoveSpeed(),
                                    controller->Gravity(),
                                    controller->JumpSpeed(),
                                    controller->StepOffset(),
                                    controller->SkinWidth(),
                                    controller->Layer(),
                                    controller->CollisionMask());
                        duplicateController.SetUseInput(
                            controller->UseInput());
                        duplicateController.SetHorizontalAction(
                            controller->HorizontalAction());
                        duplicateController.SetVerticalAction(
                            controller->VerticalAction());
                        duplicateController.SetJumpAction(
                            controller->JumpAction());
                        duplicateComponent = &duplicateController;
                    }
                    // 複製元の物理ボディ
                    else if (const auto* rigidbody =
                        dynamic_cast<const RigidbodyComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<RigidbodyComponent>(
                            rigidbody->Velocity(),
                            rigidbody->UsesGravity(),
                            rigidbody->IsKinematic(),
                            rigidbody->CollisionDetection(),
                            rigidbody->Mass(),
                            rigidbody->AngularVelocity(),
                            rigidbody->CenterOfMass(),
                            rigidbody->LinearDrag(),
                            rigidbody->AngularDrag(),
                            rigidbody->Constraints(),
                            rigidbody->Interpolates());
                    }
                    // 接続先を引き継ぐジョイント
                    else if (const auto* joint =
                        dynamic_cast<const JointComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<JointComponent>(
                                joint->Type(),
                                joint->ConnectedBodyId(),
                                joint->Anchor(),
                                joint->ConnectedAnchor(),
                                joint->Axis(),
                                joint->RestLength(),
                                joint->Stiffness(),
                                joint->Damping(),
                                joint->CollideConnected(),
                                joint->UseLimits(),
                                joint->Limits(),
                                joint->UseMotor(),
                                joint->Motor());
                    }
                    // 参照先を引き継ぐLOD設定
                    else if (const auto* lodGroup =
                        dynamic_cast<const LODGroupComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                LODGroupComponent>(
                                    lodGroup->Levels(),
                                    lodGroup->
                                        CullDistance());
                    }
                    // 複製元のネットワーク識別設定
                    else if (const auto* identity = dynamic_cast<const NetworkIdentityComponent*>(sourceComponent.get()))
                    {
                        // 重複を避ける通信のシーンキー
                        std::string key = identity->SceneKey();
                        if (!key.empty())
                        {
                            // 通信キーに加える複製番号
                            const auto suffix = ".copy" + std::to_string(duplicate.Id());
                            key.resize(std::min(key.size(), 64 - suffix.size()));
                            key += suffix;
                        }
                        // 新しいキーで生成した通信設定
                        auto& copy = duplicate.AddComponent<NetworkIdentityComponent>(key);
                        copy.SetHostOnlySimulation(identity->HostOnlySimulation());
                        copy.SetInterpolationSeconds(identity->InterpolationSeconds());
                        duplicateComponent = &copy;
                    }
                    // 複製元のネイティブスクリプト
                    else if (const auto* nativeScript =
                        dynamic_cast<
                            const NativeScriptComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                NativeScriptComponent>(
                                    nativeScript->ScriptType(),
                                    nativeScript->
                                        SerializedProperties());
                    }

                    if (duplicateComponent != nullptr)
                    {
                        duplicateComponent->SetEnabled(sourceComponent->IsEnabled());
                    }
                }

                // 再帰的に複製する子
                for (const auto* sourceChild : sourceObject.Children())
                {
                    self(self, *sourceChild, &duplicate, false);
                }

                return duplicate;
            };

        try
        {
            // 生成または複製したルート
            auto& result =
                clone(clone, source, targetParent, true);
            // 複製番号の対応(sourceId: 複製元の番号, duplicateId: 複製した対象の番号)。
            for (const auto& [sourceId, duplicateId] :
                duplicatedIds)
            {
                static_cast<void>(sourceId);
                // 新たに生成した複製先
                auto* duplicate = FindGameObject(
                    duplicateId);
                // 接続先を引き継ぐジョイント
                auto* joint = duplicate != nullptr
                    ? duplicate->GetComponent<JointComponent>()
                    : nullptr;
                if (joint != nullptr)
                {
                    // 複製先番号の対応表の位置
                    if (const auto target =
                            duplicatedIds.find(
                                joint->ConnectedBodyId());
                        target != duplicatedIds.end())
                    {
                        joint->SetConnectedBodyId(
                            target->second);
                    }
                }
                // 参照先を引き継ぐLOD設定
                if (auto* lodGroup =
                        duplicate != nullptr
                        ? duplicate->GetComponent<
                            LODGroupComponent>()
                        : nullptr)
                {
                    // 参照先を引き継ぐLOD段階
                    auto levels =
                        lodGroup->Levels();
                    // 参照番号を変えるLOD段階
                    for (auto& level : levels)
                    {
                        // 複製先番号の対応表の位置
                        if (const auto target =
                                duplicatedIds.find(
                                    level.targetId);
                            target
                                != duplicatedIds.end())
                        {
                            level.targetId =
                                target->second;
                        }
                    }
                    lodGroup->SetLevels(
                        std::move(levels));
                }
                // 一緒に複製したボーンへ付け替える2Dスキン
                if (auto* skin =
                        duplicate != nullptr
                        ? duplicate->GetComponent<
                            SpriteSkin2DComponent>()
                        : nullptr)
                {
                    // 付け替えるボーンID列
                    auto bones = skin->Bones();
                    // 付け替えるボーンID
                    for (auto& bone : bones)
                    {
                        // 複製先番号の対応表の位置
                        if (const auto target =
                                duplicatedIds.find(bone);
                            target != duplicatedIds.end())
                        {
                            bone = target->second;
                        }
                    }
                    static_cast<void>(
                        skin->RemapBones(std::move(bones)));
                }
            }
            return result;
        }
        catch (...)
        {
            if (duplicatedRoot != nullptr
                && FindGameObject(duplicatedRoot->Id()) == duplicatedRoot)
            {
                DestroyGameObject(*duplicatedRoot);
            }
            throw;
        }
    }

    GameObject* Scene::FindGameObject(const GameObjectId id) const noexcept
    {
        // 照合するシーンのオブジェクト
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->Id() == id)
            {
                return gameObject.get();
            }
        }

        return nullptr;
    }

    GameObject* Scene::FindGameObjectByName(
        const std::string_view name) const noexcept
    {
        // 照合するシーンのオブジェクト
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->Name() == name)
            {
                return gameObject.get();
            }
        }
        return nullptr;
    }

    std::vector<GameObject*> Scene::FindGameObjectsByName(
        const std::string_view name) const
    {
        // 名前やタグが一致した対象一覧
        std::vector<GameObject*> results;
        // 照合するシーンのオブジェクト
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->Name() == name)
            {
                results.push_back(gameObject.get());
            }
        }
        return results;
    }

    GameObject* Scene::FindGameObjectByTag(
        const std::string_view tag) const noexcept
    {
        // 照合するシーンのオブジェクト
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->CompareTag(tag))
            {
                return gameObject.get();
            }
        }
        return nullptr;
    }

    std::vector<GameObject*> Scene::FindGameObjectsByTag(
        const std::string_view tag) const
    {
        // 名前やタグが一致した対象一覧
        std::vector<GameObject*> results;
        // 照合するシーンのオブジェクト
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->CompareTag(tag))
            {
                results.push_back(gameObject.get());
            }
        }
        return results;
    }

    bool Scene::IsTagRegistered(
        const std::string_view tag) const noexcept
    {
        // 比較する登録タグ
        for (const auto& registered : m_registeredTags)
        {
            if (registered == tag)
            {
                return true;
            }
        }
        return false;
    }

    void Scene::WarnUnregisteredTag(
        const GameObject& gameObject) const
    {
        if (m_registeredTags.empty()
            || gameObject.Tag().empty()
            || IsTagRegistered(gameObject.Tag()))
        {
            return;
        }
        Logger::Instance().Warning(
            "未登録のタグ「" + gameObject.Tag()
                + "」が「" + gameObject.Name()
                + "」で使われています。プロジェクト設定のタグ一覧へ"
                  "追加してください。",
            gameObject.Id());
    }

    bool Scene::ReorderGameObject(
        GameObject& moved,
        const GameObject& reference,
        const bool insertAfter)
    {
        if (&moved == &reference
            || FindGameObject(moved.Id()) != &moved
            || FindGameObject(reference.Id())
                != &reference)
        {
            return false;
        }
        // 自分の子孫を基準に動かすと、自分の中へ自分を入れる形になってしまいます。
        // 移動対象の子孫か調べる祖先
        for (const auto* ancestor = reference.Parent();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
            if (ancestor == &moved)
            {
                return false;
            }
        }

        // 対象と子孫を一緒に移し、保存順で親が子より先に来る並びを保ちます。
        // 一緒に移す対象と子孫の番号
        std::unordered_set<GameObjectId> movedIds;
        // 移動対象の子孫番号を集めます(self: 再帰用の自身, current: 収集する対象)。
        const auto collectSubtree =
            [&movedIds](
                const auto& self,
                const GameObject& current) -> void
            {
                movedIds.insert(current.Id());
                // 対象の直下の子
                for (const auto* child : current.Children())
                {
                    self(self, *child);
                }
            };
        collectSubtree(collectSubtree, moved);

        // 基準が動かす側に含まれていたら何もしません。
        if (movedIds.contains(reference.Id()))
        {
            return false;
        }

        // まとまりを相対順序のまま取り出します。
        // 相対順序を保って取り外した対象
        std::vector<std::unique_ptr<GameObject>> detached;
        detached.reserve(movedIds.size());
        // 照合する所有オブジェクト
        for (auto& candidate : m_gameObjects)
        {
            if (candidate != nullptr
                && movedIds.contains(candidate->Id()))
            {
                detached.push_back(std::move(candidate));
            }
        }
        std::erase(
            m_gameObjects,
            std::unique_ptr<GameObject>{});

        // 対象を取り外した後の位置で基準を探し直します。
        // 基準対象に一致するか調べます(candidate: 所有するオブジェクト)。
        // 取り外した後の基準対象の位置
        const auto referencePosition = std::ranges::find_if(
            m_gameObjects,
            [&reference](
                const std::unique_ptr<GameObject>& candidate)
            {
                return candidate.get() == &reference;
            });
        if (referencePosition == m_gameObjects.end())
        {
            // 元へ戻す対象物体
            for (auto& object : detached)
            {
                m_gameObjects.push_back(std::move(object));
            }
            return false;
        }

        // 対象群を挿入する位置
        const auto insertPosition = insertAfter
            ? std::next(referencePosition)
            : referencePosition;
        m_gameObjects.insert(
            insertPosition,
            std::make_move_iterator(detached.begin()),
            std::make_move_iterator(detached.end()));

        // 同じ親の子を移す場合は、保存順と兄弟の表示順を合わせます。
        // 移動する対象の親
        if (auto* const parent = moved.Parent();
            parent != nullptr
            && reference.Parent() == parent)
        {
            // 表示順を更新する兄弟一覧
            auto& siblings = parent->m_children;
            std::erase(siblings, &moved);
            // 兄弟一覧の基準対象の位置
            const auto siblingPosition =
                std::ranges::find(siblings, &reference);
            if (siblingPosition == siblings.end())
            {
                siblings.push_back(&moved);
            }
            else
            {
                siblings.insert(
                    insertAfter
                        ? std::next(siblingPosition)
                        : siblingPosition,
                    &moved);
            }
        }
        return true;
    }

    bool Scene::DestroyGameObject(GameObject& gameObject)
    {
        if (FindGameObject(gameObject.Id()) != &gameObject)
        {
            return false;
        }

        // 対象と子孫の削除する番号
        std::unordered_set<GameObjectId> idsToRemove;
        // 削除対象の子孫番号を集めます(self: 再帰用の自身, current: 収集する対象)。
        const auto collectChildren =
            [&idsToRemove](const auto& self, const GameObject& current) -> void
            {
                idsToRemove.insert(current.Id());
                // 対象の直下の子
                for (const auto* child : current.Children())
                {
                    self(self, *child);
                }
            };
        collectChildren(collectChildren, gameObject);

        if (m_mainCamera != nullptr
            && idsToRemove.contains(m_mainCamera->Owner().Id()))
        {
            m_mainCamera = nullptr;
        }

        gameObject.SetParent(nullptr);
        // 削除対象の番号か調べます(candidate: 所有するオブジェクト)。
        std::erase_if(
            m_gameObjects,
            [&idsToRemove](const std::unique_ptr<GameObject>& candidate)
            {
                return idsToRemove.contains(candidate->Id());
            });
        // 削除後に残るオブジェクト
        for (const auto& remaining : m_gameObjects)
        {
            // 接続先を解除するジョイント
            if (auto* joint =
                    remaining->GetComponent<JointComponent>();
                joint != nullptr
                && idsToRemove.contains(
                    joint->ConnectedBodyId()))
            {
                joint->SetConnectedBodyId(0);
            }
            // 削除対象を参照するLOD設定
            if (auto* lodGroup =
                    remaining->GetComponent<
                        LODGroupComponent>())
            {
                // 参照先を更新するLOD段階
                auto levels =
                    lodGroup->Levels();
                // LOD対象の参照を変更した状態
                bool changed{};
                // 削除対象を調べるLOD段階
                for (auto& level : levels)
                {
                    if (idsToRemove.contains(
                        level.targetId))
                    {
                        level.targetId = 0;
                        changed = true;
                    }
                }
                if (changed)
                {
                    lodGroup->SetLevels(
                        std::move(levels));
                }
            }
        }
        return true;
    }

    void Scene::DontDestroyOnLoad(
        GameObject& gameObject,
        std::string persistenceKey)
    {
        if (FindGameObject(gameObject.Id()) != &gameObject)
        {
            throw std::invalid_argument(
                "Persistent GameObject does not belong to this Scene.");
        }
        if (gameObject.Parent() != nullptr)
        {
            throw std::invalid_argument(
                "Only a root GameObject can persist across scene loads.");
        }
        if (persistenceKey.empty())
        {
            persistenceKey = gameObject.Name();
        }
        if (persistenceKey.empty())
        {
            persistenceKey =
                "PersistentGameObject:"
                + std::to_string(gameObject.Id());
        }
        // 照合する所有オブジェクト
        for (const auto& candidate : m_gameObjects)
        {
            if (candidate.get() != &gameObject
                && candidate->m_persistent
                && candidate->m_persistenceKey
                    == persistenceKey)
            {
                throw std::invalid_argument(
                    "Persistence keys must be unique in a Scene.");
            }
        }
        gameObject.m_persistent = true;
        gameObject.m_persistenceKey =
            std::move(persistenceKey);
    }

    void Scene::DestroyOnLoad(GameObject& gameObject)
    {
        if (FindGameObject(gameObject.Id()) != &gameObject)
        {
            throw std::invalid_argument(
                "GameObject does not belong to this Scene.");
        }
        gameObject.m_persistent = false;
        gameObject.m_persistenceKey.clear();
    }

    SceneHandle Scene::FindAdditiveScene(
        const std::filesystem::path& path)
            const noexcept
    {
        if (path.empty())
        {
            return PrimarySceneHandle();
        }
        // 字句正規化した生成元パス
        const auto normalized =
            path.lexically_normal();
        // 照合する追加シーンの情報
        for (const auto& scene : m_additiveScenes)
        {
            if (scene.path == normalized)
            {
                return scene.handle;
            }
        }
        return PrimarySceneHandle();
    }

    const LoadedSceneInfo* Scene::FindAdditiveScene(
        const SceneHandle handle) const noexcept
    {
        // 照合する追加シーンの情報
        for (const auto& scene : m_additiveScenes)
        {
            if (scene.handle == handle)
            {
                return &scene;
            }
        }
        return nullptr;
    }

    bool Scene::UnloadScene(const SceneHandle handle)
    {
        if (handle == PrimarySceneHandle())
        {
            Logger::Instance().Warning(
                "主シーンはUnloadSceneでは破棄できません。"
                "切り替えにはSceneManagerのRequestLoadを"
                "使ってください。");
            return false;
        }

        // 指定した番号の追加シーンか調べます(scene: 追加シーンの情報)。
        // 破棄する追加シーンの格納位置
        const auto entry = std::find_if(
            m_additiveScenes.begin(),
            m_additiveScenes.end(),
            [handle](const LoadedSceneInfo& scene)
            {
                return scene.handle == handle;
            });
        if (entry == m_additiveScenes.end())
        {
            return false;
        }
        // ログに残す追加シーンのパス
        const auto unloadedPath = entry->path;

        // 先にルートを破棄し、主シーン側に親付けされた対象も二周目で回収します。
        // ルート優先で削除する走査番号
        for (int pass = 0; pass < 2; ++pass)
        {
            // 今回の走査で削除する対象
            std::vector<GameObject*> targets;
            // 所属や識別番号を調べる対象
            for (const auto& object : m_gameObjects)
            {
                if (object->m_sourceScene != handle)
                {
                    continue;
                }
                if (pass == 0
                    && object->Parent() != nullptr)
                {
                    continue;
                }
                targets.push_back(object.get());
            }
            // 追加シーンから削除する対象
            for (auto* target : targets)
            {
                if (FindGameObject(target->Id())
                    == target)
                {
                    static_cast<void>(
                        DestroyGameObject(*target));
                }
            }
        }

        m_additiveScenes.erase(entry);
        Logger::Instance().Info(
            "追加シーンを破棄しました: "
            + PathToUtf8(unloadedPath));
        return true;
    }

    bool Scene::UnloadScene(
        const std::filesystem::path& path)
    {
        // 破棄する追加シーン番号
        const auto handle = FindAdditiveScene(path);
        if (handle == PrimarySceneHandle())
        {
            return false;
        }
        return UnloadScene(handle);
    }

    void Scene::UnloadAllAdditiveScenes()
    {
        while (!m_additiveScenes.empty())
        {
            // 破棄する追加シーン番号
            const auto handle =
                m_additiveScenes.back().handle;
            if (!UnloadScene(handle))
            {
                // 想定外の状態でも無限ループさせません。
                m_additiveScenes.pop_back();
            }
        }
    }

    Scene::PersistentTransfer
        Scene::ExtractPersistentObjects()
    {
        // シーンをまたいで移す所有対象
        PersistentTransfer transfer;
        // 引き継ぐルートと子孫の番号
        std::unordered_set<GameObjectId>
            persistentIds;
        // 保持する子孫番号を集めます(self: 再帰用の自身, object: 収集する対象)。
        const auto collect =
            [&persistentIds](
                const auto& self,
                const GameObject& object) -> void
            {
                persistentIds.insert(object.Id());
                // 対象の直下の子
                for (const auto* child : object.Children())
                {
                    self(self, *child);
                }
            };
        // 所属や識別番号を調べる対象
        for (const auto& object : m_gameObjects)
        {
            if (object->m_persistent
                && object->Parent() == nullptr)
            {
                collect(collect, *object);
            }
        }
        if (persistentIds.empty())
        {
            return transfer;
        }

        if (m_mainCamera != nullptr
            && persistentIds.contains(
                m_mainCamera->Owner().Id()))
        {
            transfer.mainCamera = m_mainCamera;
            m_mainCamera = nullptr;
        }
        transfer.objects.reserve(
            persistentIds.size());
        // 所属や識別番号を調べる対象
        for (auto& object : m_gameObjects)
        {
            if (persistentIds.contains(object->Id()))
            {
                transfer.objects.push_back(
                    std::move(object));
            }
        }
        std::erase(
            m_gameObjects,
            std::unique_ptr<GameObject>{});
        m_activeCollisions.clear();
        return transfer;
    }

    void Scene::MergePersistentObjects(
        PersistentTransfer transfer)
    {
        if (transfer.objects.empty())
        {
            return;
        }

        // 引き継ぐルートの保持キー
        std::unordered_set<std::string>
            incomingKeys;
        // 引き継ぐオブジェクトの番号
        std::unordered_set<GameObjectId>
            incomingIds;
        // 番号衝突の解消に使う次の番号
        GameObjectId nextId = m_nextId;
        // 所属や識別番号を調べる対象
        for (const auto& object : transfer.objects)
        {
            incomingIds.insert(object->Id());
            nextId = std::max(
                nextId,
                object->Id() + 1);
            if (object->m_persistent
                && object->Parent() == nullptr)
            {
                incomingKeys.insert(
                    object->m_persistenceKey);
            }
        }

        // 保持キーが重複する既存ルート
        std::vector<GameObject*> duplicates;
        // 所属や識別番号を調べる対象
        for (const auto& object : m_gameObjects)
        {
            if (object->m_persistent
                && object->Parent() == nullptr
                && incomingKeys.contains(
                    object->m_persistenceKey))
            {
                duplicates.push_back(object.get());
            }
        }
        // 保持キーが重複した削除対象
        for (auto* duplicate : duplicates)
        {
            if (FindGameObject(duplicate->Id())
                == duplicate)
            {
                DestroyGameObject(*duplicate);
            }
        }

        // 所属や識別番号を調べる対象
        for (const auto& object : m_gameObjects)
        {
            nextId = std::max(
                nextId,
                object->Id() + 1);
        }
        // 所属や識別番号を調べる対象
        for (const auto& object : m_gameObjects)
        {
            if (incomingIds.contains(object->Id()))
            {
                object->m_id = nextId++;
            }
        }

        // 所属や識別番号を調べる対象
        for (auto& object : transfer.objects)
        {
            object->m_scene = this;
            // 引き継いだ対象は元の追加シーンがなくなるため主シーンの所属へ変えます。
            object->m_sourceScene =
                PrimarySceneHandle();
            m_gameObjects.push_back(
                std::move(object));
        }
        if (transfer.mainCamera != nullptr)
        {
            m_mainCamera = transfer.mainCamera;
        }
        m_nextId = nextId;
        m_activeCollisions.clear();
    }

    bool Scene::RemoveComponent(GameObject& gameObject, Component& component)
    {
        if (m_mainCamera == &component)
        {
            m_mainCamera = nullptr;
        }

        return gameObject.RemoveComponent(component);
    }

    void Scene::SetAmbientLightColor(
        const DirectX::XMFLOAT3& color) noexcept
    {
        m_ambientLightColor = {
            std::clamp(color.x, 0.0f, 1.0f),
            std::clamp(color.y, 0.0f, 1.0f),
            std::clamp(color.z, 0.0f, 1.0f)
        };
    }

    void Scene::SetAmbientLightIntensity(
        const float intensity) noexcept
    {
        m_ambientLightIntensity =
            std::clamp(intensity, 0.0f, 4.0f);
    }

    void Scene::SetSkySettings(
        const SkySettings& settings) noexcept
    {
        m_sky = settings;
        // 空のRGBを0〜8へ制限します(color: 有限なRGB色)。
        const auto clampColor =
            [](DirectX::XMFLOAT3 color)
            {
                return DirectX::XMFLOAT3{
                    std::clamp(color.x, 0.0f, 8.0f),
                    std::clamp(color.y, 0.0f, 8.0f),
                    std::clamp(color.z, 0.0f, 8.0f)
                };
            };
        m_sky.topColor = clampColor(m_sky.topColor);
        m_sky.horizonColor =
            clampColor(m_sky.horizonColor);
        m_sky.groundColor =
            clampColor(m_sky.groundColor);
        m_sky.intensity =
            std::clamp(m_sky.intensity, 0.0f, 8.0f);
    }

    bool Scene::ResolveSkySun(
        DirectX::XMFLOAT3& directionToSun,
        DirectX::XMFLOAT3& color,
        float& angularRadius) const noexcept
    {
        // 更新または設定を調べる対象
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }
            // 有効性を調べる平行光源
            const auto* light =
                gameObject->GetComponent<
                    DirectionalLightComponent>();
            if (light == nullptr || !light->IsEnabled())
            {
                continue;
            }
            // WorldDirection()は光が進む向きなので、太陽へ向かう向きはその逆です。
            // 平行光がワールドで進む方向
            const auto travel = light->WorldDirection();
            directionToSun = {
                -travel.x,
                -travel.y,
                -travel.z
            };
            // 平行光のRGB色
            const auto lightColor = light->Color();
            // 平行光の明るさ
            const float intensity = light->Intensity();
            color = {
                lightColor.x * intensity,
                lightColor.y * intensity,
                lightColor.z * intensity
            };
            angularRadius = DirectX::XMConvertToRadians(
                light->AngularDiameterDegrees() * 0.5f);
            return true;
        }
        return false;
    }

    SkySettings Scene::ResolvedSky() const noexcept
    {
        if (!m_sky.sunDriven)
        {
            return m_sky;
        }
        // 太陽へ向かうワールド方向
        DirectX::XMFLOAT3 directionToSun{};
        // 強さを含む太陽のRGB色
        DirectX::XMFLOAT3 color{};
        // 太陽の角半径のラジアン値
        float angularRadius{};
        if (!ResolveSkySun(directionToSun, color, angularRadius))
        {
            // 有効な方向光がなければ保存した空の設定を使います。
            return m_sky;
        }
        // 太陽方向を反映する空の三色
        const auto evaluated =
            EvaluateSunDrivenSky(directionToSun);
        // 太陽方向を反映した空の設定
        SkySettings resolved = m_sky;
        resolved.topColor = evaluated.topColor;
        resolved.horizonColor = evaluated.horizonColor;
        resolved.groundColor = evaluated.groundColor;
        return resolved;
    }

    void Scene::SetFogSettings(
        const FogSettings& settings) noexcept
    {
        m_fog = settings;
        m_fog.color = {
            std::clamp(m_fog.color.x, 0.0f, 8.0f),
            std::clamp(m_fog.color.y, 0.0f, 8.0f),
            std::clamp(m_fog.color.z, 0.0f, 8.0f)
        };
        m_fog.startDistance =
            std::clamp(
                m_fog.startDistance,
                0.0f,
                100000.0f);
        m_fog.endDistance =
            std::clamp(
                m_fog.endDistance,
                m_fog.startDistance + 0.01f,
                100000.0f);
        m_fog.density =
            std::clamp(m_fog.density, 0.0f, 10.0f);
    }

    void Scene::SetAmbientOcclusionSettings(
        const AmbientOcclusionSettings& settings) noexcept
    {
        m_ambientOcclusion = settings;
        m_ambientOcclusion.radius =
            std::clamp(m_ambientOcclusion.radius, 0.01f, 10.0f);
        m_ambientOcclusion.strength =
            std::clamp(m_ambientOcclusion.strength, 0.0f, 1.0f);
    }

    void Scene::SetTemporalAntiAliasingSettings(
        const TemporalAntiAliasingSettings& settings)
        noexcept
    {
        m_temporalAntiAliasing = settings;
        // 0.98より上は履歴がほぼ抜けなくなり、動きに追従しません。
        m_temporalAntiAliasing.historyWeight = std::clamp(
            m_temporalAntiAliasing.historyWeight,
            0.0f,
            0.98f);
        m_temporalAntiAliasing.jitterScale = std::clamp(
            m_temporalAntiAliasing.jitterScale,
            0.0f,
            2.0f);
        m_temporalAntiAliasing.clampTolerance = std::clamp(
            m_temporalAntiAliasing.clampTolerance,
            0.0f,
            8.0f);
    }

    void Scene::SetScreenSpaceReflectionSettings(
        const ScreenSpaceReflectionSettings& settings)
        noexcept
    {
        m_screenSpaceReflection = settings;
        m_screenSpaceReflection.intensity = std::clamp(
            m_screenSpaceReflection.intensity,
            0.0f,
            1.0f);
        m_screenSpaceReflection.maximumDistance =
            std::clamp(
                m_screenSpaceReflection.maximumDistance,
                0.1f,
                1000.0f);
        m_screenSpaceReflection.stepCount =
            std::clamp<std::uint32_t>(
                m_screenSpaceReflection.stepCount,
                4u,
                128u);
        m_screenSpaceReflection.thickness = std::clamp(
            m_screenSpaceReflection.thickness,
            0.01f,
            100.0f);
        m_screenSpaceReflection.roughnessCutoff =
            std::clamp(
                m_screenSpaceReflection.roughnessCutoff,
                0.0f,
                1.0f);
    }

    void Scene::SetVolumetricLightSettings(
        const VolumetricLightSettings& settings) noexcept
    {
        m_volumetricLight = settings;
        m_volumetricLight.intensity = std::clamp(
            m_volumetricLight.intensity,
            0.0f,
            4.0f);
        m_volumetricLight.sampleCount =
            std::clamp<std::uint32_t>(
                m_volumetricLight.sampleCount,
                4u,
                128u);
        m_volumetricLight.maximumDistance = std::clamp(
            m_volumetricLight.maximumDistance,
            1.0f,
            10000.0f);
        m_volumetricLight.scattering = std::clamp(
            m_volumetricLight.scattering,
            0.0f,
            0.95f);
    }

    void Scene::SetBloomSettings(
        const BloomSettings& settings) noexcept
    {
        m_bloom = settings;
        m_bloom.threshold =
            std::clamp(m_bloom.threshold, 0.0f, 4.0f);
        m_bloom.intensity =
            std::clamp(m_bloom.intensity, 0.0f, 8.0f);
        m_bloom.radius =
            std::clamp(m_bloom.radius, 0.25f, 12.0f);
    }

    void Scene::SetScreenOutlineSettings(
        const ScreenOutlineSettings& settings) noexcept
    {
        m_screenOutline = settings;
        m_screenOutline.color = {
            std::clamp(m_screenOutline.color.x, 0.0f, 1.0f),
            std::clamp(m_screenOutline.color.y, 0.0f, 1.0f),
            std::clamp(m_screenOutline.color.z, 0.0f, 1.0f)
        };
        m_screenOutline.intensity = std::clamp(
            m_screenOutline.intensity,
            0.0f,
            1.0f);
        m_screenOutline.thickness = std::clamp(
            m_screenOutline.thickness,
            1.0f,
            4.0f);
        m_screenOutline.depthThreshold = std::clamp(
            m_screenOutline.depthThreshold,
            0.0001f,
            1.0f);
        m_screenOutline.normalThreshold = std::clamp(
            m_screenOutline.normalThreshold,
            0.0f,
            1.0f);
    }

    void Scene::SetScreenSpaceLensFlareSettings(
        const ScreenSpaceLensFlareSettings& settings) noexcept
    {
        m_screenSpaceLensFlare = settings;
        m_screenSpaceLensFlare.threshold = std::clamp(
            m_screenSpaceLensFlare.threshold,
            0.0f,
            16.0f);
        m_screenSpaceLensFlare.intensity = std::clamp(
            m_screenSpaceLensFlare.intensity,
            0.0f,
            8.0f);
        m_screenSpaceLensFlare.ghostDispersal = std::clamp(
            m_screenSpaceLensFlare.ghostDispersal,
            0.01f,
            2.0f);
        m_screenSpaceLensFlare.haloWidth = std::clamp(
            m_screenSpaceLensFlare.haloWidth,
            0.05f,
            1.5f);
        m_screenSpaceLensFlare.chromaticAberration = std::clamp(
            m_screenSpaceLensFlare.chromaticAberration,
            0.0f,
            1.0f);
        m_screenSpaceLensFlare.streakIntensity = std::clamp(
            m_screenSpaceLensFlare.streakIntensity,
            0.0f,
            4.0f);
        m_screenSpaceLensFlare.streakLength = std::clamp(
            m_screenSpaceLensFlare.streakLength,
            0.0f,
            1.0f);
        m_screenSpaceLensFlare.streakDirections = std::clamp(
            m_screenSpaceLensFlare.streakDirections,
            1u,
            4u);
    }

    void Scene::SetDepthOfFieldSettings(
        const DepthOfFieldSettings& settings) noexcept
    {
        m_depthOfField = settings;
        // 焦点距離を正に保ち、0除算を避けます。
        m_depthOfField.focusDistance = std::clamp(
            m_depthOfField.focusDistance,
            0.01f,
            10000.0f);
        m_depthOfField.focusRange = std::clamp(
            m_depthOfField.focusRange,
            0.0f,
            1000.0f);
        m_depthOfField.blurStrength = std::clamp(
            m_depthOfField.blurStrength,
            0.0f,
            8.0f);
        // 標本数を保ったままぼけの粒を抑えるため半径を32画素までに制限します。
        m_depthOfField.maximumRadius = std::clamp(
            m_depthOfField.maximumRadius,
            0.0f,
            32.0f);
    }

    void Scene::SetMotionBlurSettings(
        const MotionBlurSettings& settings) noexcept
    {
        m_motionBlur = settings;
        m_motionBlur.intensity = std::clamp(
            m_motionBlur.intensity,
            0.0f,
            4.0f);
        // 標本間隔による縞を抑えるためブラー半径を64画素までに制限します。
        m_motionBlur.maximumRadius = std::clamp(
            m_motionBlur.maximumRadius,
            0.0f,
            64.0f);
    }

    void Scene::SetAutoExposureSettings(
        const AutoExposureSettings& settings) noexcept
    {
        m_autoExposure = settings;
        m_autoExposure.keyValue = std::clamp(
            m_autoExposure.keyValue,
            0.01f,
            2.0f);
        m_autoExposure.minimumLuminance = std::clamp(
            m_autoExposure.minimumLuminance,
            0.0001f,
            100.0f);
        // 自動露出の輝度上限を下限以上に保ちます。
        m_autoExposure.maximumLuminance = std::clamp(
            m_autoExposure.maximumLuminance,
            m_autoExposure.minimumLuminance,
            100.0f);
        m_autoExposure.speedToBright = std::clamp(
            m_autoExposure.speedToBright,
            0.0f,
            20.0f);
        m_autoExposure.speedToDark = std::clamp(
            m_autoExposure.speedToDark,
            0.0f,
            20.0f);
    }

    PostProcessFrame Scene::PostProcessFrameData() const
    {
        // 現在の設定と保存した描画情報
        PostProcessFrame frame{};
        frame.bloom = m_bloom;
        frame.screenOutline = m_screenOutlineFrame;
        frame.screenOutline.settings = m_screenOutline;
        frame.lensFlare = m_screenSpaceLensFlare;
        frame.colorGrading = m_colorGrading;
        frame.volumetric = m_volumetricFrame;
        frame.temporal = m_temporalFrame;
        // 直前の3D描画の行列へ最新の設定を合わせ、設定変更の遅延を避けます。
        frame.depthOfField = m_depthOfFieldFrame;
        frame.depthOfField.settings = m_depthOfField;
        frame.motionBlur = m_motionBlurFrame;
        frame.motionBlur.settings = m_motionBlur;
        frame.autoExposure.settings = m_autoExposure;
        // 一時停止中も露出が順応するよう実時間で進めます。
        frame.autoExposure.deltaSeconds =
            Time::UnscaledDeltaTime();
        return frame;
    }

    void Scene::SetColorGradingSettings(
        const ColorGradingSettings& settings) noexcept
    {
        m_colorGrading = settings;
        m_colorGrading.exposure =
            std::clamp(m_colorGrading.exposure, -8.0f, 8.0f);
        m_colorGrading.contrast =
            std::clamp(m_colorGrading.contrast, 0.0f, 4.0f);
        m_colorGrading.saturation =
            std::clamp(m_colorGrading.saturation, 0.0f, 4.0f);
        m_colorGrading.temperature =
            std::clamp(m_colorGrading.temperature, -2.0f, 2.0f);
        m_colorGrading.tint =
            std::clamp(m_colorGrading.tint, -2.0f, 2.0f);
        m_colorGrading.vignette =
            std::clamp(m_colorGrading.vignette, 0.0f, 1.0f);
    }

    void Scene::SetPhysicsBroadPhaseCellSize(
        const float size) noexcept
    {
        m_physicsBroadPhaseCellSize = std::clamp(
            size,
            0.25f,
            100.0f);
    }

    void Scene::Clear() noexcept
    {
        ++m_contentRevision;
        m_mainCamera = nullptr;
        m_gameObjects.clear();
        m_nextId = 1;
        m_loadingScene = PrimarySceneHandle();
        m_nextSceneHandle = 1;
        m_additiveScenes.clear();
        // Cameraが消えるので、名前付きレンダーテクスチャも手放します（次のフレームで必要な分だけ作り直されます）。
        m_graphics.ClearRenderTextures();
        m_activeCollisions.clear();
        m_ambientLightColor = { 0.65f, 0.72f, 0.85f };
        m_ambientLightIntensity = 0.35f;
        m_sky = {};
        m_fog = {};
        m_bloom = {};
        m_screenOutline = {};
        m_screenSpaceLensFlare = {};
        m_depthOfField = {};
        m_depthOfFieldFrame = {};
        m_screenOutlineFrame = {};
        m_motionBlur = {};
        m_motionBlurFrame = {};
        m_autoExposure = {};
        m_ambientOcclusion = {};
        // 設定と前シーンの行列・ジッター・描画参照を初期化します。
        m_screenSpaceReflection = {};
        m_temporalAntiAliasing = {};
        m_temporalFrame = {};
        m_temporalFrameIndex = 0;
        m_volumetricLight = {};
        m_volumetricFrame = {};
        m_colorGrading = {};
        m_bakedGiSettings = {};
        m_bakedGiBakedShape = {};
        m_bakedGiData.clear();
        m_bakedGiViews = {};
        m_bakedGiTexturesDirty = false;
        m_bakedGiBaking = false;
        m_bakedGiNextProbe = 0;
        m_bakedGiWorking.clear();
        m_physicsBroadPhaseCellSize = 4.0f;
        m_physicsStats = {};
        m_physicsDebugContacts.clear();
        m_physicsClock = {};
        m_renderingInterpolatedTransforms = false;
        m_frustumCullingEnabled = true;
        m_occlusionCullingEnabled = true;
        m_visibilityStats = {};
        m_renderSpatialIndex.Clear();
        m_frameReflectionProbes.clear();
        m_skyPrefilterKeyPath.clear();
        m_skyPrefilterKeySourceView.Reset();
        m_skyPrefilterKey = 0;
    }

    void Scene::Update(const float deltaTime)
    {
        Reactive::Detail::AdvanceFrame(
            deltaTime,
            Time::UnscaledDeltaTime());
        static_cast<void>(
            m_sceneManager->ProcessPending());
        // コールバック中の追加で配列が再確保されても、添字で要素を読み直して走査します。
        {
            LAMAPON_PROFILE_SCOPE("Update");
            // 更新するオブジェクトの添字
            for (std::size_t index = 0;
                index < m_gameObjects.size();
                ++index)
            {
                m_gameObjects[index]->Update(m_graphics, deltaTime);
            }
        }
        // 更新または設定を調べる対象
        for (const auto& gameObject : m_gameObjects)
        {
            // 補間指定を調べる物理ボディ
            const auto* rigidbody =
                gameObject->GetComponent<
                    RigidbodyComponent>();
            gameObject->
                SetPhysicsInterpolationActive(
                    rigidbody != nullptr
                    && rigidbody->IsEnabled()
                    && rigidbody->Interpolates());
            gameObject->
                SynchronizePhysicsInterpolation();
        }

        // フレーム開始時の物理設定
        const auto& physicsSettings = ActivePhysicsSettings();
        m_physicsClock.BeginFrame(deltaTime, physicsSettings.fixedTimeStep,
            static_cast<std::size_t>(physicsSettings.maximumCatchUpSteps));
        // 0〜0.1へ制限した経過秒数
        const float safeDeltaTime =
            std::isfinite(deltaTime) ? std::clamp(deltaTime, 0.0f, 0.1f) : 0.0f;
        if (safeDeltaTime <= 0.0f)
        {
            // エディターのゼロ時間更新では接触を再判定し、物理姿勢が変わった物体だけ補間履歴を同期します。
            StepPhysics(0.0f);
            // 更新または設定を調べる対象
            for (const auto& gameObject :
                m_gameObjects)
            {
                gameObject->
                    SynchronizePhysicsInterpolation();
            }
            // 更新するオブジェクトの添字
            for (std::size_t index = 0;
                index < m_gameObjects.size();
                ++index)
            {
                m_gameObjects[index]->LateUpdate(
                    m_graphics,
                    deltaTime);
            }
            return;
        }

        // コールバック中に設定が変更されても、このフレームの刻み幅は時計・FixedUpdate・物理計算で同じ値を使います。
        // フレーム内で共通の固定秒数
        const float fixedDeltaTime = PhysicsTiming().fixedDeltaTime;
        while (m_physicsClock.PendingStep())
        {
            // 更新または設定を調べる対象
            for (const auto& gameObject : m_gameObjects)
            {
                gameObject->
                    BeginPhysicsInterpolationStep();
            }
            {
                LAMAPON_PROFILE_SCOPE("FixedUpdate");
                // 更新するオブジェクトの添字
                for (std::size_t index = 0;
                    index < m_gameObjects.size();
                    ++index)
                {
                    m_gameObjects[index]->FixedUpdate(
                        m_graphics,
                        fixedDeltaTime);
                }
            }
            StepPhysics(fixedDeltaTime);
            // 更新または設定を調べる対象
            for (const auto& gameObject : m_gameObjects)
            {
                gameObject->
                    EndPhysicsInterpolationStep();
            }
            m_physicsClock.CompleteStep();
        }
        m_physicsClock.FinishFrame();

        LAMAPON_PROFILE_SCOPE("LateUpdate");
        // 更新するオブジェクトの添字
        for (std::size_t index = 0;
            index < m_gameObjects.size();
            ++index)
        {
            m_gameObjects[index]->LateUpdate(
                m_graphics,
                deltaTime);
        }
    }

    std::size_t Scene::PhysicsSubstepCount(
        const float deltaTime) const noexcept
    {
        // 接触品質の刻み数を自身の形状から決め、すり抜けの防止はStepPhysicsのスイープが担います。
        // 刻み秒数の絶対値
        const float absoluteDeltaTime = std::abs(deltaTime);
        // 各ボディが必要とする最大刻み数
        std::size_t requiredSubsteps{ 1 };
        // 補助刻みを調べるオブジェクト
        for (const auto& gameObject : m_gameObjects)
        {
            // 連続判定を行う動的ボディ
            const auto* body =
                gameObject->GetComponent<RigidbodyComponent>();
            if (body == nullptr
                || !body->IsEnabled()
                || body->IsKinematic()
                || body->IsSleeping()
                || !body->UsesContinuousCollisionDetection()
                || !gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            // 0.5以下の自身の形状寸法
            float ownFeatureSize = 0.5f;
            // 自身の箱コライダー
            if (const auto* box =
                    gameObject->GetComponent<
                        BoxCollider3DComponent>();
                box != nullptr && box->IsEnabled())
            {
                // 自身のコライダーのワールド境界
                const auto bounds = box->WorldBounds();
                ownFeatureSize = std::min({
                    ownFeatureSize,
                    std::abs(bounds.maximum.x - bounds.minimum.x),
                    std::abs(bounds.maximum.y - bounds.minimum.y),
                    std::abs(bounds.maximum.z - bounds.minimum.z)
                });
            }
            // 自身のカプセルコライダー
            if (const auto* capsule =
                    gameObject->GetComponent<
                        CapsuleCollider3DComponent>();
                capsule != nullptr && capsule->IsEnabled())
            {
                ownFeatureSize = std::min(
                    ownFeatureSize,
                    capsule->Radius() * 2.0f);
            }
            // 自身の球コライダー
            if (const auto* sphere =
                    gameObject->GetComponent<
                        SphereCollider3DComponent>();
                sphere != nullptr && sphere->IsEnabled())
            {
                ownFeatureSize = std::min(
                    ownFeatureSize,
                    sphere->WorldSphere().radius * 2.0f);
            }
            // 自身の箱コライダー
            if (const auto* box =
                    gameObject->GetComponent<
                        BoxCollider2DComponent>();
                box != nullptr && box->IsEnabled())
            {
                // 自身のコライダーのワールド境界
                const auto bounds = box->WorldBounds();
                ownFeatureSize = std::min({
                    ownFeatureSize,
                    std::abs(bounds.maximum.x - bounds.minimum.x),
                    std::abs(bounds.maximum.y - bounds.minimum.y)
                });
            }
            // 自身の円コライダー
            if (const auto* circle =
                    gameObject->GetComponent<
                        CircleCollider2DComponent>();
                circle != nullptr && circle->IsEnabled())
            {
                ownFeatureSize = std::min(
                    ownFeatureSize,
                    circle->WorldCircle().radius * 2.0f);
            }
            // 自身の多角形コライダー
            if (const auto* polygon =
                    gameObject->GetComponent<
                        PolygonCollider2DComponent>();
                polygon != nullptr && polygon->IsEnabled())
            {
                // 自身のコライダーのワールド境界
                const auto bounds = polygon->WorldBounds();
                ownFeatureSize = std::min({
                    ownFeatureSize,
                    std::abs(bounds.maximum.x - bounds.minimum.x),
                    std::abs(bounds.maximum.y - bounds.minimum.y)
                });
            }

            // 一刻みで移動する最大距離
            const float maximumStepDistance =
                std::max(ownFeatureSize * 0.5f, 0.01f);
            // 現在の線形速度
            const auto velocity = body->Velocity();
            // 現在の線形速度の大きさ
            const float speed = std::sqrt(
                velocity.x * velocity.x
                + velocity.y * velocity.y
                + velocity.z * velocity.z);
            // 重力を含めた見込みの移動距離
            const float distance =
                speed * absoluteDeltaTime
                + (body->UsesGravity()
                    ? 4.905f * absoluteDeltaTime * absoluteDeltaTime
                    : 0.0f);
            requiredSubsteps = std::max(
                requiredSubsteps,
                static_cast<std::size_t>(
                    std::ceil(distance / maximumStepDistance)));
        }
        return std::clamp<std::size_t>(requiredSubsteps, 1, 8);
    }

    bool Scene::CollisionSuppressedByJoint(
        const GameObject& left,
        const GameObject& right) const noexcept
    {
        // ジョイントが相手との接触を禁止するか返します(owner: ジョイントの所有者, connected: 接続先の対象)。
        const auto suppresses = [](
            const GameObject& owner,
            const GameObject& connected) noexcept
        {
            // 接触禁止を調べるジョイント
            const auto* joint = owner.GetComponent<JointComponent>();
            return joint != nullptr
                && joint->IsEnabled()
                && !joint->CollideConnected()
                && joint->ConnectedBodyId() == connected.Id();
        };
        return suppresses(left, right)
            || suppresses(right, left);
    }

    void Scene::SolveJoints(
        const float deltaTime,
        const bool applyForces)
    {
        // ローカル点をワールド座標へ変換します(object: 変換元の対象, local: 対象基準の接続点)。
        const auto worldPoint = [](
            const GameObject& object,
            const DirectX::XMFLOAT3& local) noexcept
        {
            // ワールド座標へ変換した接続点
            DirectX::XMFLOAT3 result{};
            DirectX::XMStoreFloat3(
                &result,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&local),
                    object.WorldMatrix()));
            return result;
        };
        // 有効で非キネマティックなボディか返します(body: 物理ボディかnullptr)。
        const auto isDynamic = [](const RigidbodyComponent* body) noexcept
        {
            return body != nullptr
                && body->IsEnabled()
                && !body->IsKinematic();
        };
        // 三成分の内積を返します(left: 一方のベクトル, right: もう一方のベクトル)。
        const auto dot = [](
            const DirectX::XMFLOAT3& left,
            const DirectX::XMFLOAT3& right) noexcept
        {
            return left.x * right.x
                + left.y * right.y
                + left.z * right.z;
        };
        // 三成分を同じ倍率で拡縮します(value: ベクトル, scale: 拡縮する倍率)。
        const auto multiply = [](
            const DirectX::XMFLOAT3& value,
            const float scale) noexcept
        {
            return DirectX::XMFLOAT3{
                value.x * scale,
                value.y * scale,
                value.z * scale
            };
        };
        // 三成分の差を返します(left: 引かれるベクトル, right: 引くベクトル)。
        const auto subtract = [](
            const DirectX::XMFLOAT3& left,
            const DirectX::XMFLOAT3& right) noexcept
        {
            return DirectX::XMFLOAT3{
                left.x - right.x,
                left.y - right.y,
                left.z - right.z
            };
        };

        // ジョイントを探す所有対象
        for (const auto& ownerPointer : m_gameObjects)
        {
            // ジョイントの所有者
            auto& owner = *ownerPointer;
            // 拘束を適用するジョイント
            auto* joint = owner.GetComponent<JointComponent>();
            if (!owner.IsActiveInHierarchy()
                || joint == nullptr
                || !joint->IsEnabled())
            {
                continue;
            }
            // ジョイントが接続する相手
            auto* connected =
                FindGameObject(joint->ConnectedBodyId());
            if (connected == nullptr
                || connected == &owner
                || !connected->IsActiveInHierarchy())
            {
                continue;
            }

            // 所有者の物理ボディ
            auto* ownerBody = owner.GetComponent<RigidbodyComponent>();
            // 接続先の物理ボディ
            auto* connectedBody =
                connected->GetComponent<RigidbodyComponent>();
            // 所有者が動的に応答する状態
            const bool ownerDynamic = isDynamic(ownerBody);
            // 接続先が動的に応答する状態
            const bool connectedDynamic = isDynamic(connectedBody);
            if (!ownerDynamic && !connectedDynamic)
            {
                continue;
            }

            // 所有者のワールド接続点
            const auto ownerAnchor =
                worldPoint(owner, joint->Anchor());
            // 接続先のワールド接続点
            const auto connectedAnchor =
                worldPoint(*connected, joint->ConnectedAnchor());
            // 接続点の位置差と補正移動量
            DirectX::XMFLOAT3 delta{
                connectedAnchor.x - ownerAnchor.x,
                connectedAnchor.y - ownerAnchor.y,
                connectedAnchor.z - ownerAnchor.z
            };
            // 二つの接続点の距離
            const float distance = std::sqrt(
                delta.x * delta.x
                + delta.y * delta.y
                + delta.z * delta.z);

            // 所有者の動的な質量の逆数
            const float ownerInverseMass =
                ownerDynamic
                ? ownerBody->InverseMass()
                : 0.0f;
            // 接続先の動的な質量の逆数
            const float connectedInverseMass =
                connectedDynamic
                ? connectedBody->InverseMass()
                : 0.0f;
            // 動的ボディの質量逆数の合計
            const float totalInverseMass =
                ownerInverseMass + connectedInverseMass;
            // 所有者へ割り当てる補正割合
            const float ownerShare =
                ownerInverseMass / totalInverseMass;
            // 接続先へ割り当てる補正割合
            const float connectedShare =
                connectedInverseMass / totalInverseMass;

            if (joint->Type() == JointType::Spring)
            {
                if (distance <= 0.000001f)
                {
                    continue;
                }
                // 所有者から接続先への単位方向
                const DirectX::XMFLOAT3 direction{
                    delta.x / distance,
                    delta.y / distance,
                    delta.z / distance
                };
                // 所有者の現在の線形速度
                const auto ownerVelocity = ownerDynamic
                    ? ownerBody->Velocity()
                    : DirectX::XMFLOAT3{};
                // 接続先の現在の線形速度
                const auto connectedVelocity = connectedDynamic
                    ? connectedBody->Velocity()
                    : DirectX::XMFLOAT3{};
                // 接続点方向の相対速度
                const float relativeSpeed =
                    (ownerVelocity.x - connectedVelocity.x) * direction.x
                    + (ownerVelocity.y - connectedVelocity.y) * direction.y
                    + (ownerVelocity.z - connectedVelocity.z) * direction.z;
                // 自然長からの伸び量
                const float error = distance - joint->RestLength();
                // ばねと減衰による速度変更係数
                const float impulse =
                    (joint->Stiffness() * error
                        - joint->Damping() * relativeSpeed)
                    * deltaTime;

                if (applyForces && ownerDynamic)
                {
                    ownerBody->SetVelocity({
                        ownerVelocity.x + direction.x * impulse * ownerShare,
                        ownerVelocity.y + direction.y * impulse * ownerShare,
                        ownerVelocity.z + direction.z * impulse * ownerShare
                    });
                }
                if (applyForces && connectedDynamic)
                {
                    connectedBody->SetVelocity({
                        connectedVelocity.x
                            - direction.x * impulse * connectedShare,
                        connectedVelocity.y
                            - direction.y * impulse * connectedShare,
                        connectedVelocity.z
                            - direction.z * impulse * connectedShare
                    });
                }

                // ばねの位置補正へ加える係数
                const float correctionScale = std::clamp(
                    joint->Stiffness() * deltaTime * deltaTime,
                    0.0f,
                    0.5f);
                delta = {
                    direction.x * error * correctionScale,
                    direction.y * error * correctionScale,
                    direction.z * error * correctionScale
                };
            }

            if (ownerDynamic)
            {
                owner.TranslateWorld({
                    delta.x * ownerShare,
                    delta.y * ownerShare,
                    delta.z * ownerShare
                });
            }
            if (connectedDynamic)
            {
                connected->TranslateWorld({
                    -delta.x * connectedShare,
                    -delta.y * connectedShare,
                    -delta.z * connectedShare
                });
            }

            if (joint->Type() != JointType::Spring)
            {
                // 所有者の現在の線形速度
                const auto ownerVelocity = ownerDynamic
                    ? ownerBody->Velocity()
                    : DirectX::XMFLOAT3{};
                // 接続先の現在の線形速度
                const auto connectedVelocity = connectedDynamic
                    ? connectedBody->Velocity()
                    : DirectX::XMFLOAT3{};
                // 拘束後に両者へ設定する線形速度
                const DirectX::XMFLOAT3 sharedVelocity{
                    ownerVelocity.x * (1.0f - ownerShare)
                        + connectedVelocity.x * ownerShare,
                    ownerVelocity.y * (1.0f - ownerShare)
                        + connectedVelocity.y * ownerShare,
                    ownerVelocity.z * (1.0f - ownerShare)
                        + connectedVelocity.z * ownerShare
                };
                if (ownerDynamic)
                {
                    ownerBody->SetVelocity(sharedVelocity);
                }
                if (connectedDynamic)
                {
                    connectedBody->SetVelocity(sharedVelocity);
                }

                if (joint->Type() == JointType::Hinge)
                {
                    joint->EnsureHingeReference(
                        *connected);
                    // ワールド座標のヒンジ回転軸
                    const auto axis =
                        joint->HingeWorldAxis(
                            *connected);
                    // 所有者の毎秒ラジアン角速度
                    auto ownerAngularVelocity =
                        ownerDynamic
                        ? ownerBody->AngularVelocity()
                        : DirectX::XMFLOAT3{};
                    // 接続先の毎秒ラジアン角速度
                    auto connectedAngularVelocity =
                        connectedDynamic
                        ? connectedBody->AngularVelocity()
                        : DirectX::XMFLOAT3{};
                    // 両者の毎秒ラジアン相対角速度
                    auto relativeAngularVelocity =
                        subtract(
                            ownerAngularVelocity,
                            connectedAngularVelocity);
                    // ヒンジ軸方向の相対角速度
                    float axisSpeed = dot(
                        relativeAngularVelocity,
                        axis);
                    // ヒンジ軸以外の相対角速度
                    const auto lockedAngularVelocity =
                        subtract(
                            relativeAngularVelocity,
                            multiply(axis, axisSpeed));
                    if (ownerDynamic)
                    {
                        ownerAngularVelocity =
                            subtract(
                                ownerAngularVelocity,
                                multiply(
                                    lockedAngularVelocity,
                                    ownerShare));
                        ownerBody->SetAngularVelocity(
                            ownerAngularVelocity);
                    }
                    if (connectedDynamic)
                    {
                        connectedAngularVelocity = {
                            connectedAngularVelocity.x
                                + lockedAngularVelocity.x
                                    * connectedShare,
                            connectedAngularVelocity.y
                                + lockedAngularVelocity.y
                                    * connectedShare,
                            connectedAngularVelocity.z
                                + lockedAngularVelocity.z
                                    * connectedShare
                        };
                        connectedBody->SetAngularVelocity(
                            connectedAngularVelocity);
                    }

                    // 現在のヒンジ角のラジアン値
                    const float angle =
                        joint->HingeAngleRadians(
                            *connected);
                    // 度数をラジアンへ変える係数
                    constexpr float degreesToRadians =
                        DirectX::XM_PI / 180.0f;
                    // ヒンジ角が下限以下の状態
                    bool atLowerLimit{};
                    // ヒンジ角が上限以上の状態
                    bool atUpperLimit{};
                    if (joint->UseLimits())
                    {
                        // ヒンジ角下限のラジアン値
                        const float minimum =
                            joint->Limits()
                                .minimumAngleDegrees
                            * degreesToRadians;
                        // ヒンジ角上限のラジアン値
                        const float maximum =
                            joint->Limits()
                                .maximumAngleDegrees
                            * degreesToRadians;
                        atLowerLimit =
                            angle <= minimum;
                        atUpperLimit =
                            angle >= maximum;
                        // 制限範囲へ戻したヒンジ角
                        const float target =
                            std::clamp(
                                angle,
                                minimum,
                                maximum);
                        // 制限範囲からはみ出たラジアン角
                        const float error =
                            angle - target;
                        if (std::abs(error)
                            > 0.000001f)
                        {
                            if (ownerDynamic)
                            {
                                owner.RotateWorld(
                                    multiply(
                                        axis,
                                        -error
                                            * ownerShare));
                            }
                            if (connectedDynamic)
                            {
                                connected->RotateWorld(
                                    multiply(
                                        axis,
                                        error
                                            * connectedShare));
                            }
                        }

                        // 制限の外側へ回転する状態
                        const bool movingOutward =
                            (atLowerLimit
                                && axisSpeed < 0.0f)
                            || (atUpperLimit
                                && axisSpeed > 0.0f);
                        if (movingOutward)
                        {
                            if (ownerDynamic)
                            {
                                ownerBody->SetAngularVelocity(
                                    subtract(
                                        ownerBody
                                            ->AngularVelocity(),
                                        multiply(
                                            axis,
                                            axisSpeed
                                                * ownerShare)));
                            }
                            if (connectedDynamic)
                            {
                                // 接続先の現在の角速度
                                const auto velocity =
                                    connectedBody
                                        ->AngularVelocity();
                                connectedBody
                                    ->SetAngularVelocity({
                                        velocity.x
                                            + axis.x
                                                * axisSpeed
                                                * connectedShare,
                                        velocity.y
                                            + axis.y
                                                * axisSpeed
                                                * connectedShare,
                                        velocity.z
                                            + axis.z
                                                * axisSpeed
                                                * connectedShare
                                    });
                            }
                        }
                    }

                    if (applyForces
                        && joint->UseMotor()
                        && joint->Motor()
                            .maximumTorque > 0.0f)
                    {
                        // モーターの毎秒ラジアン目標速度
                        float targetSpeed =
                            joint->Motor()
                                .targetVelocityDegrees
                            * degreesToRadians;
                        if ((atLowerLimit
                                && targetSpeed < 0.0f)
                            || (atUpperLimit
                                && targetSpeed > 0.0f))
                        {
                            targetSpeed = 0.0f;
                        }
                        relativeAngularVelocity =
                            subtract(
                                ownerDynamic
                                ? ownerBody
                                    ->AngularVelocity()
                                : DirectX::XMFLOAT3{},
                                connectedDynamic
                                ? connectedBody
                                    ->AngularVelocity()
                                : DirectX::XMFLOAT3{});
                        axisSpeed = dot(
                            relativeAngularVelocity,
                            axis);
                        // 所有者の軸方向の慣性逆数
                        const float ownerResponse =
                            ownerDynamic
                            ? dot(
                                ownerBody
                                    ->ApplyInverseInertia(
                                        axis),
                                axis)
                            : 0.0f;
                        // 接続先の軸方向の慣性逆数
                        const float connectedResponse =
                            connectedDynamic
                            ? dot(
                                connectedBody
                                    ->ApplyInverseInertia(
                                        axis),
                                axis)
                            : 0.0f;
                        // 両者の軸方向の慣性逆数の合計
                        const float response =
                            ownerResponse
                            + connectedResponse;
                        if (response > 0.000001f)
                        {
                            // 最大トルクと秒数による上限
                            const float maximumImpulse =
                                joint->Motor()
                                    .maximumTorque
                                * std::abs(deltaTime);
                            // トルク上限で制限した角インパルス
                            const float impulse =
                                std::clamp(
                                    (targetSpeed
                                        - axisSpeed)
                                        / response,
                                    -maximumImpulse,
                                    maximumImpulse);
                            if (ownerDynamic)
                            {
                                ownerBody->AddTorque(
                                    multiply(
                                        axis,
                                        impulse),
                                    ForceMode::Impulse);
                            }
                            if (connectedDynamic)
                            {
                                connectedBody->AddTorque(
                                    multiply(
                                        axis,
                                        -impulse),
                                    ForceMode::Impulse);
                            }
                        }
                    }
                }
                else if (joint->Type() == JointType::Fixed)
                {
                    // 所有者の毎秒ラジアン角速度
                    const auto ownerAngularVelocity =
                        ownerDynamic
                        ? ownerBody->AngularVelocity()
                        : DirectX::XMFLOAT3{};
                    // 接続先の毎秒ラジアン角速度
                    const auto connectedAngularVelocity =
                        connectedDynamic
                        ? connectedBody->AngularVelocity()
                        : DirectX::XMFLOAT3{};
                    // 両者へ設定する毎秒ラジアン角速度
                    const DirectX::XMFLOAT3
                        sharedAngularVelocity{
                            ownerAngularVelocity.x
                                * (1.0f - ownerShare)
                                + connectedAngularVelocity.x
                                    * ownerShare,
                            ownerAngularVelocity.y
                                * (1.0f - ownerShare)
                                + connectedAngularVelocity.y
                                    * ownerShare,
                            ownerAngularVelocity.z
                                * (1.0f - ownerShare)
                                + connectedAngularVelocity.z
                                    * ownerShare
                        };
                    if (ownerDynamic)
                    {
                        ownerBody->SetAngularVelocity(
                            sharedAngularVelocity);
                    }
                    if (connectedDynamic)
                    {
                        connectedBody->SetAngularVelocity(
                            sharedAngularVelocity);
                    }
                }
            }
        }
    }

    void Scene::StepPhysics(const float deltaTime)
    {
        LAMAPON_PROFILE_SCOPE("Physics");
        // 今回の接触組とトリガー区分
        std::map<CollisionKey, bool> currentCollisions;
        // 今回記録した表示用接触点
        std::vector<PhysicsDebugContact> debugContacts;
        // 非反発の接触面に支えられた剛体番号
        std::unordered_set<std::uint64_t>
            supportedBodies;
        // 自身から親へ最初の剛体と所有物体を探します(colliderObject: 接触形状の所有物体)。
        const auto findRigidbodyObject =
            [](GameObject& colliderObject)
        {
            // 剛体を探している親階層
            auto* current = &colliderObject;
            while (current != nullptr)
            {
                // 最初に見つかった剛体
                if (auto* body =
                    current->GetComponent<
                        RigidbodyComponent>())
                {
                    return std::pair{
                        current,
                        body
                    };
                }
                current = current->Parent();
            }
            return std::pair<
                GameObject*,
                RigidbodyComponent*>{
                    &colliderObject,
                    nullptr
                };
        };

        // 接触を通知して位置・法線力積・摩擦を解きます(left: 左形状の物体, right: 右形状の物体, contact: 左向き法線を持つ接触, is3D: 三次元判定か, isTrigger: 通知のみの接触か, leftMaterial: 左形状の応答材質, rightMaterial: 右形状の応答材質)。
        const auto processContact =
            [this,
                &currentCollisions,
                &debugContacts,
                &supportedBodies,
                &findRigidbodyObject](
                GameObject& left,
                GameObject& right,
                const Contact& contact,
                const bool is3D,
                const bool isTrigger,
                const PhysicsMaterial leftMaterial,
                const PhysicsMaterial rightMaterial)
            {
                // leftPhysicsObject: 左形状を動かす物体, leftBody: 左側の剛体
                auto [leftPhysicsObject, leftBody] =
                    findRigidbodyObject(left);
                // rightPhysicsObject: 右形状を動かす物体, rightBody: 右側の剛体
                auto [rightPhysicsObject, rightBody] =
                    findRigidbodyObject(right);
                if (leftBody != nullptr
                    && leftPhysicsObject
                        == rightPhysicsObject)
                {
                    return;
                }
                // 順序を正規化した衝突組
                const CollisionKey key{
                    std::min(left.Id(), right.Id()),
                    std::max(left.Id(), right.Id()),
                    is3D
                };
                // 物理ステップ内で最初の接触
                const bool firstContactThisFrame =
                    !currentCollisions.contains(key);
                currentCollisions[key] = isTrigger;

                // CCDのサブステップで同じ組を何度も解決するため、表示用の接触点はステップ内で最初の1回だけ控えます。
                if (m_physicsDebugCaptureEnabled && firstContactThisFrame)
                {
                    // 表示用の接触点を記録します(point: ワールド接触位置)。
                    const auto record =
                        [&](const DirectX::XMFLOAT3& point)
                        {
                            debugContacts.push_back({
                                left.Id(),
                                right.Id(),
                                point,
                                contact.normal,
                                contact.penetration,
                                is3D,
                                isTrigger });
                        };
                    if (contact.pointCount == 0)
                    {
                        record(contact.point);
                    }
                    // 接触点の添字
                    for (std::size_t pointIndex{};
                        pointIndex < std::min(
                            contact.pointCount,
                            contact.points.size());
                        ++pointIndex)
                    {
                        record(contact.points[pointIndex]);
                    }
                }

                // 右側へ通知する接触法線
                const DirectX::XMFLOAT3 oppositeNormal{
                    -contact.normal.x,
                    -contact.normal.y,
                    -contact.normal.z
                };

                // 各衝突組の通知は一回の物理ステップで一度だけ行います。
                if (!firstContactThisFrame)
                {
                }
                else if (m_activeCollisions.contains(key))
                {
                    left.NotifyCollisionStay(
                        right,
                        contact.normal,
                        contact.point,
                        contact.penetration,
                        isTrigger);
                    right.NotifyCollisionStay(
                        left,
                        oppositeNormal,
                        contact.point,
                        contact.penetration,
                        isTrigger);
                }
                else
                {
                    left.NotifyCollisionEnter(
                        right,
                        contact.normal,
                        contact.point,
                        contact.penetration,
                        isTrigger);
                    right.NotifyCollisionEnter(
                        left,
                        oppositeNormal,
                        contact.point,
                        contact.penetration,
                        isTrigger);
                }

                if (isTrigger)
                {
                    return;
                }

                // 左側を動的剛体として解くか
                const bool leftDynamic =
                    leftBody != nullptr
                    && leftBody->IsEnabled()
                    && !leftBody->IsKinematic();
                // 右側を動的剛体として解くか
                const bool rightDynamic =
                    rightBody != nullptr
                    && rightBody->IsEnabled()
                    && !rightBody->IsKinematic();

                if (!leftDynamic && !rightDynamic)
                {
                    return;
                }

                // 左側の逆質量
                const float inverseMassLeft = leftDynamic
                    ? leftBody->InverseMass()
                    : 0.0f;
                // 右側の逆質量
                const float inverseMassRight = rightDynamic
                    ? rightBody->InverseMass()
                    : 0.0f;
                // 両側の逆質量の合計
                const float inverseMassSum =
                    inverseMassLeft + inverseMassRight;
                if (inverseMassSum <= 0.0f)
                {
                    return;
                }
                // 左側の位置補正割合
                const float leftShare =
                    inverseMassLeft / inverseMassSum;
                // 右側の位置補正割合
                const float rightShare =
                    inverseMassRight / inverseMassSum;
                // 許容する食い込み深さ
                constexpr float penetrationSlop =
                    0.001f;
                // 超過した食い込みの補正率
                constexpr float positionCorrection =
                    0.8f;
                // 分配する位置補正距離
                const float correctionDepth =
                    std::max(
                        contact.penetration
                            - penetrationSlop,
                        0.0f)
                    * positionCorrection;

                if (leftDynamic)
                {
                    leftPhysicsObject->TranslateWorld({
                        contact.normal.x * correctionDepth * leftShare,
                        contact.normal.y * correctionDepth * leftShare,
                        contact.normal.z * correctionDepth * leftShare
                    });
                }

                if (rightDynamic)
                {
                    rightPhysicsObject->TranslateWorld({
                        oppositeNormal.x * correctionDepth * rightShare,
                        oppositeNormal.y * correctionDepth * rightShare,
                        oppositeNormal.z * correctionDepth * rightShare
                    });
                }

                // 両材質から合成した応答係数
                const auto material = CombinePhysicsMaterials(
                    leftMaterial,
                    rightMaterial);
                // 二つのベクトルの内積を求めます(a: 左ベクトル, b: 右ベクトル)。
                const auto dot = [](
                    const DirectX::XMFLOAT3& a,
                    const DirectX::XMFLOAT3& b) noexcept
                {
                    return a.x * b.x + a.y * b.y + a.z * b.z;
                };
                // ベクトルの差を求めます(a: 引かれるベクトル, b: 引くベクトル)。
                const auto subtract = [](
                    const DirectX::XMFLOAT3& a,
                    const DirectX::XMFLOAT3& b) noexcept
                {
                    return DirectX::XMFLOAT3{
                        a.x - b.x,
                        a.y - b.y,
                        a.z - b.z
                    };
                };
                // ベクトルを倍率で伸縮します(value: 対象ベクトル, scale: 倍率)。
                const auto multiply = [](
                    const DirectX::XMFLOAT3& value,
                    const float scale) noexcept
                {
                    return DirectX::XMFLOAT3{
                        value.x * scale,
                        value.y * scale,
                        value.z * scale
                    };
                };
                // 二つのベクトルの外積を求めます(a: 左ベクトル, b: 右ベクトル)。
                const auto cross = [](
                    const DirectX::XMFLOAT3& a,
                    const DirectX::XMFLOAT3& b) noexcept
                {
                    return DirectX::XMFLOAT3{
                        a.y * b.z - a.z * b.y,
                        a.z * b.x - a.x * b.z,
                        a.x * b.y - a.y * b.x
                    };
                };
                // 解決に使う接触点数
                const std::size_t pointCount =
                    contact.pointCount > 0
                    ? std::min(
                        contact.pointCount,
                        contact.points.size())
                    : 1;
                // 接触点ごとの累積法線力積
                std::array<float, 4>
                    accumulatedNormalImpulses{};
                // 法線力積の反復回数
                const int impulseIterations =
                    static_cast<int>(
                        ActivePhysicsSettings()
                            .solverIterations);
                // ソルバーの反復番号
                for (int iteration{};
                    // 法線力積の反復回数
                    iteration < impulseIterations;
                    ++iteration)
                {
                    // 今回の反復で加える法線力積
                    std::array<float, 4>
                        iterationNormalImpulses{};
                    // 接触点の添字
                    for (std::size_t pointIndex{};
                        // 解決に使う接触点数
                        pointIndex < pointCount;
                        ++pointIndex)
                    {
                        // 反復順を交互にした接触点添字
                        const std::size_t orderedIndex =
                            iteration % 2 == 0
                            ? pointIndex
                            : pointCount
                                - pointIndex - 1;
                        // 力積を加える接触位置
                        const auto point =
                            contact.pointCount > 0
                            ? contact.points[orderedIndex]
                            : contact.point;
                        // 左重心から接触点へのベクトル
                        const auto leftRadius =
                            leftDynamic
                            ? subtract(
                                point,
                                leftBody
                                    ->WorldCenterOfMass())
                            : DirectX::XMFLOAT3{};
                        // 右重心から接触点へのベクトル
                        const auto rightRadius =
                            rightDynamic
                            ? subtract(
                                point,
                                rightBody
                                    ->WorldCenterOfMass())
                            : DirectX::XMFLOAT3{};
                        // 接触位置の回転慣性を含む力積分母を求めます(direction: 単位力積方向)。
                        const auto impulseDenominator =
                            [&](const DirectX::XMFLOAT3&
                                direction)
                        {
                            // 回転分を含む力積の分母
                            float result =
                                inverseMassSum;
                            if (leftDynamic)
                            {
                                result += dot(
                                    cross(
                                        leftBody
                                            ->ApplyInverseInertia(
                                                cross(
                                                    leftRadius,
                                                    direction)),
                                        leftRadius),
                                    direction);
                            }
                            if (rightDynamic)
                            {
                                result += dot(
                                    cross(
                                        rightBody
                                            ->ApplyInverseInertia(
                                                cross(
                                                    rightRadius,
                                                    direction)),
                                        rightRadius),
                                    direction);
                            }
                            return result;
                        };

                        // 接触点での左側の相対速度
                        auto relativeVelocity =
                            subtract(
                                leftDynamic
                                ? leftBody
                                    ->VelocityAtPoint(
                                        point)
                                : DirectX::XMFLOAT3{},
                                rightDynamic
                                ? rightBody
                                    ->VelocityAtPoint(
                                        point)
                                : DirectX::XMFLOAT3{});
                        // 法線方向の相対速度
                        const float inwardSpeed =
                            dot(
                                relativeVelocity,
                                contact.normal);
                        // 法線力積の計算分母
                        const float normalDenominator =
                            impulseDenominator(
                                contact.normal);
                        if (inwardSpeed >= -0.000001f
                            || normalDenominator
                                <= 0.000001f)
                        {
                            continue;
                        }

                        // 初回反復だけ加える反発係数
                        const float restitution =
                            iteration == 0
                            ? material.restitution
                            : 0.0f;
                        // 接触点へ加える法線力積量
                        const float
                            normalImpulseMagnitude =
                                -(1.0f + restitution)
                                * inwardSpeed
                                / normalDenominator
                                / static_cast<float>(
                                    pointCount);
                        iterationNormalImpulses[
                            orderedIndex] =
                                normalImpulseMagnitude;
                    }
                    // 接触点の添字
                    for (std::size_t pointIndex{};
                        // 解決に使う接触点数
                        pointIndex < pointCount;
                        ++pointIndex)
                    {
                        // 接触点へ加える法線力積量
                        const float
                            normalImpulseMagnitude =
                                iterationNormalImpulses[
                                    pointIndex];
                        if (normalImpulseMagnitude
                            <= 0.0f)
                        {
                            continue;
                        }
                        accumulatedNormalImpulses[
                            pointIndex] +=
                                normalImpulseMagnitude;
                        // 力積を加える接触位置
                        const auto point =
                            contact.pointCount > 0
                            ? contact.points[pointIndex]
                            : contact.point;
                        // 接触法線方向の力積
                        const auto normalImpulse =
                            multiply(
                                contact.normal,
                                normalImpulseMagnitude);
                        if (leftDynamic)
                        {
                            leftBody
                                ->ApplyImpulseAtPoint(
                                    normalImpulse,
                                    point);
                        }
                        if (rightDynamic)
                        {
                            rightBody
                                ->ApplyImpulseAtPoint(
                                    multiply(
                                        normalImpulse,
                                        -1.0f),
                                    point);
                        }
                    }
                }
                // 接触点の添字
                for (std::size_t pointIndex{};
                    // 解決に使う接触点数
                    pointIndex < pointCount;
                    ++pointIndex)
                {
                    // 力積を加える接触位置
                    const auto point =
                        contact.pointCount > 0
                        ? contact.points[pointIndex]
                        : contact.point;
                    // 接触点での左側の相対速度
                    auto relativeVelocity =
                        subtract(
                            leftDynamic
                            ? leftBody
                                ->VelocityAtPoint(point)
                            : DirectX::XMFLOAT3{},
                            rightDynamic
                            ? rightBody
                                ->VelocityAtPoint(point)
                            : DirectX::XMFLOAT3{});
                    // 摩擦計算時の法線方向速度
                    const float normalSpeed =
                        dot(
                            relativeVelocity,
                            contact.normal);
                    // 接線速度から得る摩擦方向
                    auto tangent = subtract(
                        relativeVelocity,
                        multiply(
                            contact.normal,
                            normalSpeed));
                    // 2D接触では摩擦を面内（XY）に限定します。
                    if (!is3D)
                    {
                        tangent.z = 0.0f;
                    }
                    // 正規化前の接線速度の長さ
                    const float tangentLength =
                        std::sqrt(
                            dot(tangent, tangent));
                    if (tangentLength <= 0.005f)
                    {
                        continue;
                    }
                    tangent.x /= tangentLength;
                    tangent.y /= tangentLength;
                    tangent.z /= tangentLength;

                    // 左重心から接触点へのベクトル
                    const auto leftRadius =
                        leftDynamic
                        ? subtract(
                            point,
                            leftBody
                                ->WorldCenterOfMass())
                        : DirectX::XMFLOAT3{};
                    // 右重心から接触点へのベクトル
                    const auto rightRadius =
                        rightDynamic
                        ? subtract(
                            point,
                            rightBody
                                ->WorldCenterOfMass())
                        : DirectX::XMFLOAT3{};
                    // 接線力積の計算分母
                    float tangentDenominator =
                        inverseMassSum;
                    if (leftDynamic)
                    {
                        tangentDenominator += dot(
                            cross(
                                leftBody
                                    ->ApplyInverseInertia(
                                        cross(
                                            leftRadius,
                                            tangent)),
                                leftRadius),
                            tangent);
                    }
                    if (rightDynamic)
                    {
                        tangentDenominator += dot(
                            cross(
                                rightBody
                                    ->ApplyInverseInertia(
                                        cross(
                                            rightRadius,
                                            tangent)),
                                rightRadius),
                            tangent);
                    }
                    // 滑りを止めるための接線力積
                    const float desiredTangentImpulse =
                        -dot(
                            relativeVelocity,
                            tangent)
                        / std::max(
                            tangentDenominator,
                            0.000001f);
                    // 摩擦係数で制限する力積上限
                    const float maximumFrictionImpulse =
                        accumulatedNormalImpulses[
                            pointIndex]
                        * material.friction;
                    // 上限適用後の接線力積量
                    const float tangentImpulseMagnitude =
                        std::clamp(
                            desiredTangentImpulse,
                            -maximumFrictionImpulse,
                            maximumFrictionImpulse);
                    // 接触点へ加える摩擦力積
                    const auto tangentImpulse =
                        multiply(
                            tangent,
                            tangentImpulseMagnitude);
                    if (leftDynamic)
                    {
                        leftBody->ApplyImpulseAtPoint(
                            tangentImpulse,
                            point);
                    }
                    if (rightDynamic)
                    {
                        rightBody->ApplyImpulseAtPoint(
                            multiply(
                                tangentImpulse,
                                -1.0f),
                            point);
                    }
                }
                if (pointCount > 1
                    && material.restitution
                        <= 0.0001f)
                {
                    if (leftDynamic)
                    {
                        leftBody->RemoveInwardVelocity(
                            contact.normal);
                    }
                    if (rightDynamic)
                    {
                        rightBody->RemoveInwardVelocity(
                            oppositeNormal);
                    }
                }
                if (material.restitution
                    <= 0.0001f)
                {
                    if (leftDynamic
                        && contact.normal.y > 0.5f)
                    {
                        supportedBodies.insert(
                            leftPhysicsObject->Id());
                    }
                    if (rightDynamic
                        && oppositeNormal.y > 0.5f)
                    {
                        supportedBodies.insert(
                            rightPhysicsObject->Id());
                    }
                }
            };

        // サブステップを集計した統計
        PhysicsBroadPhaseStats aggregateStats{};
        // 移動量に応じた分割数
        const std::size_t substepCount =
            PhysicsSubstepCount(deltaTime);
        // 一サブステップの秒数
        const float substepDeltaTime =
            deltaTime / static_cast<float>(substepCount);
        // サブステップ番号
        for (std::size_t substep{}; substep < substepCount; ++substep)
        {
            // 剛体または形状を調べる物体
            for (const auto& gameObject : m_gameObjects)
            {
                if (!gameObject->IsActiveInHierarchy())
                {
                    continue;
                }

                // 積分または休止判定する剛体
                if (auto* rigidbody =
                        gameObject->GetComponent<RigidbodyComponent>();
                    rigidbody != nullptr && rigidbody->IsEnabled())
                {
                    // 衝突位置で制限した積分秒数
                    float integrateDelta = substepDeltaTime;
                    // Continuousモードは移動をスイープし、最初の衝突位置までに制限します（トンネリング防止）。
                    if (rigidbody
                            ->UsesContinuousCollisionDetection()
                        && !rigidbody->IsKinematic()
                        && !rigidbody->IsSleeping())
                    {
                        // 重心の移動速度
                        const auto velocity =
                            rigidbody->Velocity();
                        // 重心速度の大きさ
                        const float speed = std::sqrt(
                            velocity.x * velocity.x
                            + velocity.y * velocity.y
                            + velocity.z * velocity.z);
                        // travel: 一substepで進む推定距離。
                        const float travel =
                            speed * substepDeltaTime;

                        // 連続判定に使う近似球の半径
                        float sweepRadius = 0.25f;
                        // 最後に取得した形状の対象層
                        std::uint32_t sweepMask =
                            0xffffffffu;
                        // 境界の最短半寸法で近似球を縮めます(bounds: 形状のワールド境界)。
                        const auto shrinkToBounds =
                            [&sweepRadius](
                                const Bounds3D& bounds)
                        {
                            sweepRadius = std::min({
                                sweepRadius,
                                std::abs(
                                    bounds.maximum.x
                                    - bounds.minimum.x)
                                    * 0.5f,
                                std::abs(
                                    bounds.maximum.y
                                    - bounds.minimum.y)
                                    * 0.5f,
                                std::abs(
                                    bounds.maximum.z
                                    - bounds.minimum.z)
                                    * 0.5f });
                        };
                        // 近似球の寸法を得る箱形状
                        if (const auto* box =
                            gameObject->GetComponent<
                                BoxCollider3DComponent>();
                            box != nullptr
                            && box->IsEnabled())
                        {
                            shrinkToBounds(
                                box->WorldBounds());
                            sweepMask =
                                box->CollisionMask();
                        }
                        // 近似球の寸法を得る球形状
                        if (const auto* sphere =
                            gameObject->GetComponent<
                                SphereCollider3DComponent>();
                            sphere != nullptr
                            && sphere->IsEnabled())
                        {
                            sweepRadius = std::min(
                                sweepRadius,
                                sphere->WorldSphere()
                                    .radius);
                            sweepMask =
                                sphere->CollisionMask();
                        }
                        // 近似球の寸法を得るカプセル
                        if (const auto* capsule =
                            gameObject->GetComponent<
                                CapsuleCollider3DComponent>();
                            capsule != nullptr
                            && capsule->IsEnabled())
                        {
                            sweepRadius = std::min(
                                sweepRadius,
                                capsule->Radius());
                            sweepMask =
                                capsule->CollisionMask();
                        }

                        // 1サブステップで自身の断面を超えて動く場合だけスイープします。
                        if (speed > 0.0001f
                            && travel > sweepRadius)
                        {
                            // 連続判定の対象層と除外物体
                            PhysicsQueryFilter sweepFilter;
                            sweepFilter.layerMask =
                                sweepMask;
                            sweepFilter
                                .ignoredGameObjectId =
                                gameObject->Id();
                            // 重心から速度方向への探索線
                            const Ray sweepRay{
                                rigidbody
                                    ->WorldCenterOfMass(),
                                {
                                    velocity.x / speed,
                                    velocity.y / speed,
                                    velocity.z / speed
                                } };
                            // 近似球が最初に当たる情報
                            PhysicsHit sweepHit{};
                            if (SphereCast(
                                    sweepRay,
                                    sweepRadius,
                                    travel,
                                    sweepHit,
                                    sweepFilter))
                            {
                                // 衝突面から残す隙間
                                constexpr float skin =
                                    0.01f;
                                // 衝突面まで進める距離
                                const float allowed =
                                    std::max(
                                        sweepHit.distance
                                            - skin,
                                        0.0f);
                                integrateDelta =
                                    substepDeltaTime
                                    * std::clamp(
                                        allowed / travel,
                                        0.0f,
                                        1.0f);
                                // 進入方向の速度を消して次ステップの再貫通を防止。
                                rigidbody
                                    ->RemoveInwardVelocity(
                                        sweepHit.normal);
                            }
                        }
                    }
                    rigidbody->Integrate(
                        *gameObject,
                        integrateDelta);
                }
            }
            // 位置拘束の反復回数
            constexpr int jointSolverIterations = 4;
            // ソルバーの反復番号
            for (int iteration{};
                // 位置拘束の反復回数
                iteration < jointSolverIterations;
                ++iteration)
            {
                SolveJoints(
                    substepDeltaTime,
                    iteration == 0);
            }

        // このサブステップの二次元衝突候補です。
        struct ColliderEntry2D final
        {
            // 形状の所有物体
            GameObject* gameObject{};
            // 箱形状の参照
            BoxCollider2DComponent* box{};
            // 円形状の参照
            CircleCollider2DComponent* circle{};
            // 多角形状の参照
            PolygonCollider2DComponent* polygon{};
            // 候補抽出時点のワールド境界
            Bounds2D bounds{};
            // 通知のみの形状か
            bool isTrigger{};
            // 形状の所属層
            std::uint32_t layer{};
            // 判定対象層のビット集合
            std::uint32_t collisionMask{};
            // 反発と摩擦の応答材質
            PhysicsMaterial material{};
        };
        // このサブステップの三次元衝突候補です。
        struct ColliderEntry3D final
        {
            // 形状の所有物体
            GameObject* gameObject{};
            // 箱形状の参照
            BoxCollider3DComponent* box{};
            // カプセル形状の参照
            CapsuleCollider3DComponent* capsule{};
            // 球形状の参照
            SphereCollider3DComponent* sphere{};
            // 凸形状の参照
            ConvexHullCollider3DComponent* hull{};
            // 三角形メッシュ形状の参照
            MeshCollider3DComponent* mesh{};
            // 候補抽出時点のワールド境界
            Bounds3D bounds{};
            // 通知のみの形状か
            bool isTrigger{};
            // 形状の所属層
            std::uint32_t layer{};
            // 判定対象層のビット集合
            std::uint32_t collisionMask{};
            // 反発と摩擦の応答材質
            PhysicsMaterial material{};
        };

        // 有効な二次元形状の一覧
        std::vector<ColliderEntry2D> entries2D;
        // 有効な三次元形状の一覧
        std::vector<ColliderEntry3D> entries3D;
        // 二次元候補抽出用の境界
        std::vector<Bounds2D> bounds2D;
        // 三次元候補抽出用の境界
        std::vector<Bounds3D> bounds3D;
        entries2D.reserve(m_gameObjects.size());
        entries3D.reserve(m_gameObjects.size());
        bounds2D.reserve(m_gameObjects.size());
        bounds3D.reserve(m_gameObjects.size());

        // 剛体または形状を調べる物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            // 候補一覧へ登録する形状
            if (auto* collider =
                gameObject->GetComponent<
                    BoxCollider2DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                // 候補抽出時点の形状境界
                const auto bounds = collider->WorldBounds();
                entries2D.push_back({
                    gameObject.get(),
                    collider,
                    nullptr,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds2D.push_back(bounds);
            }
            // 候補一覧へ登録する形状
            if (auto* collider =
                gameObject->GetComponent<
                    CircleCollider2DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                // 候補抽出時点の形状境界
                const auto bounds = collider->WorldBounds();
                entries2D.push_back({
                    gameObject.get(),
                    nullptr,
                    collider,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds2D.push_back(bounds);
            }
            // 候補一覧へ登録する形状
            if (auto* collider =
                gameObject->GetComponent<
                    PolygonCollider2DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                // 候補抽出時点の形状境界
                const auto bounds = collider->WorldBounds();
                entries2D.push_back({
                    gameObject.get(),
                    nullptr,
                    nullptr,
                    collider,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds2D.push_back(bounds);
            }
            // 候補一覧へ登録する形状
            if (auto* collider =
                gameObject->GetComponent<
                    BoxCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                // 候補抽出時点の形状境界
                const auto bounds = collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    collider,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
            // 候補一覧へ登録する形状
            if (auto* collider =
                gameObject->GetComponent<
                    CapsuleCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                // 候補抽出時点の形状境界
                const auto bounds = collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    nullptr,
                    collider,
                    nullptr,
                    nullptr,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
            // 候補一覧へ登録する形状
            if (auto* collider =
                gameObject->GetComponent<
                    SphereCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                // 候補抽出時点の形状境界
                const auto bounds =
                    collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    nullptr,
                    nullptr,
                    collider,
                    nullptr,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
            // 候補一覧へ登録する形状
            if (auto* collider =
                gameObject->GetComponent<
                    ConvexHullCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                // 候補抽出時点の形状境界
                const auto bounds =
                    collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    collider,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
            // 候補一覧へ登録する形状
            if (auto* collider =
                gameObject->GetComponent<
                    MeshCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled()
                && collider->HasMesh())
            {
                // 候補抽出時点の形状境界
                const auto bounds =
                    collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    collider,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
        }

        // 三次元空間分割の候補組
        const auto broadPhase3D = BuildSpatialHashPairs(
            bounds3D,
            m_physicsBroadPhaseCellSize);
        // 二次元空間分割の候補組
        const auto broadPhase2D = BuildSpatialHashPairs(
            bounds2D,
            m_physicsBroadPhaseCellSize);
        // 今回のサブステップの統計
        PhysicsBroadPhaseStats stepStats{
            entries2D.size(),
            entries3D.size(),
            broadPhase2D.pairs.size(),
            broadPhase3D.pairs.size(),
            0,
            0,
            broadPhase2D.occupiedCellCount,
            broadPhase3D.occupiedCellCount,
            broadPhase2D.oversizedColliderCount,
            broadPhase3D.oversizedColliderCount,
            0
        };

        // 接触位置の反復回数
        constexpr int collisionSolverIterations = 4;
        // 接触位置を解く反復番号
        for (int collisionIteration{};
            collisionIteration
                < collisionSolverIterations;
            ++collisionIteration)
        {
            // 空間分割で重なる候補組
            for (const auto& pair :
                broadPhase3D.pairs)
            {
                // 衝突組の左側の物体または形状
                auto& left = entries3D[pair.left];
                // 衝突組の右側の物体または形状
                auto& right = entries3D[pair.right];
                // leftPhysicsObject: 左形状を動かす物体, leftBody: 左側の剛体
                const auto [leftPhysicsObject, leftBody] =
                    findRigidbodyObject(
                        *left.gameObject);
                // rightPhysicsObject: 右形状を動かす物体, rightBody: 右側の剛体
                const auto [
                    rightPhysicsObject,
                    rightBody] =
                        findRigidbodyObject(
                            *right.gameObject);
                if (left.gameObject == right.gameObject
                    || (leftBody != nullptr
                        && rightBody != nullptr
                        && leftPhysicsObject
                            == rightPhysicsObject)
                    || CollisionSuppressedByJoint(
                        *leftPhysicsObject,
                        *rightPhysicsObject)
                    || (left.collisionMask
                        & (1u << right.layer)) == 0
                    || (right.collisionMask
                        & (1u << left.layer)) == 0
                    || !LayersCanCollide(
                        left.layer,
                        right.layer))
                {
                    continue;
                }

                if (collisionIteration == 0)
                {
                    ++stepStats
                        .narrowPhaseTestCount3D;
                }

                // メッシュコライダーは三角形単位で接触を生成し、1三角形ごとにprocessContactを呼びます（コールバックはキー単位で重複抑制済み）。
                if (left.mesh != nullptr
                    || right.mesh != nullptr)
                {
                    // 静的メッシュ同士は解決不要です。
                    if (left.mesh != nullptr
                        && right.mesh != nullptr)
                    {
                        continue;
                    }
                    // 左側がメッシュ形状か
                    const bool meshIsLeft =
                        left.mesh != nullptr;
                    // 三角形を取得するメッシュ形状
                    auto& meshEntry =
                        meshIsLeft ? left : right;
                    // メッシュと判定する相手形状
                    auto& otherEntry =
                        meshIsLeft ? right : left;

                    // 三角形取得境界の拡張幅
                    constexpr float queryMargin = 0.05f;
                    // 相手の境界を拡張した探索範囲
                    Bounds3D query = otherEntry.bounds;
                    query.minimum.x -= queryMargin;
                    query.minimum.y -= queryMargin;
                    query.minimum.z -= queryMargin;
                    query.maximum.x += queryMargin;
                    query.maximum.y += queryMargin;
                    query.maximum.z += queryMargin;
                    // 探索境界に重なる三角形
                    std::vector<MeshColliderTriangle>
                        triangles;
                    meshEntry.mesh->CollectTriangles(
                        query,
                        triangles);

                    // 一形状組の三角形接触処理上限
                    constexpr std::size_t
                        MaximumTriangleContacts = 8;
                    // 処理済みの三角形接触数
                    std::size_t triangleContacts{};
                    // 接触候補のワールド三角形
                    for (const auto& triangle : triangles)
                    {
                        // 三角形を表す凸形状
                        ConvexHull3D triangleHull{
                            {
                                triangle.a,
                                triangle.b,
                                triangle.c
                            } };
                        // 単一接触点の三次元判定結果
                        std::optional<Contact3D> single;
                        if (otherEntry.box != nullptr)
                        {
                            single = LamaPon::Intersect(
                                triangleHull,
                                otherEntry.box
                                    ->WorldBox());
                        }
                        else if (otherEntry.capsule
                            != nullptr)
                        {
                            single = LamaPon::Intersect(
                                triangleHull,
                                otherEntry.capsule
                                    ->WorldCapsule());
                        }
                        else if (otherEntry.sphere
                            != nullptr)
                        {
                            single = LamaPon::Intersect(
                                triangleHull,
                                otherEntry.sphere
                                    ->WorldSphere());
                        }
                        else if (otherEntry.hull
                            != nullptr)
                        {
                            single = LamaPon::Intersect(
                                triangleHull,
                                otherEntry.hull
                                    ->WorldHull());
                        }
                        if (!single)
                        {
                            continue;
                        }
                        // 法線は第1引数（三角形＝メッシュ側）を向くため、leftへ向くよう調整します。
                        // 法線を左側向きに揃える符号
                        const float normalScale =
                            meshIsLeft ? 1.0f : -1.0f;
                        // 三角形と相手形状の接触情報
                        const Contact meshContact{
                            {
                                single->normal.x
                                    * normalScale,
                                single->normal.y
                                    * normalScale,
                                single->normal.z
                                    * normalScale
                            },
                            single->penetration,
                            single->point
                        };
                        processContact(
                            *left.gameObject,
                            *right.gameObject,
                            meshContact,
                            true,
                            left.isTrigger
                                || right.isTrigger,
                            left.material,
                            right.material);
                        if (++triangleContacts
                            >= MaximumTriangleContacts)
                        {
                            break;
                        }
                    }
                    continue;
                }

                // 三次元形状間の接触情報
                std::optional<Contact> contact;
                if (left.box != nullptr
                    && right.box != nullptr)
                {
                    // 複数点を含む三次元接触情報
                    if (const auto manifold =
                        LamaPon::IntersectManifold(
                        left.box->WorldBox(),
                        right.box->WorldBox()))
                    {
                        // 接触位置の合計から得る代表点
                        DirectX::XMFLOAT3 average{};
                        // 接触点の添字
                        for (std::size_t index{};
                            index < manifold->pointCount;
                            ++index)
                        {
                            average.x +=
                                manifold->points[index].x;
                            average.y +=
                                manifold->points[index].y;
                            average.z +=
                                manifold->points[index].z;
                        }
                        // 接触点数の逆数
                        const float scale =
                            1.0f / static_cast<float>(
                                std::max<std::size_t>(
                                    manifold->pointCount,
                                    1));
                        average.x *= scale;
                        average.y *= scale;
                        average.z *= scale;
                        contact = Contact{
                            manifold->normal,
                            manifold->penetration,
                            average,
                            manifold->points,
                            manifold->pointCount
                        };
                    }
                }
                else if (
                    (left.capsule != nullptr
                        && right.box != nullptr)
                    || (left.box != nullptr
                        && right.capsule != nullptr))
                {
                    // カプセル×ボックスは両端球の2点マニフォールド。
                    // 左側がカプセル形状か
                    const bool capsuleIsLeft =
                        left.capsule != nullptr;
                    // 複数点を含む三次元接触情報
                    const auto manifold =
                        LamaPon::IntersectManifold(
                            (capsuleIsLeft
                                ? left.capsule
                                : right.capsule)
                                ->WorldCapsule(),
                            (capsuleIsLeft
                                ? right.box
                                : left.box)
                                ->WorldBox());
                    if (manifold)
                    {
                        // 法線はカプセル側を向くため、leftへ向くように調整します。
                        // 法線を左側向きに揃える符号
                        const float normalScale =
                            capsuleIsLeft ? 1.0f : -1.0f;
                        // 接触位置の合計から得る代表点
                        DirectX::XMFLOAT3 average{};
                        // 接触点の添字
                        for (std::size_t index{};
                            index < manifold->pointCount;
                            ++index)
                        {
                            average.x +=
                                manifold->points[index].x;
                            average.y +=
                                manifold->points[index].y;
                            average.z +=
                                manifold->points[index].z;
                        }
                        // 接触点数の逆数
                        const float scale =
                            1.0f / static_cast<float>(
                                std::max<std::size_t>(
                                    manifold->pointCount,
                                    1));
                        average.x *= scale;
                        average.y *= scale;
                        average.z *= scale;
                        contact = Contact{
                            {
                                manifold->normal.x
                                    * normalScale,
                                manifold->normal.y
                                    * normalScale,
                                manifold->normal.z
                                    * normalScale
                            },
                            manifold->penetration,
                            average,
                            manifold->points,
                            manifold->pointCount
                        };
                    }
                }
                else
                {
                    // 単一接触点の三次元判定結果
                    std::optional<Contact3D> single;
                    // 判定順と衝突組の順が逆か
                    bool reverseNormal{};
                    if (left.capsule != nullptr
                        && right.capsule != nullptr)
                    {
                        single = LamaPon::Intersect(
                        left.capsule->WorldCapsule(),
                        right.capsule
                            ->WorldCapsule());
                    }
                    else if (left.sphere != nullptr
                        && right.sphere != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.sphere->WorldSphere(),
                            right.sphere
                                ->WorldSphere());
                    }
                    else if (left.sphere != nullptr
                        && right.box != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.sphere->WorldSphere(),
                            right.box->WorldBox());
                    }
                    else if (left.box != nullptr
                        && right.sphere != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.sphere
                                ->WorldSphere(),
                            left.box->WorldBox());
                        reverseNormal = true;
                    }
                    else if (left.sphere != nullptr
                        && right.capsule != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.sphere->WorldSphere(),
                            right.capsule
                                ->WorldCapsule());
                    }
                    else if (left.capsule != nullptr
                        && right.sphere != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.sphere
                                ->WorldSphere(),
                            left.capsule
                                ->WorldCapsule());
                        reverseNormal = true;
                    }
                    else if (left.hull != nullptr
                        && right.hull != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.hull->WorldHull(),
                            right.hull->WorldHull());
                    }
                    else if (left.hull != nullptr
                        && right.box != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.hull->WorldHull(),
                            right.box->WorldBox());
                    }
                    else if (left.box != nullptr
                        && right.hull != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.hull->WorldHull(),
                            left.box->WorldBox());
                        reverseNormal = true;
                    }
                    else if (left.hull != nullptr
                        && right.capsule != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.hull->WorldHull(),
                            right.capsule
                                ->WorldCapsule());
                    }
                    else if (left.capsule != nullptr
                        && right.hull != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.hull->WorldHull(),
                            left.capsule
                                ->WorldCapsule());
                        reverseNormal = true;
                    }
                    else if (left.hull != nullptr
                        && right.sphere != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.hull->WorldHull(),
                            right.sphere->WorldSphere());
                    }
                    else if (left.sphere != nullptr
                        && right.hull != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.hull->WorldHull(),
                            left.sphere->WorldSphere());
                        reverseNormal = true;
                    }
                    if (single)
                    {
                        // 法線を左側向きに揃える符号
                        const float normalScale =
                            reverseNormal
                            ? -1.0f
                            : 1.0f;
                        contact = Contact{
                            {
                                single->normal.x
                                    * normalScale,
                                single->normal.y
                                    * normalScale,
                                single->normal.z
                                    * normalScale
                            },
                            single->penetration,
                            single->point
                        };
                    }
                }
                if (contact)
                {
                    processContact(
                        *left.gameObject,
                        *right.gameObject,
                        *contact,
                        true,
                        left.isTrigger
                            || right.isTrigger,
                        left.material,
                        right.material);
                }
            }

            // 空間分割で重なる候補組
            for (const auto& pair :
                broadPhase2D.pairs)
            {
                // 衝突組の左側の物体または形状
                auto& left = entries2D[pair.left];
                // 衝突組の右側の物体または形状
                auto& right = entries2D[pair.right];
                // leftPhysicsObject: 左形状を動かす物体, leftBody: 左側の剛体
                const auto [leftPhysicsObject, leftBody] =
                    findRigidbodyObject(
                        *left.gameObject);
                // rightPhysicsObject: 右形状を動かす物体, rightBody: 右側の剛体
                const auto [
                    rightPhysicsObject,
                    rightBody] =
                        findRigidbodyObject(
                            *right.gameObject);
                if (left.gameObject == right.gameObject
                    || (leftBody != nullptr
                        && rightBody != nullptr
                        && leftPhysicsObject
                            == rightPhysicsObject)
                    || CollisionSuppressedByJoint(
                        *leftPhysicsObject,
                        *rightPhysicsObject)
                    || (left.collisionMask
                        & (1u << right.layer)) == 0
                    || (right.collisionMask
                        & (1u << left.layer)) == 0
                    || !LayersCanCollide(
                        left.layer,
                        right.layer))
                {
                    continue;
                }

                if (collisionIteration == 0)
                {
                    ++stepStats
                        .narrowPhaseTestCount2D;
                }
                // 二次元形状間の接触情報
                std::optional<Contact> contact2D;
                if (left.box != nullptr
                    && right.box != nullptr)
                {
                    contact2D = IntersectBounds(
                        left.box->WorldBounds(),
                        right.box->WorldBounds());
                }
                else if (left.circle != nullptr
                    && right.circle != nullptr)
                {
                    contact2D = IntersectCircles(
                        left.circle->WorldCircle(),
                        right.circle->WorldCircle());
                }
                else if (left.circle != nullptr
                    && right.box != nullptr)
                {
                    contact2D = IntersectCircleBounds(
                        left.circle->WorldCircle(),
                        right.box->WorldBounds());
                }
                else if (left.box != nullptr
                    && right.circle != nullptr)
                {
                    contact2D = IntersectCircleBounds(
                        right.circle->WorldCircle(),
                        left.box->WorldBounds());
                    if (contact2D)
                    {
                        // 法線を left 側へ向け直します。
                        contact2D->normal.x =
                            -contact2D->normal.x;
                        contact2D->normal.y =
                            -contact2D->normal.y;
                    }
                }
                else if (left.polygon != nullptr
                    && right.polygon != nullptr)
                {
                    contact2D = IntersectPolygons(
                        left.polygon->WorldPolygon().vertices,
                        right.polygon->WorldPolygon().vertices);
                }
                else if (left.polygon != nullptr
                    && right.circle != nullptr)
                {
                    contact2D = IntersectPolygonCircle(
                        left.polygon->WorldPolygon().vertices,
                        right.circle->WorldCircle());
                }
                else if (left.circle != nullptr
                    && right.polygon != nullptr)
                {
                    contact2D = IntersectPolygonCircle(
                        right.polygon->WorldPolygon().vertices,
                        left.circle->WorldCircle());
                    if (contact2D)
                    {
                        // 法線を left 側へ向け直します。
                        contact2D->normal.x =
                            -contact2D->normal.x;
                        contact2D->normal.y =
                            -contact2D->normal.y;
                    }
                }
                else if (left.polygon != nullptr
                    && right.box != nullptr)
                {
                    contact2D = IntersectPolygons(
                        left.polygon->WorldPolygon().vertices,
                        BoxBoundsToPolygon(
                            right.box->WorldBounds()));
                }
                else if (left.box != nullptr
                    && right.polygon != nullptr)
                {
                    contact2D = IntersectPolygons(
                        BoxBoundsToPolygon(
                            left.box->WorldBounds()),
                        right.polygon->WorldPolygon().vertices);
                }
                if (contact2D)
                {
                    processContact(
                        *left.gameObject,
                        *right.gameObject,
                        *contact2D,
                        false,
                        left.isTrigger
                            || right.isTrigger,
                        left.material,
                        right.material);
                }
            }
        }
            aggregateStats.colliderCount2D =
                stepStats.colliderCount2D;
            aggregateStats.colliderCount3D =
                stepStats.colliderCount3D;
            aggregateStats.candidatePairCount2D +=
                stepStats.candidatePairCount2D;
            aggregateStats.candidatePairCount3D +=
                stepStats.candidatePairCount3D;
            aggregateStats.narrowPhaseTestCount2D +=
                stepStats.narrowPhaseTestCount2D;
            aggregateStats.narrowPhaseTestCount3D +=
                stepStats.narrowPhaseTestCount3D;
            aggregateStats.occupiedCellCount2D =
                stepStats.occupiedCellCount2D;
            aggregateStats.occupiedCellCount3D =
                stepStats.occupiedCellCount3D;
            aggregateStats.oversizedColliderCount2D =
                stepStats.oversizedColliderCount2D;
            aggregateStats.oversizedColliderCount3D =
                stepStats.oversizedColliderCount3D;
        }

        // 剛体または形状を調べる物体
        for (const auto& gameObject : m_gameObjects)
        {
            // 積分または休止判定する剛体
            if (auto* rigidbody =
                    gameObject->GetComponent<RigidbodyComponent>();
                rigidbody != nullptr)
            {
                rigidbody->UpdateSleepState(
                    deltaTime,
                    supportedBodies.contains(
                        gameObject->Id()));
                rigidbody->ClearAccumulators();
            }
        }

        // key: 前回の衝突組, wasTrigger: 前回のトリガー区分
        for (const auto& [key, wasTrigger] : m_activeCollisions)
        {
            if (currentCollisions.contains(key))
            {
                continue;
            }

            // leftId: 左物体番号, rightId: 右物体番号, is3D: 三次元判定区分
            const auto [leftId, rightId, is3D] = key;
            static_cast<void>(is3D);
            // 衝突組の左側の物体または形状
            auto* left = FindGameObject(leftId);
            // 衝突組の右側の物体または形状
            auto* right = FindGameObject(rightId);
            if (left != nullptr && right != nullptr)
            {
                left->NotifyCollisionExit(*right, wasTrigger);
                right->NotifyCollisionExit(*left, wasTrigger);
            }
        }

        m_activeCollisions = std::move(currentCollisions);
        m_physicsStats = aggregateStats;
        m_physicsStats.activeContactCount =
            m_activeCollisions.size();
        if (m_physicsDebugCaptureEnabled)
        {
            m_physicsDebugContacts = std::move(debugContacts);
        }
    }

    void Scene::SetPhysicsDebugCaptureEnabled(
        const bool enabled) noexcept
    {
        m_physicsDebugCaptureEnabled = enabled;
        if (!enabled)
        {
            m_physicsDebugContacts.clear();
        }
    }

    void Scene::Render()
    {
        RenderTargetTextures();
        RenderMainCamera(m_graphics.AspectRatio(), true);
    }

    void Scene::RenderGameFrame(const float clearColor[4])
    {
        RenderTargetTextures();
        // D3D11はHDR、D3D12はLDRでシーンを合成します。
        m_graphics.BeginSceneComposition(clearColor);
        RenderMainCamera(
            m_graphics.AspectRatio(),
            false,
            m_graphics.SceneCompositionTarget());
        m_graphics.EndSceneComposition(PostProcessFrameData());
        Render2D();
    }

    void Scene::RenderTargetTextures()
    {
        // 再入（レンダーテクスチャ描画中の再呼び出し）は無視します。
        if (m_renderingTargetTextures)
        {
            return;
        }
        // メインカメラを描かないエディター表示でも、ここでベイク要求を処理します。
        BakePendingReflectionProbes();
        ProcessBakedGlobalIlluminationBake();

        // テクスチャへ描画するカメラ
        std::vector<CameraComponent*> targetCameras;
        // 描画対象の物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }
            // テクスチャ描画対象のカメラ
            auto* camera =
                gameObject->GetComponent<CameraComponent>();
            if (camera == nullptr
                || !camera->IsEnabled()
                || !camera->RendersToTexture())
            {
                continue;
            }
            targetCameras.push_back(camera);
        }
        if (targetCameras.empty())
        {
            return;
        }

        // 再入防止フラグの復元範囲
        const BooleanStateScope targetScope{
            m_renderingTargetTextures,
            true
        };
        // 補間描画フラグの復元範囲
        const BooleanStateScope interpolationScope{
            m_renderingInterpolatedTransforms,
            true
        };
        // 後続の描画先とUI基準サイズを例外時も復元します。
        // 元の描画先を復元する範囲
        GraphicsOutputStateScope outputStateScope{ m_graphics };
        // UI基準サイズを復元する範囲
        UIViewportSizeScope uiViewportScope{ m_graphics };
        // 対象描画のGPU計測区間
        GpuProfiler::SectionScope gpuSectionScope{
            m_graphics.Gpu(),
            "レンダーテクスチャ"
        };
        // テクスチャ描画対象のカメラ
        for (auto* camera : targetCameras)
        {
            // カメラが使う描画先
            auto& target =
                m_graphics.AcquireRenderTexture(
                    camera->TargetTexture(),
                    camera->TargetTextureWidth(),
                    camera->TargetTextureHeight());
            if (!target.IsValid())
            {
                Logger::Instance().Warning(
                    "レンダーテクスチャを作成できませんでした: "
                    + camera->TargetTexture());
                continue;
            }

            m_graphics.SetUIViewportSize(
                target.Width(),
                target.Height());
            // カメラの背景色
            const auto& clearColor =
                camera->TargetClearColor();
            // 描画先を初期化するRGBA背景色
            const float clear[4]{
                clearColor.x,
                clearColor.y,
                clearColor.z,
                clearColor.w
            };
            m_graphics.BeginOffscreenTarget(
                target,
                clear);
            RenderWithMatrices(
                camera->ViewMatrix(),
                camera->ProjectionMatrix(
                    target.AspectRatio()),
                false,
                false,
                &target);

            RunPostProcess(
                m_graphics,
                target,
                PostProcessFrameData());
            // ポスト処理でテクスチャを入れ替えるため、表示用へコピーしてから参照側に渡します。
            m_graphics.PublishOffscreenTarget(target);
        }
        gpuSectionScope.End();
        uiViewportScope.Restore();
        outputStateScope.Restore();
    }

    void Scene::RenderMainCamera(
        const float aspectRatio,
        const bool include2D,
        RenderTarget* target)
    {
        // 現在の描画へ反映するため、先にベイク要求を処理します。
        BakePendingReflectionProbes();
        ProcessBakedGlobalIlluminationBake();
        // 補間描画フラグの復元範囲
        const BooleanStateScope interpolationScope{
            m_renderingInterpolatedTransforms,
            true
        };
        if (m_mainCamera != nullptr && m_mainCamera->IsEnabled())
        {
            // メインカメラのビュー行列
            const auto view = m_mainCamera->ViewMatrix();
            // 今回の深度とカラーの射影
            const auto projection = m_mainCamera->ProjectionMatrix(aspectRatio);
            RenderWithMatrices(
                view,
                projection,
                include2D,
                false,
                target);
            return;
        }

        if (!include2D)
        {
            return;
        }

        Render2D();
    }

    void Scene::Render2D()
    {
        // 2DとUIは3Dのポストエフェクト適用後にも単独で描画できます。
        // 補間描画フラグの復元範囲
        const BooleanStateScope interpolationScope{
            m_renderingInterpolatedTransforms,
            true
        };
        // 対象描画のGPU計測区間
        GpuProfiler::SectionScope gpuSectionScope{
            m_graphics.Gpu(),
            "2D／UI"
        };
        RenderSprites2D(
            m_gameObjects,
            m_graphics);
    }

    // 指定行列でシーンを描きます(view: 可逆なビュー行列, rawProjection: ジッターなしの射影, include2D: 2Dも描くか, renderDebug: デバッグ表示を描くか, target: 深度と履歴を作る描画先)。
    void Scene::RenderWithMatrices(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX rawProjection,
        const bool include2D,
        const bool renderDebug,
        RenderTarget* target)
    {
        // 深度とカラーに同じTAAジッターを使い、プローブのベイク中はずらしません。
        m_temporalFrame = {};
        // 今回TAAのジッターを使うか
        const bool temporalWanted =
            m_temporalAntiAliasing.enabled
            && target != nullptr
            && !m_bakingReflectionProbes;
        // 今回の深度とカラーの射影
        DirectX::XMMATRIX projection = rawProjection;
        if (temporalWanted)
        {
            // サブピクセルの射影移動量
            const auto jitter = TemporalJitterOffset(
                m_temporalFrameIndex);
            projection = ApplyTemporalJitter(
                rawProjection,
                DirectX::XMFLOAT2{
                    jitter.x
                        * m_temporalAntiAliasing
                            .jitterScale,
                    jitter.y
                        * m_temporalAntiAliasing
                            .jitterScale },
                target->Width(),
                target->Height());
            ++m_temporalFrameIndex;
        }

        // SSAOが深度をビュー空間へ戻すため、現在の描画で使用する射影を記録します。
        // 深度復元へ渡す今回の射影
        DirectX::XMFLOAT4X4 storedProjection{};
        DirectX::XMStoreFloat4x4(
            &storedProjection,
            projection);
        m_graphics.SetSceneProjection(storedProjection);
        // 被写界深度には今回の深度と同じ射影を渡し、プローブのベイク中は無効にします。
        m_depthOfFieldFrame = {};
        if (!m_bakingReflectionProbes)
        {
            m_depthOfFieldFrame.settings = m_depthOfField;
            m_depthOfFieldFrame.projection = storedProjection;
        }
        m_screenOutlineFrame = {};
        if (target != nullptr && !m_bakingReflectionProbes)
        {
            m_screenOutlineFrame.projection = storedProjection;
        }
        // 補間描画フラグの復元範囲
        const BooleanStateScope interpolationScope{
            m_renderingInterpolatedTransforms,
            true
        };
        // プローブのベイク中はカリングを無効にし、必要な描画対象がベイク結果から欠落しないようにします。
        // カリングとLODの判定結果
        const auto visibility = m_bakingReflectionProbes
            ? VisibilityResult{}
            : BuildRenderVisibility(view, projection);
        if (!m_bakingReflectionProbes)
        {
            m_visibilityStats = visibility.stats;
        }

        // ベイク中の反射再帰と前回の影の流用を防ぐため、反射・影のフレーム情報を消します。
        m_volumetricFrame = {};
        m_frameReflectionProbes.clear();
        if (!m_bakingReflectionProbes)
        {
            // 描画対象の物体
            for (const auto& gameObject : m_gameObjects)
            {
                if (!gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                // 今回使える反射プローブ
                auto* probe = gameObject->GetComponent<
                    ReflectionProbeComponent>();
                if (probe != nullptr
                    && probe->IsEnabled()
                    && probe->IsBaked())
                {
                    m_frameReflectionProbes.push_back(
                        probe);
                }
            }
        }

        // 今回の描画へ渡す照明情報
        auto lighting = BuildLightingState();

        // 最初に影を生成する方向光源
        const DirectionalLightComponent* shadowLight{};
        // 固定照明配列内の方向光源添字
        std::size_t shadowLightIndex{};
        // 有効な方向光源の通し添字
        std::size_t directionalIndex{};
        // 描画対象の物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            // 影を生成できる方向光源候補
            const auto* candidate =
                gameObject->GetComponent<
                    DirectionalLightComponent>();
            if (candidate == nullptr
                || !candidate->IsEnabled())
            {
                continue;
            }
            if (directionalIndex
                == MaximumDirectionalLights)
            {
                break;
            }
            if (candidate->CastsShadows())
            {
                shadowLight = candidate;
                shadowLightIndex = directionalIndex;
                break;
            }
            ++directionalIndex;
        }

        // 影の深度パスとGPU計測区間を例外時も終了するスコープです。
        struct DepthOnlyPassScope final
        {
            // 深度パスを切り替えるデバイス
            GraphicsDevice& graphics;
            // 影描画のGPU計測区間
            GpuProfiler::SectionScope gpuSection;
            // 影の深度パスと計測を開始します(device: 描画デバイス, gpuSectionName: GPU計測区間名)。
            DepthOnlyPassScope(
                GraphicsDevice& device,
                const char* gpuSectionName)
                : graphics(device)
                , gpuSection(device.Gpu(), gpuSectionName)
            {
                graphics.SetDepthPass(
                    DepthPassKind::Shadow);
            }
            // 計測区間と影の深度パスを終了します。
            ~DepthOnlyPassScope() noexcept
            {
                gpuSection.End();
                graphics.SetDepthPass(
                    DepthPassKind::None);
            }
        };

        if (m_graphics.Settings().shadowsEnabled
            && shadowLight != nullptr
            && m_graphics.Shadows().IsValid())
        {
            // 影の深度パスを終了する範囲
            const DepthOnlyPassScope depthScope{
                m_graphics,
                "影(カスケード)" };
            // 方向光のカスケード行列群
            const auto shadowMatrices =
                BuildDirectionalShadowMatrices(
                    view,
                    projection,
                    shadowLight->WorldDirection(),
                    shadowLight->ShadowDistance(),
                    std::min({
                        static_cast<std::size_t>(
                            shadowLight->
                                ShadowCascadeCount()),
                        static_cast<std::size_t>(
                            m_graphics.Settings().
                                shadowCascadeLimit),
                        static_cast<std::size_t>(
                            m_graphics.Shadows().
                                CascadeCount())
                    }),
                    shadowLight->ShadowSplitLambda(),
                    m_graphics.Shadows().Resolution());

            // 影を参照しない深度パス用照明
            auto shadowPassLighting = lighting;
            shadowPassLighting.directionalShadow.enabled =
                false;
            shadowPassLighting.directionalShadow.texture.Reset();
            m_graphics.SetLightingState(
                shadowPassLighting);

            // 方向光の影描画先
            auto& shadowMap = m_graphics.Shadows();
            // 影のカスケード添字
            for (std::size_t cascadeIndex = 0;
                cascadeIndex < shadowMatrices.count;
                ++cascadeIndex)
            {
                // 描画するカスケードの行列
                const auto& cascade =
                    shadowMatrices.cascades[cascadeIndex];
                // 影視錐台とLODによる非表示組
                const auto shadowHidden =
                    BuildShadowFrustumHidden(
                        cascade.view,
                        cascade.projection,
                        visibility.lodHidden);
                m_graphics.BeginShadowMap(
                    shadowMap,
                    static_cast<std::uint32_t>(
                        cascadeIndex));
                try
                {
                    // 描画対象の物体
                    for (const auto& gameObject : m_gameObjects)
                    {
                        if (visibility.lodHidden.contains(
                                gameObject->Id())
                            || shadowHidden.contains(
                                gameObject->Id()))
                        {
                            continue;
                        }
                        gameObject->Render3D(
                            m_graphics,
                            cascade.view,
                            cascade.projection);
                    }
                }
                catch (...)
                {
                    m_graphics.EndShadowMap(shadowMap);
                    throw;
                }
                m_graphics.EndShadowMap(shadowMap);
            }

            // カラーへ渡す方向光の影情報
            auto& shadow =
                lighting.directionalShadow;
            shadow.cascadeCount =
                shadowMatrices.count;
            // 影のカスケード添字
            for (std::size_t cascadeIndex = 0;
                cascadeIndex < shadowMatrices.count;
                ++cascadeIndex)
            {
                // 描画するカスケードの行列
                const auto& cascade =
                    shadowMatrices.cascades[cascadeIndex];
                DirectX::XMStoreFloat4x4(
                    &shadow.lightViewProjections[
                        cascadeIndex],
                    cascade.view
                        * cascade.projection);
                shadow.cascadeSplits[cascadeIndex] =
                    cascade.splitDistance;
            }
            shadow.texture =
                shadowMap.ViewHandle();
            shadow.lightIndex = shadowLightIndex;
            shadow.bias = shadowLight->ShadowBias();
            shadow.normalBias =
                shadowLight->ShadowNormalBias();
            shadow.strength =
                shadowLight->ShadowStrength();
            shadow.enabled = true;

            // ボリュメトリックライトへ今回の影とカメラ情報を渡し、プローブのベイク中は無効にします。
            if (m_volumetricLight.enabled
                && !m_bakingReflectionProbes)
            {
                // 今回のボリュメトリック積算情報
                auto& frame = m_volumetricFrame;
                frame.settings = m_volumetricLight;
                // 影の範囲外では遮蔽を判定できないため、積算距離を影の距離までに制限します。
                frame.settings.maximumDistance = std::min(
                    frame.settings.maximumDistance,
                    shadowLight->ShadowDistance());
                // 光の積算へ渡す影とカメラ情報
                auto& inputs = frame.inputs;
                inputs.cascadeShadow = shadow.texture;
                inputs.cascadeCount =
                    static_cast<std::uint32_t>(
                        shadowMatrices.count);
                inputs.shadowBias =
                    shadowLight->ShadowBias();
                inputs.shadowResolution =
                    lighting.directionalShadowResolution;
                inputs.lightDirection =
                    shadowLight->WorldDirection();
                // 空気の濃さを表す効果設定とは別に、光源の色と強度を渡します。
                // 影を生成する光源色
                const auto& lightColor =
                    shadowLight->Color();
                // 影を生成する光源の強度
                const float lightIntensity =
                    shadowLight->Intensity();
                inputs.lightColor = {
                    lightColor.x * lightIntensity,
                    lightColor.y * lightIntensity,
                    lightColor.z * lightIntensity
                };
                // 影のカスケード添字
                for (std::size_t cascadeIndex = 0;
                    cascadeIndex < shadowMatrices.count
                        && cascadeIndex < 4;
                    ++cascadeIndex)
                {
                    inputs.cascadeViewProjections[
                        cascadeIndex] =
                        shadow.lightViewProjections[
                            cascadeIndex];
                }
                // 逆行列計算の行列式出力
                DirectX::XMVECTOR determinant{};
                // ワールド復元用の逆ビュー射影
                const auto inverseViewProjection =
                    DirectX::XMMatrixInverse(
                        &determinant,
                        view * projection);
                DirectX::XMStoreFloat4x4(
                    &inputs.inverseViewProjection,
                    inverseViewProjection);
                // カメラ位置を得る逆ビュー行列
                const auto inverseView =
                    DirectX::XMMatrixInverse(
                        &determinant,
                        view);
                DirectX::XMStoreFloat3(
                    &inputs.cameraPosition,
                    inverseView.r[3]);
            }
        }

        // スポットライトの影パス（最大MaximumSpotShadows灯）。
        if (m_graphics.Settings().shadowsEnabled
            && m_graphics.SpotShadows().IsValid())
        {
            struct SpotShadowCandidate final
            {
                // 影を生成するスポット光源
                const SpotLightComponent* light{};
                // 固定照明配列内の光源添字
                std::size_t lightIndex{};
            };
            // 影を生成するスポット光源候補
            std::array<
                SpotShadowCandidate,
                MaximumSpotShadows> candidates{};
            // 選択したスポット影の数
            std::size_t candidateCount = 0;
            // 有効なスポット光源の添字
            std::size_t spotIndex = 0;
            // 品質設定で許可する光源数
            const std::size_t spotLimit =
                std::min<std::size_t>(
                    m_graphics.Settings().spotLightLimit,
                    MaximumSpotLights);
            // 描画対象の物体
            for (const auto& gameObject : m_gameObjects)
            {
                if (!gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                // 影を生成できるスポット光源候補
                const auto* candidate =
                    gameObject->GetComponent<
                        SpotLightComponent>();
                if (candidate == nullptr
                    || !candidate->IsEnabled())
                {
                    continue;
                }
                if (spotIndex >= spotLimit)
                {
                    break;
                }
                if (candidate->CastsShadows()
                    && candidateCount
                        < MaximumSpotShadows)
                {
                    candidates[candidateCount++] = {
                        candidate,
                        spotIndex
                    };
                }
                ++spotIndex;
            }

            if (candidateCount > 0)
            {
                // 影の深度パスを終了する範囲
                const DepthOnlyPassScope depthScope{
                    m_graphics,
                    "影(スポット)" };
                m_graphics.SetLightingState(lighting);
                // スポット光の影描画先
                auto& spotShadowMap =
                    m_graphics.SpotShadows();
                // スポット影のスロット添字
                for (std::size_t slot = 0;
                    // 選択したスポット影の数
                    slot < candidateCount;
                    ++slot)
                {
                    // 影を描くスポット光源
                    const auto* light =
                        candidates[slot].light;
                    // 影カメラのワールド位置
                    const auto eye =
                        light->WorldPosition();
                    // 影カメラの視線方向
                    const auto direction =
                        light->WorldDirection();
                    // 視線と平行にならない上方向
                    const auto up =
                        std::abs(direction.y) > 0.99f
                        ? DirectX::XMFLOAT3{
                            0.0f, 0.0f, 1.0f }
                        : DirectX::XMFLOAT3{
                            0.0f, 1.0f, 0.0f };
                    // スポット影のビュー行列
                    const auto shadowView =
                        DirectX::XMMatrixLookToRH(
                            DirectX::XMLoadFloat3(&eye),
                            DirectX::XMLoadFloat3(
                                &direction),
                            DirectX::XMLoadFloat3(&up));
                    // スポット影の視野角ラジアン
                    const float fieldOfView = std::clamp(
                        light->OuterConeAngle() * 2.0f,
                        DirectX::XMConvertToRadians(5.0f),
                        DirectX::XMConvertToRadians(
                            170.0f));
                    // スポット影の射影行列
                    const auto shadowProjection =
                        DirectX::
                            XMMatrixPerspectiveFovRH(
                                fieldOfView,
                                1.0f,
                                0.1f,
                                std::max(
                                    light->Range(),
                                    0.2f));
                    // 影視錐台とLODによる非表示組
                    const auto shadowHidden =
                        BuildShadowFrustumHidden(
                            shadowView,
                            shadowProjection,
                            visibility.lodHidden);

                    m_graphics.BeginShadowMap(
                        spotShadowMap,
                        static_cast<std::uint32_t>(slot));
                    try
                    {
                        // 描画対象の物体
                        for (const auto& gameObject :
                            m_gameObjects)
                        {
                            if (visibility.lodHidden
                                    .contains(
                                        gameObject->Id())
                                || shadowHidden.contains(
                                    gameObject->Id()))
                            {
                                continue;
                            }
                            gameObject->Render3D(
                                m_graphics,
                                shadowView,
                                shadowProjection);
                        }
                    }
                    catch (...)
                    {
                        m_graphics.EndShadowMap(
                            spotShadowMap);
                        throw;
                    }
                    m_graphics.EndShadowMap(
                        spotShadowMap);

                    // カラーへ渡す局所光源の影情報
                    auto& destination =
                        lighting.spotShadows[slot];
                    DirectX::XMStoreFloat4x4(
                        &destination.lightViewProjection,
                        shadowView * shadowProjection);
                    destination.lightIndex =
                        static_cast<std::ptrdiff_t>(
                            candidates[slot].lightIndex);
                    destination.bias =
                        light->ShadowBias();
                    destination.normalBias =
                        light->ShadowNormalBias();
                    destination.strength =
                        light->ShadowStrength();
                    destination.enabled = true;
                }
                lighting.spotShadowTexture =
                    spotShadowMap.ViewHandle();
            }
        }

        // ポイントライトの影パス（最初の1灯、キューブ6面）。
        if (m_graphics.Settings().shadowsEnabled
            && m_graphics.PointShadows().IsValid())
        {
            // 最初に影を生成するポイント光源
            const PointLightComponent* pointShadowLight{};
            // 固定配列内のポイント光源添字
            std::size_t pointShadowIndex{};
            // 有効なポイント光源の添字
            std::size_t pointIndex = 0;
            // 品質設定のポイント光源上限
            const std::size_t pointLimit =
                std::min<std::size_t>(
                    m_graphics.Settings().pointLightLimit,
                    MaximumPointLights);
            // 描画対象の物体
            for (const auto& gameObject : m_gameObjects)
            {
                if (!gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                // 影を生成できるポイント光源候補
                const auto* candidate =
                    gameObject->GetComponent<
                        PointLightComponent>();
                if (candidate == nullptr
                    || !candidate->IsEnabled())
                {
                    continue;
                }
                if (pointIndex >= pointLimit)
                {
                    break;
                }
                if (candidate->CastsShadows())
                {
                    pointShadowLight = candidate;
                    pointShadowIndex = pointIndex;
                    break;
                }
                ++pointIndex;
            }

            if (pointShadowLight != nullptr)
            {
                // 影の深度パスを終了する範囲
                const DepthOnlyPassScope depthScope{
                    m_graphics,
                    "影(ポイント)" };
                m_graphics.SetLightingState(lighting);
                // ポイント光の影描画先
                auto& pointShadowMap =
                    m_graphics.PointShadows();
                // 影カメラのワールド位置
                const auto eye =
                    pointShadowLight->WorldPosition();
                // 影カメラの遠平面距離
                const float farPlane = std::max(
                    pointShadowLight->Range(),
                    0.2f);
                // キューブ面の向きは、EvaluatePointShadowの左手系深度復元と揃えます。
                // キューブ面ごとの左手系視線方向
                static constexpr DirectX::XMFLOAT3
                    FaceDirections[6]{
                        { 1.0f, 0.0f, 0.0f },
                        { -1.0f, 0.0f, 0.0f },
                        { 0.0f, 1.0f, 0.0f },
                        { 0.0f, -1.0f, 0.0f },
                        { 0.0f, 0.0f, 1.0f },
                        { 0.0f, 0.0f, -1.0f }
                    };
                // キューブ面ごとの上方向
                static constexpr DirectX::XMFLOAT3
                    FaceUps[6]{
                        { 0.0f, 1.0f, 0.0f },
                        { 0.0f, 1.0f, 0.0f },
                        { 0.0f, 0.0f, -1.0f },
                        { 0.0f, 0.0f, 1.0f },
                        { 0.0f, 1.0f, 0.0f },
                        { 0.0f, 1.0f, 0.0f }
                    };
                // キューブ影各面の射影行列
                const auto faceProjection =
                    DirectX::XMMatrixPerspectiveFovLH(
                        DirectX::XM_PIDIV2,
                        1.0f,
                        0.1f,
                        farPlane);
                // キューブ影の面番号
                for (std::uint32_t face = 0;
                    face < 6u;
                    ++face)
                {
                    // キューブ影対象面のビュー行列
                    const auto faceView =
                        DirectX::XMMatrixLookToLH(
                            DirectX::XMLoadFloat3(&eye),
                            DirectX::XMLoadFloat3(
                                &FaceDirections[face]),
                            DirectX::XMLoadFloat3(
                                &FaceUps[face]));
                    // 影視錐台とLODによる非表示組
                    const auto shadowHidden =
                        BuildShadowFrustumHidden(
                            faceView,
                            faceProjection,
                            visibility.lodHidden);
                    m_graphics.BeginShadowMap(
                        pointShadowMap,
                        face);
                    try
                    {
                        // 描画対象の物体
                        for (const auto& gameObject :
                            m_gameObjects)
                        {
                            if (visibility.lodHidden
                                    .contains(
                                        gameObject->Id())
                                || shadowHidden.contains(
                                    gameObject->Id()))
                            {
                                continue;
                            }
                            gameObject->Render3D(
                                m_graphics,
                                faceView,
                                faceProjection);
                        }
                    }
                    catch (...)
                    {
                        m_graphics.EndShadowMap(
                            pointShadowMap);
                        throw;
                    }
                    m_graphics.EndShadowMap(
                        pointShadowMap);
                }
                // カラーへ渡す局所光源の影情報
                auto& destination = lighting.pointShadow;
                destination.texture =
                    pointShadowMap.ViewHandle();
                destination.lightIndex =
                    static_cast<std::ptrdiff_t>(
                        pointShadowIndex);
                destination.bias =
                    pointShadowLight->ShadowBias();
                destination.strength =
                    pointShadowLight->ShadowStrength();
                destination.enabled = true;
            }
        }

        // SSAOは直接光を暗くしないよう、カラー描画前に環境光とIBL用の遮蔽を作ります。
        if (target != nullptr)
        {
            // 深度結果を取得し、行列を参照するコールバックはこの呼び出し内で同期実行します。
            // 可視物体を同期描画する深度パスの結果
            const auto prepass = RunDepthPrepass(
                m_graphics,
                *target,
                storedProjection,
                m_ambientOcclusion,
                m_screenSpaceReflection,
                [this, &visibility, &view, &projection]()
                {
                    // 描画対象の物体
                    for (const auto& gameObject :
                        m_gameObjects)
                    {
                        if (visibility.renderHidden
                                .contains(
                                    gameObject->Id()))
                        {
                            continue;
                        }
                        gameObject->Render3D(
                            m_graphics,
                            view,
                            projection);
                    }
                });
            if (prepass.ambientOcclusionResolved)
            {
                // カラーで参照するSSAO情報
                auto& occlusion =
                    lighting.screenAmbientOcclusion;
                occlusion.texture =
                    target->
                        AmbientOcclusionViewHandle();
                occlusion.inverseWidth = 1.0f
                    / static_cast<float>(
                        std::max(target->Width(), 1u));
                occlusion.inverseHeight = 1.0f
                    / static_cast<float>(
                        std::max(target->Height(), 1u));
                occlusion.enabled = true;
            }

            // SSRは前回のカラー履歴があるときだけ有効にします。
            if (prepass.depthAvailable
                && m_screenSpaceReflection.enabled
                && !m_bakingReflectionProbes)
            {
                // 前回のカラー履歴ビュー
                const auto history =
                    target->ColorHistoryViewHandle();
                if (history)
                {
                    // カラーで参照するSSR情報
                    auto& reflection =
                        lighting.screenSpaceReflection;
                    reflection.texture = history;
                    // 深度はコピーを読みます（本体はDSVとして刺さっているためSRVにできません）。
                    m_graphics.CaptureOffscreenTargetDepth(
                        *target);
                    // SSRの探索用にビュー空間距離の最小値を持つミップ列を作ります。
                    if (m_graphics
                            .TryBuildReflectionDepthPyramid(
                                *target,
                                storedProjection._33,
                                storedProjection._43))
                    {
                        reflection.depth =
                            target->
                                ReflectionDepthPyramidViewHandle();
                        reflection.depthPyramidMaximumMip =
                            target->ReflectionDepthPyramidMipCount()
                                > 0
                            ? target
                                ->ReflectionDepthPyramidMipCount()
                                - 1
                            : 0;
                        reflection.previousViewProjection =
                            target->
                                ColorHistoryViewProjection();
                        reflection.inverseWidth = 1.0f
                            / static_cast<float>(
                                std::max(target->Width(), 1u));
                        reflection.inverseHeight = 1.0f
                            / static_cast<float>(
                                std::max(
                                    target->Height(), 1u));
                        // 深度をビュー空間のZへ戻すための値。
                        reflection.projectionZ =
                            storedProjection._33;
                        reflection.projectionW =
                            storedProjection._43;
                        reflection.intensity =
                            m_screenSpaceReflection.intensity;
                        reflection.maximumDistance =
                            m_screenSpaceReflection
                                .maximumDistance;
                        reflection.thickness =
                            m_screenSpaceReflection.thickness;
                        reflection.roughnessCutoff =
                            m_screenSpaceReflection
                                .roughnessCutoff;
                        reflection.stepCount =
                            m_screenSpaceReflection.stepCount;
                        reflection.enabled = true;
                    }
                    else
                    {
                        // stale / 別BackendのRenderTargetはこのフレームのSSRを無効にし、native viewを誤bindしません。
                        reflection = {};
                    }
                }
            }
            // プリパスとSSAOで描画先が変わっているので、カラーへ戻します（深度は消さずにそのまま使います）。
            m_graphics.BindOffscreenTarget(*target);
        }

        // キューブマップスカイとIBL（環境反射）。
        // スカイとIBLの元キューブ
        GraphicsViewHandle skyCubemapView;
        if (m_sky.enabled
            && !m_sky.cubemapPath.empty()
            && m_graphics.IsInitialized())
        {
            try
            {
                // スカイ用キューブのアセット
                if (const auto texture =
                        m_graphics.Assets().LoadTexture(
                            m_sky.cubemapPath);
                    texture != nullptr && texture->isCube)
                {
                    // スカイの描画資源の所有参照
                    const auto skyResources =
                        texture->resources.Acquire();
                    if (skyResources != nullptr)
                    {
                        skyCubemapView =
                            skyResources->shaderResourceView;
                    }
                }
            }
            catch (...)
            {
            }
        }
        lighting.environment.texture = skyCubemapView;
        lighting.environment.intensity =
            m_sky.iblIntensity;
        lighting.environment.enabled =
            m_graphics.IsSampleableCubeView(
                skyCubemapView)
            && m_sky.iblIntensity > 0.0f;
        if (lighting.environment.enabled)
        {
            // パスまたはビューの世代が変わったら、環境キャッシュの内容ハッシュを取り直します。
            if (m_sky.cubemapPath != m_skyPrefilterKeyPath
                || skyCubemapView != m_skyPrefilterKeySourceView)
            {
                m_skyPrefilterKeyPath = m_sky.cubemapPath;
                m_skyPrefilterKeySourceView = skyCubemapView;
                m_skyPrefilterKey = 0;
                try
                {
                    // 環境キャッシュ鍵用の元データ
                    const auto cubemapBytes =
                        m_graphics.Assets().ReadFileBytes(
                            m_sky.cubemapPath);
                    m_skyPrefilterKey =
                        EnvironmentCache::HashBytes(
                            cubemapBytes);
                }
                catch (...)
                {
                }
            }
            // 事前畳み込みを取得できない場合は元のキューブを直接参照します。
            // 畳み込み済みの環境ビュー
            const auto prefiltered = m_graphics
                .TryGetPrefilteredEnvironmentViews(
                    skyCubemapView,
                    m_skyPrefilterKey);
            if (prefiltered.IsValid())
            {
                lighting.environment.specular =
                    prefiltered.specular;
                lighting.environment.irradiance =
                    prefiltered.irradiance;
                lighting.environment.specularMaximumMip =
                    prefiltered.specularMaximumMip;
            }
        }

        // 間接光はベイク時の格子形状で参照し、ベイク中は自身を含めず一回の反射として計算します。
        EnsureBakedGlobalIlluminationTextures();
        {
            // 今回の描画の間接光情報
            auto& bakedGi = lighting.bakedGlobalIllumination;
            // ベイク済み間接光を参照するか
            const bool bakedGiActive =
                m_bakedGiSettings.enabled
                && !m_bakingReflectionProbes
                && m_bakedGiViews[0]
                && m_bakedGiViews[1]
                && m_bakedGiViews[2];
            bakedGi.enabled = bakedGiActive;
            if (bakedGiActive)
            {
                // ベイク時の格子形状
                const auto& shape = m_bakedGiBakedShape;
                bakedGi.redCoefficients =
                    m_bakedGiViews[0];
                bakedGi.greenCoefficients =
                    m_bakedGiViews[1];
                bakedGi.blueCoefficients =
                    m_bakedGiViews[2];
                bakedGi.volumeMinimum = {
                    shape.center.x - shape.size.x * 0.5f,
                    shape.center.y - shape.size.y * 0.5f,
                    shape.center.z - shape.size.z * 0.5f
                };
                bakedGi.volumeSize = shape.size;
                bakedGi.resolution = {
                    static_cast<float>(shape.resolutionX),
                    static_cast<float>(shape.resolutionY),
                    static_cast<float>(shape.resolutionZ)
                };
                // 強さだけはベイクし直さずに効きます。
                bakedGi.intensity =
                    m_bakedGiSettings.intensity;
            }
        }

        // Forward+でのみクラスタ表を作り、利用できない場合は固定灯数の描画経路を使います。
        // Forward+が選択されているか
        const bool clusteredRequested =
            m_graphics.Settings().renderingPath
                == RenderingPath::ForwardPlus;
        if (clusteredRequested
            && !lighting.clusteredLights.empty()
            && !m_clusteredLightingUnavailable)
        {
            // クラスタのポイント→スポットの並びを固定配列と揃え、スポットの影スロットを対応付けます。
            // クラスタ内のスポット通し添字
            std::ptrdiff_t spotOrdinal = 0;
            // 影スロットを設定する光源
            for (auto& clusteredLight :
                lighting.clusteredLights)
            {
                if (clusteredLight.extraParameters.y
                    < 0.5f)
                {
                    continue;
                }
                // スポット影のスロット添字
                for (std::size_t slot = 0;
                    slot < MaximumSpotShadows;
                    ++slot)
                {
                    // 対応を調べるスポット影情報
                    const auto& spotShadow =
                        lighting.spotShadows[slot];
                    if (spotShadow.enabled
                        && spotShadow.lightIndex
                            == spotOrdinal)
                    {
                        clusteredLight.extraParameters.z =
                            static_cast<float>(slot + 1);
                    }
                }
                ++spotOrdinal;
            }

            try
            {
                // 対象描画のGPU計測区間
                GpuProfiler::SectionScope gpuSectionScope{
                    m_graphics.Gpu(),
                    "ライトカリング"
                };
                m_graphics.UpdateClusteredLights(
                    lighting,
                    view,
                    projection,
                    target != nullptr
                        ? target->Width()
                        : m_graphics.RenderWidth(),
                    target != nullptr
                        ? target->Height()
                        : m_graphics.RenderHeight());
            }
            // exception: クラスタ表作成時の失敗内容
            catch (const std::exception& exception)
            {
                // シェーダーを使用できない環境では16灯までの描画経路へ切り替え、同じ初期化を毎フレーム再試行しません。
                m_clusteredLightingUnavailable = true;
                lighting.clustered = {};
                Logger::Instance().Warning(
                    std::string{
                        "クラスタライトカリングを初期化"
                        "できないため、従来のライト上限で"
                        "描画します: " }
                    + exception.what());
            }
        }

        m_graphics.SetLightingState(lighting);
        {
            // 対象描画のGPU計測区間
            GpuProfiler::SectionScope gpuSectionScope{
                m_graphics.Gpu(),
                "スカイ"
            };
            // スカイへ渡す太陽描画情報
            SkySunDescription skySun{};
            // 方向光から太陽を取得できたか
            const bool hasSkySun =
                m_sky.sunDriven
                && ResolveSkySun(
                    skySun.directionToSun,
                    skySun.color,
                    skySun.angularRadius);
            m_graphics.DrawSky(
                view,
                projection,
                ResolvedSky(),
                skyCubemapView,
                hasSkySun ? &skySun : nullptr);
        }
        // 3D描画のGPU計測区間
        GpuProfiler::SectionScope renderSectionScope{
            m_graphics.Gpu(),
            "3D描画"
        };

        m_visibilityStats.modelInstanceBatchCount = 0;
        m_visibilityStats.modelInstancedRendererCount = 0;
        m_visibilityStats.meshInstanceBatchCount = 0;
        m_visibilityStats.meshInstancedRendererCount = 0;

        // 同一形状・同一マテリアルのMeshRendererをまとめてインスタンス描画します（2個以上のグループのみ）。
        // 同じメッシュと材質の一括描画候補
        std::unordered_map<
            std::uint64_t,
            std::vector<MeshRendererComponent*>>
            instanceBatches;
        // 描画対象の物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy()
                || visibility.renderHidden.contains(
                    gameObject->Id()))
            {
                continue;
            }
            // 一括描画候補のメッシュ
            auto* meshRenderer =
                gameObject->GetComponent<
                    MeshRendererComponent>();
            // アルファ合成は距離順を保つため個別描画し、順序に依存しない加算合成はまとめられます。
            if (meshRenderer != nullptr
                && meshRenderer->IsEnabled()
                && meshRenderer->CanBeInstanced()
                && !meshRenderer->IsAlphaBlended3D())
            {
                instanceBatches[
                    meshRenderer->InstanceBatchKey()]
                    .push_back(meshRenderer);
            }
        }
        // 代表コンポーネントで一描画イベントを登録し、デバッガーが描画を許可するか返します(representative: 代表コンポーネント, count: まとめる物体数)。
        const auto submitInstancedBatch =
            [this](const Component& representative, const std::size_t count)
            {
                // 描画可否を決めるデバッガー
                auto& frameDebugger = m_graphics.FrameDebug();
                if (!frameDebugger.IsEnabled())
                {
                    return true;
                }
                // 代表物体の描画イベント情報
                FrameDebugDrawDescription description;
                try
                {
                    static_cast<void>(
                        representative.DescribeDrawEvent(description));
                }
                catch (...)
                {
                    description = {};
                }
                description.instanceCount =
                    static_cast<std::uint32_t>(count);
                // 描画イベントが属するパス
                const auto pass =
                    m_graphics.DepthPass() == DepthPassKind::Shadow
                        ? FrameDebugPass::ShadowDepth
                        : (m_graphics.DepthPass() == DepthPassKind::Prepass
                            ? FrameDebugPass::DepthPrepass
                            : FrameDebugPass::Color);
                return frameDebugger.SubmitDrawEvent(
                    FrameDebugEventKind::InstancedBatch,
                    pass,
                    representative.Owner().Id(),
                    representative.Owner().Name(),
                    representative.TypeName(),
                    std::move(description));
            };
        // batchKey: 一括描画の識別鍵, batch: 同じ鍵を持つ描画候補
        for (auto& [batchKey, batch] : instanceBatches)
        {
            static_cast<void>(batchKey);
            if (batch.size() < 2
                || !submitInstancedBatch(*batch.front(), batch.size()))
            {
                continue;
            }
            batch.front()->RenderInstancedBatch(
                batch,
                view,
                projection);
            ++m_visibilityStats.meshInstanceBatchCount;
            m_visibilityStats.meshInstancedRendererCount +=
                batch.size();
        }

        // 同じモデルと材質の一括描画候補
        std::unordered_map<
            std::uint64_t,
            std::vector<ModelRendererComponent*>>
            modelInstanceBatches;
        // 描画対象の物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy()
                || visibility.renderHidden.contains(
                    gameObject->Id()))
            {
                continue;
            }
            // 一括描画候補のモデル
            auto* modelRenderer =
                gameObject->GetComponent<
                    ModelRendererComponent>();
            if (modelRenderer != nullptr
                && modelRenderer->IsEnabled()
                && modelRenderer->CanBeInstanced()
                && !modelRenderer->IsAlphaBlended3D())
            {
                modelInstanceBatches[
                    modelRenderer->InstanceBatchKey()]
                    .push_back(modelRenderer);
            }
        }
        // batchKey: 一括描画の識別鍵, batch: 同じ鍵を持つ描画候補
        for (auto& [batchKey, batch] : modelInstanceBatches)
        {
            static_cast<void>(batchKey);
            if (batch.size() >= 2
                && submitInstancedBatch(*batch.front(), batch.size())
                && batch.front()->RenderInstancedBatch(
                    batch,
                    view,
                    projection))
            {
                ++m_visibilityStats.modelInstanceBatchCount;
                m_visibilityStats.modelInstancedRendererCount +=
                    batch.size();
            }
        }

        // アルファ合成は不透明描画後に遠い順で描き、事前パスを持つ物体は連続する組を保ちます。
        struct AlphaBlendedDraw final
        {
            // 透明描画の対象物体
            GameObject* object;
            // カメラまでの距離の二乗
            float distanceSquared;
        };
        // 後で距離順に描く透明物体
        std::vector<AlphaBlendedDraw> alphaBlendedDraws;
        // 今回のビューのワールド位置
        DirectX::XMVECTOR cameraPosition =
            DirectX::XMMatrixInverse(nullptr, view).r[3];

        // 元の描画一覧内の物体添字
        for (std::size_t objectIndex = 0;
            objectIndex < m_gameObjects.size();)
        {
            // 描画対象の物体
            const auto& gameObject = m_gameObjects[objectIndex];
            if (visibility.renderHidden.contains(
                    gameObject->Id()))
            {
                ++objectIndex;
                continue;
            }

            if (!gameObject->HasPreRender3DPass(m_graphics))
            {
                if (gameObject->HasAlphaBlended3DPass(
                        m_graphics))
                {
                    // 距離順には親変換と補間を含むワールド位置を使います。
                    // カメラから物体へのベクトル
                    const auto offset =
                        DirectX::XMVectorSubtract(
                            gameObject->WorldMatrix().r[3],
                            cameraPosition);
                    alphaBlendedDraws.push_back({
                        gameObject.get(),
                        DirectX::XMVectorGetX(
                            DirectX::XMVector3LengthSq(
                                offset))
                    });
                    ++objectIndex;
                    continue;
                }
                gameObject->Render3D(
                    m_graphics,
                    view,
                    projection);
                ++objectIndex;
                continue;
            }

            // 自己遮蔽を避けるため、連続する組の事前パスをすべて描いてから通常パスを描きます。
            // 連続する事前パス対象の組
            std::vector<GameObject*> preRenderGroup;
            // 事前パス組の末尾の次の添字
            std::size_t groupEnd = objectIndex;
            for (; groupEnd < m_gameObjects.size(); ++groupEnd)
            {
                // 連続する事前パス組を調べる物体
                auto* candidate = m_gameObjects[groupEnd].get();
                if (visibility.renderHidden.contains(candidate->Id()))
                {
                    continue;
                }
                if (!candidate->HasPreRender3DPass(m_graphics))
                {
                    break;
                }
                preRenderGroup.push_back(candidate);
            }
            // 事前パス組の描画対象物体
            for (auto* candidate : preRenderGroup)
            {
                candidate->RenderPre3D(
                    m_graphics,
                    view,
                    projection);
            }
            // 事前パス組の描画対象物体
            for (auto* candidate : preRenderGroup)
            {
                candidate->Render3D(
                    m_graphics,
                    view,
                    projection);
            }
            objectIndex = groupEnd;
        }

        // 遠い順に比較し、等距離では登録順を保ちます(left: 左描画候補, right: 右描画候補)。
        std::stable_sort(
            alphaBlendedDraws.begin(),
            alphaBlendedDraws.end(),
            [](const AlphaBlendedDraw& left,
                const AlphaBlendedDraw& right)
            {
                return left.distanceSquared
                    > right.distanceSquared;
            });
        // 距離順に並べた透明描画候補
        for (const auto& draw : alphaBlendedDraws)
        {
            draw.object->Render3D(
                m_graphics,
                view,
                projection);
        }

        if (renderDebug)
        {
            // 描画対象の物体
            for (const auto& gameObject : m_gameObjects)
            {
                gameObject->RenderDebug3D(m_graphics, view, projection);
            }
        }
        renderSectionScope.End();

        // TAAの履歴は描画先ごとに保持し、初回も設定を渡して履歴を作ります。
        if (temporalWanted && target != nullptr)
        {
            // 今回のTAA再投影情報
            auto& frame = m_temporalFrame;
            frame.settings = m_temporalAntiAliasing;
            // 履歴の参照位置が揺れないよう、復元と再投影にはジッターを含まない行列を使います。
            // 逆行列計算の行列式出力
            DirectX::XMVECTOR determinant{};
            DirectX::XMStoreFloat4x4(
                &frame.inputs.inverseViewProjection,
                DirectX::XMMatrixInverse(
                    &determinant,
                    view * rawProjection));
            DirectX::XMStoreFloat4x4(
                &frame.inputs.viewProjection,
                view * rawProjection);
        }

        // モーションブラーはTAAの有効状態によらずジッターなしの行列を使い、ベイク中は無効にします。
        m_motionBlurFrame = {};
        if (target != nullptr && !m_bakingReflectionProbes)
        {
            // 逆行列計算の行列式出力
            DirectX::XMVECTOR determinant{};
            DirectX::XMStoreFloat4x4(
                &m_motionBlurFrame.inverseViewProjection,
                DirectX::XMMatrixInverse(
                    &determinant,
                    view * rawProjection));
            DirectX::XMStoreFloat4x4(
                &m_motionBlurFrame.viewProjection,
                view * rawProjection);
        }

        // SSRの履歴は読み終えた後かつポスト処理前に保存し、効果適用済みの色が反射へ混ざるのを防ぎます。
        if (target != nullptr
            && m_screenSpaceReflection.enabled
            && !m_bakingReflectionProbes)
        {
            // SSR履歴へ渡す今回のビュー射影
            DirectX::XMFLOAT4X4 storedViewProjection{};
            DirectX::XMStoreFloat4x4(
                &storedViewProjection,
                view * projection);
            m_graphics.CaptureOffscreenTargetColorHistory(
                *target,
                storedViewProjection);
        }

        if (!include2D)
        {
            return;
        }

        Render2D();
    }

    ReflectionProbeComponent*
        Scene::NearestReflectionProbe(
            const DirectX::XMFLOAT3& position)
            const noexcept
    {
        // ベイク中は無効（プローブがプローブを映す循環を防ぐ）。
        if (m_bakingReflectionProbes)
        {
            return nullptr;
        }
        // 範囲内で最も近い反射プローブ
        ReflectionProbeComponent* nearest = nullptr;
        // 最近傍プローブまでの距離二乗
        float nearestSquared =
            std::numeric_limits<float>::max();
        // 距離と影響度を調べるプローブ
        for (auto* probe : m_frameReflectionProbes)
        {
            // プローブのワールド変換
            const auto world =
                probe->Owner().WorldMatrix();
            // プローブのワールド中心
            DirectX::XMFLOAT3 center{};
            DirectX::XMStoreFloat3(&center, world.r[3]);
            // 問い合わせ位置と中心のX差
            const float dx = position.x - center.x;
            // 問い合わせ位置と中心のY差
            const float dy = position.y - center.y;
            // 問い合わせ位置と中心のZ差
            const float dz = position.z - center.z;
            // 問い合わせ位置からの距離二乗
            const float distanceSquared =
                dx * dx + dy * dy + dz * dz;
            if (distanceSquared
                    <= probe->Range() * probe->Range()
                && distanceSquared < nearestSquared)
            {
                nearest = probe;
                nearestSquared = distanceSquared;
            }
        }
        return nearest;
    }

    ReflectionProbeEnvironment
        Scene::ReflectionProbeEnvironmentAt(
            const DirectX::XMFLOAT3& position)
            const noexcept
    {
        // 選択した反射環境と混合情報
        ReflectionProbeEnvironment result{};
        // ベイク中は無効（プローブがプローブを映す循環を防ぐ）。
        if (m_bakingReflectionProbes)
        {
            return result;
        }

        // 同じ影響度では距離が近いプローブを優先します。
        // 影響度が最大のプローブ
        ReflectionProbeComponent* primary = nullptr;
        // 影響度が二番目のプローブ
        ReflectionProbeComponent* secondary = nullptr;
        // 主プローブの影響度
        float primaryInfluence = 0.0f;
        // 副プローブの影響度
        float secondaryInfluence = 0.0f;
        // 主プローブまでの距離二乗
        float primaryDistanceSquared =
            std::numeric_limits<float>::max();
        // 副プローブまでの距離二乗
        float secondaryDistanceSquared =
            std::numeric_limits<float>::max();
        // 距離と影響度を調べるプローブ
        for (auto* probe : m_frameReflectionProbes)
        {
            // 問い合わせ位置への影響度
            const float influence =
                probe->InfluenceAt(position);
            if (influence <= 0.0f)
            {
                continue;
            }
            // プローブのワールド中心
            const auto center = probe->WorldPosition();
            // 問い合わせ位置と中心のX差
            const float dx = position.x - center.x;
            // 問い合わせ位置と中心のY差
            const float dy = position.y - center.y;
            // 問い合わせ位置と中心のZ差
            const float dz = position.z - center.z;
            // 問い合わせ位置からの距離二乗
            const float distanceSquared =
                dx * dx + dy * dy + dz * dz;
            // 主候補より優先度が高いか
            const bool beatsPrimary =
                influence > primaryInfluence
                || (influence == primaryInfluence
                    && distanceSquared
                        < primaryDistanceSquared);
            if (beatsPrimary)
            {
                secondary = primary;
                secondaryInfluence = primaryInfluence;
                secondaryDistanceSquared =
                    primaryDistanceSquared;
                primary = probe;
                primaryInfluence = influence;
                primaryDistanceSquared = distanceSquared;
                continue;
            }
            // 副候補より優先度が高いか
            const bool beatsSecondary =
                influence > secondaryInfluence
                || (influence == secondaryInfluence
                    && distanceSquared
                        < secondaryDistanceSquared);
            if (beatsSecondary)
            {
                secondary = probe;
                secondaryInfluence = influence;
                secondaryDistanceSquared =
                    distanceSquared;
            }
        }

        if (primary == nullptr)
        {
            return result;
        }

        // 主プローブのベイク済み環境
        const auto& baked = primary->BakedEnvironment();
        // 有効な環境ビューがない場合は反射を無効にします。
        if (!baked.IsValid())
        {
            return result;
        }
        result.specular = baked.specular;
        result.irradiance = baked.irradiance;
        result.specularMaximumMip =
            baked.specularMaximumMip;
        result.intensity = primary->Intensity();
        result.boxCenter = primary->WorldPosition();
        result.boxExtents = primary->BoxExtents();

        // 両方のブレンド距離が0の場合は混ぜず、どちらかが正なら影響度比で混ぜます。
        // 二つの環境を混ぜる設定か
        const bool blendRequested =
            secondary != nullptr
            && (primary->BlendDistance() > 0.0f
                || secondary->BlendDistance() > 0.0f);
        if (!blendRequested)
        {
            return result;
        }

        // 主と副の影響度の合計
        const float total =
            primaryInfluence + secondaryInfluence;
        if (total <= 0.0f)
        {
            return result;
        }
        // 副プローブのベイク済み環境
        const auto& secondaryBaked =
            secondary->BakedEnvironment();
        if (!secondaryBaked.IsValid())
        {
            return result;
        }
        result.secondarySpecular =
            secondaryBaked.specular;
        result.secondaryIrradiance =
            secondaryBaked.irradiance;
        result.secondarySpecularMaximumMip =
            secondaryBaked.specularMaximumMip;
        result.secondaryBoxCenter =
            secondary->WorldPosition();
        result.secondaryBoxExtents =
            secondary->BoxExtents();
        result.secondaryWeight =
            secondaryInfluence / total;
        // 境界で明るさが飛ばないよう、環境の強度も同じ比率で混ぜます。
        result.intensity =
            primary->Intensity()
                * (1.0f - result.secondaryWeight)
            + secondary->Intensity()
                * result.secondaryWeight;
        return result;
    }

    namespace
    {
        // シーンパスとプローブの位置・範囲・解像度から環境キャッシュ鍵を作ります(scenePath: 主シーンのパス, probe: 対象プローブ, faceSize: キューブ面の解像度)。
        // シーン内容や光源の変更は鍵に含まれないため、変更時は明示的な再ベイクが必要です。
        [[nodiscard]] std::uint64_t ProbeEnvironmentCacheKey(
            const std::filesystem::path& scenePath,
            const ReflectionProbeComponent& probe,
            const std::uint32_t faceSize)
        {
            // キャッシュ鍵に使うバイト列
            std::vector<std::uint8_t> buffer;
            // キャッシュ鍵用のUTF8パス
            const auto pathUtf8 = PathToUtf8(scenePath);
            buffer.insert(
                buffer.end(),
                pathUtf8.begin(),
                pathUtf8.end());
            // 数値のビット列を鍵の素材へ追加します(value: 追加する数値)。
            const auto appendBits =
                [&buffer](const auto value)
            {
                // 数値の生バイト列の先頭
                const auto* begin =
                    reinterpret_cast<const std::uint8_t*>(
                        &value);
                buffer.insert(
                    buffer.end(),
                    begin,
                    begin + sizeof(value));
            };
            appendBits(probe.WorldPosition().x);
            appendBits(probe.WorldPosition().y);
            appendBits(probe.WorldPosition().z);
            appendBits(probe.Range());
            appendBits(faceSize);
            return EnvironmentCache::HashBytes(buffer);
        }
    }

    void Scene::BakePendingReflectionProbes()
    {
        if (m_bakingReflectionProbes
            || !m_graphics.IsInitialized())
        {
            return;
        }


        // ベイク要求のある反射プローブ
        std::vector<ReflectionProbeComponent*> pending;
        // プローブを調べる所有物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }
            // 処理する反射プローブ
            auto* probe = gameObject->GetComponent<
                ReflectionProbeComponent>();
            if (probe == nullptr
                || !probe->IsEnabled())
            {
                continue;
            }
            // 描画資源の世代が変わったプローブは、古いビューを使わず再ベイクします。
            // 現在のベイク済み環境
            const auto& baked = probe->BakedEnvironment();
            if (baked.IsValid()
                && (!m_graphics.IsGraphicsViewCurrent(baked.specular)
                    || !m_graphics.IsGraphicsViewCurrent(
                        baked.irradiance)))
            {
                probe->RequestBake();
            }
            if (!probe->IsBakeRequested())
            {
                continue;
            }
            // シーンから読み込んだプローブのみ初回のキャッシュ復元を試し、復元できなければベイクします。
            if (probe->IsLoadedFromScene()
                && !probe->RestoreAttempted())
            {
                probe->MarkRestoreAttempted();
                // ディスクから復元した環境
                auto restored =
                    m_graphics.TryLoadCachedEnvironmentViews(
                        ProbeEnvironmentCacheKey(
                            m_sceneManager != nullptr
                                ? m_sceneManager
                                    ->CurrentScenePath()
                                : std::filesystem::path{},
                            *probe,
                            EnvironmentProbeBakeFaceSize));
                if (restored.IsValid())
                {
                    probe->SetBakedEnvironment(
                        std::move(restored));
                    continue;
                }
            }
            pending.push_back(probe);
        }
        if (pending.empty())
        {
            return;
        }

        try
        {
            m_graphics.PrepareEnvironmentProbeBake();
        }
        // exception: ベイク資源の準備失敗
        catch (const std::exception& exception)
        {
            // リソースを作成できない場合は要求を解除し、毎フレームの再試行を防ぎます。
            // 処理する反射プローブ
            for (auto* probe : pending)
            {
                probe->SetBakedEnvironment({});
            }
            Logger::Instance().Warning(
                std::string{
                    "リフレクションプローブを初期化"
                    "できませんでした: " }
                + exception.what());
            return;
        }


        // 描画先の復元を保証する範囲
        GraphicsOutputStateScope outputStateScope{ m_graphics };

        // 面の並びはD3Dキューブのサンプリング方向と揃えます。
        // キューブ面ごとの視線方向
        static constexpr DirectX::XMFLOAT3
            FaceDirections[6]{
                { 1.0f, 0.0f, 0.0f },
                { -1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f },
                { 0.0f, -1.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f },
                { 0.0f, 0.0f, -1.0f }
            };
        // キューブ面ごとの上方向
        static constexpr DirectX::XMFLOAT3 FaceUps[6]{
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f }
        };

        // プローブベイクのGPU計測区間
        GpuProfiler::SectionScope gpuSectionScope{
            m_graphics.Gpu(),
            "プローブベイク"
        };
        // 反射再帰を防ぐフラグの範囲
        BooleanStateScope bakingScope{
            m_bakingReflectionProbes,
            true
        };
        try
        {
            // 処理する反射プローブ
            for (auto* probe : pending)
            {
                // 対象プローブのワールド変換
                const auto world =
                    probe->Owner().WorldMatrix();
                // キューブを描くワールド位置
                DirectX::XMFLOAT3 eye{};
                DirectX::XMStoreFloat3(&eye, world.r[3]);
                // 右手系で描いて巻き方向を保ち、D3Dキューブの向きとの差は描画後の左右反転コピーで補正します。
                // キューブ各面の射影行列
                const auto faceProjection =
                    DirectX::XMMatrixPerspectiveFovRH(
                        DirectX::XM_PIDIV2,
                        1.0f,
                        0.1f,
                        std::max(
                            probe->Range() * 4.0f,
                            100.0f));

                // シーン由来のプローブのみベイク結果をディスクキャッシュへ残します。
                // シーン由来のプローブの保存鍵
                const auto cacheKey = probe->IsLoadedFromScene()
                    ? std::optional<std::uint64_t>{
                        ProbeEnvironmentCacheKey(
                            m_sceneManager != nullptr
                                ? m_sceneManager
                                    ->CurrentScenePath()
                                : std::filesystem::path{},
                            *probe,
                            EnvironmentProbeBakeFaceSize) }
                    : std::nullopt;
                // キューブの各面を同期描画した反射環境(face: D3Dキューブの面番号)。
                auto baked = m_graphics
                    .BakeReflectionProbeViews(
                        [this,
                         &eye,
                         &faceProjection](const std::uint32_t face)
                        {
                            // キューブ対象面のビュー行列
                            const auto faceView =
                                DirectX::XMMatrixLookToRH(
                                    DirectX::XMLoadFloat3(&eye),
                                    DirectX::XMLoadFloat3(
                                        &FaceDirections[face]),
                                    DirectX::XMLoadFloat3(
                                        &FaceUps[face]));
                            // ポスト処理なしのHDRリニアで焼きます（IBLはトーンマップ前の値が正）。
                            RenderWithMatrices(
                                faceView,
                                faceProjection,
                                false,
                                false);
                        },
                        cacheKey);
                probe->SetBakedEnvironment(
                    std::move(baked));
            }
        }
        catch (...)
        {
            // 描画先・計測区間・再入フラグはscope guardが戻します。
            throw;
        }
        bakingScope.Restore();
        gpuSectionScope.End();
        outputStateScope.Restore();
    }


    void Scene::SetBakedGlobalIlluminationSettings(
        const BakedGlobalIlluminationSettings& settings)
        noexcept
    {
        m_bakedGiSettings = settings;
        m_bakedGiSettings.size.x =
            std::max(m_bakedGiSettings.size.x, 0.1f);
        m_bakedGiSettings.size.y =
            std::max(m_bakedGiSettings.size.y, 0.1f);
        m_bakedGiSettings.size.z =
            std::max(m_bakedGiSettings.size.z, 0.1f);
        // 格子は各軸64点、合計32768点までに制限します。
        m_bakedGiSettings.resolutionX = std::clamp(
            m_bakedGiSettings.resolutionX,
            1u,
            BakedGlobalIlluminationMaximumAxisResolution);
        m_bakedGiSettings.resolutionY = std::clamp(
            m_bakedGiSettings.resolutionY,
            1u,
            BakedGlobalIlluminationMaximumAxisResolution);
        m_bakedGiSettings.resolutionZ = std::clamp(
            m_bakedGiSettings.resolutionZ,
            1u,
            BakedGlobalIlluminationMaximumAxisResolution);
        while (static_cast<std::uint64_t>(
                m_bakedGiSettings.resolutionX)
            * m_bakedGiSettings.resolutionY
            * m_bakedGiSettings.resolutionZ
            > BakedGlobalIlluminationMaximumProbeCount)
        {

            // 点数を半減する最長の格子軸
            auto* largest = &m_bakedGiSettings.resolutionX;
            if (m_bakedGiSettings.resolutionY > *largest)
            {
                largest = &m_bakedGiSettings.resolutionY;
            }
            if (m_bakedGiSettings.resolutionZ > *largest)
            {
                largest = &m_bakedGiSettings.resolutionZ;
            }
            *largest = std::max(1u, *largest / 2u);
        }
        m_bakedGiSettings.intensity = std::clamp(
            m_bakedGiSettings.intensity, 0.0f, 8.0f);
    }

    void Scene::RequestBakedGlobalIlluminationBake() noexcept
    {
        // 進行中の設定変更が格子を崩さないよう、要求時の形状を固定します。
        m_bakedGiBakedShape = m_bakedGiSettings;
        // 要求時の格子の総プローブ数
        const std::size_t total =
            static_cast<std::size_t>(
                m_bakedGiBakedShape.resolutionX)
            * m_bakedGiBakedShape.resolutionY
            * m_bakedGiBakedShape.resolutionZ;
        if (total == 0)
        {
            return;
        }
        try
        {
            m_bakedGiWorking.assign(
                total
                    * BakedGlobalIlluminationCoefficientsPerProbe,
                0.0f);
        }
        catch (...)
        {
            return;
        }
        m_bakedGiNextProbe = 0;
        m_bakedGiBaking = true;
    }

    void Scene::RestoreBakedGlobalIllumination(
        const BakedGlobalIlluminationSettings& shape,
        std::vector<std::uint16_t> payload) noexcept
    {
        // 格子の点数に係数データの長さが一致しない場合は復元しません。
        // 復元形状の検証済み点数
        const auto probeCount =
            BakedGlobalIlluminationProbeCount(
                shape.resolutionX,
                shape.resolutionY,
                shape.resolutionZ);
        if (!probeCount.has_value()
            || payload.size()
                != *probeCount
                    * BakedGlobalIlluminationCoefficientsPerProbe)
        {
            return;
        }
        m_bakedGiBakedShape = shape;
        m_bakedGiData = std::move(payload);
        m_bakedGiTexturesDirty = true;
        m_bakedGiBaking = false;
        m_bakedGiNextProbe = 0;
        m_bakedGiWorking.clear();
    }

    float Scene::BakedGlobalIlluminationBakeProgress()
        const noexcept
    {
        if (!m_bakedGiBaking)
        {
            return -1.0f;
        }
        // 要求時の格子の総プローブ数
        const std::size_t total =
            static_cast<std::size_t>(
                m_bakedGiBakedShape.resolutionX)
            * m_bakedGiBakedShape.resolutionY
            * m_bakedGiBakedShape.resolutionZ;
        if (total == 0)
        {
            return -1.0f;
        }
        return static_cast<float>(m_bakedGiNextProbe)
            / static_cast<float>(total);
    }

    void Scene::ProcessBakedGlobalIlluminationBake()
    {
        if (!m_bakedGiBaking
            || m_bakingReflectionProbes
            || !m_graphics.IsInitialized())
        {
            return;
        }
        // 要求時に固定した格子形状
        const auto& shape = m_bakedGiBakedShape;
        // 要求時の格子の総プローブ数
        const std::size_t total =
            static_cast<std::size_t>(shape.resolutionX)
            * shape.resolutionY
            * shape.resolutionZ;
        if (total == 0
            || m_bakedGiWorking.size() != total * 12)
        {
            m_bakedGiBaking = false;
            return;
        }

        try
        {
            m_graphics.PrepareEnvironmentProbeBake();
        }
        // exception: 間接光ベイクの失敗内容
        catch (const std::exception& exception)
        {
            m_bakedGiBaking = false;
            Logger::Instance().Warning(
                std::string{
                    "GIベイクを初期化できませんでした: " }
                + exception.what());
            return;
        }


        // 描画先を復元する範囲
        GraphicsOutputStateScope outputStateScope{ m_graphics };

        // 面の並びをD3Dキューブと反射プローブのベイクに揃えます。
        // キューブ面ごとの視線方向
        static constexpr DirectX::XMFLOAT3 FaceDirections[6]{
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f }
        };
        // キューブ面ごとの上方向
        static constexpr DirectX::XMFLOAT3 FaceUps[6]{
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f }
        };


        // 一回の処理でベイクする点数上限
        constexpr std::size_t ProbesPerFrame = 8;

        // 間接光ベイクのGPU計測区間
        GpuProfiler::SectionScope gpuSectionScope{
            m_graphics.Gpu(),
            "GIベイク"
        };
        // 自身の間接光と反射プローブを参照しない描画から、一回の反射として間接光をベイクします。
        // 反射再帰を防ぐフラグの範囲
        BooleanStateScope bakingScope{
            m_bakingReflectionProbes,
            true
        };
        try
        {
            // 格子全域を覆う遠平面距離
            const float farPlane = std::max(
                std::sqrt(
                    shape.size.x * shape.size.x
                    + shape.size.y * shape.size.y
                    + shape.size.z * shape.size.z)
                    * 2.0f,
                100.0f);
            // キューブ各面の射影行列
            const auto faceProjection =
                DirectX::XMMatrixPerspectiveFovRH(
                    DirectX::XM_PIDIV2,
                    1.0f,
                    0.1f,
                    farPlane);

            // 今回処理する末尾の次の添字
            const std::size_t endProbe = std::min(
                m_bakedGiNextProbe + ProbesPerFrame,
                total);
            for (; m_bakedGiNextProbe < endProbe;
                ++m_bakedGiNextProbe)
            {
                // ベイク中のプローブの通し添字
                const std::size_t index =
                    m_bakedGiNextProbe;
                // プローブの格子X座標
                const std::uint32_t gridX =
                    static_cast<std::uint32_t>(
                        index % shape.resolutionX);
                // プローブの格子Y座標
                const std::uint32_t gridY =
                    static_cast<std::uint32_t>(
                        (index / shape.resolutionX)
                        % shape.resolutionY);
                // プローブの格子Z座標
                const std::uint32_t gridZ =
                    static_cast<std::uint32_t>(
                        index
                        / (static_cast<std::size_t>(
                            shape.resolutionX)
                            * shape.resolutionY));
                // 格子端間を等分する比率を返し、一点だけの軸は中央に置きます(position: 軸上の格子添字, resolution: 軸の点数)。
                const auto axisFraction =
                    [](const std::uint32_t position,
                       const std::uint32_t resolution)
                {

                    return resolution <= 1
                        ? 0.5f
                        : static_cast<float>(position)
                            / static_cast<float>(
                                resolution - 1);
                };
                // プローブのワールド位置
                DirectX::XMFLOAT3 eye{
                    shape.center.x - shape.size.x * 0.5f
                        + shape.size.x
                            * axisFraction(
                                gridX,
                                shape.resolutionX),
                    shape.center.y - shape.size.y * 0.5f
                        + shape.size.y
                            * axisFraction(
                                gridY,
                                shape.resolutionY),
                    shape.center.z - shape.size.z * 0.5f
                        + shape.size.z
                            * axisFraction(
                                gridZ,
                                shape.resolutionZ)
                };

                // キューブの各面を同期描画して得る照度係数(face: D3Dキューブの面番号)。
                const auto coefficients =
                    m_graphics.BakeIrradianceProbe(
                        [this,
                         &eye,
                         &faceProjection](
                            const std::uint32_t face)
                        {
                            // 対象キューブ面のビュー行列
                            const auto faceView =
                                DirectX::XMMatrixLookToRH(
                                    DirectX::XMLoadFloat3(
                                        &eye),
                                    DirectX::XMLoadFloat3(
                                        &FaceDirections[face]),
                                    DirectX::XMLoadFloat3(
                                        &FaceUps[face]));
                            // プローブベイクと同じくポスト処理なしのHDRリニアで焼きます。
                            RenderWithMatrices(
                                faceView,
                                faceProjection,
                                false,
                                false);
                        });
                if (!coefficients)
                {
                    throw std::runtime_error(
                        "GI probe readback failed.");
                }
                std::copy(
                    coefficients->begin(),
                    coefficients->end(),
                    m_bakedGiWorking.data() + index * 12);
            }
        }
        // exception: 間接光ベイクの失敗内容
        catch (const std::exception& exception)
        {
            m_bakedGiBaking = false;
            Logger::Instance().Warning(
                std::string{ "GIベイクに失敗しました: " }
                + exception.what());
            return;
        }
        catch (...)
        {
            m_bakedGiBaking = false;
            Logger::Instance().Warning(
                "GIベイクに失敗しました: 不明な描画エラー");
            return;
        }
        bakingScope.Restore();
        gpuSectionScope.End();
        outputStateScope.Restore();

        if (m_bakedGiNextProbe < total)
        {
            return;
        }

        // 係数を[R,G,B]×[z,y,x]×(x,y,z,定数項)のfp16へ並べ直し、Texture3Dの再作成を予約します。
        m_bakedGiBaking = false;
        m_bakedGiData.assign(total * 12, 0);
        // fp16へ変換するプローブ添字
        for (std::size_t probe = 0; probe < total; ++probe)
        {
            // 係数のRGBチャンネル添字
            for (int channel = 0; channel < 3; ++channel)
            {
                // 係数の方向項と定数項の添字
                for (int component = 0;
                    component < 4;
                    ++component)
                {
                    m_bakedGiData[
                        static_cast<std::size_t>(channel)
                            * total * 4
                        + probe * 4
                        + component] =
                        DirectX::PackedVector::
                            XMConvertFloatToHalf(
                                m_bakedGiWorking[
                                    probe * 12
                                    + channel * 4
                                    + component]);
                }
            }
        }
        m_bakedGiWorking.clear();
        m_bakedGiWorking.shrink_to_fit();
        m_bakedGiTexturesDirty = true;
        Logger::Instance().Info(
            "GIベイクが完了しました（"
            + std::to_string(total)
            + "点）");
    }

    void Scene::EnsureBakedGlobalIlluminationTextures()
    {
        if (!m_bakedGiTexturesDirty)
        {
            return;
        }
        m_bakedGiTexturesDirty = false;
        m_bakedGiViews = {};
        // 要求時に固定した格子形状
        const auto& shape = m_bakedGiBakedShape;
        m_bakedGiViews =
            m_graphics.UploadBakedGlobalIlluminationViews(
                shape.resolutionX,
                shape.resolutionY,
                shape.resolutionZ,
                m_bakedGiData);
    }

    LightingState Scene::BuildLightingState() const noexcept
    {
        // 各描画経路へ渡す照明情報
        LightingState lighting;
        lighting.ambientColor = m_ambientLightColor;
        lighting.ambientIntensity = m_ambientLightIntensity;
        // 太陽連動モードでは環境光の色と強さも太陽高度に追従させます。
        if (m_sky.sunDriven)
        {
            // 解決した太陽へのワールド方向
            DirectX::XMFLOAT3 directionToSun{};
            // 解決した太陽の色
            DirectX::XMFLOAT3 sunColor{};
            // 解決した太陽の角半径ラジアン
            float sunAngularRadius{};
            if (ResolveSkySun(
                    directionToSun,
                    sunColor,
                    sunAngularRadius))
            {
                // 太陽高度に応じた空と環境光
                const auto evaluated =
                    EvaluateSunDrivenSky(directionToSun);
                lighting.ambientColor =
                    evaluated.ambientColor;
                lighting.ambientIntensity =
                    evaluated.ambientIntensity;
            }
        }
        lighting.directionalShadowResolution =
            static_cast<float>(
                std::max(
                    m_graphics.Settings()
                        .shadowResolution,
                    1u));
        lighting.localShadowResolution =
            static_cast<float>(
                std::max(
                    m_graphics.Settings()
                            .shadowResolution
                        / 2u,
                    256u));
        lighting.fog = m_fog;
        lighting.fog.enabled =
            lighting.fog.enabled
            && m_graphics.Settings().fogEnabled;

        // 有効な光源を調べる物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            // 照明情報へ登録する光源
            const auto* light =
                gameObject->GetComponent<DirectionalLightComponent>();
            if (light == nullptr || !light->IsEnabled())
            {
                continue;
            }

            // 固定配列の光源情報の書込先
            auto& destination =
                lighting.directionalLights[
                    lighting.directionalLightCount++];
            destination.direction = light->WorldDirection();
            destination.color = light->Color();
            destination.intensity = light->Intensity();
            // 太陽の角直径を度から半径のラジアンへ変換してシェーダーへ渡します。
            destination.angularRadius =
                DirectX::XMConvertToRadians(
                    light->AngularDiameterDegrees() * 0.5f);

            if (lighting.directionalLightCount
                == MaximumDirectionalLights)
            {
                break;
            }
        }

        // 局所光源を品質上限の固定配列とForward+用の全体配列へ、ポイント→スポットの物体順で集めます。
        // 影の番号対応はこの順序に依存するため、変更時はRenderWithMatricesの影割り当ても更新します。
        // 固定配列のポイント光源上限
        const std::size_t pointLimit =
            std::min<std::size_t>(
                m_graphics.Settings().pointLightLimit,
                MaximumPointLights);
        // 有効な光源を調べる物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (lighting.clusteredLights.size()
                >= MaximumClusteredLights)
            {
                break;
            }
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            // 照明情報へ登録する光源
            const auto* light =
                gameObject->GetComponent<PointLightComponent>();
            if (light == nullptr || !light->IsEnabled())
            {
                continue;
            }

            // 光源のワールド位置
            const auto position = light->WorldPosition();
            // 光源の色
            const auto color = light->Color();
            if (lighting.pointLightCount < pointLimit)
            {
                // 固定配列の光源情報の書込先
                auto& destination =
                    lighting.pointLights[
                        lighting.pointLightCount];
                destination.position = position;
                destination.color = color;
                destination.intensity =
                    light->Intensity();
                destination.range = light->Range();
                ++lighting.pointLightCount;
            }

            // 全体配列へ登録するGPU光源
            GpuLight clustered{};
            clustered.positionRange = {
                position.x,
                position.y,
                position.z,
                light->Range()
            };
            clustered.colorIntensity = {
                color.x,
                color.y,
                color.z,
                light->Intensity()
            };
            // ポイント光源の通し番号+1を保存し、シェーダーで固定配列の影対象と照合します。
            clustered.extraParameters = {
                0.0f,
                0.0f,
                static_cast<float>(
                    lighting.clusteredLights.size() + 1),
                0.0f
            };
            lighting.clusteredLights.push_back(clustered);
        }
        // ポイント光源を登録し終えた時点のクラスタ内の光源数
        const std::size_t clusteredPointCount =
            lighting.clusteredLights.size();
        static_cast<void>(clusteredPointCount);

        // 固定配列のスポット光源上限
        const std::size_t spotLimit =
            std::min<std::size_t>(
                m_graphics.Settings().spotLightLimit,
                MaximumSpotLights);
        // 有効な光源を調べる物体
        for (const auto& gameObject : m_gameObjects)
        {
            if (lighting.clusteredLights.size()
                >= MaximumClusteredLights)
            {
                break;
            }
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            // 照明情報へ登録する光源
            const auto* light =
                gameObject->GetComponent<SpotLightComponent>();
            if (light == nullptr || !light->IsEnabled())
            {
                continue;
            }

            // 光源のワールド位置
            const auto position = light->WorldPosition();
            // スポット光源のワールド方向
            const auto direction = light->WorldDirection();
            // 光源の色
            const auto color = light->Color();
            // スポット内側円錐角の余弦
            const float innerCosine =
                std::cos(light->InnerConeAngle());
            // スポット外側円錐角の余弦
            const float outerCosine =
                std::cos(light->OuterConeAngle());
            if (lighting.spotLightCount < spotLimit)
            {
                // 固定配列の光源情報の書込先
                auto& destination =
                    lighting.spotLights[
                        lighting.spotLightCount++];
                destination.position = position;
                destination.direction = direction;
                destination.color = color;
                destination.intensity =
                    light->Intensity();
                destination.range = light->Range();
                destination.innerConeCosine = innerCosine;
                destination.outerConeCosine = outerCosine;
            }

            // 全体配列へ登録するGPU光源
            GpuLight clustered{};
            clustered.positionRange = {
                position.x,
                position.y,
                position.z,
                light->Range()
            };
            clustered.colorIntensity = {
                color.x,
                color.y,
                color.z,
                light->Intensity()
            };
            clustered.directionInnerCosine = {
                direction.x,
                direction.y,
                direction.z,
                innerCosine
            };
            // 影スロットはRenderWithMatricesの影割り当て後に書き込まれます（ここでは影なし）。
            clustered.extraParameters = {
                outerCosine,
                1.0f,
                0.0f,
                0.0f
            };
            lighting.clusteredLights.push_back(clustered);
        }

        return lighting;
    }
}
