#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstddef>
#include <span>
#include <vector>

namespace LamaPon
{
    class NavMeshComponent;

    class NavMeshAgentComponent final : public Component
    {
    public:
        explicit NavMeshAgentComponent(
            float speed = 3.0f,
            float stoppingDistance = 0.1f,
            bool rotateToPath = true) noexcept;

        void SetSpeed(float speed) noexcept;
        void SetStoppingDistance(
            float distance) noexcept;
        void SetRotateToPath(
            bool enabled) noexcept
        {
            m_rotateToPath = enabled;
        }

        [[nodiscard]] float Speed() const noexcept
        {
            return m_speed;
        }
        [[nodiscard]] float
            StoppingDistance() const noexcept
        {
            return m_stoppingDistance;
        }
        [[nodiscard]] bool
            RotateToPath() const noexcept
        {
            return m_rotateToPath;
        }
        [[nodiscard]] const DirectX::XMFLOAT3&
            Destination() const noexcept
        {
            return m_destination;
        }
        [[nodiscard]] const std::vector<
            DirectX::XMFLOAT3>&
            Path() const noexcept
        {
            return m_path;
        }
        [[nodiscard]] bool
            HasPath() const noexcept
        {
            return m_currentWaypoint
                < m_path.size();
        }
        [[nodiscard]] bool
            HasArrived() const noexcept
        {
            return m_arrived;
        }

        bool SetDestination(
            const DirectX::XMFLOAT3&
                destination,
            const NavMeshComponent& navMesh);
        // シーン内のNavMeshサーフェスから最適なものを自動選択して
        // 経路探索します（複数フロア対応）。エージェントの現在地を
        // 含み、高さが最も近いサーフェスを優先します。
        bool SetDestination(
            const DirectX::XMFLOAT3& destination);
        [[nodiscard]] const NavMeshComponent*
            FindBestNavMesh(
                const DirectX::XMFLOAT3&
                    destination) const;
        void SetPath(
            const DirectX::XMFLOAT3&
                destination,
            std::span<
                const DirectX::XMFLOAT3>
                    path);
        void Stop() noexcept;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "NavMeshAgent";
        }

    protected:
        void OnUpdate(float deltaTime) override;
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX
                projection) override;

    private:
        float m_speed;
        float m_stoppingDistance;
        bool m_rotateToPath;
        DirectX::XMFLOAT3 m_destination{};
        std::vector<DirectX::XMFLOAT3> m_path;
        std::size_t m_currentWaypoint{};
        bool m_arrived{ true };
    };
}
