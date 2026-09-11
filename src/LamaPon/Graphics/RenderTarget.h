#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>

namespace LamaPon
{
    namespace Detail
    {
        struct RenderTargetBackendAccess;
        struct RenderTargetBackendState;
    }

    // オフスクリーン描画先のAPI-neutralな公開facadeです。native資源と
    // 描画API固有の処理は、選択中のBackendが所有するopaque stateへ
    // 閉じ込めます。
    class RenderTarget final
    {
    public:
        RenderTarget() noexcept;
        ~RenderTarget() noexcept;

        RenderTarget(const RenderTarget&) = delete;
        RenderTarget& operator=(const RenderTarget&) = delete;
        RenderTarget(RenderTarget&&) = delete;
        RenderTarget& operator=(RenderTarget&&) = delete;

        // 順応した平均輝度（0なら未測定）。エディターの表示用です。
        [[nodiscard]] float AdaptedLuminance() const noexcept;
        // 今かかっている自動露出の補正（段数）。
        [[nodiscard]] float AutoExposureStops() const noexcept;
        [[nodiscard]] GraphicsViewHandle
            AmbientOcclusionViewHandle() const noexcept;
        // ポスト処理のping-pong後も、現在のカラーを指すviewです。
        // 戻り値はBackend資源を強所有するため、呼び出し中にtargetが
        // resizeされても取得時点のresource自体は安全に保持されます。
        [[nodiscard]] GraphicsViewHandle
            CurrentColorViewHandle() const noexcept;
        // 履歴がまだ無い最初のフレームではemptyを返します。
        [[nodiscard]] GraphicsViewHandle
            ColorHistoryViewHandle() const noexcept;
        // TAAで前フレームの解決済みカラーを参照するviewです。最初の
        // CaptureTemporalHistoryより前はemptyを返します。
        [[nodiscard]] GraphicsViewHandle
            TemporalHistoryViewHandle() const noexcept;
        [[nodiscard]] const DirectX::XMFLOAT4X4&
            ColorHistoryViewProjection() const noexcept;
        [[nodiscard]] GraphicsViewHandle
            DepthViewHandle() const noexcept;
        // SSRのHi-Z用深度ピラミッドです。構築とnative mip操作は
        // Backend内で行い、公開側は完成したviewだけを参照します。
        [[nodiscard]] GraphicsViewHandle
            ReflectionDepthPyramidViewHandle() const noexcept;
        [[nodiscard]] std::uint32_t
            ReflectionDepthPyramidMipCount() const noexcept;
        // Compute Shaderから表示用テクスチャへ直接書けるようにする
        // かどうか。Resizeより前に呼んでください（資源の作成フラグは
        // 作成時にしか決められないため）。既定はfalseです。
        void SetComputeWritable(bool value) noexcept;
        // PublishOffscreenTargetで完成画像がコピーされる、ping-pongに
        // 左右されない表示面です。ImGuiや名前付きRenderTextureは
        // CurrentColorViewHandleではなくこちらを保持してください。
        [[nodiscard]] GraphicsViewHandle
            DisplayViewHandle() const noexcept;
        [[nodiscard]] std::uint32_t Width() const noexcept;
        [[nodiscard]] std::uint32_t Height() const noexcept;
        [[nodiscard]] float AspectRatio() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        // Backendだけがopaque stateを参照・transactionalに差し替えます。
        // 具象Backendを直接friendにしないことで、この公開ヘッダーを
        // DirectX 11/12のどちらからも利用できる状態に保ちます。
        friend struct Detail::RenderTargetBackendAccess;

        std::unique_ptr<Detail::RenderTargetBackendState> m_backendState;
        // 参照を返す既存APIの寿命をstate差し替えから独立させます。
        DirectX::XMFLOAT4X4 m_publicHistoryViewProjection{};
        bool m_computeWritable{};
    };
}
