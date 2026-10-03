#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/SpriteRendering.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    class D3D12Backend;
    class RenderTarget;
    struct AmbientOcclusionSettings;
    struct BloomSettings;
    struct ColorGradingSettings;
    struct DepthOfFieldSettings;
    struct MotionBlurSettings;
    struct ScreenOutlineSettings;
    struct ScreenSpaceLensFlareSettings;
    struct SkySettings;
    struct SkySunDescription;
    struct TemporalAntiAliasingInputs;
    struct TemporalAntiAliasingSettings;
    struct VolumetricLightInputs;
    struct VolumetricLightSettings;
}

namespace LamaPon::Detail
{
    class D3D12SpriteRenderer final
    {
    public:
        // 描画器を作ります(backend: 初期化済みのバックエンド)。
        // backendは描画器より長く生存させます。
        explicit D3D12SpriteRenderer(D3D12Backend& backend);
        // 所有する描画資源とシェーダーを破棄します。
        ~D3D12SpriteRenderer() noexcept;

        // 描画器のコピーを禁止します。
        D3D12SpriteRenderer(const D3D12SpriteRenderer&) = delete;
        // 描画器のコピー代入を禁止します。
        D3D12SpriteRenderer& operator=(
            const D3D12SpriteRenderer&) = delete;

        // パスを開始して識別番号を返します(description: 合成・シェーダー・定数, fallbackTexture: 白の代替画像, assets: 自作シェーダーの取得元, status: 使用シェーダー状態の出力)。
        [[nodiscard]] std::uint64_t Begin(
            const SpritePassDescription& description,
            const GraphicsViewHandle& fallbackTexture,
            AssetManager* assets,
            SpriteShaderStatus& status);
        // パスへ矩形を積めたか返します(token: パス識別番号, request: 画像と位置・色・変形)。
        [[nodiscard]] bool Draw(
            std::uint64_t token,
            const SpriteDrawRequest& request);
        // クリップ範囲を積めたか返します(token: パス識別番号, rectangle: ピクセル座標の矩形)。
        [[nodiscard]] bool PushScissor(
            std::uint64_t token,
            const SpriteClipRectangle& rectangle);
        // 直前のクリップ範囲へ戻せたか返します(token: パス識別番号)。
        [[nodiscard]] bool PopScissor(std::uint64_t token);
        // 待機中の矩形を送信して終了します(token: パス識別番号)。
        void End(std::uint64_t token);
        // 可能な範囲で送信し、例外を外へ出さず終了します(token: パス識別番号)。
        void Abort(std::uint64_t token) noexcept;
        // 現在の出力全体へ合成します(texture: 処理済みシーン画像, fallbackTexture: 無効な入力の代替画像)。
        void CompositeScene(
            const GraphicsViewHandle& texture,
            const GraphicsViewHandle& fallbackTexture);

