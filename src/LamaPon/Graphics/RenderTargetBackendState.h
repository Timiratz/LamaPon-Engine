#pragma once

// RenderTargetの公開レイアウトからBackend stateを隠すための
// Runtime内部ヘッダーです。SDKにはインストールしません。
#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>

namespace LamaPon
{
    class RenderTarget;
}

namespace LamaPon::Detail
{
    // 描画APIに依存しない公開状態だけを保持します。具象stateはこの
    // オブジェクトとnative資源を一緒に所有し、完成した世代だけを
    // RenderTargetへ差し替えます。
    struct RenderTargetBackendState
    {
        virtual ~RenderTargetBackendState() noexcept = default;

        GraphicsViewHandle m_currentColorView;
        GraphicsViewHandle m_postColorView;
        GraphicsViewHandle m_displayView;
        GraphicsViewHandle m_depthView;
        GraphicsViewHandle m_ambientOcclusionView;
        GraphicsViewHandle m_colorHistoryView;
        GraphicsViewHandle m_temporalHistoryView;
        GraphicsViewHandle m_reflectionDepthPyramidViewHandle;

        float m_adaptedLuminance{};
        float m_autoExposureStops{};
        DirectX::XMFLOAT4X4 m_motionBlurPreviousViewProjection{};
        bool m_motionBlurPreviousValid{};
        DirectX::XMFLOAT4X4 m_historyViewProjection{};
        bool m_historyValid{};
        DirectX::XMFLOAT4X4 m_temporalHistoryViewProjection{};
        bool m_temporalHistoryValid{};
        std::uint32_t m_reflectionDepthPyramidMipCount{};
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        bool m_initialized{};
    };

    // Backend実装だけがopaque stateを参照・公開するための内部bridgeです。
    // 公開RenderTargetは具象Backendをfriendにせず、API-neutralに保ちます。
    struct RenderTargetBackendAccess final
    {
        [[nodiscard]] static RenderTargetBackendState*
            Get(RenderTarget& target) noexcept;
        [[nodiscard]] static const RenderTargetBackendState*
            Get(const RenderTarget& target) noexcept;
        [[nodiscard]] static bool ComputeWritable(
            const RenderTarget& target) noexcept;
        static void SetPublicHistoryViewProjection(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) noexcept;
        static void Publish(
            RenderTarget& target,
            std::unique_ptr<RenderTargetBackendState> state) noexcept;
    };
}
