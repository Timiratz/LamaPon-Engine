#include "LamaPon/Components/NavMeshAgentComponent.h"

#include "LamaPon/Components/NavMeshComponent.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace LamaPon
{
    NavMeshAgentComponent::
        NavMeshAgentComponent(
            const float speed,
            const float stoppingDistance,
            const bool rotateToPath) noexcept
        : m_speed(speed)
        , m_stoppingDistance(
            stoppingDistance)
        , m_rotateToPath(rotateToPath)
    {
        SetSpeed(speed);
        SetStoppingDistance(
            stoppingDistance);
    }

    void NavMeshAgentComponent::SetSpeed(
        const float speed) noexcept
    {
        m_speed = std::clamp(
            std::abs(speed),
            0.0f,
            1000.0f);
    }

    void NavMeshAgentComponent::
        SetStoppingDistance(
            const float distance) noexcept
    {
        m_stoppingDistance =
            std::clamp(
                std::abs(distance),
                0.0f,
                100.0f);
    }

    bool NavMeshAgentComponent::
        SetDestination(
            const DirectX::XMFLOAT3&
                destination,
            const NavMeshComponent& navMesh)
    {
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        const DirectX::XMFLOAT3 start{
            world._41,
            world._42,
            world._43
        };
        auto path = navMesh.FindPath(
            start,
            destination);
        SetPath(destination, path);
        return !path.empty();
    }

    bool NavMeshAgentComponent::SetDestination(
        const DirectX::XMFLOAT3& destination)
    {
        const auto* navMesh =
            FindBestNavMesh(destination);
        if (navMesh == nullptr)
        {
            Stop();
            return false;
        }
        return SetDestination(destination, *navMesh);
    }

    const NavMeshComponent*
        NavMeshAgentComponent::FindBestNavMesh(
            const DirectX::XMFLOAT3& destination) const
    {
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        const DirectX::XMFLOAT3 position{
            world._41,
            world._42,
            world._43
        };

        const NavMeshComponent* best{};
        int bestScore = -1;
        float bestHeightDistance =
            std::numeric_limits<float>::max();
        for (const auto* candidate :
            Owner().GetScene().FindComponentsOfType<
                NavMeshComponent>())
        {
            if (!candidate->IsBaked())
            {
                continue;
            }
            // 現在地を含むサーフェスを最優先、次に目的地。
            int score = 0;
            if (candidate->ContainsPoint(position))
            {
                score += 2;
            }
            if (candidate->ContainsPoint(destination))
            {
                score += 1;
            }
            const float heightDistance = std::abs(
                candidate->SurfaceHeight()
                - position.y);
            if (score > bestScore
                || (score == bestScore
                    && heightDistance
                        < bestHeightDistance))
            {
                best = candidate;
                bestScore = score;
                bestHeightDistance = heightDistance;
            }
        }
        return best;
    }

    void NavMeshAgentComponent::SetPath(
        const DirectX::XMFLOAT3&
            destination,
        const std::span<
            const DirectX::XMFLOAT3> path)
    {
        m_destination = destination;
        m_path.assign(
            path.begin(),
            path.end());
        m_currentWaypoint =
            m_path.size() > 1
            ? 1
            : m_path.size();
        m_arrived = m_path.empty();
    }

    void NavMeshAgentComponent::Stop() noexcept
    {
        m_path.clear();
        m_currentWaypoint = 0;
        m_arrived = true;
    }

    void NavMeshAgentComponent::OnUpdate(
        const float deltaTime)
    {
        if (!HasPath()
            || deltaTime <= 0.0f)
        {
            return;
        }

        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        const auto& target =
            m_path[m_currentWaypoint];
        const float deltaX =
            target.x - world._41;
        const float deltaZ =
            target.z - world._43;
        const float distance =
            std::sqrt(
                deltaX * deltaX
                + deltaZ * deltaZ);
        const float threshold =
            m_currentWaypoint + 1
                    == m_path.size()
                ? m_stoppingDistance
                : std::min(
                    m_stoppingDistance,
                    0.05f);
        if (distance <= threshold)
        {
            ++m_currentWaypoint;
            if (!HasPath())
            {
                m_arrived = true;
            }
            return;
        }

        const float travel =
            std::min(
                m_speed * deltaTime,
                distance);
        const float inverseDistance =
            1.0f / distance;
        Owner().TranslateWorld({
            deltaX * inverseDistance
                * travel,
            0.0f,
            deltaZ * inverseDistance
                * travel
        });
        if (m_rotateToPath)
        {
            // 進行方向へ向けます（ヨーだけの絶対指定なので、
            // ピッチとロールは0になります）。
            Owner().GetTransform().SetEulerAngles(
                0.0f,
                std::atan2(deltaX, deltaZ),
                0.0f);
        }
    }

    void NavMeshAgentComponent::
        OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection)
    {
        if (m_path.size() < 2)
        {
            return;
        }

        std::vector<DirectX::XMFLOAT3>
            lines;
        lines.reserve(
            (m_path.size() - 1) * 2);
        for (std::size_t index = 1;
            index < m_path.size();
            ++index)
        {
            auto start = m_path[index - 1];
            auto end = m_path[index];
            start.y += 0.12f;
            end.y += 0.12f;
            lines.push_back(start);
            lines.push_back(end);
        }
        graphics.Debug().DrawLines(
            lines,
            DirectX::XMVectorSet(
                1.0f,
                0.82f,
                0.08f,
                1.0f),
            view,
            projection);
    }
}
