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

    // クラスタライトカリング（Forward+）のAPI-neutralな公開facadeです。
    // 視錐台を16×9×24のクラスタに分割する契約だけを公開し、Compute
    // Shaderやbufferなどのnative資源はBackend専用stateが所有します。
    // 従来の「シーン全体で先着16灯」の定数バッファは、自作Shaderと
    // DirectXTKフォールバックの互換のため引き続き別経路に残ります。
    class ClusteredLights final
    {
    public:
        // グリッドの分割数。シェーダー側と一致させてください。
        static constexpr std::uint32_t GridWidth = 16;
        static constexpr std::uint32_t GridHeight = 9;
        static constexpr std::uint32_t GridDepth = 24;
        static constexpr std::uint32_t ClusterCount =
            GridWidth * GridHeight * GridDepth;
        // 1クラスタに入るライトの上限。超えた分は落とします。
        static constexpr std::uint32_t
            MaximumLightsPerCluster = 32;

        ClusteredLights() noexcept;
        ~ClusteredLights() noexcept;

        ClusteredLights(const ClusteredLights&) = delete;
        ClusteredLights& operator=(const ClusteredLights&) = delete;
        ClusteredLights(ClusteredLights&&) = delete;
        ClusteredLights& operator=(ClusteredLights&&) = delete;

    private:
        friend struct Detail::ClusteredLightsBackendAccess;

        // Backendはnative資源と3本のneutral viewを完成させてから、
        // stateを一度に公開します。作成失敗時に部分状態を残しません。
        std::unique_ptr<Detail::ClusteredLightsBackendState>
            m_backendState;
    };
}
