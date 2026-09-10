#pragma once

// VolumetricInputsがEnvironmentRendererの入れ子型のため、
// 前方宣言では足りずヘッダが必要です。
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace LamaPon
{
    class D3D11Backend;
    class EnvironmentRenderer;
    class GraphicsDevice;
    class ScreenEffect;
    struct AmbientOcclusionSettings;
    struct BloomSettings;
    struct ScreenOutlineSettings;
    struct ScreenSpaceLensFlareSettings;
    struct DepthOfFieldSettings;
    struct MotionBlurSettings;
    struct AutoExposureSettings;
    struct ColorGradingSettings;

    class RenderTarget final
    {
    public:
        RenderTarget() = default;

        RenderTarget(const RenderTarget&) = delete;
        RenderTarget& operator=(const RenderTarget&) = delete;

        // 順応した平均輝度（0なら未測定）。エディターの表示用です。
        [[nodiscard]] float AdaptedLuminance() const noexcept
        {
            return m_adaptedLuminance;
        }
        // 今かかっている自動露出の補正（段数）。
        [[nodiscard]] float AutoExposureStops() const noexcept
        {
            return m_autoExposureStops;
        }
        [[nodiscard]] GraphicsViewHandle
            AmbientOcclusionViewHandle() const noexcept
        {
            return m_ambientOcclusionView;
        }
        // ポスト処理のping-pong後も、現在のカラーを指すviewです。
        // 戻り値はBackend資源を強所有するため、呼び出し中にtargetが
        // resizeされても取得時点のresource自体は安全に保持されます。
        [[nodiscard]] GraphicsViewHandle
            CurrentColorViewHandle() const noexcept
        {
            return m_currentColorView;
        }
        // 履歴がまだ無い最初のフレームではemptyを返します。
        [[nodiscard]] GraphicsViewHandle
            ColorHistoryViewHandle() const noexcept
        {
            return m_historyValid
                ? m_colorHistoryView
                : GraphicsViewHandle{};
        }
        // TAAで前フレームの解決済みカラーを参照するviewです。最初の
        // CaptureTemporalHistoryより前はemptyを返します。
        [[nodiscard]] GraphicsViewHandle
            TemporalHistoryViewHandle() const noexcept
        {
            return m_temporalHistoryValid
                ? m_temporalHistoryView
                : GraphicsViewHandle{};
        }
        [[nodiscard]] const DirectX::XMFLOAT4X4&
            ColorHistoryViewProjection() const noexcept
        {
            return m_historyViewProjection;
        }
        // Litパス中に深度を読むSSR用のコピーです。深度そのものは
        // DSVとして刺さっているため、同じリソースをSRVとしても
        // 読むことはできません（D3D11がSRVを黙ってnullにします）。
        [[nodiscard]] ID3D11ShaderResourceView*
            DepthCopyShaderResourceView() const noexcept
        {
            return m_depthCopyShaderResourceView.Get();
        }
        // 深度そのもののSRV。ポスト処理のようにDSVを外して描く
        // パスからは、コピーを取らずに直接読めます（同時に刺すと
        // D3D11がSRVを黙ってnullにするので、Litパス中は上の
        // コピーの方を使ってください）。
        [[nodiscard]] GraphicsViewHandle
            DepthViewHandle() const noexcept
        {
            return m_depthView;
        }
        // SSRのHi-Z用の深度ピラミッド（R32F、各ミップが2x2の
        // 最小値）。中身はEnvironmentRendererの
        // BuildReflectionDepthPyramidが毎フレーム書きます。
        [[nodiscard]] GraphicsViewHandle
            ReflectionDepthPyramidViewHandle() const noexcept
        {
            return m_reflectionDepthPyramidViewHandle;
        }
        [[nodiscard]] std::uint32_t
            ReflectionDepthPyramidMipCount() const noexcept
        {
            return static_cast<std::uint32_t>(
                m_reflectionDepthPyramidTargets.size());
        }
        [[nodiscard]] ID3D11RenderTargetView*
            ReflectionDepthPyramidMipTarget(
                const std::uint32_t mip) const noexcept
        {
            return mip < m_reflectionDepthPyramidTargets
                    .size()
                ? m_reflectionDepthPyramidTargets[mip].Get()
                : nullptr;
        }
        [[nodiscard]] ID3D11ShaderResourceView*
            ReflectionDepthPyramidMipView(
                const std::uint32_t mip) const noexcept
        {
            return mip < m_reflectionDepthPyramidMipViews
                    .size()
                ? m_reflectionDepthPyramidMipViews[mip].Get()
                : nullptr;
        }
        // Compute Shaderから表示用テクスチャへ直接書けるようにする
        // かどうか。Resizeより前に呼んでください（バインド
        // フラグは作成時にしか決められないため）。既定はfalseで、
        // 通常の描画先には余計なフラグを付けません。
        void SetComputeWritable(bool value) noexcept
        {
            m_computeWritable = value;
        }
        [[nodiscard]] ID3D11UnorderedAccessView*
            DisplayUnorderedAccessView() const noexcept
        {
            return m_displayUnorderedAccessView.Get();
        }
        // 表示用テクスチャの実体。中身をCPUへ読み戻して確かめたい
        // ときに使います（Compute Shaderの出力の検査など）。
        [[nodiscard]] ID3D11Texture2D*
            DisplayTexture() const noexcept
        {
            return m_displayColorTexture.Get();
        }
        // PublishOffscreenTargetで完成画像がコピーされる、ping-pongに
        // 左右されない表示面です。ImGuiや名前付きRenderTextureは
        // CurrentColorViewHandleではなくこちらを保持してください。
        [[nodiscard]] GraphicsViewHandle
            DisplayViewHandle() const noexcept
        {
            return m_displayView;
        }
        [[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
        [[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }
        [[nodiscard]] float AspectRatio() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_initialized && m_renderTargetView != nullptr;
        }

    private:
        // 基本的な資源作成・bind・clear・publishはGraphicsDeviceの
        // 共通facadeからD3D11Backendを経由してだけ呼びます。D3D11型を
        // 使用する旧経路をprivateにし、呼び出し側の迂回を防ぎます。
        friend class D3D11Backend;
        // 自動露出のreadbackと次回用転送はGraphicsDeviceがBackendの
        // 前後で順序付けるため、高水準の更新処理も直接公開しません。
        friend class GraphicsDevice;

        // API 61以前に公開していたraw D3D11 getterです。新規コードは
        // neutral handleを使い、旧名はprivate互換shimとしてだけ残します。
        [[nodiscard]] ID3D11ShaderResourceView*
            ShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            DisplayShaderResourceView() const noexcept;

        // API 60以前のEnvironmentRenderer / raw SRV入口は
        // GraphicsDeviceのneutral facadeだけが呼ぶprivate互換shimです。
        void ApplyBloom(
            EnvironmentRenderer& renderer,
            const BloomSettings& settings);
        void ApplyScreenOutline(
            EnvironmentRenderer& renderer,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        void ApplyScreenSpaceLensFlare(
            EnvironmentRenderer& renderer,
            const ScreenSpaceLensFlareSettings& settings);
        void ApplyTemporalAntiAliasing(
            EnvironmentRenderer& renderer,
            const TemporalAntiAliasingSettings& settings,
            const EnvironmentRenderer::TemporalInputs& inputs);
        void ApplyVolumetricLight(
            EnvironmentRenderer& renderer,
            const VolumetricLightSettings& settings,
            const EnvironmentRenderer::VolumetricInputs& inputs);
        void ApplyDepthOfField(
            EnvironmentRenderer& renderer,
            const DepthOfFieldSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        void ApplyMotionBlur(
            EnvironmentRenderer& renderer,
            const MotionBlurSettings& settings,
            const DirectX::XMFLOAT4X4& inverseViewProjection,
            const DirectX::XMFLOAT4X4& viewProjection,
            std::uint32_t sampleCount);
        void ApplyToneMapping(
            EnvironmentRenderer& renderer,
            const ColorGradingSettings& settings);
        void ApplyFXAA(EnvironmentRenderer& renderer);
        [[nodiscard]] bool ResolveAmbientOcclusion(
            EnvironmentRenderer& renderer,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        void ApplyScreenEffect(
            ScreenEffect& effect,
            const std::array<ID3D11ShaderResourceView*, 2>&
                auxiliaryTextures,
            const DirectX::XMFLOAT4& depthParameters,
            const DirectX::XMFLOAT4& depthUnprojection,
            const std::array<DirectX::XMFLOAT4, 8>& parameters);

        // API 54以前のGame Moduleが公開名を解決してからAPI不一致を
        // 案内できるよう、旧raw view getterのbinary symbolだけを
        // private shimとして残します。
        [[nodiscard]] ID3D11ShaderResourceView*
            AmbientOcclusionShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            ColorHistoryShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            ReflectionDepthPyramidShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            DepthShaderResourceView() const noexcept;

        void Resize(
            ID3D11Device* device,
            std::uint32_t width,
            std::uint32_t height);
        void Bind(ID3D11DeviceContext* context) const;
        void Clear(
            ID3D11DeviceContext* context,
            const float color[4]) const;
        // 完成した画像を表示専用テクスチャへコピーします。ポスト処理は
        // 内部テクスチャの交換（swap）で進むため、フレーム途中で取得した
        // SRVはswap回数によって別のテクスチャを指すことがあります。
        // ImGui等での表示は、描画完了時にこれを呼んだ上で常に
        // DisplayViewHandle()を使ってください。
        void CopyToDisplay(ID3D11DeviceContext* context) const;
        // 深度だけを描画先にします（深度プリパス用）。カラーを
        // 割り当てないので、ピクセルシェーダーを外した描画がそのまま
        // 深度書き込みだけになります。深度はclearしません。
        void BindDepthOnly(ID3D11DeviceContext* context) const;
        // 現在の深度をSSRがLitパス中に読むコピーへ控えます。
        // 描画先のbind状態は変更しません。
        void CaptureDepthForReflections(
            ID3D11DeviceContext* context) const;
        // 今のカラーをSSRが次フレームで読む履歴へ控え、
        // その画像を描いた行列もビューごとに保存します。
        void CaptureColorHistory(
            ID3D11DeviceContext* context,
            const DirectX::XMFLOAT4X4& viewProjection);
        // TAAで解決したカラーを次フレームの履歴へ控え、
        // 再投影に使うずらし無しの行列も保存します。
        void CaptureTemporalHistory(
            ID3D11DeviceContext* context,
            const DirectX::XMFLOAT4X4& viewProjection);
        // 前フレームの測定値からCPU側の順応を進め、現在のHDRを
        // 輝度テクスチャへ描きます。readbackと次回用転送の順序は
        // GraphicsDeviceが管理します。
        [[nodiscard]] float UpdateAutoExposure(
            EnvironmentRenderer& renderer,
            std::optional<float> measuredLuminance,
            const AutoExposureSettings& settings,
            float deltaSeconds);
        // D3D11の1x1 readbackを待たずに試します。値はhalfで格納した
        // 対数平均から線形輝度へ戻して返します。
        [[nodiscard]] std::optional<float>
            TryReadAutoExposureLuminance(
                ID3D11DeviceContext* context);
        // 現在の最小輝度mip（RGBA16F、8バイト）を次フレーム用の
        // staging textureへ転送します。
        void CaptureAutoExposureLuminance(
            ID3D11DeviceContext* context);
        // native texture / RTV / SRVと公開neutral handleを同じtransactionで
        // 入れ替え、片方だけが古いping-pong面を指す状態を防ぎます。
        void SwapPostProcessBuffers() noexcept;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_colorTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shaderResourceView;
        GraphicsViewHandle m_currentColorView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_postColorTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_postRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_postShaderResourceView;
        GraphicsViewHandle m_postColorView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_displayColorTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_displayShaderResourceView;
        GraphicsViewHandle m_displayView;
        // Compute Shaderの書き込み先（SetComputeWritable時のみ）。
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_displayUnorderedAccessView;
        bool m_computeWritable{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthShaderResourceView;
        GraphicsViewHandle m_depthView;
        // SSAO用（半解像度）。(1)遮蔽を求める先と(2)ブラーの出力先の
        // 2枚を使います。AOは低周波なので半分の解像度で十分で、
        // 計算量が1/4になります。1チャンネルなので1080pでも
        // 2枚あわせて約1MBです。
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_occlusionTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_occlusionRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_occlusionShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_occlusionBlurTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_occlusionBlurRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_occlusionBlurShaderResourceView;
        GraphicsViewHandle m_ambientOcclusionView;
        std::uint32_t m_occlusionWidth{};
        std::uint32_t m_occlusionHeight{};
        // レンズフレアの筋を作る1/4解像度のping-pong。
        // ストライドを広げながら書き戻すので2枚要ります。
        // 1080pでもRGBA16Fで2枚あわせて約1MBです。
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_streakTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_streakRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_streakShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_streakBlurTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_streakBlurRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_streakBlurShaderResourceView;
        std::uint32_t m_streakWidth{};
        std::uint32_t m_streakHeight{};
        // 被写界深度の作業用です。(1)色とCoCの書き出し先と、
        // (2)ぼかしの出力先で半解像度のテクスチャを2枚使います。
        // ぼけた絵では細部を保持する必要がないため、計算量が1/4になります。
        // RGBA16Fなので
        // 1080pでも2枚あわせて約8MBです。
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_depthOfFieldTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_depthOfFieldRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthOfFieldShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_depthOfFieldBlurTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_depthOfFieldBlurRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthOfFieldBlurShaderResourceView;
        std::uint32_t m_depthOfFieldWidth{};
        std::uint32_t m_depthOfFieldHeight{};
        // 自動露出の明るさ測定（1/4解像度＋ミップ連鎖）。いちばん
        // 小さいミップが画面全体の対数平均になります。
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_luminanceTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_luminanceRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_luminanceShaderResourceView;
        // 1x1ミップをCPUへ渡すための1x1（STAGING）。
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_luminanceStagingTexture;
        std::uint32_t m_luminanceWidth{};
        std::uint32_t m_luminanceHeight{};
        std::uint32_t m_luminanceMipLevels{};
        // 前フレームのコピーが読める状態か（初回は読みません）。
        bool m_luminanceStagingReady{};
        // 順応した平均輝度。0以下なら「まだ一度も測っていない」で、
        // 次に測れた値へそのまま飛びます（暗転から始まるのを防ぐため）。
        float m_adaptedLuminance{};
        float m_autoExposureStops{};
        // モーションブラー用の前フレームのビュー射影（ずらし無し）。
        // ビューごとに別なのでここに持ちます。
        DirectX::XMFLOAT4X4
            m_motionBlurPreviousViewProjection{};
        bool m_motionBlurPreviousValid{};
        D3D11_VIEWPORT m_viewport{};
        // SSR用の「前フレームのカラー」。カラーと同じ形式・同じ
        // 大きさなので、CopyResourceでそのまま控えられます。
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_historyTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_historyShaderResourceView;
        GraphicsViewHandle m_colorHistoryView;
        // TAAの履歴＝前フレームの解決済みの絵。
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_temporalHistoryTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_temporalHistoryShaderResourceView;
        GraphicsViewHandle m_temporalHistoryView;
        // このビューの前フレームのビュー射影（ずらし無し）。
        DirectX::XMFLOAT4X4
            m_temporalHistoryViewProjection{};
        bool m_temporalHistoryValid{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_depthCopyTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthCopyShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_reflectionDepthPyramidTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_reflectionDepthPyramidView;
        GraphicsViewHandle m_reflectionDepthPyramidViewHandle;
        std::vector<
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView>>
            m_reflectionDepthPyramidTargets;
        std::vector<
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>
            m_reflectionDepthPyramidMipViews;
        DirectX::XMFLOAT4X4 m_historyViewProjection{};
        bool m_historyValid{};
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        // 同じ寸法でもBackendのDeviceが変わった場合は全resourceを
        // 作り直し、旧Deviceのviewを新しいcontextへ渡しません。
        Microsoft::WRL::ComPtr<ID3D11Device> m_ownerDevice;
        // 全てのsize-dependent resourceが完成した世代だけを有効とします。
        bool m_initialized{};
    };
}
