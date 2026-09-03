#include "LamaPon/Graphics/DebugRenderer.h"

#include <CommonStates.h>
#include <Effects.h>
#include <PrimitiveBatch.h>
#include <VertexTypes.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace LamaPon
{
    struct DebugRenderer::InputLayoutHolder final
    {
        Microsoft::WRL::ComPtr<ID3D11InputLayout> value;
    };

    DebugRenderer::DebugRenderer(
        ID3D11Device* device,
        ID3D11DeviceContext* context)
        : m_context(context)
        , m_effect(std::make_unique<DirectX::BasicEffect>(device))
        , m_states(std::make_unique<DirectX::CommonStates>(device))
        , m_batch(std::make_unique<
            DirectX::PrimitiveBatch<DirectX::VertexPositionColor>>(context))
        , m_inputLayout(std::make_unique<InputLayoutHolder>())
    {
        m_effect->SetVertexColorEnabled(true);

        const void* shaderByteCode{};
        std::size_t byteCodeLength{};
        m_effect->GetVertexShaderBytecode(&shaderByteCode, &byteCodeLength);

        const HRESULT result = device->CreateInputLayout(
            DirectX::VertexPositionColor::InputElements,
            DirectX::VertexPositionColor::InputElementCount,
            shaderByteCode,
            byteCodeLength,
            m_inputLayout->value.ReleaseAndGetAddressOf());
        if (FAILED(result))
        {
            throw std::runtime_error("Failed to create debug renderer input layout.");
        }
    }

    DebugRenderer::~DebugRenderer() = default;

    void DebugRenderer::Prepare(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        // 直前に走ったパス（2D/UIのシザー矩形付きラスタライザー等）の
        // 状態を引き継ぐと補助線が描画されないことがあるため、
        // 必要なステートを毎回明示的に設定します。深度テストは無効にし、
        // グリッドやコライダー枠が地面や自分の形状表面と同じ深度で
        // Zファイティングして消えないよう、常に手前へ描きます。
        m_context->OMSetBlendState(
            m_states->NonPremultiplied(),
            nullptr,
            0xFFFFFFFF);
        m_context->OMSetDepthStencilState(
            m_states->DepthNone(),
            0);
        m_context->RSSetState(m_states->CullNone());

        m_effect->SetWorld(DirectX::XMMatrixIdentity());
        m_effect->SetView(view);
        m_effect->SetProjection(projection);
        m_effect->Apply(m_context);
        m_context->IASetInputLayout(m_inputLayout->value.Get());
    }

    void DebugRenderer::DrawBounds(
        const Bounds2D& bounds,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        Prepare(view, projection);

        const DirectX::VertexPositionColor bottomLeft{
            { bounds.minimum.x, bounds.minimum.y, 0.0f },
            color
        };
        const DirectX::VertexPositionColor bottomRight{
            { bounds.maximum.x, bounds.minimum.y, 0.0f },
            color
        };
        const DirectX::VertexPositionColor topRight{
            { bounds.maximum.x, bounds.maximum.y, 0.0f },
            color
        };
        const DirectX::VertexPositionColor topLeft{
            { bounds.minimum.x, bounds.maximum.y, 0.0f },
            color
        };

        m_batch->Begin();
        m_batch->DrawLine(bottomLeft, bottomRight);
        m_batch->DrawLine(bottomRight, topRight);
        m_batch->DrawLine(topRight, topLeft);
        m_batch->DrawLine(topLeft, bottomLeft);
        m_batch->End();
    }

    void DebugRenderer::DrawBounds(
        const Bounds3D& bounds,
        DirectX::FXMVECTOR color,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        Prepare(view, projection);

        const std::array<DirectX::VertexPositionColor, 8> vertices{
            DirectX::VertexPositionColor{
                { bounds.minimum.x, bounds.minimum.y, bounds.minimum.z }, color },
            DirectX::VertexPositionColor{
                { bounds.maximum.x, bounds.minimum.y, bounds.minimum.z }, color },
            DirectX::VertexPositionColor{
                { bounds.maximum.x, bounds.maximum.y, bounds.minimum.z }, color },
            DirectX::VertexPositionColor{
                { bounds.minimum.x, bounds.maximum.y, bounds.minimum.z }, color },
            DirectX::VertexPositionColor{
                { bounds.minimum.x, bounds.minimum.y, bounds.maximum.z }, color },
            DirectX::VertexPositionColor{
                { bounds.maximum.x, bounds.minimum.y, bounds.maximum.z }, color },
            DirectX::VertexPositionColor{
                { bounds.maximum.x, bounds.maximum.y, bounds.maximum.z }, color },
            DirectX::VertexPositionColor{
                { bounds.minimum.x, bounds.maximum.y, bounds.maximum.z }, color }
        };
        constexpr std::array edges{
            std::pair{ 0, 1 }, std::pair{ 1, 2 },
            std::pair{ 2, 3 }, std::pair{ 3, 0 },
            std::pair{ 4, 5 }, std::pair{ 5, 6 },
            std::pair{ 6, 7 }, std::pair{ 7, 4 },
            std::pair{ 0, 4 }, std::pair{ 1, 5 },
            std::pair{ 2, 6 }, std::pair{ 3, 7 }
        };

        m_batch->Begin();
        for (const auto [start, end] : edges)
        {
            m_batch->DrawLine(vertices[start], vertices[end]);
        }
        m_batch->End();
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

        Prepare(view, projection);

        const int lineCount = std::clamp(
            static_cast<int>(std::ceil(halfExtent / spacing)),
            1,
            200);
        const float extent = spacing * static_cast<float>(lineCount);
        const auto minorColor =
            DirectX::XMVectorSet(0.22f, 0.25f, 0.30f, 0.48f);
        const auto majorColor =
            DirectX::XMVectorSet(0.34f, 0.38f, 0.45f, 0.65f);
        const auto xAxisColor =
            DirectX::XMVectorSet(0.85f, 0.20f, 0.20f, 0.90f);
        const auto zAxisColor =
            DirectX::XMVectorSet(0.20f, 0.42f, 0.90f, 0.90f);

        m_batch->Begin();
        for (int index = -lineCount; index <= lineCount; ++index)
        {
            const float coordinate = spacing * static_cast<float>(index);
            const auto xLineColor = index == 0
                ? zAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);
            const auto zLineColor = index == 0
                ? xAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);

            m_batch->DrawLine(
                DirectX::VertexPositionColor{
                    { -extent, 0.0f, coordinate },
                    xLineColor
                },
                DirectX::VertexPositionColor{
                    { extent, 0.0f, coordinate },
                    xLineColor
                });
            m_batch->DrawLine(
                DirectX::VertexPositionColor{
                    { coordinate, 0.0f, -extent },
                    zLineColor
                },
                DirectX::VertexPositionColor{
                    { coordinate, 0.0f, extent },
                    zLineColor
                });
        }
        m_batch->End();
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

        Prepare(view, projection);

        const int lineCount = std::clamp(
            static_cast<int>(std::ceil(halfExtent / spacing)),
            1,
            200);
        const float extent = spacing * static_cast<float>(lineCount);
        const auto minorColor =
            DirectX::XMVectorSet(0.22f, 0.25f, 0.30f, 0.48f);
        const auto majorColor =
            DirectX::XMVectorSet(0.34f, 0.38f, 0.45f, 0.65f);
        const auto xAxisColor =
            DirectX::XMVectorSet(0.85f, 0.20f, 0.20f, 0.90f);
        const auto yAxisColor =
            DirectX::XMVectorSet(0.20f, 0.78f, 0.28f, 0.90f);

        m_batch->Begin();
        for (int index = -lineCount; index <= lineCount; ++index)
        {
            const float coordinate = spacing * static_cast<float>(index);
            const auto horizontalColor = index == 0
                ? xAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);
            const auto verticalColor = index == 0
                ? yAxisColor
                : (index % 5 == 0 ? majorColor : minorColor);

            m_batch->DrawLine(
                DirectX::VertexPositionColor{
                    { -extent, coordinate, 0.0f },
                    horizontalColor
                },
                DirectX::VertexPositionColor{
                    { extent, coordinate, 0.0f },
                    horizontalColor
                });
            m_batch->DrawLine(
                DirectX::VertexPositionColor{
                    { coordinate, -extent, 0.0f },
                    verticalColor
                },
                DirectX::VertexPositionColor{
                    { coordinate, extent, 0.0f },
                    verticalColor
                });
        }
        m_batch->End();
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

        Prepare(view, projection);
        m_batch->Begin();
        for (std::size_t index = 1;
            index < points.size();
            index += 2)
        {
            m_batch->DrawLine(
                DirectX::VertexPositionColor(
                    DirectX::XMLoadFloat3(
                        &points[index - 1]),
                    color),
                DirectX::VertexPositionColor(
                    DirectX::XMLoadFloat3(
                        &points[index]),
                    color));
        }
        m_batch->End();
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

        std::array<DirectX::VertexPositionColor, 8> vertices{};
        for (std::size_t index = 0; index < localCorners.size(); ++index)
        {
            DirectX::XMStoreFloat3(
                &vertices[index].position,
                DirectX::XMVector3Transform(
                    DirectX::XMLoadFloat3(&localCorners[index]),
                    cameraWorld));
            vertices[index].color = storedColor;
        }

        DirectX::XMFLOAT3 origin{};
        DirectX::XMStoreFloat3(
            &origin,
            DirectX::XMVector3Transform(
                DirectX::XMVectorZero(),
                cameraWorld));
        const DirectX::VertexPositionColor cameraOrigin{
            origin,
            storedColor
        };

        constexpr std::array edges{
            std::pair{ 0, 1 }, std::pair{ 1, 2 },
            std::pair{ 2, 3 }, std::pair{ 3, 0 },
            std::pair{ 4, 5 }, std::pair{ 5, 6 },
            std::pair{ 6, 7 }, std::pair{ 7, 4 },
            std::pair{ 0, 4 }, std::pair{ 1, 5 },
            std::pair{ 2, 6 }, std::pair{ 3, 7 }
        };

        Prepare(view, projection);
        m_batch->Begin();
        for (const auto [start, end] : edges)
        {
            m_batch->DrawLine(vertices[start], vertices[end]);
        }
        for (std::size_t corner = 4; corner < vertices.size(); ++corner)
        {
            m_batch->DrawLine(cameraOrigin, vertices[corner]);
        }
        m_batch->End();
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
        const auto vertex =
            [&storedColor](FXMVECTOR position)
            {
                VertexPositionColor result{};
                XMStoreFloat3(&result.position, position);
                result.color = storedColor;
                return result;
            };

        Prepare(view, projection);
        m_batch->Begin();
        m_batch->DrawLine(vertex(origin), vertex(end));
        m_batch->DrawLine(
            vertex(end),
            vertex(XMVectorMultiplyAdd(
                XMVectorReplicate(0.22f),
                right,
                arrowBase)));
        m_batch->DrawLine(
            vertex(end),
            vertex(XMVectorMultiplyAdd(
                XMVectorReplicate(-0.22f),
                right,
                arrowBase)));
        m_batch->DrawLine(
            vertex(end),
            vertex(XMVectorMultiplyAdd(
                XMVectorReplicate(0.22f),
                up,
                arrowBase)));
        m_batch->DrawLine(
            vertex(end),
            vertex(XMVectorMultiplyAdd(
                XMVectorReplicate(-0.22f),
                up,
                arrowBase)));
        m_batch->End();
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

        const auto makeVertex =
            [&center, &storedColor, displayRadius](
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
                return VertexPositionColor{ position, storedColor };
            };

        Prepare(view, projection);
        m_batch->Begin();
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
                m_batch->DrawLine(
                    makeVertex(
                        std::cos(firstAngle),
                        std::sin(firstAngle),
                        plane),
                    makeVertex(
                        std::cos(secondAngle),
                        std::sin(secondAngle),
                        plane));
            }
        }
        m_batch->End();
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
        const auto vertex =
            [&storedColor](FXMVECTOR position)
            {
                VertexPositionColor result{};
                XMStoreFloat3(&result.position, position);
                result.color = storedColor;
                return result;
            };

        Prepare(view, projection);
        m_batch->Begin();
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
        for (std::size_t segment = 0;
            segment < SegmentCount;
            ++segment)
        {
            m_batch->DrawLine(
                vertex(ring[segment]),
                vertex(ring[(segment + 1) % SegmentCount]));
        }
        for (const std::size_t corner :
            { std::size_t{ 0 }, std::size_t{ 6 },
              std::size_t{ 12 }, std::size_t{ 18 } })
        {
            m_batch->DrawLine(
                vertex(origin),
                vertex(ring[corner]));
        }
        m_batch->End();
    }
}
