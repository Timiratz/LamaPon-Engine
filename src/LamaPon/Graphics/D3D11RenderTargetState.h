#pragma once

#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace LamaPon
{
    class ScreenEffect;
}

namespace LamaPon::Detail
{
    // ポスト処理中は深度描画先を外し、資源と同じデバイスのコンテキストを使う。
    struct D3D11RenderTargetState final : RenderTargetBackendState
    {
        // 初期化済みでカラー描画先が存在するか返す。
        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_initialized && m_renderTargetView != nullptr;
        }

        // 現在のカラー参照を借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            ShaderResourceView() const noexcept;
        // 表示面の画像参照を借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            DisplayShaderResourceView() const noexcept;
        // 平滑化済みの遮蔽参照を借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            AmbientOcclusionShaderResourceView() const noexcept;
        // HDR色履歴を借用し、未取得なら空を返す。
        [[nodiscard]] ID3D11ShaderResourceView*
            ColorHistoryShaderResourceView() const noexcept;
        // 全ミップのHi-Z深度参照を借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            ReflectionDepthPyramidShaderResourceView() const noexcept;
        // 深度の読取参照を借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            DepthShaderResourceView() const noexcept;

        // 一画素以上の寸法で資源を作り直す(device: 描画デバイス, width: 要求幅, height: 要求高さ)。
        // 同じ寸法とデバイスなら維持し、再生成の失敗時は未初期化の部分資源が残る。
        void Resize(
            ID3D11Device* device,
            std::uint32_t width,
            std::uint32_t height);
        // カラーと深度の描画先を設定する(context: 同じデバイスのコンテキスト)。
        // Bind・Clear・CopyToDisplay・BindDepthOnlyには非空のコンテキストを渡す。
        void Bind(ID3D11DeviceContext* context) const;
        // カラーと深度・ステンシルを消去する(context: 同じデバイスのコンテキスト, color: 四成分の消去色)。
        void Clear(
            ID3D11DeviceContext* context,
            const float color[4]) const;
        // 現在のカラーを表示面へコピーする(context: 同じデバイスのコンテキスト)。
        void CopyToDisplay(ID3D11DeviceContext* context) const;
        // PSのt0〜t15を外して深度だけの描画先を設定する(context: 同じデバイスのコンテキスト)。
        void BindDepthOnly(ID3D11DeviceContext* context) const;
        // 深度を反射用の読取資源へコピーする(context: 任意の描画コンテキスト)。
        void CaptureDepthForReflections(
            ID3D11DeviceContext* context) const;
        // 現在のHDR色と行列を履歴に保存する(context: 任意の描画コンテキスト, viewProjection: 履歴のビュー射影行列)。
        void CaptureColorHistory(
            ID3D11DeviceContext* context,
            const DirectX::XMFLOAT4X4& viewProjection);
        // 現在のTAA色と行列を履歴に保存する(context: 任意の描画コンテキスト, viewProjection: 履歴のビュー射影行列)。
        void CaptureTemporalHistory(
            ID3D11DeviceContext* context,
            const DirectX::XMFLOAT4X4& viewProjection);
        // 順応を進めて次回測定の対数輝度を描く(renderer: 輝度処理器, measuredLuminance: 任意の線形平均輝度, settings: 自動露出設定, deltaSeconds: 実時間の経過秒数)。
        [[nodiscard]] float UpdateAutoExposure(
            EnvironmentRenderer& renderer,
            std::optional<float> measuredLuminance,
            const AutoExposureSettings& settings,
            float deltaSeconds);
        // GPUを待たず線形輝度を読み、未完了なら値を返さない(context: 任意の描画コンテキスト)。
        [[nodiscard]] std::optional<float>
            TryReadAutoExposureLuminance(
                ID3D11DeviceContext* context);
        // 最小ミップを次回CPU読取用にコピーする(context: 任意の描画コンテキスト)。
        void CaptureAutoExposureLuminance(
            ID3D11DeviceContext* context);
        // 幅を一以上の高さで割った縦横比を返す。
        [[nodiscard]] float AspectRatio() const noexcept;
        // 現在と次のカラー資源・描画先・参照をまとめて入れ替える。
        void SwapPostProcessBuffers() noexcept;

