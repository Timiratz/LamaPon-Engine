#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace LamaPon
{
    // 二つの材質で合成方法が異なる場合は、列挙値の大きい方法を優先します。
    enum class PhysicsMaterialCombine : std::uint8_t
    {
        // 算術平均による合成
        Average = 0,
        // 幾何平均による合成
        GeometricMean = 1,
        // 積による合成
        Multiply = 2,
        // 小さい値を採用
        Minimum = 3,
        // 大きい値を採用
        Maximum = 4
    };

    struct PhysicsMaterial final
    {
        // 摩擦係数
        float friction{ 0.5f };
        // 反発係数
        float restitution{};
        // 摩擦係数の合成方法
        PhysicsMaterialCombine frictionCombine{
            PhysicsMaterialCombine::GeometricMean };
        // 反発係数の合成方法
        PhysicsMaterialCombine restitutionCombine{
            PhysicsMaterialCombine::Maximum };

        // 摩擦を0〜4、反発係数を0〜1へ制限します。
        void Clamp() noexcept
        {
            friction = std::clamp(friction, 0.0f, 4.0f);
            restitution = std::clamp(restitution, 0.0f, 1.0f);
        }
    };

    // 指定方法で二つの係数を合成します(left: 非負の係数, right: 非負の係数, mode: 合成方法)。
    // 未定義の方法は幾何平均になります。
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

    // 入力係数を制限して材質を合成します(left: 第1材質, right: 第2材質)。
    // 合成後の係数には再度のClampを行いません。
    [[nodiscard]] inline PhysicsMaterial CombinePhysicsMaterials(
        PhysicsMaterial left,
        PhysicsMaterial right) noexcept
    {
        left.Clamp();
        right.Clamp();
        // 優先される摩擦の合成方法
        const auto frictionMode = std::max(
            left.frictionCombine,
            right.frictionCombine);
        // 優先される反発の合成方法
        const auto restitutionMode = std::max(
            left.restitutionCombine,
            right.restitutionCombine);
        // 両材質から合成した接触材質
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
