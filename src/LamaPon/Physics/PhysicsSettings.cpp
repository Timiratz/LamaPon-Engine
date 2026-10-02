#include "LamaPon/Physics/PhysicsSettings.h"

#include <algorithm>

namespace LamaPon
{
    namespace
    {
        // EXEとDLLが共有する物理設定
        PhysicsSettings g_physicsSettings{};
    }

    const PhysicsSettings& ActivePhysicsSettings() noexcept
    {
        return g_physicsSettings;
    }

    void SetActivePhysicsSettings(
        const PhysicsSettings& settings) noexcept
    {
        // 範囲と対称性を調整する設定コピー
        PhysicsSettings sanitized = settings;
        sanitized.fixedTimeStep = std::clamp(
            sanitized.fixedTimeStep,
            1.0f / 1000.0f,
            0.1f);
        sanitized.maximumCatchUpSteps = std::clamp(
            sanitized.maximumCatchUpSteps,
            1u,
            32u);
        sanitized.solverIterations = std::clamp(
            sanitized.solverIterations,
            1u,
            64u);
        sanitized.sleepLinearVelocity =
            std::max(0.0f, sanitized.sleepLinearVelocity);
        sanitized.sleepAngularVelocity =
            std::max(0.0f, sanitized.sleepAngularVelocity);
        sanitized.sleepDelay = std::clamp(
            sanitized.sleepDelay,
            0.0f,
            60.0f);
        sanitized.discreteSafeSpeed = std::clamp(
            sanitized.discreteSafeSpeed,
            0.01f,
            100000.0f);
        // 対称化する基準レイヤー番号
        for (std::size_t row = 0;
            row < CollisionLayerCount;
            ++row)
        {
            // 対称化する相手レイヤー番号
            for (std::size_t column = row + 1;
                column < CollisionLayerCount;
                ++column)
            {
                // 両方向で接触を許可するか
                const bool collide =
                    (sanitized.collisionMatrix[row]
                        & (1u << column)) != 0
                    && (sanitized.collisionMatrix[column]
                        & (1u << row)) != 0;
                if (collide)
                {
                    continue;
                }
                sanitized.collisionMatrix[row] &=
                    ~(1u << column);
                sanitized.collisionMatrix[column] &=
                    ~(1u << row);
            }
        }
        g_physicsSettings = sanitized;
    }

    bool LayersCanCollide(
        const std::uint32_t layerA,
        const std::uint32_t layerB) noexcept
    {
        return (g_physicsSettings.collisionMatrix[
                    layerA & 31u]
                & (1u << (layerB & 31u))) != 0;
    }
}
