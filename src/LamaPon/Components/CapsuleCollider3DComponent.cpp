#include "LamaPon/Components/CapsuleCollider3DComponent.h"

#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace LamaPon
{
    CapsuleCollider3DComponent::CapsuleCollider3DComponent(
        const float radius,
        const float height,
        const DirectX::XMFLOAT3 offset,
        const bool isTrigger,
        const std::uint32_t layer,
        const std::uint32_t collisionMask,
        PhysicsMaterial material) noexcept
        : m_radius(std::max(radius, 0.01f))
        , m_height(std::max(height, m_radius * 2.0f))
        , m_offset(offset)
        , m_isTrigger(isTrigger)
        , m_layer(layer % 32u)
        , m_collisionMask(collisionMask)
        , m_material(material)
    {
        m_material.Clamp();
    }

    void CapsuleCollider3DComponent::SetRadius(
        const float value) noexcept
    {
        m_radius = std::max(value, 0.01f);
        m_height = std::max(m_height, m_radius * 2.0f);
    }

    void CapsuleCollider3DComponent::SetHeight(
        const float value) noexcept
    {
        m_height = std::max(value, m_radius * 2.0f);
    }

    Capsule3D CapsuleCollider3DComponent::WorldCapsule() const noexcept
    {
        using namespace DirectX;
        // 所有物体のワールド変換
        const auto world = Owner().WorldMatrix();
        // カプセルのワールド中心
        XMFLOAT3 center{};
        XMStoreFloat3(
            &center,
            XMVector3TransformCoord(
                XMLoadFloat3(&m_offset),
                world));
        // ワールド変換したX単位軸
        const auto worldX = XMVector3TransformNormal(
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
            world);
        // ワールド変換したY単位軸
        const auto worldY = XMVector3TransformNormal(
            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
            world);
        // ワールド変換したZ単位軸
        const auto worldZ = XMVector3TransformNormal(
            XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
            world);
        // X軸のワールド拡大倍率
        const float scaleX = XMVectorGetX(XMVector3Length(worldX));
        // Y軸のワールド拡大倍率
        const float scaleY = XMVectorGetX(XMVector3Length(worldY));
        // Z軸のワールド拡大倍率
        const float scaleZ = XMVectorGetX(XMVector3Length(worldZ));
        // 近似したワールド半径
        const float radius =
            m_radius * std::max(scaleX, scaleZ);
        // カプセル中心線の方向
        const auto axis = scaleY > 0.000001f
            ? XMVectorScale(worldY, 1.0f / scaleY)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        // 中心線のワールド半長
        const float halfSegment =
            std::max(m_height * 0.5f - m_radius, 0.0f)
            * scaleY;
        // 中心線のワールド始点
        XMFLOAT3 start{};
        // 中心線のワールド終点
        XMFLOAT3 end{};
        XMStoreFloat3(
            &start,
            XMVectorSubtract(
                XMLoadFloat3(&center),
                XMVectorScale(axis, halfSegment)));
        XMStoreFloat3(
            &end,
            XMVectorAdd(
                XMLoadFloat3(&center),
                XMVectorScale(axis, halfSegment)));
        return { start, end, radius };
    }

    Bounds3D CapsuleCollider3DComponent::WorldBounds() const noexcept
    {
        return BoundsOf(WorldCapsule());
    }

    void CapsuleCollider3DComponent::OnRenderDebug3D(
        GraphicsDevice& graphics,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;
        // 形状のデバッグRGBA色
        const auto color = m_isTrigger
            ? XMVectorSet(1.0f, 0.75f, 0.1f, 1.0f)
            : XMVectorSet(0.2f, 0.8f, 1.0f, 1.0f);
        // ワールドカプセルの形状
        const auto capsule = WorldCapsule();
        // 中心線のワールド始点
        const XMVECTOR start = XMLoadFloat3(&capsule.start);
        // 中心線のワールド終点
        const XMVECTOR end = XMLoadFloat3(&capsule.end);
        // カプセル中心線の方向
        XMVECTOR axis = XMVectorSubtract(end, start);
        // 中心線のワールド長さ
        const float axisLength = XMVectorGetX(XMVector3Length(axis));
        axis = axisLength > 0.000001f
            ? XMVectorScale(axis, 1.0f / axisLength)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        // 中心線と平行でない補助軸
        const XMVECTOR reference =
            std::abs(XMVectorGetY(axis)) < 0.9f
                ? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
                : XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        // 半径方向の第1単位軸
        const XMVECTOR tangent =
            XMVector3Normalize(XMVector3Cross(axis, reference));
        // 半径方向の第2単位軸
        const XMVECTOR bitangent =
            XMVector3Normalize(XMVector3Cross(axis, tangent));
        // 輪郭円の分割数
        constexpr int segments = 16;
        // カプセル輪郭の線分端点列
        std::vector<XMFLOAT3> lines;
        lines.reserve(segments * 12 + 16);
        // 線分端点を追加します(a: ワールド始点, b: ワールド終点)。
        const auto appendLine = [&lines](FXMVECTOR a, FXMVECTOR b)
        {
            // 線分のワールド始点
            XMFLOAT3 first{};
            // 線分のワールド終点
            XMFLOAT3 second{};
            XMStoreFloat3(&first, a);
            XMStoreFloat3(&second, b);
            lines.push_back(first);
            lines.push_back(second);
        };
        // 半径方向ベクトルを返します(angle: 円周角ラジアン)。
        const auto radial = [&](const float angle)
        {
            return XMVectorScale(
                XMVectorAdd(
                    XMVectorScale(tangent, std::cos(angle)),
                    XMVectorScale(bitangent, std::sin(angle))),
                capsule.radius);
        };
        // 輪郭線の番号
        for (int index{}; index < segments; ++index)
        {
            // 輪郭線の始点角ラジアン
            const float a =
                XM_2PI * static_cast<float>(index)
                / static_cast<float>(segments);
            // 輪郭線の終点角ラジアン
            const float b =
                XM_2PI * static_cast<float>(index + 1)
                / static_cast<float>(segments);
            appendLine(
                XMVectorAdd(start, radial(a)),
                XMVectorAdd(start, radial(b)));
            appendLine(
                XMVectorAdd(end, radial(a)),
                XMVectorAdd(end, radial(b)));
        }
        // 輪郭線の番号
        for (int index{}; index < 4; ++index)
        {
            // 側線の半径方向ベクトル
            const auto direction = radial(
                XM_PIDIV2 * static_cast<float>(index));
            appendLine(
                XMVectorAdd(start, direction),
                XMVectorAdd(end, direction));
        }
        // 半球の輪郭面の半径単位軸
        for (const XMVECTOR plane : { tangent, bitangent })
        {
            // 輪郭線の番号
            for (int index{}; index < segments / 2; ++index)
            {
                // 輪郭線の始点角ラジアン
                const float a =
                    XM_PI * static_cast<float>(index)
                    / static_cast<float>(segments / 2);
                // 輪郭線の終点角ラジアン
                const float b =
                    XM_PI * static_cast<float>(index + 1)
                    / static_cast<float>(segments / 2);
                // 半球の円周位置を返します(center: 半球の中心, angle: 円周角ラジアン, sign: 中心線方向の符号)。
                const auto capPoint =
                    [&](FXMVECTOR center, const float angle, const float sign)
                    {
                        return XMVectorAdd(
                            center,
                            XMVectorScale(
                                XMVectorAdd(
                                    XMVectorScale(plane, std::cos(angle)),
                                    XMVectorScale(axis, sign * std::sin(angle))),
                                capsule.radius));
                    };
                appendLine(
                    capPoint(start, a, -1.0f),
                    capPoint(start, b, -1.0f));
                appendLine(
                    capPoint(end, a, 1.0f),
                    capPoint(end, b, 1.0f));
            }
        }
        graphics.Debug().DrawLines(lines, color, view, projection);
    }
}
