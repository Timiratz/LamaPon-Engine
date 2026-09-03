#pragma once

#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace LamaPon
{
    class NavMeshComponent final : public Component
    {
    public:
        using CellCoordinate = std::pair<
            std::uint32_t,
            std::uint32_t>;

        explicit NavMeshComponent(
            DirectX::XMFLOAT2 surfaceSize =
                { 20.0f, 20.0f },
            float cellSize = 1.0f,
            float agentRadius = 0.4f,
            float agentHeight = 1.8f) noexcept;

        void SetSurfaceSize(
            const DirectX::XMFLOAT2& size) noexcept;
        void SetCellSize(float size) noexcept;
        void SetAgentRadius(float radius) noexcept;
        void SetAgentHeight(float height) noexcept;

        [[nodiscard]] const DirectX::XMFLOAT2&
            SurfaceSize() const noexcept
        {
            return m_surfaceSize;
        }
        [[nodiscard]] float
            CellSize() const noexcept
        {
            return m_cellSize;
        }
        [[nodiscard]] float
            AgentRadius() const noexcept
        {
            return m_agentRadius;
        }
        [[nodiscard]] float
            AgentHeight() const noexcept
        {
            return m_agentHeight;
        }
        [[nodiscard]] std::uint32_t
            GridWidth() const noexcept;
        [[nodiscard]] std::uint32_t
            GridDepth() const noexcept;
        [[nodiscard]] bool
            IsBaked() const noexcept
        {
            return !m_blocked.empty();
        }
        [[nodiscard]] std::size_t
            BlockedCellCount() const noexcept;
        [[nodiscard]] bool IsBlocked(
            std::uint32_t x,
            std::uint32_t z) const noexcept;
        [[nodiscard]] const std::vector<
            std::uint8_t>&
            BakedCells() const noexcept
        {
            return m_blocked;
        }

        void Bake(
            std::span<const Bounds3D>
                obstacles);
        void ClearBake() noexcept
        {
            m_blocked.clear();
        }
        void RestoreBake(
            std::span<
                const CellCoordinate>
                    blockedCells);

        [[nodiscard]] std::vector<
            DirectX::XMFLOAT3> FindPath(
                const DirectX::XMFLOAT3&
                    start,
                const DirectX::XMFLOAT3&
                    destination) const;
        [[nodiscard]] DirectX::XMFLOAT3
            CellCenter(
                std::uint32_t x,
                std::uint32_t z) const noexcept;
        // XZがサーフェス範囲内ならtrue（ベイク状態は不問）。
        [[nodiscard]] bool ContainsPoint(
            const DirectX::XMFLOAT3& point)
            const noexcept;
        // サーフェスのワールドY（オーナーのY位置）。
        [[nodiscard]] float
            SurfaceHeight() const noexcept;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "NavMesh";
        }

    protected:
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX
                projection) override;

    private:
        [[nodiscard]] bool WorldToCell(
            const DirectX::XMFLOAT3&
                point,
            int& x,
            int& z) const noexcept;
        [[nodiscard]] CellCoordinate
            NearestWalkable(
                int x,
                int z) const;
        // 2セル中心間の線分が通行可能セルだけを通るならtrue
        // （経路平滑化用）。
        [[nodiscard]] bool HasLineOfSight(
            const CellCoordinate& from,
            const CellCoordinate& to) const noexcept;
        [[nodiscard]] std::size_t
            CellIndex(
                std::uint32_t x,
                std::uint32_t z) const noexcept
        {
            return static_cast<std::size_t>(z)
                    * GridWidth()
                + x;
        }

        DirectX::XMFLOAT2 m_surfaceSize;
        float m_cellSize;
        float m_agentRadius;
        float m_agentHeight;
        std::vector<std::uint8_t> m_blocked;
    };
}
