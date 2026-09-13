#pragma once

// DirectX 12 ExperimentalでSpriteRenderPassを描く既定pipelineです。D3D11の
// DirectXTK SpriteBatchと同じ座標・UV・blend規則で、Deferred順のquadを
// 現在のprimary / offscreen outputへ送ります。Runtime内部headerで、
// SDKにはinstallしません。
#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/SpriteRendering.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LamaPon
{
    class D3D12Backend;
    class RenderTarget;
    struct AmbientOcclusionSettings;
    struct BloomSettings;
    struct ColorGradingSettings;
    struct DepthOfFieldSettings;
    struct MotionBlurSettings;
    struct ScreenOutlineSettings;
    struct TemporalAntiAliasingInputs;
    struct TemporalAntiAliasingSettings;
}

namespace LamaPon::Detail
{
    class D3D12SpriteRenderer final
    {
    public:
        explicit D3D12SpriteRenderer(D3D12Backend& backend);
        ~D3D12SpriteRenderer() noexcept;

        D3D12SpriteRenderer(const D3D12SpriteRenderer&) = delete;
        D3D12SpriteRenderer& operator=(
            const D3D12SpriteRenderer&) = delete;

        // passを開始してtokenを返します。custom pixel shaderはまだD3D12
        // pipelineを持たないため、既定pipelineで描いて理由をstatusへ返します。
        [[nodiscard]] std::uint64_t Begin(
            const SpritePassDescription& description,
            const GraphicsViewHandle& fallbackTexture,
            SpriteShaderStatus& status);
        [[nodiscard]] bool Draw(
            std::uint64_t token,
            const SpriteDrawRequest& request);
        [[nodiscard]] bool PushScissor(
            std::uint64_t token,
            const SpriteClipRectangle& rectangle);
        [[nodiscard]] bool PopScissor(std::uint64_t token);
        void End(std::uint64_t token);
        // DirectXTKのSpriteBatchと同じく、積んだSpriteを可能な範囲で描いて
        // passを閉じます。例外は外へ出しません。
        void Abort(std::uint64_t token) noexcept;
        // post-process済みのScene textureを現在の出力全体へ転写します。
        // GraphicsDeviceの最終合成専用です。
        void CompositeScene(
            const GraphicsViewHandle& texture,
            const GraphicsViewHandle& fallbackTexture);

