#pragma once

// VolumetricInputsがEnvironmentRendererの入れ子型のため、
// 前方宣言では足りずヘッダが必要です。
#include "LamaPon/Graphics/EnvironmentRenderer.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <vector>

namespace LamaPon
{
    class EnvironmentRenderer;
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

        void Resize(ID3D11Device* device, std::uint32_t width, std::uint32_t height);
        void Bind(ID3D11DeviceContext* context) const;
        // 深度だけを描画先にします（深度プリパス用）。カラーを
        // 割り当てないので、ピクセルシェーダーを外した描画がそのまま
        // 深度書き込みだけになります。
        void BindDepthOnly(ID3D11DeviceContext* context) const;
        void Clear(ID3D11DeviceContext* context, const float color[4]) const;
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
        // TAA（時間的アンチエイリアス）。今のフレームと前フレームの
        // 結果を混ぜ、結果を次フレームの履歴として控えます。
        // ポスト処理の先頭（Bloomより前）で呼んでください。
        //
        // 履歴がまだ無い最初のフレームは混ぜずに、今の絵を履歴として
        // 控えるだけです。
        // contextは解決した絵を履歴へ控えるのに使います
        // （EnvironmentRendererは自分の分しか持っていないため）。
        void ApplyTemporalAntiAliasing(
            EnvironmentRenderer& renderer,
            ID3D11DeviceContext* context,
            const TemporalAntiAliasingSettings& settings,
            const EnvironmentRenderer::TemporalInputs&
                inputs);
        // ボリュメトリックライト（光の筋）。深度と影を読むので、
        // トーンマップより前（HDRのうち）にかけます。
        void ApplyVolumetricLight(
            EnvironmentRenderer& renderer,
            const VolumetricLightSettings& settings,
            const EnvironmentRenderer::VolumetricInputs&
                inputs);
        // 被写界深度（DoF）。深度を読むので、トーンマップより前
        // （HDRのうち）にかけます。projectionはこのターゲットを
        // 描いたときの射影行列です（深度をカメラからの距離へ戻すのに
        // 必要）。半解像度の作業用テクスチャはこのターゲットが
        // 持っているものを使います。
        void ApplyDepthOfField(
            EnvironmentRenderer& renderer,
            const DepthOfFieldSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // モーションブラー（カメラの動きによるブレ）。深度と前フレーム
        // の行列を使うので、トーンマップより前（HDRのうち）にかけます。
        // 行列はずらしを含まないものを渡してください。前フレームの
        // 行列はこのターゲットが自分で覚えます（ビューをまたいで
        // 共有すると再投影が壊れます）。
        void ApplyMotionBlur(
            EnvironmentRenderer& renderer,
            const MotionBlurSettings& settings,
            const DirectX::XMFLOAT4X4& inverseViewProjection,
            const DirectX::XMFLOAT4X4& viewProjection,
            std::uint32_t sampleCount);
        // 自動露出。トーンマップ前のHDRの明るさを測り、露出への補正
        // （段数）を返します。返った値をColorGradingSettingsの露出へ
        // 足してからApplyToneMappingを呼んでください。
        //
        // 測定はGPU、順応の計算はCPUです。読み出すのは1x1ミップの
        // 4バイトだけで、しかも1フレーム遅れで読みます（同じ
        // フレームで読むとGPUの完了待ちで必ず止まります）。目の順応
        // 自体が時間をかける表現なので、1フレームの遅れは見えません。
        [[nodiscard]] float UpdateAutoExposure(
            EnvironmentRenderer& renderer,
            ID3D11DeviceContext* context,
            const AutoExposureSettings& settings,
            float deltaSeconds);
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
        void ApplyToneMapping(
            EnvironmentRenderer& renderer,
            const ColorGradingSettings& settings);
        void ApplyFXAA(
            EnvironmentRenderer& renderer);
        // SSAO。深度プリパスが書いた深度から遮蔽を求め、ブラーまで
        // 済ませた結果を内部テクスチャへ残します。色には触りません
        // （Litシェーダーが環境光項へ掛けるため）。
        // projectionはこのターゲットを描いたときの射影行列（深度を
        // ビュー空間へ戻すために必要）です。
        // sampleCountは品質設定から渡す遮蔽の探索回数です。
        // 戻り値がtrueのときだけ
        // AmbientOcclusionShaderResourceView()が使えます。
        [[nodiscard]] bool ResolveAmbientOcclusion(
            EnvironmentRenderer& renderer,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        [[nodiscard]] ID3D11ShaderResourceView*
            AmbientOcclusionShaderResourceView()
                const noexcept
        {
            return m_occlusionBlurShaderResourceView.Get();
        }
        void ApplyScreenEffect(
            ScreenEffect& effect,
            const std::array<ID3D11ShaderResourceView*, 2>&
                auxiliaryTextures,
            const DirectX::XMFLOAT4& depthParameters,
            const DirectX::XMFLOAT4& depthUnprojection,
            const std::array<DirectX::XMFLOAT4, 8>&
                parameters);

        [[nodiscard]] ID3D11ShaderResourceView* ShaderResourceView() const noexcept
        {
            return m_shaderResourceView.Get();
        }
        // 完成した画像を表示専用テクスチャへコピーします。ポスト処理は
        // 内部テクスチャの交換（swap）で進むため、フレーム途中で取得した
        // SRVはswap回数によって別のテクスチャを指すことがあります。
        // ImGui等での表示は、描画完了時にこれを呼んだ上で常に
        // DisplayShaderResourceView()を使ってください。
        void CopyToDisplay(ID3D11DeviceContext* context) const;
        // 今のカラーを「前フレームのカラー」として控えます。SSRが
        // 次のフレームでこれを読みます（今描いている絵は完成前なので
        // 読めないため）。viewProjectionは今のフレームの行列で、
        // 当たった点をこの絵の画面座標へ戻すのに使います。
        //
        // 呼ぶのはLitパスが終わった後・ポスト処理の前です。順番を
        // 逆にすると、まだ読んでいない履歴を潰してしまいます。
        void CaptureColorHistory(
            ID3D11DeviceContext* context,
            const DirectX::XMFLOAT4X4& viewProjection);
        // 履歴がまだ無い最初のフレームではnullptrを返します。
        [[nodiscard]] ID3D11ShaderResourceView*
            ColorHistoryShaderResourceView()
                const noexcept
        {
            return m_historyValid
                ? m_historyShaderResourceView.Get()
                : nullptr;
        }
        [[nodiscard]] const DirectX::XMFLOAT4X4&
            ColorHistoryViewProjection() const noexcept
        {
            return m_historyViewProjection;
        }
        // Litパス中に深度を読みたいときは、こちらのコピーを使い
        // ます。深度そのものはDSVとして刺さっているため、同じ
        // リソースをSRVとしても読むことはできません（D3D11が
        // SRVを黙ってnullにします）。
        void CaptureDepthForReflections(
            ID3D11DeviceContext* context) const;
        [[nodiscard]] ID3D11ShaderResourceView*
            DepthCopyShaderResourceView() const noexcept
        {
            return m_depthCopyShaderResourceView.Get();
        }
        // 深度そのもののSRV。ポスト処理のようにDSVを外して描く
        // パスからは、コピーを取らずに直接読めます（同時に刺すと
        // D3D11がSRVを黙ってnullにするので、Litパス中は上の
        // コピーの方を使ってください）。
        [[nodiscard]] ID3D11ShaderResourceView*
            DepthShaderResourceView() const noexcept
        {
            return m_depthShaderResourceView.Get();
        }
        // SSRのHi-Z用の深度ピラミッド（R32F、各ミップが2x2の
        // 最小値）。中身はEnvironmentRendererの
        // BuildReflectionDepthPyramidが毎フレーム書きます。
        [[nodiscard]] ID3D11ShaderResourceView*
            ReflectionDepthPyramidShaderResourceView()
            const noexcept
        {
            return m_reflectionDepthPyramidView.Get();
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
        [[nodiscard]] ID3D11ShaderResourceView*
            DisplayShaderResourceView() const noexcept
        {
            return m_displayShaderResourceView.Get();
        }
        [[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
        [[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }
        [[nodiscard]] float AspectRatio() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept { return m_renderTargetView != nullptr; }

    private:
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_colorTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_shaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_postColorTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_postRenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_postShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_displayColorTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_displayShaderResourceView;
        // Compute Shaderの書き込み先（SetComputeWritable時のみ）。
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_displayUnorderedAccessView;
        bool m_computeWritable{};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthShaderResourceView;
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
        // TAAの履歴＝前フレームの解決済みの絵。
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_temporalHistoryTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_temporalHistoryShaderResourceView;
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
    };
}