        // 有効なブルームを適用してカラーを切り替える(renderer: 効果処理器, settings: ブルーム設定)。
        void ApplyBloom(
            EnvironmentRenderer& renderer,
            const BloomSettings& settings);
        // 有効な射影で輪郭線を適用してカラーを切り替える(renderer: 効果処理器, settings: 輪郭設定, projection: 射影行列)。
        void ApplyScreenOutline(
            EnvironmentRenderer& renderer,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        // 筋を生成してレンズフレアを適用する(renderer: 効果処理器, settings: フレア設定)。
        void ApplyScreenSpaceLensFlare(
            EnvironmentRenderer& renderer,
            const ScreenSpaceLensFlareSettings& settings);
        // 描画先自身の深度と履歴でTAAを適用する(renderer: 効果処理器, settings: TAA設定, inputs: 現在のフレーム入力)。
        void ApplyTemporalAntiAliasing(
            EnvironmentRenderer& renderer,
            const TemporalAntiAliasingSettings& settings,
            const EnvironmentRenderer::TemporalInputs& inputs);
        // 描画先自身の深度で体積光を適用する(renderer: 効果処理器, settings: 体積光設定, inputs: 現在の光源入力)。
        void ApplyVolumetricLight(
            EnvironmentRenderer& renderer,
            const VolumetricLightSettings& settings,
            const EnvironmentRenderer::VolumetricInputs& inputs);
        // 描画先自身の深度と作業資源で被写界深度を適用する(renderer: 効果処理器, settings: 焦点設定, projection: 射影行列, sampleCount: サンプル数)。
        void ApplyDepthOfField(
            EnvironmentRenderer& renderer,
            const DepthOfFieldSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // 前回行列でブラーを適用し、今回行列を保存する(renderer: 効果処理器, settings: ブラー設定, inverseViewProjection: 今回の逆ビュー射影行列, viewProjection: 今回のビュー射影行列, sampleCount: サンプル数)。
        void ApplyMotionBlur(
            EnvironmentRenderer& renderer,
            const MotionBlurSettings& settings,
            const DirectX::XMFLOAT4X4& inverseViewProjection,
            const DirectX::XMFLOAT4X4& viewProjection,
            std::uint32_t sampleCount);
        // カラーをトーン変換して切り替える(renderer: 効果処理器, settings: 色調設定)。
        void ApplyToneMapping(
            EnvironmentRenderer& renderer,
            const ColorGradingSettings& settings);
        // FXAAを適用してカラーを切り替える(renderer: 効果処理器)。
        void ApplyFXAA(EnvironmentRenderer& renderer);
        // 深度から遮蔽を求めて平滑化し、成功を返す(renderer: 遮蔽処理器, settings: 遮蔽設定, projection: 射影行列, sampleCount: サンプル数)。
        [[nodiscard]] bool ResolveAmbientOcclusion(
            EnvironmentRenderer& renderer,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // 描画先の深度を入力して独自効果を適用する(effect: 画面効果, auxiliaryTextures: 補助テクスチャ参照, depthParameters: 深度係数, depthUnprojection: 深度の逆射影係数, parameters: 独自定数配列)。
        void ApplyScreenEffect(
            ScreenEffect& effect,
            const std::array<ID3D11ShaderResourceView*, 2>&
                auxiliaryTextures,
            const DirectX::XMFLOAT4& depthParameters,
            const DirectX::XMFLOAT4& depthUnprojection,
            const std::array<DirectX::XMFLOAT4, 8>& parameters);

        // 現在のHDRカラー資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_colorTexture;
        // 現在のカラー描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
        // 現在のカラー参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_shaderResourceView;
        // 次のポスト処理用カラー資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_postColorTexture;
        // 次のポスト処理描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_postRenderTargetView;
        // 次のポスト処理カラー参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_postShaderResourceView;
        // 表示用の安定した画像資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_displayColorTexture;
        // 表示用の画像参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_displayShaderResourceView;
        // 表示面の計算書込み参照
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_displayUnorderedAccessView;
        // 表示面への計算書込みを許可
        bool m_computeWritable{};
        // 深度ステンシル資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        // 深度ステンシル描画先
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            m_depthStencilView;
        // 深度の読取参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthShaderResourceView;

        // 半解像度の遮蔽資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_occlusionTexture;
        // 遮蔽の描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_occlusionRenderTargetView;
        // 遮蔽の読取参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_occlusionShaderResourceView;
        // 平滑化した遮蔽資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_occlusionBlurTexture;
        // 遮蔽平滑化の描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_occlusionBlurRenderTargetView;
        // 平滑化した遮蔽の参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_occlusionBlurShaderResourceView;
        // 遮蔽資源の幅
        std::uint32_t m_occlusionWidth{};
        // 遮蔽資源の高さ
        std::uint32_t m_occlusionHeight{};

        // 四分の一解像度の筋資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_streakTexture;
        // フレアの筋の描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_streakRenderTargetView;
        // フレアの筋の読取参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_streakShaderResourceView;
        // フレアの筋の作業資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_streakBlurTexture;
        // 筋の平滑化用描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_streakBlurRenderTargetView;
        // 筋の平滑化用読取参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_streakBlurShaderResourceView;
        // フレアの筋資源の幅
        std::uint32_t m_streakWidth{};
        // フレアの筋資源の高さ
        std::uint32_t m_streakHeight{};

        // 半解像度の被写界深度資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_depthOfFieldTexture;
        // 被写界深度の準備描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_depthOfFieldRenderTargetView;
        // 被写界深度の準備参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthOfFieldShaderResourceView;
        // 被写界深度の平滑化資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_depthOfFieldBlurTexture;
        // 被写界深度の平滑化描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_depthOfFieldBlurRenderTargetView;
        // 被写界深度の平滑化参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthOfFieldBlurShaderResourceView;
        // 被写界深度資源の幅
        std::uint32_t m_depthOfFieldWidth{};
        // 被写界深度資源の高さ
        std::uint32_t m_depthOfFieldHeight{};

        // 平均対数輝度の資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_luminanceTexture;
        // 対数輝度の描画先
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_luminanceRenderTargetView;
        // 全ミップの対数輝度参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_luminanceShaderResourceView;
        // CPU読取用の一画素輝度資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_luminanceStagingTexture;
        // 輝度測定資源の幅
        std::uint32_t m_luminanceWidth{};
        // 輝度測定資源の高さ
        std::uint32_t m_luminanceHeight{};
        // 輝度測定資源のミップ段数
        std::uint32_t m_luminanceMipLevels{};
        // 輝度の読取コピー発行済み
        bool m_luminanceStagingReady{};
        // 描画先のビューポート
        D3D11_VIEWPORT m_viewport{};

        // 前フレームのHDR色資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_historyTexture;
        // 前フレームのHDR色参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_historyShaderResourceView;
        // 前フレームのTAA色資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_temporalHistoryTexture;
        // 前フレームのTAA色参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_temporalHistoryShaderResourceView;
        // 反射読取用の複製深度資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthCopyTexture;
        // 反射読取用の複製深度参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_depthCopyShaderResourceView;
        // 反射用のHi-Z深度資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            m_reflectionDepthPyramidTexture;
        // 全ミップのHi-Z深度参照
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_reflectionDepthPyramidView;
        // Hi-Z各段の描画先
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>>
            m_reflectionDepthPyramidTargets;
        // Hi-Z各段の読取参照
        std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>
            m_reflectionDepthPyramidMipViews;


        // 資源を生成したデバイス
        Microsoft::WRL::ComPtr<ID3D11Device> m_ownerDevice;
    };
}