        // ACES近似と色調補正を適用します(target: HDRの描画先, fallbackTexture: 代替画像, colorGrading: 色調補正の設定)。
        void ApplyToneMapping(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const ColorGradingSettings& colorGrading);
        // 高輝度部を9点でぼかします(target: HDRの描画先, fallbackTexture: 代替画像, settings: ブルームの設定)。
        void ApplyBloom(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const BloomSettings& settings);
        // 光条・ゴースト・ハローを合成します(target: HDRの描画先, fallbackTexture: 代替画像, settings: レンズフレアの設定)。
        void ApplyScreenSpaceLensFlare(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const ScreenSpaceLensFlareSettings& settings);
        // 輝度の縁を検出して平滑化します(target: 処理する描画先, fallbackTexture: 代替画像)。
        void ApplyFXAA(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture);
        // 深度で再投影した履歴を混ぜます(target: 処理する描画先, fallbackTexture: 代替画像, settings: TAAの設定, inputs: 履歴と再投影行列)。
        void ApplyTemporalAntiAliasing(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const TemporalAntiAliasingSettings& settings,
            const TemporalAntiAliasingInputs& inputs);
        // 影を参照して光の筋を合成します(target: HDRの描画先, fallbackTexture: 代替画像, settings: 光の筋の設定, inputs: カメラと光源・影情報)。
        void ApplyVolumetricLight(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const VolumetricLightSettings& settings,
            const VolumetricLightInputs& inputs);
        // 深度と再構成法線から輪郭を合成します(target: 処理する描画先, fallbackTexture: 代替画像, settings: 輪郭の設定, projection: 深度復元の射影行列)。
        void ApplyScreenOutline(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        // 前フレームとの差に沿って色を平均します(target: HDRの描画先, fallbackTexture: 代替画像, settings: 動きぼかしの設定, inverseViewProjection: 世界座標を復元する逆行列, previousViewProjection: 前フレームの射影行列, sampleCount: 採取点数)。
        void ApplyMotionBlur(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const MotionBlurSettings& settings,
            const DirectX::XMFLOAT4X4& inverseViewProjection,
            const DirectX::XMFLOAT4X4& previousViewProjection,
            std::uint32_t sampleCount);
        // 焦点帯の外を深度に応じてぼかします(target: HDRの描画先, fallbackTexture: 代替画像, settings: 被写界深度の設定, projection: 深度復元の射影行列, sampleCount: 採取点数)。
        void ApplyDepthOfField(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const DepthOfFieldSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // 半解像度の遮蔽率を生成します(target: 深度入力と遮蔽率の出力先, fallbackTexture: 代替画像, settings: 遮蔽の設定, projection: 深度復元の射影行列, sampleCount: 採取点数)。
        // 距離を復元できなければfalseで、終了後は深度専用の描画先へ戻します。
        [[nodiscard]] bool ResolveAmbientOcclusion(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // SSR用の距離最小値の全ミップを生成します(target: 深度入力と距離の出力先, fallbackTexture: 代替画像, projectionZ: 射影行列の深度係数, projectionW: 射影行列の距離係数)。
        // 終了後は開始前の深度専用またはカラー描画先へ戻します。
        [[nodiscard]] bool BuildReflectionDepthPyramid(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            float projectionZ,
            float projectionW);
        // 対数輝度を縮小して1画素へ平均します(target: HDR入力と輝度の出力先, fallbackTexture: 代替画像)。
        // 終了後はtargetの通常の描画先へ戻します。
        void MeasureLuminance(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture);
        // 画面効果を準備します(assets: 取得元, shaderPath: シェーダーのパス, describeFailure: 失敗文の整形関数, generation: 世代番号の任意出力, error: 失敗理由の任意出力)。
        // 250ms間隔で保存を確認し、コンパイルに失敗した場合は直前の正常版を保持します。
        [[nodiscard]] bool PrepareScreenEffect(
            AssetManager& assets,
            const std::filesystem::path& shaderPath,
            const std::function<std::string(const char*)>& describeFailure,
            std::uint64_t* generation,
            std::string* error);
        // b0とt0～t3で画面効果を適用します(target: 処理する描画先, fallbackTexture: 代替画像, assets: 取得元, shaderPath: シェーダーのパス, auxiliaryTextures: t1・t2の追加入力, parameters: b0の8本の定数, depthParameters: 深度復元と有効状態, depthUnprojection: 深度の距離復元係数)。
        void ApplyScreenEffect(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            AssetManager& assets,
            const std::filesystem::path& shaderPath,
            const std::array<GraphicsViewHandle, 2>& auxiliaryTextures,
            const std::array<DirectX::XMFLOAT4, 8>& parameters,
            const DirectX::XMFLOAT4& depthParameters,
            const DirectX::XMFLOAT4& depthUnprojection);
        // 次回の強制再読込を要求します(assets: パスの解決元, shaderPath: シェーダーのパス)。
        void InvalidateScreenEffect(
            AssetManager& assets,
            const std::filesystem::path& shaderPath) noexcept;
        // 深度を読まず書かず空と太陽を描きます(view: ビュー行列, projection: 射影行列, settings: 空の設定, cubemap: 入力キューブ, sun: 太陽情報で空なら表示なし, fallbackTexture: 代替画像)。
        // 深度専用なら描画せず、無効なキューブはグラデーションへ戻します。
        void DrawSky(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const SkySettings& settings,
            const GraphicsViewHandle& cubemap,
            const SkySunDescription* sun,
            const GraphicsViewHandle& fallbackTexture);

    private:
        // quadを塗るpixel shaderです。
        // None以外は出力全体へ1枚を描くfullscreen pass用で、root constants（b1）の意味もshaderごとに異なります。
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
            AmbientOcclusionBlur,
            VolumetricLight,
            LensFlareStreak,
            LensFlareComposite,
            ReflectionDepthLinearize,
            ReflectionDepthDownsample,
            Sky
        };

        // 画面全体の描画プログラム数
        static constexpr std::size_t FullscreenProgramCount = 17u;
        // 深度あり・なしの種類数
        static constexpr std::size_t DepthFormatVariants = 2u;
        // 出力形式の種類数
        static constexpr std::size_t ColorFormatVariants = 4u;
        // 合成4種とクリップ2種
        static constexpr std::size_t BlendVariants = 8u;

        struct Vertex final
        {
            // 画面上の頂点位置
            DirectX::XMFLOAT3 position{};
            // RGBAの頂点色
            DirectX::XMFLOAT4 color{};
            // 画像のUV座標
            DirectX::XMFLOAT2 textureCoordinate{};
        };

        struct QueuedSprite final
        {
            // 矩形の4頂点
            std::array<Vertex, 4> vertices{};
            // 入力画像のGPU記述子
            D3D12_GPU_DESCRIPTOR_HANDLE texture{};
            // 命令記録まで保持する入力ビュー
            GraphicsViewHandle view;
        };

        struct CustomShaderEntry final
        {
            // 自作の画素シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
            // 深度・出力・合成別のPSO
            std::array<
                Microsoft::WRL::ComPtr<ID3D12PipelineState>,
                DepthFormatVariants
                    * ColorFormatVariants
                    * BlendVariants>
                pipelineStates;
            // シェーダーの世代番号
            std::uint64_t generation{};
        };

        struct ScreenShaderEntry final
        {
            // 画面効果の頂点シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
            // 画面効果の画素シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
            // 出力形式別のPSO
            std::array<
                Microsoft::WRL::ComPtr<ID3D12PipelineState>,
                ColorFormatVariants>
                pipelineStates;
            // シェーダーの世代番号
            std::uint64_t generation{};
            // 準備の失敗理由
            std::string error;

            // 次の保存時刻確認
            std::chrono::steady_clock::time_point nextCheck{};
            // 前回確認した保存時刻
            std::filesystem::file_time_type writeTime{};
            // 保存状態の確認済み
            bool observed{};
            // 次回の強制再読込
            bool forceReload{};
            // 前回の元ファイル存在
            bool sourceExists{};
        };

        struct ScreenEffectConstants final
        {
            // b0の自作シェーダー引数
            std::array<DirectX::XMFLOAT4, 8> parameters{};
            // xy=画面寸法、zw=逆数
            DirectX::XMFLOAT4 screenSize{};
            // 深度復元と有効状態
            DirectX::XMFLOAT4 depthParameters{};
            // 深度の距離復元係数
            DirectX::XMFLOAT4 depthUnprojection{};
        };
        static_assert(sizeof(ScreenEffectConstants) == 176u);

        // VolumetricBufferと同じ384バイトで、ルート定数の上限を超えるためCBVで渡します。
        struct VolumetricPassConstants final
        {
            // 世界座標復元の逆行列
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // 視点XYZと光の筋の最大距離
            DirectX::XMFLOAT4 cameraPosition{};
            // 光源方向XYZと採取点数
            DirectX::XMFLOAT4 lightDirection{};
            // 強度付き光源RGBと散乱係数
            DirectX::XMFLOAT4 lightColor{};
            // カスケードの影行列
            std::array<DirectX::XMFLOAT4X4, 4> cascades{};
            // 影の段数・バイアス・画素幅
            DirectX::XMFLOAT4 shadowParameters{};
        };
        static_assert(sizeof(VolumetricPassConstants) == 384u);

        // 現在のパス番号か検証します(token: 操作するパス識別番号)。
        void RequireOwner(std::uint64_t token) const;
        // 待機中の矩形を現在の出力へ送信します。
        void Flush();
        // 送信に失敗したパスを閉じ、再初期化まで新規パスを拒否します。
        void FlushOrFail();
        // パスの参照と送信待ちの矩形を解放します。
        void ClearPass() noexcept;
        // 出力全体へ画像を描きます(texture: 主入力画像, fallbackTexture: 代替画像, program: 描画プログラム, constants: b1の16値, auxiliaryViews: 3本の追加入力, matrixConstants: b2の32値)。
        void DrawFullscreen(
            const GraphicsViewHandle& texture,
            const GraphicsViewHandle& fallbackTexture,
            FullscreenProgram program,
            const std::array<float, 16>& constants,
            const std::array<GraphicsViewHandle, 3>& auxiliaryViews = {},
            const std::array<float, 32>& matrixConstants = {});
        // 入力色へ処理を適用して出力を交換します(target: 処理する描画先, fallbackTexture: 代替画像, program: 描画プログラム, constants: b1の16値)。
        // 失敗時は画像を交換せず、元の描画先へ戻します。
        void ApplyPostProcessPass(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            FullscreenProgram program,
            const std::array<float, 16>& constants);
        // 状態に合うPSOを取得・生成します(blend: 合成方式, scissored: クリップ使用, colorFormat: カラー出力形式, depthFormat: 深度出力形式, program: 描画プログラム)。
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            SpriteBlendMode blend,
            bool scissored,
            DXGI_FORMAT colorFormat,
            DXGI_FORMAT depthFormat,
            FullscreenProgram program);
        // 画面効果のPSOを取得・生成します(shader: 準備済みシェーダー, colorFormat: カラー出力形式)。
        [[nodiscard]] ID3D12PipelineState* ScreenEffectPipelineState(
            ScreenShaderEntry& shader,
            DXGI_FORMAT colorFormat);

        // 借用する描画バックエンド
        D3D12Backend* m_backend{};
        // スプライトのルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        // 画面効果のルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature>
            m_screenEffectRootSignature;
        // 矩形の頂点シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        // 既定の画素シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        // 色調変換の画素シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_toneMapPixelShader;
        // 高輝度ぼかしのシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_bloomPixelShader;
        // 輪郭平滑化のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_fxaaPixelShader;
        // 対数輝度のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_luminancePixelShader;
        // TAAの画素シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_temporalPixelShader;
        // 画面輪郭のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_screenOutlinePixelShader;
        // 動きぼかしのシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_motionBlurPixelShader;
        // 被写界深度のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_depthOfFieldPixelShader;
        // 遮蔽率のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_ambientOcclusionPixelShader;
        // 遮蔽率ぼかしのシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_ambientOcclusionBlurPixelShader;
        // 光の筋を描くシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_volumetricLightPixelShader;
        // 光条のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_lensFlareStreakPixelShader;
        // レンズフレア合成のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_lensFlareCompositePixelShader;
        // SSR距離復元のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob>
            m_reflectionDepthLinearizePixelShader;
        // SSR最小深度のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob>
            m_reflectionDepthDownsamplePixelShader;
        // 空と太陽のシェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_skyPixelShader;
        // 出力・深度・合成・クリップ別PSO
        std::array<
            Microsoft::WRL::ComPtr<ID3D12PipelineState>,
            FullscreenProgramCount
                * DepthFormatVariants
                * ColorFormatVariants
                * BlendVariants>
            m_pipelineStates;
        // 自作スプライトのキャッシュ
        std::unordered_map<std::filesystem::path, CustomShaderEntry>
            m_customShaders;
        // 画面効果のキャッシュ
        std::unordered_map<std::filesystem::path, ScreenShaderEntry>
            m_screenShaders;
        // パスで使用中の自作シェーダー
        CustomShaderEntry* m_activeCustomShader{};
        // 次の自作シェーダー世代
        std::uint64_t m_nextCustomShaderGeneration{ 1 };
        // 次の画面効果の世代
        std::uint64_t m_nextScreenShaderGeneration{ 1 };
        // 矩形の共通頂点番号
        Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
        // 未指定入力の代替画像
        GraphicsViewHandle m_fallbackTexture;
        // 送信待ちの矩形列
        std::vector<QueuedSprite> m_sprites;
        // 積まれたクリップ範囲
        std::vector<D3D12_RECT> m_scissorStack;
        // パスの合成方式
        SpriteBlendMode m_blend{ SpriteBlendMode::NonPremultiplied };
        // b1のパス定数
        std::array<float, 16> m_passConstants{};
        // b2の行列定数
        std::array<float, 32> m_matrixConstants{};
        // 保持する追加入力ビュー
        std::array<GraphicsViewHandle, 3> m_auxiliaryViews;
        // 追加入力のGPU記述子
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> m_auxiliaryTextures{};
        // 光の筋のb3定数
        VolumetricPassConstants m_volumetricConstants{};
        // b0の自作シェーダー引数
        std::array<DirectX::XMFLOAT4, 8> m_customParameters{};
        // スプライトのb1照明情報
        Sprite2DLighting m_spriteLighting{};
        // 使用中の描画プログラム
        FullscreenProgram m_program{ FullscreenProgram::None };
        // 使用中のパス識別番号
        std::uint64_t m_activeToken{};
        // 次のパス識別番号
        std::uint64_t m_nextToken{ 1 };
        // 再初期化が必要な失敗状態
        bool m_failed{};
    };
}
