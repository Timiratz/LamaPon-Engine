#include "LamaPon/Physics/Collision3D.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace
{
    using DirectX::XMFLOAT3;

    // 二つのベクトルを加算します(a: 第1ベクトル, b: 第2ベクトル)。
    XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b) noexcept
    {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }
    // 第1ベクトルから第2を引きます(a: 第1ベクトル, b: 第2ベクトル)。
    XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b) noexcept
    {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }
    // ベクトルを倍率倍します(value: ベクトル, scale: 倍率)。
    XMFLOAT3 Mul(const XMFLOAT3& value, const float scale) noexcept
    {
        return { value.x * scale, value.y * scale, value.z * scale };
    }
    // 二つのベクトルの内積を求めます(a: 第1ベクトル, b: 第2ベクトル)。
    float Dot(const XMFLOAT3& a, const XMFLOAT3& b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    // 二つのベクトルの外積を求めます(a: 第1ベクトル, b: 第2ベクトル)。
    XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b) noexcept
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }
    // ベクトルの長さの二乗を求めます(value: ベクトル)。
    float LengthSquared(const XMFLOAT3& value) noexcept
    {
        return Dot(value, value);
    }
    // 二重の外積を求めます(a: 第1ベクトル, b: 第2ベクトル, c: 第3ベクトル)。
    XMFLOAT3 TripleCross(
        const XMFLOAT3& a,
        const XMFLOAT3& b,
        const XMFLOAT3& c) noexcept
    {
        return Cross(Cross(a, b), c);
    }
    // ベクトルを正規化します(value: ベクトル, fallback: 微小長時の代替方向)。
    XMFLOAT3 Normalize(
        const XMFLOAT3& value,
        const XMFLOAT3& fallback = { 0.0f, 1.0f, 0.0f }) noexcept
    {
        // 正規化前の長さの二乗
        const float lengthSquared = LengthSquared(value);
        return lengthSquared <= 0.0000001f
            ? fallback
            : Mul(value, 1.0f / std::sqrt(lengthSquared));
    }
    // ベクトルの軸成分を取得します(value: ベクトル, index: 0〜2の軸番号)。
    float At(const XMFLOAT3& value, const std::size_t index) noexcept
    {
        return (&value.x)[index];
    }
    // カプセル中心線上の点を求めます(capsule: カプセル, amount: 始点から終点への補間量)。
    XMFLOAT3 PointOn(
        const LamaPon::Capsule3D& capsule,
        const float amount) noexcept
    {
        return Add(
            capsule.start,
            Mul(Sub(capsule.end, capsule.start), amount));
    }
    // 入力点を箱の局所座標へ写します(point: 同じ座標系の点, box: 回転箱)。
    XMFLOAT3 ToLocal(
        const XMFLOAT3& point,
        const LamaPon::OrientedBox3D& box) noexcept
    {
        // 箱の中心から入力点への差
        const auto delta = Sub(point, box.center);
        return {
            Dot(delta, box.axes[0]),
            Dot(delta, box.axes[1]),
            Dot(delta, box.axes[2])
        };
    }
    // 箱の局所座標を入力形状の座標系へ戻します(point: 局所座標, box: 回転箱)。
    XMFLOAT3 ToWorld(
        const XMFLOAT3& point,
        const LamaPon::OrientedBox3D& box) noexcept
    {
        // 箱の座標系へ戻した位置
        auto result = box.center;
        // 変換する箱の局所軸番号
        for (std::size_t index{}; index < 3; ++index)
        {
            result = Add(
                result,
                Mul(box.axes[index], At(point, index)));
        }
        return result;
    }
    // 箱の局所境界内に点を制限します(point: 局所座標, half: 軸別の半径)。
    XMFLOAT3 ClampToBox(
        const XMFLOAT3& point,
        const XMFLOAT3& half) noexcept
    {
        return {
            std::clamp(point.x, -half.x, half.x),
            std::clamp(point.y, -half.y, half.y),
            std::clamp(point.z, -half.z, half.z)
        };
    }
    // 方向に最も張り出す箱の点を求めます(box: 回転箱, direction: 探索方向)。
    XMFLOAT3 Support(
        const LamaPon::OrientedBox3D& box,
        const XMFLOAT3& direction) noexcept
    {
        // 方向へ張り出す箱の支持点
        auto result = box.center;
        // 張り出しを求める局所軸番号
        for (std::size_t index{}; index < 3; ++index)
        {
            // 探索方向と局所軸の内積
            const float projection =
                Dot(box.axes[index], direction);
            // 局所軸の張り出す向き
            const float sign =
                projection > 0.00001f
                ? 1.0f
                : (projection < -0.00001f
                    ? -1.0f
                    : 0.0f);
            result = Add(
                result,
                Mul(
                    box.axes[index],
                    At(box.halfExtents, index)
                        * sign));
        }
        return result;
    }
    // 許容誤差内で箱が点を含むか調べます(box: 回転箱, point: 同じ座標系の点, tolerance: 許容距離)。
    bool Contains(
        const LamaPon::OrientedBox3D& box,
        const XMFLOAT3& point,
        const float tolerance = 0.0001f) noexcept
    {
        // 箱の局所座標へ写した点
        const auto local = ToLocal(point, box);
        return std::abs(local.x)
                <= box.halfExtents.x + tolerance
            && std::abs(local.y)
                <= box.halfExtents.y + tolerance
            && std::abs(local.z)
                <= box.halfExtents.z + tolerance;
    }
    // 中心線上の点と箱の距離の二乗を求めます(capsule: カプセル, box: 回転箱, amount: 中心線の補間量, segmentPoint: 任意の中心線点の出力, boxPoint: 任意の箱側最近点の出力)。
    float DistanceToBoxSquared(
        const LamaPon::Capsule3D& capsule,
        const LamaPon::OrientedBox3D& box,
        const float amount,
        XMFLOAT3* segmentPoint = nullptr,
        XMFLOAT3* boxPoint = nullptr) noexcept
    {
        // 中心線上の対象点
        const auto segment = PointOn(capsule, amount);
        // 箱側の最近点
        const auto closest = ToWorld(
            ClampToBox(ToLocal(segment, box), box.halfExtents),
            box);
        if (segmentPoint != nullptr) *segmentPoint = segment;
        if (boxPoint != nullptr) *boxPoint = closest;
        return LengthSquared(Sub(segment, closest));
    }
    // 二つのカプセル中心線の最近点を求めます(left: 第1カプセル, right: 第2カプセル, leftPoint: 第1線上の出力, rightPoint: 第2線上の出力)。
    void ClosestSegments(
        const LamaPon::Capsule3D& left,
        const LamaPon::Capsule3D& right,
        XMFLOAT3& leftPoint,
        XMFLOAT3& rightPoint) noexcept
    {
        // 第1中心線の始点から終点への差
        const auto d1 = Sub(left.end, left.start);
        // 第2中心線の始点から終点への差
        const auto d2 = Sub(right.end, right.start);
        // 第2始点から第1始点への差
        const auto r = Sub(left.start, right.start);
        // 第1中心線の長さの二乗
        const float a = Dot(d1, d1);
        // 第2中心線の長さの二乗
        const float e = Dot(d2, d2);
        // 第2中心線と始点差の内積
        const float f = Dot(d2, r);
        // 退化と平行を判定する閾値
        constexpr float epsilon = 0.000001f;
        // 第1中心線上の0〜1の位置
        float s{};
        // 第2中心線上の0〜1の位置
        float t{};
        if (a <= epsilon && e <= epsilon)
        {
            leftPoint = left.start;
            rightPoint = right.start;
            return;
        }
        if (a <= epsilon)
        {
            t = std::clamp(f / e, 0.0f, 1.0f);
        }
        else
        {
            // 第1中心線と始点差の内積
            const float c = Dot(d1, r);
            if (e <= epsilon)
            {
                s = std::clamp(-c / a, 0.0f, 1.0f);
            }
            else
            {
                // 二つの中心線の方向の内積
                const float b = Dot(d1, d2);
                // 最近点の連立式の分母
                const float denominator = a * e - b * b;
                if (std::abs(denominator) > epsilon)
                {
                    s = std::clamp(
                        (b * f - c * e) / denominator,
                        0.0f,
                        1.0f);
                }
                t = (b * s + f) / e;
                if (t < 0.0f)
                {
                    t = 0.0f;
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                }
                else if (t > 1.0f)
                {
                    t = 1.0f;
                    s = std::clamp((b - c) / a, 0.0f, 1.0f);
                }
            }
        }
        leftPoint = PointOn(left, s);
        rightPoint = PointOn(right, t);
    }

    // 方向に最も張り出すカプセルの点を求めます(capsule: カプセル, direction: 探索方向)。
    XMFLOAT3 Support(
        const LamaPon::Capsule3D& capsule,
        const XMFLOAT3& direction) noexcept
    {
        // 探索方向へ張り出す中心線端点
        const XMFLOAT3 base =
            Dot(capsule.start, direction)
                    > Dot(capsule.end, direction)
                ? capsule.start
                : capsule.end;
        return Add(
            base,
            Mul(
                Normalize(direction),
                std::max(capsule.radius, 0.0f)));
    }

    // 方向に最も張り出す球の点を求めます(sphere: 球, direction: 探索方向)。
    XMFLOAT3 Support(
        const LamaPon::Sphere3D& sphere,
        const XMFLOAT3& direction) noexcept
    {
        return Add(
            sphere.center,
            Mul(
                Normalize(direction),
                std::max(sphere.radius, 0.0f)));
    }

    // 方向に最も張り出す凸包頂点を求めます(hull: 空でない凸包, direction: 探索方向)。
    XMFLOAT3 Support(
        const LamaPon::ConvexHull3D& hull,
        const XMFLOAT3& direction) noexcept
    {
        // 最大投影値を持つ凸包頂点
        XMFLOAT3 best = hull.points.front();
        // 現在の最大投影値
        float bestDot = Dot(best, direction);
        // 投影値を比較する凸包頂点
        for (const auto& point : hull.points)
        {
            // 探索方向上の頂点の投影値
            const float projection = Dot(point, direction);
            if (projection > bestDot)
            {
                bestDot = projection;
                best = point;
            }
        }
        return best;
    }

    // 凸包頂点の平均位置を求めます(hull: 凸包)。
    XMFLOAT3 Centroid(
        const LamaPon::ConvexHull3D& hull) noexcept
    {
        if (hull.points.empty()) return {};
        // 凸包の全頂点位置の合計
        XMFLOAT3 sum{};
        // 合計する凸包頂点
        for (const auto& point : hull.points)
        {
            sum = Add(sum, point);
        }
        return Mul(
            sum,
            1.0f / static_cast<float>(hull.points.size()));
    }

    // 入力に垂直な単位方向を作ります(value: 基準ベクトル)。
    XMFLOAT3 Perpendicular(const XMFLOAT3& value) noexcept
    {
        // 垂直方向の外積を作る基準軸
        const XMFLOAT3 candidate =
            std::abs(value.x) < 0.9f
                ? XMFLOAT3{ 1.0f, 0.0f, 0.0f }
                : XMFLOAT3{ 0.0f, 1.0f, 0.0f };
        return Normalize(Cross(value, candidate));
    }

    // 二つの方向の内積が正か調べます(a: 第1方向, b: 第2方向)。
    bool SameDirection(
        const XMFLOAT3& a,
        const XMFLOAT3& b) noexcept
    {
        return Dot(a, b) > 0.0f;
    }

    // 形状A-Bの支持点と元の形状上の対応点を保持し、EPAで接触点を復元します。
    struct MinkowskiPoint final
    {
        // 形状A-Bの差の支持点
        XMFLOAT3 point{};
        // 差を作った第1形状上の点
        XMFLOAT3 onA{};
        // 差を作った第2形状上の点
        XMFLOAT3 onB{};
    };

    struct Simplex3D final
    {
        // GJKの最大4点の支持点配列
        std::array<MinkowskiPoint, 4> points{};
        // 単体の有効な支持点数
        std::size_t count{};
    };

    // 単体を1点へ縮小します(simplex: 更新する単体, a: 残す支持点)。
    void SetSimplex(
        Simplex3D& simplex,
        const MinkowskiPoint& a) noexcept
    {
        simplex.points[0] = a;
        simplex.count = 1;
    }
    // 単体を線分へ縮小します(simplex: 更新する単体, a: 第1支持点, b: 第2支持点)。
    void SetSimplex(
        Simplex3D& simplex,
        const MinkowskiPoint& a,
        const MinkowskiPoint& b) noexcept
    {
        simplex.points[0] = a;
        simplex.points[1] = b;
        simplex.count = 2;
    }
    // 単体を三角形へ縮小します(simplex: 更新する単体, a: 第1支持点, b: 第2支持点, c: 第3支持点)。
    void SetSimplex(
        Simplex3D& simplex,
        const MinkowskiPoint& a,
        const MinkowskiPoint& b,
        const MinkowskiPoint& c) noexcept
    {
        simplex.points[0] = a;
        simplex.points[1] = b;
        simplex.points[2] = c;
        simplex.count = 3;
    }

    // 単体を更新して原点包含を判定します(simplex: 更新する単体, direction: 次の方向の出力)。
    bool NextSimplex(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept;

    // 線分の単体から次の探索方向を求めます(simplex: 更新する単体, direction: 次の方向の出力)。
    bool LineCase(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept
    {
        // 単体の第1支持点
        const auto a = simplex.points[0];
        // 単体の第2支持点
        const auto b = simplex.points[1];
        // 第1から第2支持点への差
        const auto ab = Sub(b.point, a.point);
        // 第1支持点から原点への差
        const auto ao = Mul(a.point, -1.0f);
        if (SameDirection(ab, ao))
        {
            direction = TripleCross(ab, ao, ab);
            if (LengthSquared(direction) <= 0.0000001f)
            {
                direction = Perpendicular(ab);
            }
        }
        else
        {
            SetSimplex(simplex, a);
            direction = ao;
        }
        return false;
    }

    // 三角形の単体を更新して探索方向を求めます(simplex: 更新する単体, direction: 次の方向の出力)。
    bool TriangleCase(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept
    {
        // 単体の第1支持点
        const auto a = simplex.points[0];
        // 単体の第2支持点
        const auto b = simplex.points[1];
        // 単体の第3支持点
        const auto c = simplex.points[2];
        // 第1から第2支持点への差
        const auto ab = Sub(b.point, a.point);
        // 第1から第3支持点への差
        const auto ac = Sub(c.point, a.point);
        // 第1支持点から原点への差
        const auto ao = Mul(a.point, -1.0f);
        // 単体の三角形法線
        const auto abc = Cross(ab, ac);

        if (SameDirection(Cross(abc, ac), ao))
        {
            if (SameDirection(ac, ao))
            {
                SetSimplex(simplex, a, c);
                direction = TripleCross(ac, ao, ac);
            }
            else
            {
                SetSimplex(simplex, a, b);
                return LineCase(simplex, direction);
            }
        }
        else if (SameDirection(Cross(ab, abc), ao))
        {
            SetSimplex(simplex, a, b);
            return LineCase(simplex, direction);
        }
        else if (SameDirection(abc, ao))
        {
            SetSimplex(simplex, a, b, c);
            direction = abc;
        }
        else
        {
            SetSimplex(simplex, a, c, b);
            direction = Mul(abc, -1.0f);
        }
        return false;
    }

    // 四面体が原点を含むか調べ単体を更新します(simplex: 更新する単体, direction: 次の方向の出力)。
    bool TetrahedronCase(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept
    {
        // 単体の第1支持点
        const auto a = simplex.points[0];
        // 単体の第2支持点
        const auto b = simplex.points[1];
        // 単体の第3支持点
        const auto c = simplex.points[2];
        // 単体の第4支持点
        const auto d = simplex.points[3];
        // 第1から第2支持点への差
        const auto ab = Sub(b.point, a.point);
        // 第1から第3支持点への差
        const auto ac = Sub(c.point, a.point);
        // 第1から第4支持点への差
        const auto ad = Sub(d.point, a.point);
        // 第1支持点から原点への差
        const auto ao = Mul(a.point, -1.0f);
        // 第1・第2・第3点の面法線
        const auto abc = Cross(ab, ac);
        // 第1・第3・第4点の面法線
        const auto acd = Cross(ac, ad);
        // 第1・第4・第2点の面法線
        const auto adb = Cross(ad, ab);

        if (SameDirection(abc, ao))
        {
            SetSimplex(simplex, a, b, c);
            return TriangleCase(simplex, direction);
        }
        if (SameDirection(acd, ao))
        {
            SetSimplex(simplex, a, c, d);
            return TriangleCase(simplex, direction);
        }
        if (SameDirection(adb, ao))
        {
            SetSimplex(simplex, a, d, b);
            return TriangleCase(simplex, direction);
        }
        return true;
    }

    // 単体を更新して原点包含を判定します(simplex: 更新する単体, direction: 次の方向の出力)。
    bool NextSimplex(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept
    {
        switch (simplex.count)
        {
        case 2: return LineCase(simplex, direction);
        case 3: return TriangleCase(simplex, direction);
        case 4: return TetrahedronCase(simplex, direction);
        default: return false;
        }
    }

    struct EpaFace final
    {
        // 面の三つの支持点番号
        std::array<std::size_t, 3> indices{};
        // 原点から外向きの単位面法線
        XMFLOAT3 normal{};
        // 面の原点からの距離
        float distance{ std::numeric_limits<float>::max() };
    };

    // EPAの面を原点から外向きに計算します(points: 支持点列, i0: 第1頂点番号, i1: 第2頂点番号, i2: 第3頂点番号)。
    EpaFace ComputeEpaFace(
        const std::vector<MinkowskiPoint>& points,
        const std::size_t i0,
        const std::size_t i1,
        const std::size_t i2) noexcept
    {
        // 面の第1頂点の差形状座標
        const auto& p0 = points[i0].point;
        // 面の第2頂点の差形状座標
        const auto& p1 = points[i1].point;
        // 面の第3頂点の差形状座標
        const auto& p2 = points[i2].point;
        // 原点から外向きにする面法線
        auto normal = Cross(Sub(p1, p0), Sub(p2, p0));
        // 面法線の長さの二乗
        const float lengthSquared = LengthSquared(normal);
        if (lengthSquared <= 0.0000001f)
        {
            return EpaFace{
                { i0, i1, i2 },
                {},
                std::numeric_limits<float>::max()
            };
        }
        normal = Mul(normal, 1.0f / std::sqrt(lengthSquared));
        // 面の原点からの距離
        float distance = Dot(normal, p0);
        if (distance < 0.0f)
        {
            normal = Mul(normal, -1.0f);
            distance = -distance;
        }
        return EpaFace{ { i0, i1, i2 }, normal, distance };
    }

    // EPAで接触法線・深さ・点を近似します(startingSimplex: 原点を含む四面体, minkowskiSupport: 差形状の支持点取得)。
    template<typename MinkowskiSupportFn>
    std::optional<LamaPon::Contact3D> RunEpa(
        const Simplex3D& startingSimplex,
        MinkowskiSupportFn&& minkowskiSupport) noexcept
    {
        // EPAで拡張する支持点の列
        std::vector<MinkowskiPoint> points(
            startingSimplex.points.begin(),
            startingSimplex.points.begin()
                + static_cast<std::ptrdiff_t>(
                    startingSimplex.count));

        // EPAで更新する凸多面体の面
        std::vector<EpaFace> faces{
            ComputeEpaFace(points, 0, 1, 2),
            ComputeEpaFace(points, 0, 2, 3),
            ComputeEpaFace(points, 0, 3, 1),
            ComputeEpaFace(points, 1, 3, 2)
        };

        // EPAの最大反復回数
        constexpr int maxEpaIterations = 32;
        // EPA収束を判定する距離の閾値
        constexpr float epaEpsilon = 0.0001f;
        // 現在の原点に最も近い面
        EpaFace bestFace{};
        // EPAの現在の反復番号
        for (int iteration{}; iteration < maxEpaIterations; ++iteration)
        {
            // 原点に最も近い面の番号
            std::size_t closestIndex{};
            // 原点に最も近い面の距離
            float closestDistance = std::numeric_limits<float>::max();
            // 検査または削除する面番号
            for (std::size_t index{}; index < faces.size(); ++index)
            {
                if (faces[index].distance < closestDistance)
                {
                    closestDistance = faces[index].distance;
                    closestIndex = index;
                }
            }
            bestFace = faces[closestIndex];
            if (closestDistance
                >= std::numeric_limits<float>::max() * 0.5f)
            {
                break;
            }

            // 現在の面法線方向の支持点
            const auto support =
                minkowskiSupport(bestFace.normal);
            // 面法線上の支持点投影距離
            const float supportDistance =
                Dot(support.point, bestFace.normal);
            if (supportDistance - closestDistance < epaEpsilon)
            {
                break;
            }

            // 追加した支持点の番号
            const std::size_t newIndex = points.size();
            points.push_back(support);

            // 新しい面を張る境界辺の列
            std::vector<std::pair<std::size_t, std::size_t>> edges;
            // 逆向きの辺を相殺して外周辺を残します(a: 辺の始点番号, b: 辺の終点番号)。
            const auto addOrRemoveEdge = [&](
                const std::size_t a,
                const std::size_t b)
            {
                // 逆向きの登録辺か調べます(edge: 境界辺の番号組)。
                // 逆向きに登録済みの辺の位置
                const auto existing = std::find_if(
                    edges.begin(),
                    edges.end(),
                    [&](const auto& edge)
                    {
                        return edge.first == b
                            && edge.second == a;
                    });
                if (existing != edges.end())
                {
                    edges.erase(existing);
                }
                else
                {
                    edges.emplace_back(a, b);
                }
            };

            // 検査または削除する面番号
            for (std::size_t index{}; index < faces.size();)
            {
                // 可視性を調べる既存の面
                const auto& face = faces[index];
                if (Dot(face.normal, support.point)
                    > face.distance)
                {
                    addOrRemoveEdge(
                        face.indices[0], face.indices[1]);
                    addOrRemoveEdge(
                        face.indices[1], face.indices[2]);
                    addOrRemoveEdge(
                        face.indices[2], face.indices[0]);
                    faces.erase(faces.begin()
                        + static_cast<std::ptrdiff_t>(index));
                }
                else
                {
                    ++index;
                }
            }

            // 新しい支持点へ面を張る境界辺
            for (const auto& edge : edges)
            {
                faces.push_back(ComputeEpaFace(
                    points, edge.first, edge.second, newIndex));
            }
        }

        if (LengthSquared(bestFace.normal) <= 0.0000001f)
        {
            return std::nullopt;
        }

        // 最近接面の第1支持点
        const auto& mp0 = points[bestFace.indices[0]];
        // 最近接面の第2支持点
        const auto& mp1 = points[bestFace.indices[1]];
        // 最近接面の第3支持点
        const auto& mp2 = points[bestFace.indices[2]];
        // 原点を最近接面へ投影した位置
        const XMFLOAT3 projected =
            Mul(bestFace.normal, bestFace.distance);

        // 最近接面の第1辺の方向
        const auto v0 = Sub(mp1.point, mp0.point);
        // 最近接面の第2辺の方向
        const auto v1 = Sub(mp2.point, mp0.point);
        // 面の第1頂点から投影点への差
        const auto v2 = Sub(projected, mp0.point);
        // 第1辺の長さの二乗
        const float d00 = Dot(v0, v0);
        // 最近接面の二辺の内積
        const float d01 = Dot(v0, v1);
        // 第2辺の長さの二乗
        const float d11 = Dot(v1, v1);
        // 投影点との差と第1辺の内積
        const float d20 = Dot(v2, v0);
        // 投影点との差と第2辺の内積
        const float d21 = Dot(v2, v1);
        // 重心座標を求める分母
        const float denom = d00 * d11 - d01 * d01;
        // 最近接面の第2点の重心座標
        float v{ 0.0f };
        // 最近接面の第3点の重心座標
        float w{ 0.0f };
        if (std::abs(denom) > 0.0000001f)
        {
            v = (d11 * d20 - d01 * d21) / denom;
            w = (d00 * d21 - d01 * d20) / denom;
        }
        // 最近接面の第1点の重心座標
        const float u = 1.0f - v - w;

        // 第1形状上へ復元した接触点
        const auto onA = Add(
            Add(Mul(mp0.onA, u), Mul(mp1.onA, v)),
            Mul(mp2.onA, w));
        // 第2形状上へ復元した接触点
        const auto onB = Add(
            Add(Mul(mp0.onB, u), Mul(mp1.onB, v)),
            Mul(mp2.onB, w));

        // 差形状A-Bの外向き法線を反転し、第1形状側へ向けます。
        return LamaPon::Contact3D{
            Mul(bestFace.normal, -1.0f),
            std::max(bestFace.distance, 0.0f),
            Mul(Add(onA, onB), 0.5f)
        };
    }

    // GJKで重なりを調べEPAで接触を近似します(supportA: 第1形状の支持点取得, supportB: 第2形状の支持点取得, initialDirection: 初期探索方向)。
    template<typename SupportA, typename SupportB>
    std::optional<LamaPon::Contact3D> GjkEpaIntersect(
        SupportA&& supportA,
        SupportB&& supportB,
        XMFLOAT3 initialDirection) noexcept
    {
        if (LengthSquared(initialDirection) <= 0.0000001f)
        {
            initialDirection = { 1.0f, 0.0f, 0.0f };
        }

        // 二形状の対応点を持つ差の支持点を作ります(direction: 探索方向)。
        const auto minkowskiSupport =
            [&](const XMFLOAT3& direction) -> MinkowskiPoint
        {
            // 支持点を探す単位方向
            const auto normalizedDirection =
                Normalize(direction, { 1.0f, 0.0f, 0.0f });
            // 第1形状上の支持点
            const auto a = supportA(normalizedDirection);
            // 第2形状上の支持点
            const auto b =
                supportB(Mul(normalizedDirection, -1.0f));
            return { Sub(a, b), a, b };
        };

        // GJKで更新する最大4点の単体
        Simplex3D simplex{};
        // GJKの次の探索方向
        XMFLOAT3 direction = initialDirection;
        // 初期探索方向の支持点
        const auto first = minkowskiSupport(direction);
        SetSimplex(simplex, first);
        direction = Mul(first.point, -1.0f);

        // 四面体が原点を包含したか
        bool intersecting{};
        // GJKの最大反復回数
        constexpr int maxGjkIterations = 32;
        // GJKの現在の反復番号
        for (int iteration{}; iteration < maxGjkIterations; ++iteration)
        {
            // 次の探索方向の支持点
            const auto support = minkowskiSupport(direction);
            if (Dot(support.point, direction) < 0.0f)
            {
                return std::nullopt;
            }

            // 新支持点用にずらす単体の番号
            for (std::size_t index =
                    std::min<std::size_t>(simplex.count, 3);
                index > 0;
                --index)
            {
                simplex.points[index] = simplex.points[index - 1];
            }
            simplex.points[0] = support;
            simplex.count =
                std::min<std::size_t>(simplex.count + 1, 4);

            if (NextSimplex(simplex, direction))
            {
                intersecting = true;
                break;
            }
        }

        if (!intersecting || simplex.count < 4)
        {
            return std::nullopt;
        }

        return RunEpa(simplex, minkowskiSupport);
    }
}

namespace LamaPon
{
    std::array<DirectX::XMFLOAT3, 8> CornersOf(
        const OrientedBox3D& box) noexcept
    {
        // 回転箱の8頂点の出力配列
        std::array<DirectX::XMFLOAT3, 8> result;
        // 次に格納する頂点番号
        std::size_t index{};
        // X軸の正負の頂点側
        for (int x = -1; x <= 1; x += 2)
        {
            // Y軸の正負の頂点側
            for (int y = -1; y <= 1; y += 2)
            {
                // Z軸の正負の頂点側
                for (int z = -1; z <= 1; z += 2)
                {
                    // 現在構築する箱の頂点
                    auto corner = box.center;
                    corner = Add(corner, Mul(
                        box.axes[0], box.halfExtents.x * x));
                    corner = Add(corner, Mul(
                        box.axes[1], box.halfExtents.y * y));
                    corner = Add(corner, Mul(
                        box.axes[2], box.halfExtents.z * z));
                    result[index++] = corner;
                }
            }
        }
        return result;
    }

    Bounds3D BoundsOf(const OrientedBox3D& box) noexcept
    {
        // 全箱頂点を含む軸平行境界
        Bounds3D result{
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
        // 境界へ含める箱の頂点
        for (const auto& corner : CornersOf(box))
        {
            result.minimum.x = std::min(result.minimum.x, corner.x);
            result.minimum.y = std::min(result.minimum.y, corner.y);
            result.minimum.z = std::min(result.minimum.z, corner.z);
            result.maximum.x = std::max(result.maximum.x, corner.x);
            result.maximum.y = std::max(result.maximum.y, corner.y);
            result.maximum.z = std::max(result.maximum.z, corner.z);
        }
        return result;
    }

    Bounds3D BoundsOf(const Capsule3D& capsule) noexcept
    {
        // 非負に制限したカプセル半径
        const float radius = std::max(capsule.radius, 0.0f);
        return {
            {
                std::min(capsule.start.x, capsule.end.x) - radius,
                std::min(capsule.start.y, capsule.end.y) - radius,
                std::min(capsule.start.z, capsule.end.z) - radius
            },
            {
                std::max(capsule.start.x, capsule.end.x) + radius,
                std::max(capsule.start.y, capsule.end.y) + radius,
                std::max(capsule.start.z, capsule.end.z) + radius
            }
        };
    }

    Bounds3D BoundsOf(const Sphere3D& sphere) noexcept
    {
        // 非負に制限した球の半径
        const float radius =
            std::max(sphere.radius, 0.0f);
        return {
            {
                sphere.center.x - radius,
                sphere.center.y - radius,
                sphere.center.z - radius
            },
            {
                sphere.center.x + radius,
                sphere.center.y + radius,
                sphere.center.z + radius
            }
        };
    }

    Bounds3D BoundsOf(const ConvexHull3D& hull) noexcept
    {
        // 凸包頂点を含む軸平行境界
        Bounds3D result{
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
        // 境界へ含める凸包頂点
        for (const auto& point : hull.points)
        {
            result.minimum.x = std::min(result.minimum.x, point.x);
            result.minimum.y = std::min(result.minimum.y, point.y);
            result.minimum.z = std::min(result.minimum.z, point.z);
            result.maximum.x = std::max(result.maximum.x, point.x);
            result.maximum.y = std::max(result.maximum.y, point.y);
            result.maximum.z = std::max(result.maximum.z, point.z);
        }
        return result;
    }

    std::optional<Contact3D> Intersect(
        const OrientedBox3D& left,
        const OrientedBox3D& right) noexcept
    {
        // 箱同士の複数点接触の結果
        const auto manifold =
            IntersectManifold(left, right);
        if (!manifold)
        {
            return std::nullopt;
        }
        // 接触点の合計後に求める平均点
        XMFLOAT3 average{};
        // 平均へ含める接触点番号
        for (std::size_t index{};
            index < manifold->pointCount;
            ++index)
        {
            average = Add(
                average,
                manifold->points[index]);
        }
        average = Mul(
            average,
            1.0f / static_cast<float>(
                std::max<std::size_t>(
                    manifold->pointCount,
                    1)));
        return Contact3D{
            manifold->normal,
            manifold->penetration,
            average
        };
    }

    std::optional<ContactManifold3D> IntersectManifold(
        const OrientedBox3D& left,
        const OrientedBox3D& right) noexcept
    {
        // 第1箱から第2箱の中心への差
        const auto delta = Sub(right.center, left.center);
        // 最も浅いめり込み距離
        float minimum = std::numeric_limits<float>::max();
        // 第1箱側へ向けた接触法線
        XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        // 分離と最小めり込みを検査します(raw: 分離軸, edgeAxis: 辺同士の外積軸か)。
        const auto test = [&](
            const XMFLOAT3& raw,
            const bool edgeAxis = false)
        {
            if (LengthSquared(raw) <= 0.0000001f) return true;
            // 正規化したSATの分離軸
            const auto axis = Normalize(raw);
            // 第1箱の分離軸上の投影半径
            float leftRadius{};
            // 第2箱の分離軸上の投影半径
            float rightRadius{};
            // 投影半径を求める箱の局所軸番号
            for (std::size_t index{}; index < 3; ++index)
            {
                leftRadius += At(left.halfExtents, index)
                    * std::abs(Dot(left.axes[index], axis));
                rightRadius += At(right.halfExtents, index)
                    * std::abs(Dot(right.axes[index], axis));
            }
            // 分離軸上のめり込み距離
            const float overlap = leftRadius + rightRadius
                - std::abs(Dot(delta, axis));
            if (overlap <= 0.0f) return false;
            // 辺の軸を選びにくくする距離
            const float selectionBias =
                edgeAxis
                ? std::max(
                    0.005f,
                    minimum * 0.05f)
                : 0.0f;
            if (overlap + selectionBias
                < minimum)
            {
                minimum = overlap;
                normal = Dot(delta, axis) > 0.0f
                    ? Mul(axis, -1.0f)
                    : axis;
            }
            return true;
        };
        // SATで判定する箱の局所軸番号
        for (std::size_t index{}; index < 3; ++index)
        {
            if (!test(left.axes[index]) || !test(right.axes[index]))
                return std::nullopt;
        }
        // 第1箱の外積用局所軸
        for (const auto& a : left.axes)
            // 第2箱の外積用局所軸
            for (const auto& b : right.axes)
                if (!test(Cross(a, b), true))
                    return std::nullopt;

        // 最大4点を保持する接触結果
        ContactManifold3D result{
            normal,
            minimum
        };
        // 第1箱の8頂点
        const auto leftCorners = CornersOf(left);
        // 第2箱の8頂点
        const auto rightCorners = CornersOf(right);
        // 含まれた第1箱頂点の最小投影
        float minimumLeftProjection =
            std::numeric_limits<float>::max();
        // 含まれた第2箱頂点の最大投影
        float maximumRightProjection =
            -std::numeric_limits<float>::max();
        // 第2箱内に第1箱の頂点があるか
        bool hasLeftCandidate{};
        // 第1箱内に第2箱の頂点があるか
        bool hasRightCandidate{};
        // 相手箱内かを調べる箱の頂点
        for (const auto& point : leftCorners)
        {
            if (Contains(right, point))
            {
                minimumLeftProjection =
                    std::min(
                        minimumLeftProjection,
                        Dot(point, normal));
                hasLeftCandidate = true;
            }
        }
        // 相手箱内かを調べる箱の頂点
        for (const auto& point : rightCorners)
        {
            if (Contains(left, point))
            {
                maximumRightProjection =
                    std::max(
                        maximumRightProjection,
                        Dot(point, normal));
                hasRightCandidate = true;
            }
        }

        // 接触面側の第1箱の支持点
        const auto leftSupport =
            Support(left, Mul(normal, -1.0f));
        // 接触面側の第2箱の支持点
        const auto rightSupport =
            Support(right, normal);
        // 接触面の法線上の投影位置
        const float contactPlane =
            (Dot(leftSupport, normal)
                + Dot(rightSupport, normal))
            * 0.5f;
        // 面へ投影し重複を除いて接触点を追加します(point: 候補点)。
        const auto addPoint =
            [&](XMFLOAT3 point)
        {
            // 頂点を接触面へ投影する距離
            const float distance =
                contactPlane - Dot(point, normal);
            point = Add(
                point,
                Mul(normal, distance));
            // 重複を照合する接触点番号
            for (std::size_t index{};
                index < result.pointCount;
                ++index)
            {
                if (LengthSquared(
                        Sub(
                            result.points[index],
                            point))
                    < 0.000001f)
                {
                    return;
                }
            }
            if (result.pointCount
                < result.points.size())
            {
                result.points[
                    result.pointCount++] = point;
            }
        };

        // 接触面上の頂点を含む許容距離
        constexpr float faceTolerance = 0.0001f;
        if (hasLeftCandidate)
        {
            // 相手箱内かを調べる箱の頂点
            for (const auto& point : leftCorners)
            {
                if (Contains(right, point)
                    && Dot(point, normal)
                        <= minimumLeftProjection
                            + faceTolerance)
                {
                    addPoint(point);
                }
            }
        }
        if (hasRightCandidate
            && result.pointCount
                < result.points.size())
        {
            // 相手箱内かを調べる箱の頂点
            for (const auto& point : rightCorners)
            {
                if (Contains(left, point)
                    && Dot(point, normal)
                        >= maximumRightProjection
                            - faceTolerance)
                {
                    addPoint(point);
                }
            }
        }
        if (result.pointCount == 0)
        {
            addPoint(
                Mul(
                    Add(leftSupport, rightSupport),
                    0.5f));
        }
        return result;
    }

    std::optional<Contact3D> Intersect(
        const Capsule3D& left,
        const Capsule3D& right) noexcept
    {
        // 第1カプセル中心線上の最近点
        XMFLOAT3 leftPoint{};
        // 第2カプセル中心線上の最近点
        XMFLOAT3 rightPoint{};
        ClosestSegments(left, right, leftPoint, rightPoint);
        // 第2最近点から第1最近点への差
        const auto delta = Sub(leftPoint, rightPoint);
        // 二つの中心線の最短距離の二乗
        const float squared = LengthSquared(delta);
        // 両カプセルの非負半径の合計
        const float radius =
            std::max(left.radius, 0.0f)
            + std::max(right.radius, 0.0f);
        if (squared >= radius * radius) return std::nullopt;
        // 中心線同士の最短距離
        const float distance = std::sqrt(std::max(squared, 0.0f));
        // 第1カプセルへ向けた接触法線
        const auto normal =
            distance > 0.00001f
            ? Mul(delta, 1.0f / distance)
            : Normalize(Sub(
                Add(left.start, left.end),
                Add(right.start, right.end)));
        // 第1カプセルの非負半径
        const float leftRadius =
            std::max(left.radius, 0.0f);
        // 第2カプセルの非負半径
        const float rightRadius =
            std::max(right.radius, 0.0f);
        return Contact3D{
            normal,
            radius - distance,
            Mul(
                Add(
                    Sub(
                        leftPoint,
                        Mul(normal, leftRadius)),
                    Add(
                        rightPoint,
                        Mul(normal, rightRadius))),
                0.5f)
        };
    }

    std::optional<Contact3D> Intersect(
        const Capsule3D& capsule,
        const OrientedBox3D& box) noexcept
    {
        // 中心線の三分探索区間の下限
        float low{};
        // 中心線の三分探索区間の上限
        float high{ 1.0f };
        // 最近点の三分探索の反復番号
        for (int iteration{}; iteration < 28; ++iteration)
        {
            // 三分探索の手前の評価位置
            const float a = low + (high - low) / 3.0f;
            // 三分探索の奥の評価位置
            const float b = high - (high - low) / 3.0f;
            if (DistanceToBoxSquared(capsule, box, a)
                < DistanceToBoxSquared(capsule, box, b))
                high = b;
            else
                low = a;
        }
        // カプセル中心線上の最近点
        XMFLOAT3 segmentPoint{};
        // 箱側の最近点
        XMFLOAT3 boxPoint{};
        // 中心線と箱の最短距離の二乗
        const float squared = DistanceToBoxSquared(
            capsule,
            box,
            (low + high) * 0.5f,
            &segmentPoint,
            &boxPoint);
        // 非負に制限したカプセル半径
        const float radius = std::max(capsule.radius, 0.0f);
        if (squared >= radius * radius) return std::nullopt;
        // 中心線と箱の最短距離
        const float distance = std::sqrt(std::max(squared, 0.0f));
        if (distance > 0.00001f)
        {
            return Contact3D{
                Mul(Sub(segmentPoint, boxPoint), 1.0f / distance),
                radius - distance,
                Mul(
                    Add(
                        Sub(
                            segmentPoint,
                            Mul(
                                Mul(
                                    Sub(
                                        segmentPoint,
                                        boxPoint),
                                    1.0f / distance),
                                radius)),
                        boxPoint),
                    0.5f)
            };
        }
        // 最近点を箱の局所座標へ写した点
        const auto local = ToLocal(segmentPoint, box);
        // 各箱面までの局所距離
        const std::array distances{
            box.halfExtents.x - std::abs(local.x),
            box.halfExtents.y - std::abs(local.y),
            box.halfExtents.z - std::abs(local.z)
        };
        // 内包時に最も近い箱の面番号
        const std::size_t face = static_cast<std::size_t>(
            std::min_element(distances.begin(), distances.end())
            - distances.begin());
        // 最近点に対応する面の正負
        const float sign = At(local, face) >= 0.0f ? 1.0f : -1.0f;
        // 第1形状側へ向く接触法線
        const auto normal =
            Mul(box.axes[face], sign);
        // 最近面上の局所座標
        auto faceLocal = local;
        (&faceLocal.x)[face] =
            At(box.halfExtents, face)
            * sign;
        // 最近面上の入力座標系の点
        const auto facePoint =
            ToWorld(faceLocal, box);
        return Contact3D{
            normal,
            radius + distances[face],
            Mul(
                Add(
                    Sub(
                        segmentPoint,
                        Mul(normal, radius)),
                    facePoint),
                0.5f)
        };
    }

    std::optional<Contact3D> Intersect(
        const Sphere3D& left,
        const Sphere3D& right) noexcept
    {
        return Intersect(
            Capsule3D{
                left.center,
                left.center,
                left.radius
            },
            Capsule3D{
                right.center,
                right.center,
                right.radius
            });
    }

    std::optional<Contact3D> Intersect(
        const Sphere3D& sphere,
        const OrientedBox3D& box) noexcept
    {
        return Intersect(
            Capsule3D{
                sphere.center,
                sphere.center,
                sphere.radius
            },
            box);
    }

    std::optional<Contact3D> Intersect(
        const Sphere3D& sphere,
        const Capsule3D& capsule) noexcept
    {
        return Intersect(
            Capsule3D{
                sphere.center,
                sphere.center,
                sphere.radius
            },
            capsule);
    }

    std::optional<Contact3D> Intersect(
        const ConvexHull3D& left,
        const ConvexHull3D& right) noexcept
    {
        if (left.points.empty() || right.points.empty())
        {
            return std::nullopt;
        }
        // 両形状の支持点を取得します(direction: 各形状の探索方向)。
        return GjkEpaIntersect(
            [&](const XMFLOAT3& direction)
            {
                return Support(left, direction);
            },
            [&](const XMFLOAT3& direction)
            {
                return Support(right, direction);
            },
            Sub(Centroid(right), Centroid(left)));
    }

    std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const OrientedBox3D& box) noexcept
    {
        if (hull.points.empty())
        {
            return std::nullopt;
        }
        // 両形状の支持点を取得します(direction: 各形状の探索方向)。
        return GjkEpaIntersect(
            [&](const XMFLOAT3& direction)
            {
                return Support(hull, direction);
            },
            [&](const XMFLOAT3& direction)
            {
                return Support(box, direction);
            },
            Sub(box.center, Centroid(hull)));
    }

    std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const Capsule3D& capsule) noexcept
    {
        if (hull.points.empty())
        {
            return std::nullopt;
        }
        // 両形状の支持点を取得します(direction: 各形状の探索方向)。
        return GjkEpaIntersect(
            [&](const XMFLOAT3& direction)
            {
                return Support(hull, direction);
            },
            [&](const XMFLOAT3& direction)
            {
                return Support(capsule, direction);
            },
            Sub(
                Mul(
                    Add(capsule.start, capsule.end),
                    0.5f),
                Centroid(hull)));
    }

    std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const Sphere3D& sphere) noexcept
    {
        if (hull.points.empty())
        {
            return std::nullopt;
        }
        // 両形状の支持点を取得します(direction: 各形状の探索方向)。
        return GjkEpaIntersect(
            [&](const XMFLOAT3& direction)
            {
                return Support(hull, direction);
            },
            [&](const XMFLOAT3& direction)
            {
                return Support(sphere, direction);
            },
            Sub(sphere.center, Centroid(hull)));
    }
}

namespace LamaPon
{
    std::optional<ContactManifold3D> IntersectManifold(
        const Capsule3D& capsule,
        const OrientedBox3D& box) noexcept
    {
        // カプセル始端の球
        const Sphere3D startSphere{
            capsule.start,
            capsule.radius };
        // カプセル終端の球
        const Sphere3D endSphere{
            capsule.end,
            capsule.radius };
        // 始端球と箱の接触結果
        const auto startContact =
            Intersect(startSphere, box);
        // 終端球と箱の接触結果
        const auto endContact =
            Intersect(endSphere, box);

        if (startContact && endContact)
        {
            // 始端側のめり込みが深いか
            const bool startDeeper =
                startContact->penetration
                >= endContact->penetration;
            // 最大2点の接触結果
            ContactManifold3D manifold{};
            manifold.normal = startDeeper
                ? startContact->normal
                : endContact->normal;
            manifold.penetration = startDeeper
                ? startContact->penetration
                : endContact->penetration;
            manifold.points[0] = startContact->point;
            manifold.points[1] = endContact->point;
            manifold.pointCount = 2;
            return manifold;
        }

        // 両端か円筒部の単一点接触
        const auto single = (startContact || endContact)
            ? (startContact ? startContact : endContact)
            : Intersect(capsule, box);
        if (!single)
        {
            return std::nullopt;
        }
        // 最大2点の接触結果
        ContactManifold3D manifold{};
        manifold.normal = single->normal;
        manifold.penetration = single->penetration;
        manifold.points[0] = single->point;
        manifold.pointCount = 1;
        return manifold;
    }
}
