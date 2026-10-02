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
        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // 探索を始めるワールド位置
        const DirectX::XMFLOAT3 start{
            world._41,
            world._42,
            world._43
        };
        // 探索で得られた経路
        auto path = navMesh.FindPath(
            start,
            destination);
        SetPath(destination, path);
        return !path.empty();
    }

    bool NavMeshAgentComponent::SetDestination(
        const DirectX::XMFLOAT3& destination)
    {
        // 自動選択したサーフェス
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
        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // 所有者のワールド位置
        const DirectX::XMFLOAT3 position{
            world._41,
            world._42,
            world._43
        };

        // 現在の最良サーフェス
        const NavMeshComponent* best{};
        // 最良候補のXZ包含点数
        int bestScore = -1;
        // 最良候補と現在地の高さ差
        float bestHeightDistance =
            std::numeric_limits<float>::max();
        // 選択を調べるサーフェス
        for (const auto* candidate :
            Owner().GetScene().FindComponentsOfType<
                NavMeshComponent>())
        {
            if (!candidate->IsBaked())
            {
                continue;
            }

            // 現在地2点と目的地1点の評価
            int score = 0;
            if (candidate->ContainsPoint(position))
            {
                score += 2;
            }
            if (candidate->ContainsPoint(destination))
            {
                score += 1;
            }
            // 候補と現在地の高さ差
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

        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // 次に向かうワールド経路点
        const auto& target =
            m_path[m_currentWaypoint];
        // 次の点までのX方向の差
        const float deltaX =
            target.x - world._41;
        // 次の点までのZ方向の差
        const float deltaZ =
            target.z - world._43;
        // 次の点までのXZ平面の距離
        const float distance =
            std::sqrt(
                deltaX * deltaX
                + deltaZ * deltaZ);
        // この経路点の到達許容距離
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

        // 今回の更新で進む距離
        const float travel =
            std::min(
                m_speed * deltaTime,
                distance);
        // 進行方向を正規化する逆距離
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
            // ワールド上の進行方向からローカル回転を上書きし、ピッチとロールを0にする。
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

        // デバッグ経路線の頂点
        std::vector<DirectX::XMFLOAT3>
            lines;
        lines.reserve(
            (m_path.size() - 1) * 2);
        // 線の終点となる経路点番号
        for (std::size_t index = 1;
            index < m_path.size();
            ++index)
        {
            // デバッグ経路線の始点
            auto start = m_path[index - 1];
            // 高さを上げた経路線の終点
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