        // 以下はGraphicsDeviceのpost-process専用です。offscreen targetの
        // current colorを読んでpost colorへ書き、両者を交換します。
        // D3D11のPSToneMapと同じカラーグレーディングとACES近似です。
        void ApplyToneMapping(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const ColorGradingSettings& colorGrading);
        // D3D11のPSBloomと同じ9tapで高輝度部を滲ませます。
        void ApplyBloom(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const BloomSettings& settings);
        // D3D11のPSFXAAと同じ輝度の縁検出で輪郭を平滑化します。
        void ApplyFXAA(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture);
        // 前フレームの解決済みカラーと現在深度を使って再投影し、
        // 近傍クランプ後の履歴をcurrent colorへ混ぜます。
        void ApplyTemporalAntiAliasing(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const TemporalAntiAliasingSettings& settings,
            const TemporalAntiAliasingInputs& inputs);
        // 深度の距離差と再構成法線から輪郭を検出し、current colorへ
        // 指定色を重ねます。
        void ApplyScreenOutline(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        // 深度から現在のworld位置を戻し、前フレームの射影位置との差に
        // 沿ってHDR colorを平均します。
        void ApplyMotionBlur(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const MotionBlurSettings& settings,
            const DirectX::XMFLOAT4X4& inverseViewProjection,
            const DirectX::XMFLOAT4X4& previousViewProjection,
            std::uint32_t sampleCount);
        // main pass深度からCoCを求め、焦点帯の外側を深度対応の円形
        // サンプリングでぼかします。
        void ApplyDepthOfField(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const DepthOfFieldSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // D3D11のRenderAmbientOcclusion / BlurAmbientOcclusionと同じく、
        // 深度コピーから半解像度の遮蔽を求めて深度対応のブラーを掛け、
        // AmbientOcclusionViewHandleへ書きます。射影から距離を戻せない
        // 場合は何もせずfalseを返します。終了後は深度専用の描画先へ戻します。
        [[nodiscard]] bool ResolveAmbientOcclusion(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // 自動露出用に、current colorの対数輝度を1/4解像度で書いてから
        // 2x2平均で1x1まで縮めます。D3D11のPSLuminanceとGenerateMipsに
        // 相当し、終了後はtargetの通常の描画先へ戻します。
        void MeasureLuminance(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture);

    private:
        // quadを塗るpixel shaderです。None以外は出力全体へ1枚を描く
        // fullscreen pass用で、root constants（b1）の意味もshaderごとに
        // 異なります。
        enum class FullscreenProgram : std::uint8_t
        {
            None,
            ToneMap,
            Bloom,
            Fxaa,
            Luminance,
            Temporal,
            ScreenOutline,
            MotionBlur,
            DepthOfField,
            AmbientOcclusion,
            AmbientOcclusionBlur
        };

        // PSOの組み合わせ数です。深度はprimary / 無し、出力はRGBA8 /
        // RGBA16F / R8、blendは4種と通常 / scissor passの2通りです。
        static constexpr std::size_t FullscreenProgramCount = 11u;
        static constexpr std::size_t DepthFormatVariants = 2u;
        static constexpr std::size_t ColorFormatVariants = 3u;
        static constexpr std::size_t BlendVariants = 8u;

        struct Vertex final
        {
            DirectX::XMFLOAT3 position{};
            DirectX::XMFLOAT4 color{};
            DirectX::XMFLOAT2 textureCoordinate{};
        };

        struct QueuedSprite final
        {
            std::array<Vertex, 4> vertices{};
            D3D12_GPU_DESCRIPTOR_HANDLE texture{};
            // 描画を記録するまでviewを保持します。記録後はBackendの遅延
            // 解放がGPU完了まで実体とdescriptorを守ります。
            GraphicsViewHandle view;
        };

        void RequireOwner(std::uint64_t token) const;
        void Flush();
        // 描画送信に失敗したpassを閉じ、再初期化まで新しいpassを拒否します。
        void FlushOrFail();
        void ClearPass() noexcept;
        // 現在の出力全体へtextureを1枚、指定shaderで描きます。
        void DrawFullscreen(
            const GraphicsViewHandle& texture,
            const GraphicsViewHandle& fallbackTexture,
            FullscreenProgram program,
            const std::array<float, 16>& constants,
            const std::array<GraphicsViewHandle, 2>& auxiliaryViews = {},
            const std::array<float, 32>& matrixConstants = {});
        // targetのcurrent colorを入力にしたfullscreen passをpost colorへ
        // 書いて交換します。失敗時は交換せず元の出力へ戻します。
        void ApplyPostProcessPass(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            FullscreenProgram program,
            const std::array<float, 16>& constants);
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            SpriteBlendMode blend,
            bool scissored,
            DXGI_FORMAT colorFormat,
            DXGI_FORMAT depthFormat,
            FullscreenProgram program);

        D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_toneMapPixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_bloomPixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_fxaaPixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_luminancePixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_temporalPixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_screenOutlinePixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_motionBlurPixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_depthOfFieldPixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_ambientOcclusionPixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_ambientOcclusionBlurPixelShader;
        // pixel shader、深度format、出力format、blend modeごとに、通常pass
        // とscissor passのcull違いを持ちます。
        std::array<
            Microsoft::WRL::ComPtr<ID3D12PipelineState>,
            FullscreenProgramCount
                * DepthFormatVariants
                * ColorFormatVariants
                * BlendVariants>
            m_pipelineStates;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
        GraphicsViewHandle m_fallbackTexture;
        std::vector<QueuedSprite> m_sprites;
        std::vector<D3D12_RECT> m_scissorStack;
        SpriteBlendMode m_blend{ SpriteBlendMode::NonPremultiplied };
        std::array<float, 16> m_passConstants{};
        std::array<float, 32> m_matrixConstants{};
        std::array<GraphicsViewHandle, 2> m_auxiliaryViews;
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 2> m_auxiliaryTextures{};
        FullscreenProgram m_program{ FullscreenProgram::None };
        std::uint64_t m_activeToken{};
        std::uint64_t m_nextToken{ 1 };
        bool m_failed{};
    };
}
