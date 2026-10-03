#pragma once

#include <cstddef>
#include <mutex>

namespace LamaPon
{
    class GraphicsDevice;

    namespace Detail
    {
        // デバイス・リース・スプライトパスで共有する寿命ゲート。
        // 終了判定と借用ownerを使う操作を同じmutexで保護し、破棄済みデバイスへの参照を防ぐ。
        struct GraphicsDeviceResourceLeaseState final
        {
            // 所有デバイスを借用するゲートを初期化する(deviceOwner: 借用するデバイス)。
            explicit GraphicsDeviceResourceLeaseState(
                GraphicsDevice* const deviceOwner) noexcept
                : owner(deviceOwner)
            {
            }

            // ゲート状態とパス操作の排他制御
            std::mutex mutex;
            // 資源を外部で保持するリース数
            std::size_t activeLeases{};
            // 再初期化の進行中有無
            bool transitionInProgress{};
            // 新規取得とパス操作の禁止有無
            bool closed{};
            // 生存中だけ使う借用デバイス
            GraphicsDevice* owner{};
        };
    }
}
