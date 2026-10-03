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
        // 転送するビュー行列
        DirectX::XMFLOAT4X4 storedView{};
        // 転送する射影行列
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
        // 線分へ設定する色
        DirectX::XMFLOAT4 storedColor{};
        DirectX::XMStoreFloat4(&storedColor, color);
        // 矩形の左下座標
        const DirectX::XMFLOAT3 bottomLeft{
            bounds.minimum.x,
            bounds.minimum.y,
            0.0f };
        // 矩形の右下座標
        const DirectX::XMFLOAT3 bottomRight{
            bounds.maximum.x,
            bounds.minimum.y,
            0.0f };
        // 矩形の右上座標
        const DirectX::XMFLOAT3 topRight{
            bounds.maximum.x,
            bounds.maximum.y,
            0.0f };
        // 矩形の左上座標
        const DirectX::XMFLOAT3 topLeft{
            bounds.minimum.x,
            bounds.maximum.y,
            0.0f };
        // 描画する線分列
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
        // 線分へ設定する色
        DirectX::XMFLOAT4 storedColor{};
        DirectX::XMStoreFloat4(&storedColor, color);
        // 境界・視錐台の頂点座標
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
        // 境界の各辺の頂点番号の組
        constexpr std::array edges{
            std::pair{ 0, 1 }, std::pair{ 1, 2 },
            std::pair{ 2, 3 }, std::pair{ 3, 0 },
            std::pair{ 4, 5 }, std::pair{ 5, 6 },
            std::pair{ 6, 7 }, std::pair{ 7, 4 },
            std::pair{ 0, 4 }, std::pair{ 1, 5 },
            std::pair{ 2, 6 }, std::pair{ 3, 7 }
        };

        // 境界の辺を描く線分列
        std::array<DebugLine, edges.size()> lines{};
        // 線分や頂点の要素番号
        for (std::size_t index = 0;
            index < edges.size();
            ++index)
        {
            // start: 辺の始点番号、end: 辺の終点番号
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

        // 原点から片側の格子線数
        const int lineCount = std::clamp(
            static_cast<int>(std::ceil(halfExtent / spacing)),
            1,
            200);
        // 格子の片側の描画範囲
        const float extent = spacing * static_cast<float>(lineCount);
        // 通常の格子線の色
        constexpr DirectX::XMFLOAT4 minorColor{
            0.22f, 0.25f, 0.30f, 0.48f };
        // 五本ごとの格子線の色
        constexpr DirectX::XMFLOAT4 majorColor{
            0.34f, 0.38f, 0.45f, 0.65f };
        // X軸の線の色
        constexpr DirectX::XMFLOAT4 xAxisColor{
            0.85f, 0.20f, 0.20f, 0.90f };
        // Z軸の線の色
        constexpr DirectX::XMFLOAT4 zAxisColor{
            0.20f, 0.42f, 0.90f, 0.90f };

        // 描画する線分列
        std::vector<DebugLine> lines;
        lines.reserve(static_cast<std::size_t>(
            (lineCount * 2 + 1) * 2));
        // 線分や頂点の要素番号
        for (int index = -lineCount; index <= lineCount; ++index)
        {
            // 各格子線の座標
            const float coordinate = spacing * static_cast<float>(index);
            // X方向の格子線の色
            const auto xLineColor = index == 0
                ? zAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);
            // Z方向の格子線の色
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

        // 原点から片側の格子線数
        const int lineCount = std::clamp(
            static_cast<int>(std::ceil(halfExtent / spacing)),
            1,
            200);
        // 格子の片側の描画範囲
        const float extent = spacing * static_cast<float>(lineCount);
        // 通常の格子線の色
        constexpr DirectX::XMFLOAT4 minorColor{
            0.22f, 0.25f, 0.30f, 0.48f };
        // 五本ごとの格子線の色
        constexpr DirectX::XMFLOAT4 majorColor{
            0.34f, 0.38f, 0.45f, 0.65f };
        // X軸の線の色
        constexpr DirectX::XMFLOAT4 xAxisColor{
            0.85f, 0.20f, 0.20f, 0.90f };
        // Y軸の線の色
        constexpr DirectX::XMFLOAT4 yAxisColor{
            0.20f, 0.78f, 0.28f, 0.90f };

        // 描画する線分列
        std::vector<DebugLine> lines;
        lines.reserve(static_cast<std::size_t>(
            (lineCount * 2 + 1) * 2));
        // 線分や頂点の要素番号
        for (int index = -lineCount; index <= lineCount; ++index)
        {
            // 各格子線の座標
            const float coordinate = spacing * static_cast<float>(index);
            // 横方向の格子線の色
            const auto horizontalColor = index == 0
                ? xAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);
            // 縦方向の格子線の色
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

        // 線分へ設定する色
        DirectX::XMFLOAT4 storedColor{};
        DirectX::XMStoreFloat4(&storedColor, color);
        // 描画する線分列
        std::vector<DebugLine> lines;
        lines.reserve(points.size() / 2);
        // 線分や頂点の要素番号
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
        // 補正した近面の距離
        const float safeNear = std::max(nearDistance, 0.01f);
        // 補正した遠面の距離
        const float safeFar = std::max(farDistance, safeNear + 0.01f);
        // 補正した画面の縦横比
        const float safeAspect = std::max(aspectRatio, 0.01f);
        // 縦視野角の半角の正接
        const float tangent = std::tan(verticalFieldOfView * 0.5f);
        // 近面の半分の高さ
        const float nearHalfHeight = tangent * safeNear;
        // 近面の半分の幅
        const float nearHalfWidth = nearHalfHeight * safeAspect;
        // 遠面の半分の高さ
        const float farHalfHeight = tangent * safeFar;
        // 遠面の半分の幅
        const float farHalfWidth = farHalfHeight * safeAspect;
        // 線分へ設定する色
        DirectX::XMFLOAT4 storedColor{};
        DirectX::XMStoreFloat4(&storedColor, color);

        // 視錐台のローカル頂点
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

        // 境界・視錐台の頂点座標
        std::array<DirectX::XMFLOAT3, 8> vertices{};
        // 線分や頂点の要素番号
        for (std::size_t index = 0; index < localCorners.size(); ++index)
        {
            DirectX::XMStoreFloat3(
                &vertices[index],
                DirectX::XMVector3Transform(
                    DirectX::XMLoadFloat3(&localCorners[index]),
                    cameraWorld));
        }

        // カメラまたは光源の原点
        DirectX::XMFLOAT3 origin{};
        DirectX::XMStoreFloat3(
            &origin,
            DirectX::XMVector3Transform(
                DirectX::XMVectorZero(),
                cameraWorld));

        // 境界の各辺の頂点番号の組
        constexpr std::array edges{
            std::pair{ 0, 1 }, std::pair{ 1, 2 },
            std::pair{ 2, 3 }, std::pair{ 3, 0 },
            std::pair{ 4, 5 }, std::pair{ 5, 6 },
            std::pair{ 6, 7 }, std::pair{ 7, 4 },
            std::pair{ 0, 4 }, std::pair{ 1, 5 },
            std::pair{ 2, 6 }, std::pair{ 3, 7 }
        };

        // 視錐台の辺と遠面への線分列
        std::array<DebugLine, edges.size() + 4> lines{};
        // 次に書き込む線分番号
        std::size_t lineIndex{};
        // start: 辺の始点番号、end: 辺の終点番号
        for (const auto [start, end] : edges)
        {
            lines[lineIndex++] = DebugLine{
                vertices[start],
                vertices[end],
                storedColor };
        }
        // 遠面または円周の頂点番号
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

        // カメラまたは光源の原点
        const XMVECTOR origin = lightWorld.r[3];
        // 光が進む単位方向
        XMVECTOR direction = XMVectorNegate(lightWorld.r[2]);
        if (XMVectorGetX(XMVector3LengthSq(direction)) <= 0.000001f)
        {
            direction = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
        }
        direction = XMVector3Normalize(direction);

        // 光源の横向きの単位方向
        XMVECTOR right = lightWorld.r[0];
        if (XMVectorGetX(XMVector3LengthSq(right)) <= 0.000001f)
        {
            right = g_XMIdentityR0;
        }
        right = XMVector3Normalize(right);

        // 光源の上向きの単位方向
        XMVECTOR up = lightWorld.r[1];
        if (XMVectorGetX(XMVector3LengthSq(up)) <= 0.000001f)
        {
            up = g_XMIdentityR1;
        }
        up = XMVector3Normalize(up);

        // 平行光源の矢印の先端
        const XMVECTOR end = XMVectorMultiplyAdd(
            XMVectorReplicate(2.0f),
            direction,
            origin);
        // 矢印の羽根の基点
        const XMVECTOR arrowBase = XMVectorMultiplyAdd(
            XMVectorReplicate(-0.42f),
            direction,
            end);

        // 線分へ設定する色
        XMFLOAT4 storedColor{};
        XMStoreFloat4(&storedColor, color);
        // ベクトルを三成分座標へ保存する(position: 保存する座標)。
        const auto storePosition =
            [](FXMVECTOR position)
            {
                // 三成分へ保存した座標
                XMFLOAT3 result{};
                XMStoreFloat3(&result, position);
                return result;
            };

        // 矢印の先端座標
        const XMFLOAT3 storedEnd = storePosition(end);
        // 描画する線分列
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

        // 円周の線分数
        constexpr std::size_t SegmentCount = 24;
        // 表示用に制限した光源半径
        const float displayRadius = std::clamp(
            radius,
            0.1f,
            3.0f);
        // 球または円錐底面の中心
        XMFLOAT3 center{};
        XMStoreFloat3(&center, lightWorld.r[3]);
        // 線分へ設定する色
        XMFLOAT4 storedColor{};
        XMStoreFloat4(&storedColor, color);

        // 指定平面の円周点を求める(first: 第一軸の単位座標, second: 第二軸の単位座標, plane: XY・XZ・YZの番号)。
        const auto makePosition =
            [&center, displayRadius](
                const float first,
                const float second,
                const int plane)
            {
                // 円周点のワールド座標
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

        // 描画する線分列
        std::vector<DebugLine> lines;
        lines.reserve(SegmentCount * 3);
        // 描画する円の平面番号
        for (int plane = 0; plane < 3; ++plane)
        {
            // 描画する円周の線分番号
            for (std::size_t segment = 0;
                // 円周の線分数
                segment < SegmentCount;
                ++segment)
            {
                // 円周線分の始点角、ラジアン
                const float firstAngle =
                    XM_2PI
                    * static_cast<float>(segment)
                    / static_cast<float>(SegmentCount);
                // 円周線分の終点角、ラジアン
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

        // 円周の線分数
        constexpr std::size_t SegmentCount = 24;
        // 表示用に制限した光源距離
        const float displayRange = std::clamp(
            range,
            0.1f,
            5.0f);
        // 円錐底面の半径
        const float coneRadius = std::tan(
            std::clamp(
                outerConeAngle,
                XMConvertToRadians(1.0f),
                XMConvertToRadians(89.0f)))
            * displayRange;

        // カメラまたは光源の原点
        const XMVECTOR origin = lightWorld.r[3];
        // 光が進む単位方向
        XMVECTOR direction = XMVector3Normalize(
            XMVectorNegate(lightWorld.r[2]));
        // 光源の横向きの単位方向
        XMVECTOR right = XMVector3Normalize(lightWorld.r[0]);
        // 光源の上向きの単位方向
        XMVECTOR up = XMVector3Normalize(lightWorld.r[1]);
        // 球または円錐底面の中心
        const XMVECTOR center = XMVectorMultiplyAdd(
            XMVectorReplicate(displayRange),
            direction,
            origin);

        // 線分へ設定する色
        XMFLOAT4 storedColor{};
        XMStoreFloat4(&storedColor, color);
        // ベクトルを三成分座標へ保存する(position: 保存する座標)。
        const auto storePosition =
            [](FXMVECTOR position)
            {
                // 三成分へ保存した座標
                XMFLOAT3 result{};
                XMStoreFloat3(&result, position);
                return result;
            };

        // 円錐底面の円周点列
        std::array<XMVECTOR, SegmentCount> ring{};
        // 描画する円周の線分番号
        for (std::size_t segment = 0;
            // 円周の線分数
            segment < SegmentCount;
            ++segment)
        {
            // 円周点の角度、ラジアン
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
        // 円周と光源へ戻る円錐の線分列
        std::array<DebugLine, SegmentCount + 4> lines{};
        // 次に書き込む線分番号
        std::size_t lineIndex{};
        // 描画する円周の線分番号
        for (std::size_t segment = 0;
            // 円周の線分数
            segment < SegmentCount;
            ++segment)
        {
            lines[lineIndex++] = DebugLine{
                storePosition(ring[segment]),
                storePosition(ring[(segment + 1) % SegmentCount]),
                storedColor };
        }
        // 遠面または円周の頂点番号
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
