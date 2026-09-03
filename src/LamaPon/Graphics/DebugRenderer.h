#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <memory>
#include <span>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11InputLayout;

namespace DirectX
{
    inline namespace DX11
    {
        class BasicEffect;
        class CommonStates;
        template<typename TVertex>
        class PrimitiveBatch;
        struct VertexPositionColor;
    }
}

namespace LamaPon
{
    class DebugRenderer final
    {
    public:
        DebugRenderer(ID3D11Device* device, ID3D11DeviceContext* context);
        ~DebugRenderer();

        DebugRenderer(const DebugRenderer&) = delete;
        DebugRenderer& operator=(const DebugRenderer&) = delete;

        void DrawBounds(
            const Bounds2D& bounds,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void DrawBounds(
            const Bounds3D& bounds,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void DrawGridXZ(
            float spacing,
            float halfExtent,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void DrawGridXY(
            float spacing,
            float halfExtent,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void DrawLines(
            std::span<
                const DirectX::XMFLOAT3> points,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void DrawDirectionalLight(
            DirectX::FXMMATRIX lightWorld,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void DrawPointLight(
            DirectX::FXMMATRIX lightWorld,
            float radius,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void DrawSpotLight(
            DirectX::FXMMATRIX lightWorld,
            float range,
            float outerConeAngle,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        void DrawFrustum(
            DirectX::FXMMATRIX cameraWorld,
            float verticalFieldOfView,
            float aspectRatio,
            float nearDistance,
            float farDistance,
            DirectX::FXMVECTOR color,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);

    private:
        void Prepare(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);

        ID3D11DeviceContext* m_context{};
        std::unique_ptr<DirectX::BasicEffect> m_effect;
        std::unique_ptr<DirectX::CommonStates> m_states;
        std::unique_ptr<DirectX::PrimitiveBatch<DirectX::VertexPositionColor>> m_batch;
        struct InputLayoutHolder;
        std::unique_ptr<InputLayoutHolder> m_inputLayout;
    };
}
