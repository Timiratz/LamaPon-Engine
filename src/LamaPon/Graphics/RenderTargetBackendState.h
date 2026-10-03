#pragma once

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
    // 共通の公開状態を持ち、API固有資源と一緒に所有して完成した世代を描画先へ渡す。
    struct RenderTargetBackendState
    {
        // API固有資源を含む派生状態を解放する。
        virtual ~RenderTargetBackendState() noexcept = default;

        // 現在のカラー参照
        GraphicsViewHandle m_currentColorView;
        // 次のポスト処理用カラー参照
        GraphicsViewHandle m_postColorView;
        // 完成画像を公開する参照
        GraphicsViewHandle m_displayView;
        // 深度の参照
        GraphicsViewHandle m_depthView;
        // 画面空間遮蔽の参照
        GraphicsViewHandle m_ambientOcclusionView;
        // 前フレームのHDR色参照
        GraphicsViewHandle m_colorHistoryView;
        // 前フレームのTAA色参照
        GraphicsViewHandle m_temporalHistoryView;
        // 反射用Hi-Z深度参照
        GraphicsViewHandle m_reflectionDepthPyramidViewHandle;

        // 順応済み輝度、ゼロは未測定
        float m_adaptedLuminance{};
        // 自動露出の補正段数
        float m_autoExposureStops{};
        // ブラー用の前回ビュー射影行列
        DirectX::XMFLOAT4X4 m_motionBlurPreviousViewProjection{};
        // ブラー用の前回行列が有効
        bool m_motionBlurPreviousValid{};
        // HDR履歴のビュー射影行列
        DirectX::XMFLOAT4X4 m_historyViewProjection{};
        // HDR履歴が有効
        bool m_historyValid{};
        // TAA履歴のビュー射影行列
        DirectX::XMFLOAT4X4 m_temporalHistoryViewProjection{};
        // TAA履歴が有効
        bool m_temporalHistoryValid{};
        // Hi-Z深度のミップ段数
        std::uint32_t m_reflectionDepthPyramidMipCount{};
        // 描画先の幅
        std::uint32_t m_width{};
        // 描画先の高さ
        std::uint32_t m_height{};
        // 描画先の初期化済み
        bool m_initialized{};
    };

    // 描画基盤が内部状態を参照・公開するための窓口。
    struct RenderTargetBackendAccess final
    {
        // 描画先の内部状態を借用し、未生成なら空を返す(target: 描画先)。
        [[nodiscard]] static RenderTargetBackendState*
            Get(RenderTarget& target) noexcept;
        // 描画先の内部状態を借用し、未生成なら空を返す(target: 描画先)。
        [[nodiscard]] static const RenderTargetBackendState*
            Get(const RenderTarget& target) noexcept;
        // 表示面への計算書込み指定を返す(target: 描画先)。
        [[nodiscard]] static bool ComputeWritable(
            const RenderTarget& target) noexcept;
        // 公開用の履歴行列を更新する(target: 描画先, viewProjection: 履歴のビュー射影行列)。
        static void SetPublicHistoryViewProjection(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) noexcept;
        // 旧状態を解放して完成した状態の所有権を渡す(target: 描画先, state: 新しい内部状態)。
        static void Publish(
            RenderTarget& target,
            std::unique_ptr<RenderTargetBackendState> state) noexcept;
    };
}
