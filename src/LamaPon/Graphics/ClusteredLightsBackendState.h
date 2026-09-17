#pragma once

// ClusteredLightsの公開レイアウトからBackend stateを隠すための
// Runtime内部ヘッダーです。SDKにはインストールしません。
#include "LamaPon/Graphics/GraphicsResource.h"

#include <memory>

namespace LamaPon
{
    class ClusteredLights;
}

namespace LamaPon::Detail
{
    // 描画APIに依存しない出力状態です。3本のviewは同じBackend世代で
    // 全て作成できた後にだけ、具象stateと一緒に公開します。
    struct ClusteredLightsBackendState
    {
        virtual ~ClusteredLightsBackendState() noexcept = default;

        GraphicsViewHandle m_lightView;
        GraphicsViewHandle m_indexListView;
        GraphicsViewHandle m_countView;
        bool m_initialized{};
    };

    // Backend実装だけがopaque stateを参照・公開するための内部bridgeです。
    struct ClusteredLightsBackendAccess final
    {
        [[nodiscard]] static ClusteredLightsBackendState*
            Get(ClusteredLights& clusteredLights) noexcept;
        [[nodiscard]] static const ClusteredLightsBackendState*
            Get(const ClusteredLights& clusteredLights) noexcept;
        static void Publish(
            ClusteredLights& clusteredLights,
            std::unique_ptr<ClusteredLightsBackendState> state) noexcept;
    };
}
