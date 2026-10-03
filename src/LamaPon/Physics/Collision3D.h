#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <array>
#include <optional>
#include <vector>

namespace LamaPon
{
    // 形状同士は同じ座標系で渡し、箱のaxesは互いに直交する単位ベクトルにします。
    struct OrientedBox3D final
    {
        // 箱の中心座標
        DirectX::XMFLOAT3 center{};
        // 箱の直交する三つの局所軸
        std::array<DirectX::XMFLOAT3, 3> axes{
            DirectX::XMFLOAT3{ 1.0f, 0.0f, 0.0f },
            DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f },
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 1.0f }
        };
        // 局所軸に沿う箱の半径
        DirectX::XMFLOAT3 halfExtents{ 0.5f, 0.5f, 0.5f };
    };

    struct Capsule3D final
    {
        // 中心線の始点座標
        DirectX::XMFLOAT3 start{};
        // 中心線の終点座標
        DirectX::XMFLOAT3 end{};
        // カプセルの半径
        float radius{ 0.5f };
    };

    struct Sphere3D final
    {
        // 球の中心座標
        DirectX::XMFLOAT3 center{};
        // 球の半径
        float radius{ 0.5f };
    };

    struct ConvexHull3D final
    {
        // 凸包を定義する頂点列
        std::vector<DirectX::XMFLOAT3> points;
    };

    // 法線は第1引数の形状側を向き、距離と点は入力形状と同じ座標系です。
    struct Contact3D final
    {
        // 第1形状側へ向く接触法線
        DirectX::XMFLOAT3 normal{};
        // 接触面のめり込み深さ
        float penetration{};
        // 接触点の座標
        DirectX::XMFLOAT3 point{};
    };

    // 各点と法線はContact3Dと同じ規則で、pointCountだけが有効な接触点です。
    struct ContactManifold3D final
    {
        // 第1形状側へ向く接触法線
        DirectX::XMFLOAT3 normal{};
        // 接触面のめり込み深さ
        float penetration{};
        // 最大4点の接触点配列
        std::array<DirectX::XMFLOAT3, 4> points{};
        // 有効な接触点の個数
        std::size_t pointCount{};
    };

    // 回転箱を囲む軸平行境界を求めます(box: 回転箱)。
    [[nodiscard]] Bounds3D BoundsOf(
        const OrientedBox3D& box) noexcept;
    // カプセルを囲む軸平行境界を求めます(capsule: カプセル)。
    // 負の半径は0として扱います。
    [[nodiscard]] Bounds3D BoundsOf(
        const Capsule3D& capsule) noexcept;
    // 球を囲む軸平行境界を求めます(sphere: 球)。
    // 負の半径は0として扱います。
    [[nodiscard]] Bounds3D BoundsOf(
        const Sphere3D& sphere) noexcept;
    // 凸包の頂点を囲む軸平行境界を求めます(hull: 頂点列を持つ凸包)。
    // 空の凸包はminがmaxより大きい無効な境界を返します。
    [[nodiscard]] Bounds3D BoundsOf(
        const ConvexHull3D& hull) noexcept;
    // 回転箱の8頂点を求めます(box: 回転箱)。
    [[nodiscard]] std::array<DirectX::XMFLOAT3, 8>
        CornersOf(const OrientedBox3D& box) noexcept;

    // 回転箱同士の接触点の平均を求めます(left: 第1回転箱, right: 第2回転箱)。
    // 正のめり込みがない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const OrientedBox3D& left,
        const OrientedBox3D& right) noexcept;
    // 回転箱同士の最大4接触点を求めます(left: 第1回転箱, right: 第2回転箱)。
    // SATで正のめり込みがない場合はnulloptです。
    [[nodiscard]] std::optional<ContactManifold3D>
        IntersectManifold(
            const OrientedBox3D& left,
            const OrientedBox3D& right) noexcept;
    // 中心線の最近点からカプセル接触を求めます(left: 第1カプセル, right: 第2カプセル)。
    // 正のめり込みがない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Capsule3D& left,
        const Capsule3D& right) noexcept;
    // 中心線と箱の最近点から接触を求めます(capsule: カプセル, box: 回転箱)。
    // 正のめり込みがない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Capsule3D& capsule,
        const OrientedBox3D& box) noexcept;
    // カプセルと箱の最大2接触点を求めます(capsule: カプセル, box: 回転箱)。
    // 両端球から接触点を作り、円筒部だけの接触は単一点判定へ戻します。
    [[nodiscard]] std::optional<ContactManifold3D>
        IntersectManifold(
            const Capsule3D& capsule,
            const OrientedBox3D& box) noexcept;
    // 球同士の接触を求めます(left: 第1球, right: 第2球)。
    // 正のめり込みがない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Sphere3D& left,
        const Sphere3D& right) noexcept;
    // 球と回転箱の接触を求めます(sphere: 球, box: 回転箱)。
    // 正のめり込みがない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Sphere3D& sphere,
        const OrientedBox3D& box) noexcept;
    // 球とカプセルの接触を求めます(sphere: 球, capsule: カプセル)。
    // 正のめり込みがない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const Sphere3D& sphere,
        const Capsule3D& capsule) noexcept;

    // GJKとEPAで凸包同士の接触を求めます(left: 第1凸包, right: 第2凸包)。
    // 空の凸包やGJKの未収束、EPAで有効法線を得られない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const ConvexHull3D& left,
        const ConvexHull3D& right) noexcept;
    // GJKとEPAで凸包と箱の接触を求めます(hull: 凸包, box: 回転箱)。
    // 空の凸包やGJKの未収束、EPAで有効法線を得られない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const OrientedBox3D& box) noexcept;
    // GJKとEPAで凸包とカプセルの接触を求めます(hull: 凸包, capsule: カプセル)。
    // 空の凸包やGJKの未収束、EPAで有効法線を得られない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const Capsule3D& capsule) noexcept;
    // GJKとEPAで凸包と球の接触を求めます(hull: 凸包, sphere: 球)。
    // 空の凸包やGJKの未収束、EPAで有効法線を得られない場合はnulloptです。
    [[nodiscard]] std::optional<Contact3D> Intersect(
        const ConvexHull3D& hull,
        const Sphere3D& sphere) noexcept;
}
