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

    // 描画API固有資源を内部状態に所有する、オフスクリーン描画先。
    class RenderTarget final
    {
    public:
        // 未初期化の描画先を生成する。
        RenderTarget() noexcept;
        // API固有の描画先資源を解放する。
        ~RenderTarget() noexcept;

        // 描画先の複製を禁止する。
        RenderTarget(const RenderTarget&) = delete;
        // 描画先の複製代入を禁止する。
        RenderTarget& operator=(const RenderTarget&) = delete;
        // 描画先の移動生成を禁止する。
        RenderTarget(RenderTarget&&) = delete;
        // 描画先の移動代入を禁止する。
        RenderTarget& operator=(RenderTarget&&) = delete;

        // 順応した平均輝度を返し、未測定ならゼロを返す。
        [[nodiscard]] float AdaptedLuminance() const noexcept;
        // 現在の自動露出補正を段数で返す。
        [[nodiscard]] float AutoExposureStops() const noexcept;
        // 画面空間遮蔽の参照を保持し、未生成なら空を返す。
        [[nodiscard]] GraphicsViewHandle
            AmbientOcclusionViewHandle() const noexcept;
        // ポスト処理後の現在のカラー参照を保持して返す。
        // 取得した資源は描画先のサイズ変更後も参照が保持するが、基盤の再初期化後には使えない。
        [[nodiscard]] GraphicsViewHandle
            CurrentColorViewHandle() const noexcept;
        // 前フレームのHDR色参照を保持し、履歴がなければ空を返す。
        [[nodiscard]] GraphicsViewHandle
            ColorHistoryViewHandle() const noexcept;
        // 前フレームのTAA色参照を保持し、履歴がなければ空を返す。
        [[nodiscard]] GraphicsViewHandle
            TemporalHistoryViewHandle() const noexcept;
        // 描画先の寿命中有効な履歴行列を借用する。
        [[nodiscard]] const DirectX::XMFLOAT4X4&
            ColorHistoryViewProjection() const noexcept;
        // 深度参照を保持し、未生成なら空を返す。
        [[nodiscard]] GraphicsViewHandle
            DepthViewHandle() const noexcept;
        // 反射用の完成したHi-Z深度参照を保持して返す。
        [[nodiscard]] GraphicsViewHandle
            ReflectionDepthPyramidViewHandle() const noexcept;
        // Hi-Z深度のミップ段数を返し、未生成ならゼロを返す。
        [[nodiscard]] std::uint32_t
            ReflectionDepthPyramidMipCount() const noexcept;
        // 次回の資源生成時に計算書込みを許可する(value: 表示面への書込み可否)。
        // Resizeより前に指定し、作成済みの資源のフラグは変更しない。
        void SetComputeWritable(bool value) noexcept;
        // 公開した完成画像の表示面を保持して返す。
        // UIや名前付き描画テクスチャは、ポスト処理で切り替わるカラー参照ではなくこの表示面を保持する。
        [[nodiscard]] GraphicsViewHandle
            DisplayViewHandle() const noexcept;
        // 描画先の幅を返し、未生成ならゼロを返す。
        [[nodiscard]] std::uint32_t Width() const noexcept;
        // 描画先の高さを返し、未生成ならゼロを返す。
        [[nodiscard]] std::uint32_t Height() const noexcept;
        // 幅を一以上の高さで割った縦横比を返す。
        [[nodiscard]] float AspectRatio() const noexcept;
        // API固有状態が初期化されているか返す。
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        // 内部アクセスだけが描画先の状態を参照・差替えする。
        friend struct Detail::RenderTargetBackendAccess;

        // API固有の描画先状態
        std::unique_ptr<Detail::RenderTargetBackendState> m_backendState;
        // 行列への借用参照の寿命を内部状態の差替えから独立させる。
        // 公開する履歴のビュー射影行列
        DirectX::XMFLOAT4X4 m_publicHistoryViewProjection{};
        // 表示面への計算書込みを許可
        bool m_computeWritable{};
    };
}
