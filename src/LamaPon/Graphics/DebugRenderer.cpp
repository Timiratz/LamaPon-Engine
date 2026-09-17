#include "LamaPon/Graphics/DebugRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace LamaPon
{
    DebugRenderer::DebugRenderer(
        std::unique_ptr<DebugDrawingBackend> backend)
        : m_backend(std::move(backend))
    {
        if (!m_backend)
        {
            throw std::invalid_argument(
                "DebugRenderer requires a drawing backend.");
        }
    }

    DebugRenderer::~DebugRenderer() = default;

    void DebugRenderer::Submit(
        const std::span<const DebugLine> lines,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        DirectX::XMFLOAT4X4 storedView{};
        DirectX::XMFLOAT4X4 storedProjection{};
        DirectX::XMStoreFloat4x4(&storedView, view);
        DirectX::XMStoreFloat4x4(
            &storedProjection,
            projection);
        m_backend->DrawLines(
            lines,
            storedView,
            storedProjection);
    }

    void DebugRenderer::DrawBounds(
        const Bounds2D& bounds,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        DirectX::XMFLOAT4 storedColor{};
        DirectX::XMStoreFloat4(&storedColor, color);
        const DirectX::XMFLOAT3 bottomLeft{
            bounds.minimum.x,
            bounds.minimum.y,
            0.0f };
        const DirectX::XMFLOAT3 bottomRight{
            bounds.maximum.x,
            bounds.minimum.y,
            0.0f };
        const DirectX::XMFLOAT3 topRight{
            bounds.maximum.x,
            bounds.maximum.y,
            0.0f };
        const DirectX::XMFLOAT3 topLeft{
            bounds.minimum.x,
            bounds.maximum.y,
            0.0f };
        const std::array lines{
            DebugLine{ bottomLeft, bottomRight, storedColor },
            DebugLine{ bottomRight, topRight, storedColor },
            DebugLine{ topRight, topLeft, storedColor },
            DebugLine{ topLeft, bottomLeft, storedColor }
        };
        Submit(lines, view, projection);
    }

    void DebugRenderer::DrawBounds(
        const Bounds3D& bounds,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        DirectX::XMFLOAT4 storedColor{};
        DirectX::XMStoreFloat4(&storedColor, color);
        const std::array<DirectX::XMFLOAT3, 8> vertices{
            DirectX::XMFLOAT3{
                bounds.minimum.x, bounds.minimum.y, bounds.minimum.z },
            DirectX::XMFLOAT3{
                bounds.maximum.x, bounds.minimum.y, bounds.minimum.z },
            DirectX::XMFLOAT3{
                bounds.maximum.x, bounds.maximum.y, bounds.minimum.z },
            DirectX::XMFLOAT3{
                bounds.minimum.x, bounds.maximum.y, bounds.minimum.z },
            DirectX::XMFLOAT3{
                bounds.minimum.x, bounds.minimum.y, bounds.maximum.z },
            DirectX::XMFLOAT3{
                bounds.maximum.x, bounds.minimum.y, bounds.maximum.z },
            DirectX::XMFLOAT3{
                bounds.maximum.x, bounds.maximum.y, bounds.maximum.z },
            DirectX::XMFLOAT3{
                bounds.minimum.x, bounds.maximum.y, bounds.maximum.z }
        };
        constexpr std::array edges{
            std::pair{ 0, 1 }, std::pair{ 1, 2 },
            std::pair{ 2, 3 }, std::pair{ 3, 0 },
            std::pair{ 4, 5 }, std::pair{ 5, 6 },
            std::pair{ 6, 7 }, std::pair{ 7, 4 },
            std::pair{ 0, 4 }, std::pair{ 1, 5 },
            std::pair{ 2, 6 }, std::pair{ 3, 7 }
        };

        std::array<DebugLine, edges.size()> lines{};
        for (std::size_t index = 0;
            index < edges.size();
            ++index)
        {
            const auto [start, end] = edges[index];
            lines[index] = DebugLine{
                vertices[start],
                vertices[end],
                storedColor };
        }
        Submit(lines, view, projection);
    }

    void DebugRenderer::DrawGridXZ(
        const float spacing,
        const float halfExtent,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (spacing <= 0.0f || halfExtent <= 0.0f)
        {
            return;
        }

        const int lineCount = std::clamp(
            static_cast<int>(std::ceil(halfExtent / spacing)),
            1,
            200);
        const float extent = spacing * static_cast<float>(lineCount);
        constexpr DirectX::XMFLOAT4 minorColor{
            0.22f, 0.25f, 0.30f, 0.48f };
        constexpr DirectX::XMFLOAT4 majorColor{
            0.34f, 0.38f, 0.45f, 0.65f };
        constexpr DirectX::XMFLOAT4 xAxisColor{
            0.85f, 0.20f, 0.20f, 0.90f };
        constexpr DirectX::XMFLOAT4 zAxisColor{
            0.20f, 0.42f, 0.90f, 0.90f };

        std::vector<DebugLine> lines;
        lines.reserve(static_cast<std::size_t>(
            (lineCount * 2 + 1) * 2));
        for (int index = -lineCount; index <= lineCount; ++index)
        {
            const float coordinate = spacing * static_cast<float>(index);
            const auto xLineColor = index == 0
                ? zAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);
            const auto zLineColor = index == 0
                ? xAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);

            lines.push_back(DebugLine{
                { -extent, 0.0f, coordinate },
                { extent, 0.0f, coordinate },
                xLineColor });
            lines.push_back(DebugLine{
                { coordinate, 0.0f, -extent },
                { coordinate, 0.0f, extent },
                zLineColor });
        }
        Submit(lines, view, projection);
    }

    void DebugRenderer::DrawGridXY(
        const float spacing,
        const float halfExtent,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (spacing <= 0.0f || halfExtent <= 0.0f)
        {
            return;
        }

        const int lineCount = std::clamp(
            static_cast<int>(std::ceil(halfExtent / spacing)),
            1,
            200);
        const float extent = spacing * static_cast<float>(lineCount);
        constexpr DirectX::XMFLOAT4 minorColor{
            0.22f, 0.25f, 0.30f, 0.48f };
        constexpr DirectX::XMFLOAT4 majorColor{
            0.34f, 0.38f, 0.45f, 0.65f };
        constexpr DirectX::XMFLOAT4 xAxisColor{
            0.85f, 0.20f, 0.20f, 0.90f };
        constexpr DirectX::XMFLOAT4 yAxisColor{
            0.20f, 0.78f, 0.28f, 0.90f };

        std::vector<DebugLine> lines;
        lines.reserve(static_cast<std::size_t>(
            (lineCount * 2 + 1) * 2));
        for (int index = -lineCount; index <= lineCount; ++index)
        {
            const float coordinate = spacing * static_cast<float>(index);
            const auto horizontalColor = index == 0
                ? xAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);
            const auto verticalColor = index == 0
                ? yAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);

            lines.push_back(DebugLine{
                { -extent, coordinate, 0.0f },
                { extent, coordinate, 0.0f },
                horizontalColor });
            lines.push_back(DebugLine{
                { coordinate, -extent, 0.0f },
                { coordinate, extent, 0.0f },
                verticalColor });
        }
        Submit(lines, view, projection);
    }

    void DebugRenderer::DrawLines(
        const std::span<
            const DirectX::XMFLOAT3> points,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (points.size() < 2)
        {
            return;
        }

        DirectX::XMFLOAT4 storedColor{};
        DirectX::XMStoreFloat4(&storedColor, color);
        std::vector<DebugLine> lines;
        lines.reserve(points.size() / 2);
        for (std::size_t index = 1;
            index < points.size();
            index += 2)
        {
            lines.push_back(DebugLine{
                points[index - 1],
                points[index],
                storedColor });
        }
        Submit(lines, view, projection);
    }

    void DebugRenderer::DrawFrustum(
        DirectX::FXMMATRIX cameraWorld,
        const float verticalFieldOfView,
        const float aspectRatio,
        const float nearDistance,
        const float farDistance,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        const float safeNear = std::max(nearDistance, 0.01f);
        const float safeFar = std::max(farDistance, safeNear + 0.01f);
        const float safeAspect = std::max(aspectRatio, 0.01f);
        const float tangent = std::tan(verticalFieldOfView * 0.5f);
        const float nearHalfHeight = tangent * safeNear;
        const float nearHalfWidth = nearHalfHeight * safeAspect;
        const float farHalfHeight = tangent * safeFar;
        const float farHalfWidth = farHalfHeight * safeAspect;
        DirectX::XMFLOAT4 storedColor{};
        DirectX::XMStoreFloat4(&storedColor, color);

        const std::array localCorners{
            DirectX::XMFLOAT3{ -nearHalfWidth, -nearHalfHeight, -safeNear },
            DirectX::XMFLOAT3{ nearHalfWidth, -nearHalfHeight, -safeNear },
            DirectX::XMFLOAT3{ nearHalfWidth, nearHalfHeight, -safeNear },
            DirectX::XMFLOAT3{ -nearHalfWidth, nearHalfHeight, -safeNear },
            DirectX::XMFLOAT3{ -farHalfWidth, -farHalfHeight, -safeFar },
            DirectX::XMFLOAT3{ farHalfWidth, -farHalfHeight, -safeFar },
            DirectX::XMFLOAT3{ farHalfWidth, farHalfHeight, -safeFar },
            DirectX::XMFLOAT3{ -farHalfWidth, farHalfHeight, -safeFar }
        };

        std::array<DirectX::XMFLOAT3, 8> vertices{};
        for (std::size_t index = 0; index < localCorners.size(); ++index)
        {
            DirectX::XMStoreFloat3(
                &vertices[index],
                DirectX::XMVector3Transform(
                    DirectX::XMLoadFloat3(&localCorners[index]),
                    cameraWorld));
        }

        DirectX::XMFLOAT3 origin{};
        DirectX::XMStoreFloat3(
            &origin,
            DirectX::XMVector3Transform(
                DirectX::XMVectorZero(),
                cameraWorld));

        constexpr std::array edges{
            std::pair{ 0, 1 }, std::pair{ 1, 2 },
            std::pair{ 2, 3 }, std::pair{ 3, 0 },
            std::pair{ 4, 5 }, std::pair{ 5, 6 },
            std::pair{ 6, 7 }, std::pair{ 7, 4 },
            std::pair{ 0, 4 }, std::pair{ 1, 5 },
            std::pair{ 2, 6 }, std::pair{ 3, 7 }
        };

        std::array<DebugLine, edges.size() + 4> lines{};
        std::size_t lineIndex{};
        for (const auto [start, end] : edges)
        {
            lines[lineIndex++] = DebugLine{
                vertices[start],
                vertices[end],
                storedColor };
        }
        for (std::size_t corner = 4; corner < vertices.size(); ++corner)
        {
            lines[lineIndex++] = DebugLine{
                origin,
                vertices[corner],
                storedColor };
        }
        Submit(lines, view, projection);
    }

    void DebugRenderer::DrawDirectionalLight(
        DirectX::FXMMATRIX lightWorld,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;

        const XMVECTOR origin = lightWorld.r[3];
        XMVECTOR direction = XMVectorNegate(lightWorld.r[2]);
        if (XMVectorGetX(XMVector3LengthSq(direction)) <= 0.000001f)
        {
            direction = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
        }
        direction = XMVector3Normalize(direction);

        XMVECTOR right = lightWorld.r[0];
        if (XMVectorGetX(XMVector3LengthSq(right)) <= 0.000001f)
        {
            right = g_XMIdentityR0;
        }
        right = XMVector3Normalize(right);

        XMVECTOR up = lightWorld.r[1];
        if (XMVectorGetX(XMVector3LengthSq(up)) <= 0.000001f)
        {
            up = g_XMIdentityR1;
        }
        up = XMVector3Normalize(up);

        const XMVECTOR end = XMVectorMultiplyAdd(
            XMVectorReplicate(2.0f),
            direction,
            origin);
        const XMVECTOR arrowBase = XMVectorMultiplyAdd(
            XMVectorReplicate(-0.42f),
            direction,
            end);

        XMFLOAT4 storedColor{};
        XMStoreFloat4(&storedColor, color);
        const auto storePosition =
            [](FXMVECTOR position)
            {
                XMFLOAT3 result{};
                XMStoreFloat3(&result, position);
                return result;
            };

        const XMFLOAT3 storedEnd = storePosition(end);
        const std::array lines{
            DebugLine{
                storePosition(origin),
                storedEnd,
                storedColor },
            DebugLine{
                storedEnd,
                storePosition(XMVectorMultiplyAdd(
                    XMVectorReplicate(0.22f),
                    right,
                    arrowBase)),
                storedColor },
            DebugLine{
                storedEnd,
                storePosition(XMVectorMultiplyAdd(
                    XMVectorReplicate(-0.22f),
                    right,
                    arrowBase)),
                storedColor },
            DebugLine{
                storedEnd,
                storePosition(XMVectorMultiplyAdd(
                    XMVectorReplicate(0.22f),
                    up,
                    arrowBase)),
                storedColor },
            DebugLine{
                storedEnd,
                storePosition(XMVectorMultiplyAdd(
                    XMVectorReplicate(-0.22f),
                    up,
                    arrowBase)),
                storedColor }
        };
        Submit(lines, view, projection);
    }

    void DebugRenderer::DrawPointLight(
        DirectX::FXMMATRIX lightWorld,
        const float radius,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;

        constexpr std::size_t SegmentCount = 24;
        const float displayRadius = std::clamp(
            radius,
            0.1f,
            3.0f);
        XMFLOAT3 center{};
        XMStoreFloat3(&center, lightWorld.r[3]);
        XMFLOAT4 storedColor{};
        XMStoreFloat4(&storedColor, color);

        const auto makePosition =
            [&center, displayRadius](
                const float first,
                const float second,
                const int plane)
            {
                XMFLOAT3 position = center;
                if (plane == 0)
                {
                    position.x += first * displayRadius;
                    position.y += second * displayRadius;
                }
                else if (plane == 1)
                {
                    position.x += first * displayRadius;
                    position.z += second * displayRadius;
                }
                else
                {
                    position.y += first * displayRadius;
                    position.z += second * displayRadius;
                }
                return position;
            };

        std::vector<DebugLine> lines;
        lines.reserve(SegmentCount * 3);
        for (int plane = 0; plane < 3; ++plane)
        {
            for (std::size_t segment = 0;
                segment < SegmentCount;
                ++segment)
            {
                const float firstAngle =
                    XM_2PI
                    * static_cast<float>(segment)
                    / static_cast<float>(SegmentCount);
                const float secondAngle =
                    XM_2PI
                    * static_cast<float>(segment + 1)
                    / static_cast<float>(SegmentCount);
                lines.push_back(DebugLine{
                    makePosition(
                        std::cos(firstAngle),
                        std::sin(firstAngle),
                        plane),
                    makePosition(
                        std::cos(secondAngle),
                        std::sin(secondAngle),
                        plane),
                    storedColor });
            }
        }
        Submit(lines, view, projection);
    }

    void DebugRenderer::DrawSpotLight(
        DirectX::FXMMATRIX lightWorld,
        const float range,
        const float outerConeAngle,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        using namespace DirectX;

        constexpr std::size_t SegmentCount = 24;
        const float displayRange = std::clamp(
            range,
            0.1f,
            5.0f);
        const float coneRadius = std::tan(
            std::clamp(
                outerConeAngle,
                XMConvertToRadians(1.0f),
                XMConvertToRadians(89.0f)))
            * displayRange;

        const XMVECTOR origin = lightWorld.r[3];
        XMVECTOR direction = XMVector3Normalize(
            XMVectorNegate(lightWorld.r[2]));
        XMVECTOR right = XMVector3Normalize(lightWorld.r[0]);
        XMVECTOR up = XMVector3Normalize(lightWorld.r[1]);
        const XMVECTOR center = XMVectorMultiplyAdd(
            XMVectorReplicate(displayRange),
            direction,
            origin);

        XMFLOAT4 storedColor{};
        XMStoreFloat4(&storedColor, color);
        const auto storePosition =
            [](FXMVECTOR position)
            {
                XMFLOAT3 result{};
                XMStoreFloat3(&result, position);
                return result;
            };

        std::array<XMVECTOR, SegmentCount> ring{};
        for (std::size_t segment = 0;
            segment < SegmentCount;
            ++segment)
        {
            const float angle =
                XM_2PI
                * static_cast<float>(segment)
                / static_cast<float>(SegmentCount);
            ring[segment] = XMVectorAdd(
                center,
                XMVectorAdd(
                    XMVectorScale(
                        right,
                        std::cos(angle) * coneRadius),
                    XMVectorScale(
                        up,
                        std::sin(angle) * coneRadius)));
        }
        std::array<DebugLine, SegmentCount + 4> lines{};
        std::size_t lineIndex{};
        for (std::size_t segment = 0;
            segment < SegmentCount;
            ++segment)
        {
            lines[lineIndex++] = DebugLine{
                storePosition(ring[segment]),
                storePosition(ring[(segment + 1) % SegmentCount]),
                storedColor };
        }
        for (const std::size_t corner :
            { std::size_t{ 0 }, std::size_t{ 6 },
              std::size_t{ 12 }, std::size_t{ 18 } })
        {
            lines[lineIndex++] = DebugLine{
                storePosition(origin),
                storePosition(ring[corner]),
                storedColor };
        }
        Submit(lines, view, projection);
    }
}
