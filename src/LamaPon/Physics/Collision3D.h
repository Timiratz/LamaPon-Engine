#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <array>
#include <optional>
#include <vector>

namespace LamaPon
{
    struct OrientedBox3D final
    {
        DirectX::XMFLOAT3 center{};
        std::array<DirectX::XMFLOAT3, 3> axes{
            DirectX::XMFLOAT3{ 1.0f, 0.0f, 0.0f },
            DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f },
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 1.0f }
        };
        DirectX::XMFLOAT3 halfExtents{ 0.5f, 0.5f, 0.5f };
    };

    struct Capsule3D final
    {
        DirectX::XMFLOAT3 start{};
        DirectX::XMFLOAT3 end{};
        float radius{ 0.5f };
    };

    struct Sphere3D final
    {
        DirectX::XMFLOAT3 center{};
        float radius{ 0.5f };
    };

    struct ConvexHull3D final
    {
        std::vector<DirectX::XMFLOAT3> points;
    };

    struct Contact3D final
    {
        DirectX::XMFLOAT3 normal{};
        float penetration{};
        DirectX::XMFLOAT3 point{};
    };

    struct ContactManifold3D final
    {
        DirectX::XMFLOAT3 normal{};
        float penetration{};
        std::array<DirectX::XMFLOAT3, 4> points{};
        std::size_t pointCount{};
    };

    [[nodiscard]] Bounds3D BoundsOf(
        const OrientedBox3D& box) noexcept;
    [[nodiscard]] Bounds3D BoundsOf(
        const Capsule3D& capsule) noexcept;
    [[nodiscard]] Bounds3D BoundsOf(
        const Sphere3D& sphere) noexcept;
    [[nodiscard]] Bounds3D BoundsOf(
        const ConvexHull3D& hull) noexcept;
    [[nodiscard]] std::array<DirectX::XMFLOAT3, 8>
        CornersOf(const OrientedBox3D& box) noexcept;

    [[nodiscard]] std::optional<Contact3D> Intersect(
        const OrientedBox3D& left,
        const OrientedBox3D& right) noexcept;
    [[nodiscard]] std::optional<ContactManifold3D>
        IntersectManifold(
            const OrientedBox3D& left,
            const OrientedBox3D& right) noexcept;
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Capsule3D& left,
        const Capsule3D& right) noexcept;
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Capsule3D& capsule,
        const OrientedBox3D& box) noexcept;
    // カプセルの両端球から最大2点の接触を作ります。カプセルが
    // 横倒しで面に乗るときの安定性が単一点より向上します。
    [[nodiscard]] std::optional<ContactManifold3D>
        IntersectManifold(
            const Capsule3D& capsule,
            const OrientedBox3D& box) noexcept;
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Sphere3D& left,
        const Sphere3D& right) noexcept;
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Sphere3D& sphere,
        const OrientedBox3D& box) noexcept;
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Sphere3D& sphere,
        const Capsule3D& capsule) noexcept;

    [[nodiscard]] std::optional<Contact3D> Intersect(
        const ConvexHull3D& left,
        const ConvexHull3D& right) noexcept;
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const OrientedBox3D& box) noexcept;
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const Capsule3D& capsule) noexcept;
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const Sphere3D& sphere) noexcept;
}
