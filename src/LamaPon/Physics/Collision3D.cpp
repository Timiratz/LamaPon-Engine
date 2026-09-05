#include "LamaPon/Physics/Collision3D.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace
{
    using DirectX::XMFLOAT3;

    XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b) noexcept
    {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }
    XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b) noexcept
    {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }
    XMFLOAT3 Mul(const XMFLOAT3& value, const float scale) noexcept
    {
        return { value.x * scale, value.y * scale, value.z * scale };
    }
    float Dot(const XMFLOAT3& a, const XMFLOAT3& b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b) noexcept
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }
    float LengthSquared(const XMFLOAT3& value) noexcept
    {
        return Dot(value, value);
    }
    XMFLOAT3 TripleCross(
        const XMFLOAT3& a,
        const XMFLOAT3& b,
        const XMFLOAT3& c) noexcept
    {
        return Cross(Cross(a, b), c);
    }
    XMFLOAT3 Normalize(
        const XMFLOAT3& value,
        const XMFLOAT3& fallback = { 0.0f, 1.0f, 0.0f }) noexcept
    {
        const float lengthSquared = LengthSquared(value);
        return lengthSquared <= 0.0000001f
            ? fallback
            : Mul(value, 1.0f / std::sqrt(lengthSquared));
    }
    float At(const XMFLOAT3& value, const std::size_t index) noexcept
    {
        return (&value.x)[index];
    }
    XMFLOAT3 PointOn(
        const LamaPon::Capsule3D& capsule,
        const float amount) noexcept
    {
        return Add(
            capsule.start,
            Mul(Sub(capsule.end, capsule.start), amount));
    }
    XMFLOAT3 ToLocal(
        const XMFLOAT3& point,
        const LamaPon::OrientedBox3D& box) noexcept
    {
        const auto delta = Sub(point, box.center);
        return {
            Dot(delta, box.axes[0]),
            Dot(delta, box.axes[1]),
            Dot(delta, box.axes[2])
        };
    }
    XMFLOAT3 ToWorld(
        const XMFLOAT3& point,
        const LamaPon::OrientedBox3D& box) noexcept
    {
        auto result = box.center;
        for (std::size_t index{}; index < 3; ++index)
        {
            result = Add(
                result,
                Mul(box.axes[index], At(point, index)));
        }
        return result;
    }
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
    XMFLOAT3 Support(
        const LamaPon::OrientedBox3D& box,
        const XMFLOAT3& direction) noexcept
    {
        auto result = box.center;
        for (std::size_t index{}; index < 3; ++index)
        {
            const float projection =
                Dot(box.axes[index], direction);
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
    bool Contains(
        const LamaPon::OrientedBox3D& box,
        const XMFLOAT3& point,
        const float tolerance = 0.0001f) noexcept
    {
        const auto local = ToLocal(point, box);
        return std::abs(local.x)
                <= box.halfExtents.x + tolerance
            && std::abs(local.y)
                <= box.halfExtents.y + tolerance
            && std::abs(local.z)
                <= box.halfExtents.z + tolerance;
    }
    float DistanceToBoxSquared(
        const LamaPon::Capsule3D& capsule,
        const LamaPon::OrientedBox3D& box,
        const float amount,
        XMFLOAT3* segmentPoint = nullptr,
        XMFLOAT3* boxPoint = nullptr) noexcept
    {
        const auto segment = PointOn(capsule, amount);
        const auto closest = ToWorld(
            ClampToBox(ToLocal(segment, box), box.halfExtents),
            box);
        if (segmentPoint != nullptr) *segmentPoint = segment;
        if (boxPoint != nullptr) *boxPoint = closest;
        return LengthSquared(Sub(segment, closest));
    }
    void ClosestSegments(
        const LamaPon::Capsule3D& left,
        const LamaPon::Capsule3D& right,
        XMFLOAT3& leftPoint,
        XMFLOAT3& rightPoint) noexcept
    {
        const auto d1 = Sub(left.end, left.start);
        const auto d2 = Sub(right.end, right.start);
        const auto r = Sub(left.start, right.start);
        const float a = Dot(d1, d1);
        const float e = Dot(d2, d2);
        const float f = Dot(d2, r);
        constexpr float epsilon = 0.000001f;
        float s{};
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
            const float c = Dot(d1, r);
            if (e <= epsilon)
            {
                s = std::clamp(-c / a, 0.0f, 1.0f);
            }
            else
            {
                const float b = Dot(d1, d2);
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

    XMFLOAT3 Support(
        const LamaPon::Capsule3D& capsule,
        const XMFLOAT3& direction) noexcept
    {
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

    XMFLOAT3 Support(
        const LamaPon::ConvexHull3D& hull,
        const XMFLOAT3& direction) noexcept
    {
        XMFLOAT3 best = hull.points.front();
        float bestDot = Dot(best, direction);
        for (const auto& point : hull.points)
        {
            const float projection = Dot(point, direction);
            if (projection > bestDot)
            {
                bestDot = projection;
                best = point;
            }
        }
        return best;
    }

    XMFLOAT3 Centroid(
        const LamaPon::ConvexHull3D& hull) noexcept
    {
        if (hull.points.empty()) return {};
        XMFLOAT3 sum{};
        for (const auto& point : hull.points)
        {
            sum = Add(sum, point);
        }
        return Mul(
            sum,
            1.0f / static_cast<float>(hull.points.size()));
    }

    XMFLOAT3 Perpendicular(const XMFLOAT3& value) noexcept
    {
        const XMFLOAT3 candidate =
            std::abs(value.x) < 0.9f
                ? XMFLOAT3{ 1.0f, 0.0f, 0.0f }
                : XMFLOAT3{ 0.0f, 1.0f, 0.0f };
        return Normalize(Cross(value, candidate));
    }

    bool SameDirection(
        const XMFLOAT3& a,
        const XMFLOAT3& b) noexcept
    {
        return Dot(a, b) > 0.0f;
    }

    // ミンコフスキー差のサポート点です。onAとonBには、各形状でこの点を
    // 作った対応点を保存します。EPAで最近接面が求まった後、接触点を
    // 復元するために使います。
    struct MinkowskiPoint final
    {
        XMFLOAT3 point{};
        XMFLOAT3 onA{};
        XMFLOAT3 onB{};
    };

    struct Simplex3D final
    {
        std::array<MinkowskiPoint, 4> points{};
        std::size_t count{};
    };

    void SetSimplex(
        Simplex3D& simplex,
        const MinkowskiPoint& a) noexcept
    {
        simplex.points[0] = a;
        simplex.count = 1;
    }
    void SetSimplex(
        Simplex3D& simplex,
        const MinkowskiPoint& a,
        const MinkowskiPoint& b) noexcept
    {
        simplex.points[0] = a;
        simplex.points[1] = b;
        simplex.count = 2;
    }
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

    bool NextSimplex(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept;

    bool LineCase(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept
    {
        const auto a = simplex.points[0];
        const auto b = simplex.points[1];
        const auto ab = Sub(b.point, a.point);
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

    bool TriangleCase(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept
    {
        const auto a = simplex.points[0];
        const auto b = simplex.points[1];
        const auto c = simplex.points[2];
        const auto ab = Sub(b.point, a.point);
        const auto ac = Sub(c.point, a.point);
        const auto ao = Mul(a.point, -1.0f);
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

    bool TetrahedronCase(
        Simplex3D& simplex,
        XMFLOAT3& direction) noexcept
    {
        const auto a = simplex.points[0];
        const auto b = simplex.points[1];
        const auto c = simplex.points[2];
        const auto d = simplex.points[3];
        const auto ab = Sub(b.point, a.point);
        const auto ac = Sub(c.point, a.point);
        const auto ad = Sub(d.point, a.point);
        const auto ao = Mul(a.point, -1.0f);
        const auto abc = Cross(ab, ac);
        const auto acd = Cross(ac, ad);
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
        std::array<std::size_t, 3> indices{};
        XMFLOAT3 normal{};
        float distance{ std::numeric_limits<float>::max() };
    };

    EpaFace ComputeEpaFace(
        const std::vector<MinkowskiPoint>& points,
        const std::size_t i0,
        const std::size_t i1,
        const std::size_t i2) noexcept
    {
        const auto& p0 = points[i0].point;
        const auto& p1 = points[i1].point;
        const auto& p2 = points[i2].point;
        auto normal = Cross(Sub(p1, p0), Sub(p2, p0));
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
        float distance = Dot(normal, p0);
        if (distance < 0.0f)
        {
            normal = Mul(normal, -1.0f);
            distance = -distance;
        }
        return EpaFace{ { i0, i1, i2 }, normal, distance };
    }

    template<typename MinkowskiSupportFn>
    std::optional<LamaPon::Contact3D> RunEpa(
        const Simplex3D& startingSimplex,
        MinkowskiSupportFn&& minkowskiSupport) noexcept
    {
        std::vector<MinkowskiPoint> points(
            startingSimplex.points.begin(),
            startingSimplex.points.begin()
                + static_cast<std::ptrdiff_t>(
                    startingSimplex.count));

        std::vector<EpaFace> faces{
            ComputeEpaFace(points, 0, 1, 2),
            ComputeEpaFace(points, 0, 2, 3),
            ComputeEpaFace(points, 0, 3, 1),
            ComputeEpaFace(points, 1, 3, 2)
        };

        constexpr int maxEpaIterations = 32;
        constexpr float epaEpsilon = 0.0001f;
        EpaFace bestFace{};
        for (int iteration{}; iteration < maxEpaIterations; ++iteration)
        {
            std::size_t closestIndex{};
            float closestDistance = std::numeric_limits<float>::max();
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

            const auto support =
                minkowskiSupport(bestFace.normal);
            const float supportDistance =
                Dot(support.point, bestFace.normal);
            if (supportDistance - closestDistance < epaEpsilon)
            {
                break;
            }

            const std::size_t newIndex = points.size();
            points.push_back(support);

            std::vector<std::pair<std::size_t, std::size_t>> edges;
            const auto addOrRemoveEdge = [&](
                const std::size_t a,
                const std::size_t b)
            {
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

            for (std::size_t index{}; index < faces.size();)
            {
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

        const auto& mp0 = points[bestFace.indices[0]];
        const auto& mp1 = points[bestFace.indices[1]];
        const auto& mp2 = points[bestFace.indices[2]];
        const XMFLOAT3 projected =
            Mul(bestFace.normal, bestFace.distance);

        const auto v0 = Sub(mp1.point, mp0.point);
        const auto v1 = Sub(mp2.point, mp0.point);
        const auto v2 = Sub(projected, mp0.point);
        const float d00 = Dot(v0, v0);
        const float d01 = Dot(v0, v1);
        const float d11 = Dot(v1, v1);
        const float d20 = Dot(v2, v0);
        const float d21 = Dot(v2, v1);
        const float denom = d00 * d11 - d01 * d01;
        float v{ 0.0f };
        float w{ 0.0f };
        if (std::abs(denom) > 0.0000001f)
        {
            v = (d11 * d20 - d01 * d21) / denom;
            w = (d00 * d21 - d01 * d20) / denom;
        }
        const float u = 1.0f - v - w;

        const auto onA = Add(
            Add(Mul(mp0.onA, u), Mul(mp1.onA, v)),
            Mul(mp2.onA, w));
        const auto onB = Add(
            Add(Mul(mp0.onB, u), Mul(mp1.onB, v)),
            Mul(mp2.onB, w));

        // ミンコフスキー差をA-Bとする規約では、bestFace.normalは最初の
        // サポート形状から2番目の形状へ向きます。このファイルのほかの
        // Intersect()と同じく、法線を第1引数の形状へ向けるため反転します。
        return LamaPon::Contact3D{
            Mul(bestFace.normal, -1.0f),
            std::max(bestFace.distance, 0.0f),
            Mul(Add(onA, onB), 0.5f)
        };
    }

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

        const auto minkowskiSupport =
            [&](const XMFLOAT3& direction) -> MinkowskiPoint
        {
            const auto normalizedDirection =
                Normalize(direction, { 1.0f, 0.0f, 0.0f });
            const auto a = supportA(normalizedDirection);
            const auto b =
                supportB(Mul(normalizedDirection, -1.0f));
            return { Sub(a, b), a, b };
        };

        Simplex3D simplex{};
        XMFLOAT3 direction = initialDirection;
        const auto first = minkowskiSupport(direction);
        SetSimplex(simplex, first);
        direction = Mul(first.point, -1.0f);

        bool intersecting{};
        constexpr int maxGjkIterations = 32;
        for (int iteration{}; iteration < maxGjkIterations; ++iteration)
        {
            const auto support = minkowskiSupport(direction);
            if (Dot(support.point, direction) < 0.0f)
            {
                return std::nullopt;
            }

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
        std::array<DirectX::XMFLOAT3, 8> result;
        std::size_t index{};
        for (int x = -1; x <= 1; x += 2)
        {
            for (int y = -1; y <= 1; y += 2)
            {
                for (int z = -1; z <= 1; z += 2)
                {
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
        const auto manifold =
            IntersectManifold(left, right);
        if (!manifold)
        {
            return std::nullopt;
        }
        XMFLOAT3 average{};
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
        const auto delta = Sub(right.center, left.center);
        float minimum = std::numeric_limits<float>::max();
        XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        const auto test = [&](
            const XMFLOAT3& raw,
            const bool edgeAxis = false)
        {
            if (LengthSquared(raw) <= 0.0000001f) return true;
            const auto axis = Normalize(raw);
            float leftRadius{};
            float rightRadius{};
            for (std::size_t index{}; index < 3; ++index)
            {
                leftRadius += At(left.halfExtents, index)
                    * std::abs(Dot(left.axes[index], axis));
                rightRadius += At(right.halfExtents, index)
                    * std::abs(Dot(right.axes[index], axis));
            }
            const float overlap = leftRadius + rightRadius
                - std::abs(Dot(delta, axis));
            if (overlap <= 0.0f) return false;
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
        for (std::size_t index{}; index < 3; ++index)
        {
            if (!test(left.axes[index]) || !test(right.axes[index]))
                return std::nullopt;
        }
        for (const auto& a : left.axes)
            for (const auto& b : right.axes)
                if (!test(Cross(a, b), true))
                    return std::nullopt;

        ContactManifold3D result{
            normal,
            minimum
        };
        const auto leftCorners = CornersOf(left);
        const auto rightCorners = CornersOf(right);
        float minimumLeftProjection =
            std::numeric_limits<float>::max();
        float maximumRightProjection =
            -std::numeric_limits<float>::max();
        bool hasLeftCandidate{};
        bool hasRightCandidate{};
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

        const auto leftSupport =
            Support(left, Mul(normal, -1.0f));
        const auto rightSupport =
            Support(right, normal);
        const float contactPlane =
            (Dot(leftSupport, normal)
                + Dot(rightSupport, normal))
            * 0.5f;
        const auto addPoint =
            [&](XMFLOAT3 point)
        {
            const float distance =
                contactPlane - Dot(point, normal);
            point = Add(
                point,
                Mul(normal, distance));
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

        constexpr float faceTolerance = 0.0001f;
        if (hasLeftCandidate)
        {
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
        XMFLOAT3 leftPoint{};
        XMFLOAT3 rightPoint{};
        ClosestSegments(left, right, leftPoint, rightPoint);
        const auto delta = Sub(leftPoint, rightPoint);
        const float squared = LengthSquared(delta);
        const float radius =
            std::max(left.radius, 0.0f)
            + std::max(right.radius, 0.0f);
        if (squared >= radius * radius) return std::nullopt;
        const float distance = std::sqrt(std::max(squared, 0.0f));
        const auto normal =
            distance > 0.00001f
            ? Mul(delta, 1.0f / distance)
            : Normalize(Sub(
                Add(left.start, left.end),
                Add(right.start, right.end)));
        const float leftRadius =
            std::max(left.radius, 0.0f);
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
        float low{};
        float high{ 1.0f };
        for (int iteration{}; iteration < 28; ++iteration)
        {
            const float a = low + (high - low) / 3.0f;
            const float b = high - (high - low) / 3.0f;
            if (DistanceToBoxSquared(capsule, box, a)
                < DistanceToBoxSquared(capsule, box, b))
                high = b;
            else
                low = a;
        }
        XMFLOAT3 segmentPoint{};
        XMFLOAT3 boxPoint{};
        const float squared = DistanceToBoxSquared(
            capsule,
            box,
            (low + high) * 0.5f,
            &segmentPoint,
            &boxPoint);
        const float radius = std::max(capsule.radius, 0.0f);
        if (squared >= radius * radius) return std::nullopt;
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
        const auto local = ToLocal(segmentPoint, box);
        const std::array distances{
            box.halfExtents.x - std::abs(local.x),
            box.halfExtents.y - std::abs(local.y),
            box.halfExtents.z - std::abs(local.z)
        };
        const std::size_t face = static_cast<std::size_t>(
            std::min_element(distances.begin(), distances.end())
            - distances.begin());
        const float sign = At(local, face) >= 0.0f ? 1.0f : -1.0f;
        const auto normal =
            Mul(box.axes[face], sign);
        auto faceLocal = local;
        (&faceLocal.x)[face] =
            At(box.halfExtents, face)
            * sign;
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
    // カプセルの両端球それぞれをボックスと判定し、最大2点の
    // マニフォールドへまとめます。中央の円筒部だけが接触する
    // 場合は従来の単一点判定へフォールバックします。
    std::optional<ContactManifold3D> IntersectManifold(
        const Capsule3D& capsule,
        const OrientedBox3D& box) noexcept
    {
        const Sphere3D startSphere{
            capsule.start,
            capsule.radius };
        const Sphere3D endSphere{
            capsule.end,
            capsule.radius };
        const auto startContact =
            Intersect(startSphere, box);
        const auto endContact =
            Intersect(endSphere, box);

        if (startContact && endContact)
        {
            // 深い方の法線を採用します（両端はほぼ平行のはず）。
            const bool startDeeper =
                startContact->penetration
                >= endContact->penetration;
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

        const auto single = (startContact || endContact)
            ? (startContact ? startContact : endContact)
            : Intersect(capsule, box);
        if (!single)
        {
            return std::nullopt;
        }
        ContactManifold3D manifold{};
        manifold.normal = single->normal;
        manifold.penetration = single->penetration;
        manifold.points[0] = single->point;
        manifold.pointCount = 1;
        return manifold;
    }
}
