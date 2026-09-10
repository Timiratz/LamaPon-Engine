#pragma once

// VolumetricInputsがEnvironmentRendererの入れ子型のため、
// 前方宣言では足りずヘッダが必要です。
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>
#include <d3d11.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace LamaPon
{
    namespace Detail
    {
        struct RenderTargetBackendState;
    }

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
        // Litパス中に深度を読むSSR用のコピーです。深度そのものは
        // DSVとして刺さっているため、同じリソースをSRVとしても
        // 読むことはできません（D3D11がSRVを黙ってnullにします）。
        [[nodiscard]] ID3D11ShaderResourceView*
            DepthCopyShaderResourceView() const noexcept;
        // 深度そのもののSRV。ポスト処理のようにDSVを外して描く
        // パスからは、コピーを取らずに直接読めます（同時に刺すと
        // D3D11がSRVを黙ってnullにするので、Litパス中は上の
        // コピーの方を使ってください）。
        [[nodiscard]] GraphicsViewHandle
            DepthViewHandle() const noexcept;
        // SSRのHi-Z用の深度ピラミッド（R32F、各ミップが2x2の
        // 最小値）。中身はEnvironmentRendererの
        // BuildReflectionDepthPyramidが毎フレーム書きます。
        [[nodiscard]] GraphicsViewHandle
            ReflectionDepthPyramidViewHandle() const noexcept;
        [[nodiscard]] std::uint32_t
            ReflectionDepthPyramidMipCount() const noexcept;
        [[nodiscard]] ID3D11RenderTargetView*
            ReflectionDepthPyramidMipTarget(
                std::uint32_t mip) const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            ReflectionDepthPyramidMipView(
                std::uint32_t mip) const noexcept;
        // Compute Shaderから表示用テクスチャへ直接書けるようにする
        // かどうか。Resizeより前に呼んでください（バインド
        // フラグは作成時にしか決められないため）。既定はfalseで、
        // 通常の描画先には余計なフラグを付けません。
        void SetComputeWritable(bool value) noexcept;
        [[nodiscard]] ID3D11UnorderedAccessView*
            DisplayUnorderedAccessView() const noexcept;
        // 表示用テクスチャの実体。中身をCPUへ読み戻して確かめたい
        // ときに使います（Compute Shaderの出力の検査など）。
        [[nodiscard]] ID3D11Texture2D*
            DisplayTexture() const noexcept;
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

        // native stateはBackend単位で丸ごと差し替えます。作成フラグと
        // 公開した履歴行列の参照先だけはRenderTarget自身に残し、既存の
        // noexcept/default/reference semanticsを保ちます。
        std::unique_ptr<Detail::RenderTargetBackendState> m_backendState;
        DirectX::XMFLOAT4X4 m_publicHistoryViewProjection{};
        bool m_computeWritable{};
    };
}
