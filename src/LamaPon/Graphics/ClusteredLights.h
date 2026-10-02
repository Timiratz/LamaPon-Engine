#pragma once

#include <cstdint>
#include <memory>

namespace LamaPon
{
    namespace Detail
    {
        struct ClusteredLightsBackendAccess;
        struct ClusteredLightsBackendState;
    }

    // 視錐台を16×9×24へ分割するクラスタ照明の公開窓口。
    class ClusteredLights final
    {
    public:
        // HLSL側のグリッド分割数と一致させる。
        // グリッドの横分割数
        static constexpr std::uint32_t GridWidth = 16;
        // グリッドの縦分割数
        static constexpr std::uint32_t GridHeight = 9;
        // グリッドの奥行分割数
        static constexpr std::uint32_t GridDepth = 24;
        // クラスタの総数
        static constexpr std::uint32_t ClusterCount =
            GridWidth * GridHeight * GridDepth;
        // クラスタの上限を超えたライトは除く。
        // クラスタごとの保持ライト上限
        static constexpr std::uint32_t
            MaximumLightsPerCluster = 32;

        // バックエンド状態を持たないクラスタ照明を作成する。
        ClusteredLights() noexcept;
        // 所有するバックエンド状態を解放する。
        ~ClusteredLights() noexcept;

        // 所有状態のコピーを禁止する。
        ClusteredLights(const ClusteredLights&) = delete;
        // 所有状態のコピー代入を禁止する。
        ClusteredLights& operator=(const ClusteredLights&) = delete;
        // 公開窓口の移動を禁止する。
        ClusteredLights(ClusteredLights&&) = delete;
        // 公開窓口の移動代入を禁止する。
        ClusteredLights& operator=(ClusteredLights&&) = delete;

    private:
        friend struct Detail::ClusteredLightsBackendAccess;

        // 全ビューを作成してから公開する所有状態
        std::unique_ptr<Detail::ClusteredLightsBackendState>
            m_backendState;
    };
}
