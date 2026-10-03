#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <string>

namespace LamaPon
{
    // 32ビット衝突マスクのレイヤー数
    inline constexpr std::size_t CollisionLayerCount = 32;

    namespace Detail
    {
        // 全レイヤー間の衝突を許可する行列を作ります。
        [[nodiscard]] constexpr
            std::array<std::uint32_t, CollisionLayerCount>
            AllLayersCollide() noexcept
        {
            // 全ビットを立てる衝突許可行列
            std::array<std::uint32_t, CollisionLayerCount>
                matrix{};
            // 衝突許可行列の一行
            for (auto& row : matrix)
            {
                row = 0xFFFFFFFFu;
            }
            return matrix;
        }
    }

    // プロジェクトと配布ゲームで共有し、値の範囲はValidateProjectSettingsが検証します。
    // 新しいフィールドは末尾へ追加し、サイズ変更時はGameModuleApiVersionで旧DLLを拒否します。
    // clampDiscreteSpeedはCCD・キネマティック・休止中を除く離散判定の物体だけへ作用します。
    // 衝突行列は個別マスクと併用して接触を絞り、公開Raycast・OverlapBoxの検索条件には使いません。
    // 行列の[i]のビットjはレイヤーiとjの許可を示し、読み込み時に対称へ正規化します。
    struct PhysicsSettings final
    {
        // 重力加速度のメートル毎秒二乗
        DirectX::XMFLOAT3 gravity{ 0.0f, -9.81f, 0.0f };

        // 固定更新一回分の秒数
        float fixedTimeStep{ 1.0f / 60.0f };

        // フレーム内の固定更新回数上限
        std::uint32_t maximumCatchUpSteps{ 8 };

        // 接触解決を繰り返す回数
        std::uint32_t solverIterations{ 8 };

        // 休止判定のメートル毎秒速さ
        float sleepLinearVelocity{ 0.25f };
        // 休止判定のラジアン毎秒角速度
        float sleepAngularVelocity{ 0.35f };

        // 休止条件を保つ待機秒数
        float sleepDelay{ 0.5f };

        // 離散判定のメートル毎秒速度
        float discreteSafeSpeed{ 40.0f };

        // 離散判定の速度制限有無
        bool clampDiscreteSpeed{ false };

        // 衝突レイヤーの表示名
        std::array<std::string, CollisionLayerCount>
            layerNames{ "Default" };

        // 接触を許可するレイヤー行列
        std::array<std::uint32_t, CollisionLayerCount>
            collisionMatrix{ Detail::AllLayersCollide() };
    };

    // 有効な行列で接触の許可を調べます(layerA: 第1レイヤー番号, layerB: 第2レイヤー番号)。
    // 範囲外の番号は下位5ビットで0〜31へ丸めます。
    [[nodiscard]] bool LayersCanCollide(
        std::uint32_t layerA,
        std::uint32_t layerB) noexcept;

    // EXEとDLLで共有する現在の物理設定を参照します。
    [[nodiscard]] const PhysicsSettings&
        ActivePhysicsSettings() noexcept;
    // 物理設定の数値範囲と行列の対称性を正規化します(settings: 新しい物理設定)。
    // 非対称な許可は両方向のANDで揃え、有限値の検証は呼び出し側で行います。
    void SetActivePhysicsSettings(
        const PhysicsSettings& settings) noexcept;
}
