#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace LamaPon
{
    // マテリアル値の合成方法。2つのコライダーでモードが異なる
    // 場合は、数値の大きいモードが優先されます。
    enum class PhysicsMaterialCombine : std::uint8_t
    {
        Average = 0,
        GeometricMean = 1,
        Multiply = 2,
        Minimum = 3,
        Maximum = 4
    };

    struct PhysicsMaterial final
    {
        float friction{ 0.5f };
        float restitution{};
        // 既定値は従来の挙動（摩擦=幾何平均、反発=最大）と同じです。
        PhysicsMaterialCombine frictionCombine{
            PhysicsMaterialCombine::GeometricMean };
        PhysicsMaterialCombine restitutionCombine{
            PhysicsMaterialCombine::Maximum };

        void Clamp() noexcept
        {
            friction = std::clamp(friction, 0.0f, 4.0f);
            restitution = std::clamp(restitution, 0.0f, 1.0f);
        }
    };

    [[nodiscard]] inline float CombinePhysicsValues(
        const float left,
        const float right,
        const PhysicsMaterialCombine mode) noexcept
    {
        switch (mode)
        {
        case PhysicsMaterialCombine::Average:
            return (left + right) * 0.5f;
        case PhysicsMaterialCombine::Multiply:
            return left * right;
        case PhysicsMaterialCombine::Minimum:
            return std::min(left, right);
        case PhysicsMaterialCombine::Maximum:
            return std::max(left, right);
        case PhysicsMaterialCombine::GeometricMean:
        default:
            return std::sqrt(left * right);
        }
    }

    [[nodiscard]] inline PhysicsMaterial CombinePhysicsMaterials(
        PhysicsMaterial left,
        PhysicsMaterial right) noexcept
    {
        left.Clamp();
        right.Clamp();
        const auto frictionMode = std::max(
            left.frictionCombine,
            right.frictionCombine);
        const auto restitutionMode = std::max(
            left.restitutionCombine,
            right.restitutionCombine);
        PhysicsMaterial result{
            CombinePhysicsValues(
                left.friction,
                right.friction,
                frictionMode),
            CombinePhysicsValues(
                left.restitution,
                right.restitution,
                restitutionMode)
        };
        result.frictionCombine = frictionMode;
        result.restitutionCombine = restitutionMode;
        return result;
    }
}
