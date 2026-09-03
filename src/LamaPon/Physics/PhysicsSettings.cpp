#include "LamaPon/Physics/PhysicsSettings.h"

#include <algorithm>

namespace LamaPon
{
    namespace
    {
        // **実体はここ（.cpp）に1つだけ置きます。** ヘッダの
        // inline staticにすると、EXE側とLamaPonRuntime.dll側で
        // 別々の実体になり、設定した値が読む側へ届きません
        // （--warpで実際に踏んでいます。エラーも警告も出ません）。
        // 取得と設定の両方をここで定義するのも同じ理由です
        // ——片方でもinlineにすると、往復のテストが嘘の合格を出します。
        PhysicsSettings g_physicsSettings{};
    }

    const PhysicsSettings& ActivePhysicsSettings() noexcept
    {
        return g_physicsSettings;
    }

    void SetActivePhysicsSettings(
        const PhysicsSettings& settings) noexcept
    {
        // 壊れる値だけはここで止めます。設定画面を通らない経路
        // （古いプロジェクトのJSON、スクリプトからの直接指定）でも
        // 0除算や「一歩も進まない」状態にならないようにするためです。
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
        // 0以下だと「常にすり抜ける速さ」になり、頭打ちを
        // オンにした瞬間に全部止まります。
        sanitized.discreteSafeSpeed = std::clamp(
            sanitized.discreteSafeSpeed,
            0.01f,
            100000.0f);
        // マトリクスは対称が前提です。手で編集したJSONなどで
        // 非対称になっていたら「両方が許可しているときだけ当たる」
        // 側へ丸めます（ANDで揃える）。判定側で毎回両方向を見るより、
        // 入口で1回揃える方が安くて確実です。
        for (std::size_t row = 0;
            row < CollisionLayerCount;
            ++row)
        {
            for (std::size_t column = row + 1;
                column < CollisionLayerCount;
                ++column)
            {
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
        // Layer()は0〜31の想定ですが、範囲外の値が来ても落ちない
        // よう31でマスクします（シフト量32以上は未定義動作）。
        return (g_physicsSettings.collisionMatrix[
                    layerA & 31u]
                & (1u << (layerB & 31u))) != 0;
    }
}
