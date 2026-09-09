#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <memory>
#include <span>

namespace LamaPon
{
    struct DebugLine final
    {
        DirectX::XMFLOAT3 start{};
        DirectX::XMFLOAT3 end{};
        DirectX::XMFLOAT4 color{};
    };

    // 形状から生成済みの線分を実効描画APIへ送る同期sinkです。
    // spanと行列は呼び出し中だけ有効で、Backend側は保持しません。
    class DebugDrawingBackend
    {
    public:
        virtual ~DebugDrawingBackend() = default;

        DebugDrawingBackend() = default;
        DebugDrawingBackend(const DebugDrawingBackend&) = delete;
        DebugDrawingBackend& operator=(
            const DebugDrawingBackend&) = delete;

        virtual void DrawLines(
            std::span<const DebugLine> lines,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection) = 0;
    };

    class DebugRenderer final
    {
    public:
        explicit DebugRenderer(
            std::unique_ptr<DebugDrawingBackend> backend);
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
        void Submit(
            std::span<const DebugLine> lines,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);

        std::unique_ptr<DebugDrawingBackend> m_backend;
    };
}
