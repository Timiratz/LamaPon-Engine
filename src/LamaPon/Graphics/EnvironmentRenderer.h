#pragma once

#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/PrefilteredEnvironment.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace LamaPon
{
    class AssetManager;
    class D3D11Backend;
    class GraphicsDevice;
    class RenderTarget;

    class EnvironmentRenderer final
    {
        // 生ビューは対応する描画の完了まで保持し、ポスト処理の入力と出力には別の画像を渡す。
    public:
        // 所有するシェーダー・定数・プローブ資源を解放する。
        ~EnvironmentRenderer();

        // 描画器のコピーを禁止する。
        EnvironmentRenderer(const EnvironmentRenderer&) = delete;
        // 描画器のコピー代入を禁止する。
        EnvironmentRenderer& operator=(
            const EnvironmentRenderer&) = delete;

        // 空に描く太陽の方向・色・角半径。
        struct SkySun final
        {
            // 太陽へ向かうワールド方向
            DirectX::XMFLOAT3 directionToSun{ 0.0f, 1.0f, 0.0f };
            // 強度を含む太陽のRGB色
            DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
            // 太陽円盤の角半径のラジアン
            float angularRadius{ 0.004625f };
        };

    private:

        // 旧Game Moduleを読み込んでAPI不一致を案内するため、互換シンボルを保持する。
        // 設定中の描画先へ空を描く(view: ビュー行列, projection: 射影行列, settings: 空の設定, cubemap: 空なら色の勾配, sun: 空なら太陽円盤なし)。
        void DrawSky(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const SkySettings& settings,
            ID3D11ShaderResourceView* cubemap = nullptr,
            const SkySun* sun = nullptr);

    public:
        // ブルームを合成し、無効時は強度ゼロで描く(source: 入力SRV, destination: 別画像のRTV, width: 出力幅, height: 出力高, settings: ブルーム設定)。
        void ApplyBloom(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const BloomSettings& settings);
        // 深度と法線差の輪郭を合成する(source: 入力SRV, depth: シーン深度, destination: 別画像のRTV, width: 出力幅, height: 出力高, settings: 輪郭設定, projection: 画像の射影行列)。
        void ApplyScreenOutline(
            ID3D11ShaderResourceView* source,
            ID3D11ShaderResourceView* depth,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        // 光条を3パスで作り、結果をLastLensFlareStreakResourceで返す(source: 元の画像, firstTarget: 作業先1, firstResource: 作業先1のSRV, secondTarget: 作業先2, secondResource: 作業先2のSRV, width: 1/4画像幅, height: 1/4画像高, settings: 光条設定)。
        void BuildLensFlareStreaks(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* firstTarget,
            ID3D11ShaderResourceView* firstResource,
            ID3D11RenderTargetView* secondTarget,
            ID3D11ShaderResourceView* secondResource,
            std::uint32_t width,
            std::uint32_t height,
            const ScreenSpaceLensFlareSettings& settings);
        // 直前の光条結果を借用し、処理を省いた場合は空を返す。
        [[nodiscard]] ID3D11ShaderResourceView*
            LastLensFlareStreakResource() const noexcept
        {
            return m_lastStreakResource;
        }

        // レンズフレアと光条を合成する(source: 入力SRV, destination: 別画像のRTV, width: 出力幅, height: 出力高, settings: レンズフレア設定, streak: 空なら光条なし)。
        void ApplyScreenSpaceLensFlare(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const ScreenSpaceLensFlareSettings& settings,
            ID3D11ShaderResourceView* streak = nullptr);
        // HDRの色を補正し、トーン無効時はコピーする(source: 入力SRV, destination: 別画像のRTV, width: 出力幅, height: 出力高, settings: トーンと色の設定)。
        void ApplyToneMapping(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const ColorGradingSettings& settings);
        // 画素の輪郭を平滑化する(source: 入力SRV, destination: 別画像のRTV, width: 出力幅, height: 出力高)。
        void ApplyFXAA(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height);
        // 深度から遮蔽を描き、描画しなければ偽を返す(depth: シーン深度, destination: 半解像度のRTV, width: 遮蔽画像幅, height: 遮蔽画像高, settings: AO設定, projection: 透視射影行列, sampleCount: 4～32に補正する採取数)。
        [[nodiscard]] bool RenderAmbientOcclusion(
            ID3D11ShaderResourceView* depth,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // 直前のAO定数を使って遮蔽をぼかす(occlusion: 遮蔽SRV, depth: シーン深度, destination: 別の半解像度RTV, width: 出力幅, height: 出力高)。
        void BlurAmbientOcclusion(
            ID3D11ShaderResourceView* occlusion,
            ID3D11ShaderResourceView* depth,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height);
        // カスケード影から光の筋を求めるための入力。
        struct VolumetricInputs final
        {
            // 保持するシーン深度ビュー
            GraphicsViewHandle depth;
            // 保持するカスケード影ビュー
            GraphicsViewHandle cascadeShadow;
            // 現在の逆ビュー射影行列
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // ワールド座標でのカメラ位置
            DirectX::XMFLOAT3 cameraPosition{};
            // 光源から出るワールド方向
            DirectX::XMFLOAT3 lightDirection{};
            // 平行光のRGB色
            DirectX::XMFLOAT3 lightColor{ 1.0f, 1.0f, 1.0f };
            // 4段の影のビュー射影行列
            std::array<DirectX::XMFLOAT4X4, 4>
                cascadeViewProjections{};
            // 有効な影カスケードの数
            std::uint32_t cascadeCount{};
            // 影判定に加えるバイアス
            float shadowBias{ 0.002f };
            // 影画像の一辺の画素数
            float shadowResolution{ 2048.0f };
        };
        // 履歴は描画ビューごとに保持し、再投影行列にはジッターを含めない。
        struct TemporalInputs final
        {
            // 保持する解決済み色履歴
            GraphicsViewHandle history;
            // 保持する現在のシーン深度
            GraphicsViewHandle depth;
            // 現在のジッターなし逆行列
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // 履歴へ保存する現在の行列
            DirectX::XMFLOAT4X4 viewProjection{};
            // このビューの前フレーム行列
            DirectX::XMFLOAT4X4 previousViewProjection{};
            // 前フレームの履歴が有効か
            bool previousValid{};
        };
        // 履歴と深度を検証してTAAを解決し、描画したときだけ真を返す(source: 現在色, destination: 別画像のRTV, width: 出力幅, height: 出力高, settings: TAA設定, inputs: ビューごとの履歴と行列)。
        bool ApplyTemporalAntiAliasing(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const TemporalAntiAliasingSettings& settings,
            const TemporalInputs& inputs);

        // 深度と影を検証して光の筋を合成し、描画したときだけ真を返す(source: 現在色, destination: 別画像のRTV, width: 出力幅, height: 出力高, settings: 散乱設定, inputs: 深度・平行光・影)。
        bool ApplyVolumetricLight(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const VolumetricLightSettings& settings,
            const VolumetricInputs& inputs);

        // 被写界深度用の深度参照と半解像度の作業画像。
        struct DepthOfFieldInputs final
        {
            // 借用するメイン描画の深度
            ID3D11ShaderResourceView* depth{};
            // 画像を描いた透視射影行列
            DirectX::XMFLOAT4X4 projection{};
            // 色とぼけ量を準備する描画先
            ID3D11RenderTargetView* prepareTarget{};
            // 準備した色とぼけ量のSRV
            ID3D11ShaderResourceView* prepareResource{};
            // 半解像度のぼかし描画先
            ID3D11RenderTargetView* blurTarget{};
            // ぼかした半解像度のSRV
            ID3D11ShaderResourceView* blurResource{};
            // 作業画像の幅
            std::uint32_t halfWidth{};
            // 作業画像の高さ
            std::uint32_t halfHeight{};
            // ぼかしのサンプル数
            std::uint32_t sampleCount{ 22 };
        };
        // 準備・ぼかし・合成を行い、描画したときだけ真を返す(source: 現在色, destination: 別画像のRTV, width: 出力幅, height: 出力高, settings: 被写界深度設定, inputs: 深度・透視射影・作業画像)。
        [[nodiscard]] bool ApplyDepthOfField(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const DepthOfFieldSettings& settings,
            const DepthOfFieldInputs& inputs);

        // ビューごとの前フレーム行列と、ジッターなしの現フレーム逆行列を渡す。
        struct MotionBlurInputs final
        {
            // 借用する現在のシーン深度
            ID3D11ShaderResourceView* depth{};
            // 現在のジッターなし逆行列
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // このビューの前フレーム行列
            DirectX::XMFLOAT4X4 previousViewProjection{};
            // 前フレーム行列が有効か
            bool previousValid{};
            // ブレの方向に沿った採取数
            std::uint32_t sampleCount{ 8 };
        };
        // 前フレームへ再投影してブレを合成し、描画したときだけ真を返す(source: 現在色, destination: 別画像のRTV, width: 出力幅, height: 出力高, settings: ブレの設定, inputs: 深度とビューごとの行列)。
        [[nodiscard]] bool ApplyMotionBlur(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const MotionBlurSettings& settings,
            const MotionBlurInputs& inputs);

        // 対数輝度を描いて1画素まで平均するミップを生成する(source: 入力SRV, destination: 測定段0のRTV, resource: 測定画像の全ミップSRV, width: 1/4画像幅, height: 1/4画像高)。
        void RenderLuminance(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            ID3D11ShaderResourceView* resource,
            std::uint32_t width,
            std::uint32_t height);

        // 描画先とビューポートを設定して画像をコピーする(source: 入力SRV, destination: 別画像のRTV, destinationWidth: 出力幅, destinationHeight: 出力高)。
        void Copy(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t destinationWidth,
            std::uint32_t destinationHeight);
        // 描画先とビューポートを維持して画像をコピーする(source: 描画先と異なる入力SRV)。
        void CopyToBoundRenderTarget(
            ID3D11ShaderResourceView* source);

        // 鏡面環境の一辺の画素数
        static constexpr std::uint32_t PrefilteredSpecularSize = 128;
        // 鏡面環境のミップ段数
        static constexpr std::uint32_t PrefilteredSpecularMipLevels = 8;
        // 拡散環境の一辺の画素数
        static constexpr std::uint32_t PrefilteredIrradianceSize = 16;
        // 拡散環境のミップ段数
        static constexpr std::uint32_t PrefilteredIrradianceMipLevels = 1;

        struct PrefilteredEnvironment final
        {
            // 借用する粗さ別の鏡面キューブ
            ID3D11ShaderResourceView* specular{};
            // 借用する拡散照明キューブ
            ID3D11ShaderResourceView* irradiance{};
            // 鏡面キューブの最終ミップ番号
            float specularMaximumMip{};
        };

        // 呼び出し側が保持するプローブ用の鏡面・拡散画像。
        struct OwnedPrefilteredEnvironment final
        {
            // 保持する粗さ別の鏡面キューブ
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView> specular;
            // 保持する拡散照明キューブ
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView> irradiance;
            // 鏡面キューブの最終ミップ番号
            float specularMaximumMip{};

            // 鏡面と拡散の両方の画像を保持しているか返す。
            [[nodiscard]] bool IsValid() const noexcept
            {
                return specular != nullptr
                    && irradiance != nullptr;
            }
        };

        // プローブ各面の一辺の画素数
        static constexpr std::uint32_t ProbeBakeFaceSize =
            EnvironmentProbeBakeFaceSize;
        using ProbeFaceRenderer = EnvironmentProbeFaceRenderer;

    private:
        // ベイクの再入は禁止し、呼び出し側が元の描画先を例外時も復元する。
        // 6面を同期描画しRGB各4個のSH係数を返す(renderFace: HDR描画だけを行う各面のコールバック)。
        [[nodiscard]] std::optional<std::array<float, 12>>
            BakeIrradianceProbe(
                const ProbeFaceRenderer& renderFace);

        // 深度を距離へ変換して最小値ピラミッドを作り、主描画先とビューポートを戻す(target: 同じ機器の描画先, projectionZ: 射影33, projectionW: 射影43)。
        void BuildReflectionDepthPyramid(
            RenderTarget& target,
            float projectionZ,
            float projectionW);

    private:
        friend class GraphicsDevice;

        // 環境描画のシェーダーと資源を生成する(device: 寿命まで借用する機器, context: 寿命まで借用する描画先, assets: 読み込み元, shaderPath: 環境HLSLのパス)。
        EnvironmentRenderer(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);

        // 中立ビューの解決先を借用する(backend: 描画器の寿命まで有効なD3D11機器)。
        void AttachD3D11Backend(D3D11Backend* backend) noexcept
        {
            m_backend = backend;
        }


        // 元の参照と鍵の両方が一致する場合だけ再利用し、次の生成や描画器の破棄で借用結果が無効になる。
        // スカイ用の鏡面・拡散画像を生成して借用する(source: 元のキューブSRV, cacheKey: 0ならディスク保存なし)。
        [[nodiscard]] PrefilteredEnvironment
            GetPrefilteredEnvironment(
                ID3D11ShaderResourceView* source,
                std::uint64_t cacheKey = 0);


        // 未生成なら、6面のベイクで再利用するHDR・深度・作業画像を生成する。
        void PrepareProbeBake();
        // ベイクの再入は禁止し、呼び出し側が元の描画先を例外時も復元する。
        // 6面を同期描画し所有する鏡面・拡散画像を返す(renderFace: HDR描画だけを行う各面のコールバック, cacheKey: 未指定ならディスク保存なし)。
        [[nodiscard]] OwnedPrefilteredEnvironment
            BakeReflectionProbe(
                const ProbeFaceRenderer& renderFace,
                std::optional<std::uint64_t> cacheKey =
                    std::nullopt);

        // 描画状態を復元してキューブを畳み込み、元画像がなければ空を返す(source: 元のキューブSRV, includeSpecular: 鏡面画像も生成するか)。
        [[nodiscard]] OwnedPrefilteredEnvironment
            CreatePrefilteredEnvironment(
                ID3D11ShaderResourceView* source,
                bool includeSpecular = true);
        // 6面を消去・同期描画して左右反転し、最後に描画先を解除する(renderFace: 0～5の各面を1回描くコールバック)。
        void RenderProbeCube(
            const ProbeFaceRenderer& renderFace);
        // RGBA16Fキューブを同期読み戻ししてSH係数を出力する(irradiance: 拡散照明キューブ, coefficients: RGB各xyz・定数の12要素)。
        [[nodiscard]] bool ProjectIrradianceToSh(
            ID3D11ShaderResourceView* irradiance,
            std::array<float, 12>& coefficients);
        // 右手系の面をD3Dのキューブ規約へ合わせて左右反転する(source: 入力SRV, destination: 別画像のRTV, destinationWidth: 出力幅, destinationHeight: 出力高)。
        void CopyMirroredX(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t destinationWidth,
            std::uint32_t destinationHeight);
        // 結果を全て生成後に一括更新し、失敗時は旧キャッシュを維持する(source: 元のキューブSRV, cacheKey: 0ならディスク保存なし)。
        void BuildPrefilteredEnvironment(
            ID3D11ShaderResourceView* source,
            std::uint64_t cacheKey);
        // 深度を距離へ変換するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_reflectionLinearizePixelShader;
        // 最小深度を縮小するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_reflectionDownsamplePixelShader;
        struct SkyConstants final
        {
            // 空の方向を復元する逆行列
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // ワールド座標でのカメラ位置
            DirectX::XMFLOAT4 cameraPosition{};
            // 空の上側のRGB色と強度
            DirectX::XMFLOAT4 topColor{};
            // 空の地平線のRGB色と予約
            DirectX::XMFLOAT4 horizonColor{};
            // 空の下側のRGB色と予約
            DirectX::XMFLOAT4 groundColor{};
            // キューブ使用の有無と予約
            DirectX::XMFLOAT4 options{};
            // 太陽方向のxyzと角半径
            DirectX::XMFLOAT4 sunDirection{};
            // 太陽のRGB色と円盤の有効値
            DirectX::XMFLOAT4 sunDiskColor{};
        };

        struct BloomConstants final
        {
            // 画像の逆幅と逆高さ
            DirectX::XMFLOAT2 texelSize{};
            // 抽出する明るさのしきい値
            float threshold{};
            // ブルームの強度
            float intensity{};
            // ぼかしの半径
            float radius{};
            // 定数バッファの配置用予約
            DirectX::XMFLOAT3 padding{};
        };

        struct ScreenOutlineConstants final
        {
            // 輪郭のRGB色と強度
            DirectX::XMFLOAT4 color{};
            // 画素幅・深度差・法線差・予約
            DirectX::XMFLOAT4 parameters{};
            // 射影33・43と11・22の逆数
            DirectX::XMFLOAT4 projection{};
            // 画像の逆幅・逆高さ・幅・高さ
            DirectX::XMFLOAT4 texel{};
        };

        struct LensFlareConstants final
        {
            // 逆幅・逆高さ・しきい値・強度
            DirectX::XMFLOAT4 primary{};
            // ゴースト・ハロー・色分散・光条
            DirectX::XMFLOAT4 secondary{};
            // 光条の長さと予約
            DirectX::XMFLOAT4 tertiary{};
            // タップ間隔・方向数・角度・初回
            DirectX::XMFLOAT4 streakPass{};
        };

        struct ColorGradingConstants final
        {
            // 露出・コントラスト・彩度・色温度
            DirectX::XMFLOAT4 primary{};
            // 色調・周辺減光・有効・自動露出
            DirectX::XMFLOAT4 secondary{};
        };

        struct AmbientOcclusionConstants final
        {
            // 逆幅・逆高さ・半径・遮蔽強度
            DirectX::XMFLOAT4 parameters{};
            // 射影33・43と11・22の逆数
            DirectX::XMFLOAT4 projection{};
            // 遮蔽のサンプル数と予約
            DirectX::XMFLOAT4 quality{};
        };

        // LamaPonEnvironment.hlslの対応する定数バッファと、各構造体のフィールド配置を揃える。
        struct TemporalConstants final
        {
            // 現在のジッターなし逆行列
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // このビューの前フレーム行列
            DirectX::XMFLOAT4X4 previousViewProjection{};
            // 履歴比率・許容値・逆幅・逆高さ
            DirectX::XMFLOAT4 parameters{};
        };


        struct DepthOfFieldConstants final
        {
            // 焦点距離・合焦幅・強度・半径
            DirectX::XMFLOAT4 parameters{};
            // 射影33・射影43と予約
            DirectX::XMFLOAT4 projection{};
            // 逆幅・逆高さ・サンプル数・予約
            DirectX::XMFLOAT4 texel{};
        };


        struct MotionBlurConstants final
        {
            // 現在のジッターなし逆行列
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // このビューの前フレーム行列
            DirectX::XMFLOAT4X4 previousViewProjection{};
            // ブレ強度・最大画素長・採取数
            DirectX::XMFLOAT4 parameters{};
            // 画像の逆幅・逆高さと予約
            DirectX::XMFLOAT4 texel{};
        };


        struct LuminanceConstants final
        {
            // 測定画像の逆幅・逆高さと予約
            DirectX::XMFLOAT4 texel{};
        };

        struct VolumetricConstants final
        {
            // 現在の逆ビュー射影行列
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // カメラ位置のxyzと最大距離
            DirectX::XMFLOAT4 cameraPosition{};
            // 光の方向のxyzと採取数
            DirectX::XMFLOAT4 lightDirection{};
            // 強度を含むRGB色と前方散乱
            DirectX::XMFLOAT4 lightColor{};
            // 影カスケードの変換行列
            std::array<DirectX::XMFLOAT4X4, 4> cascades{};
            // 影段数・バイアス・逆解像度
            DirectX::XMFLOAT4 shadowParameters{};
        };

        // 被写界深度の1パスを描き、読み取り参照を解除する(pixelShader: パスのPS, source: t0の入力, depth: t2の深度, work: t6の作業画像, destination: 出力RTV, width: 出力幅, height: 出力高)。
        void DrawDepthOfFieldPass(
            ID3D11PixelShader* pixelShader,
            ID3D11ShaderResourceView* source,
            ID3D11ShaderResourceView* depth,
            ID3D11ShaderResourceView* work,
            ID3D11RenderTargetView* destination,
            float width,
            float height);

        // AOの1パスを描き、読み取り参照を解除する(pixelShader: パスのPS, source: t0の入力, depth: t2の深度, destination: 出力RTV, width: 出力幅, height: 出力高)。
        void DrawAmbientOcclusionPass(
            ID3D11PixelShader* pixelShader,
            ID3D11ShaderResourceView* source,
            ID3D11ShaderResourceView* depth,
            ID3D11RenderTargetView* destination,
            float width,
            float height);

        struct PrefilterConstants final
        {
            // キューブ面・粗さ・元解像度・予約
            DirectX::XMFLOAT4 parameters{};
        };

        // 描画器の寿命まで借用する機器
        ID3D11Device* m_device{};
        // 寿命まで借用する描画先
        ID3D11DeviceContext* m_context{};
        // 中立ビューを解決する機器
        D3D11Backend* m_backend{};
        struct ProbeBakeResources;
        // 再利用するプローブ描画資源
        std::unique_ptr<ProbeBakeResources> m_probeBakeResources;
        // プローブのベイクを実行中か
        bool m_probeBakeActive{};

        // キャッシュが保持する元の画像
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_prefilterSource;
        // 環境畳み込みのキャッシュ鍵
        std::uint64_t m_prefilterCacheKey{};
        // 保持する鏡面環境キューブ
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_prefilteredSpecular;
        // 保持する拡散環境キューブ
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_prefilteredIrradiance;
        // 鏡面環境の最終ミップ番号
        float m_prefilteredMaximumMip{};
        // 鏡面環境を畳み込むPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_prefilterPixelShader;
        // 拡散環境を畳み込むPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_irradiancePixelShader;
        // 環境畳み込み用の定数バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_prefilterBuffer;
        // 全画面三角形を作るVS
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
        // 空を描くPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_skyPixelShader;
        // ブルームを合成するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_bloomPixelShader;
        // 深度と法線の輪郭を描くPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_screenOutlinePixelShader;
        // レンズフレアを合成するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_lensFlarePixelShader;
        // トーンと色を補正するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_toneMapPixelShader;
        // FXAAを適用するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_fxaaPixelShader;
        // 画像をコピーするPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_copyPixelShader;
        // 画像を左右反転するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_copyMirrorPixelShader;
        // 光の筋を合成するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_volumetricPixelShader;
        // TAAの履歴を合成するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_temporalPixelShader;

        // 色とぼけ量を準備するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_depthOfFieldPreparePixelShader;
        // 円形にぼかすPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_depthOfFieldBlurPixelShader;
        // 被写界深度を合成するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_depthOfFieldCompositePixelShader;
        // 被写界深度用の定数バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_depthOfFieldBuffer;
        // モーションブラーを合成するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_motionBlurPixelShader;
        // モーションブラー用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_motionBlurBuffer;

        // 対数輝度を測定するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_luminancePixelShader;
        // 対数輝度測定用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_luminanceBuffer;
        // TAA再投影用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_temporalBuffer;
        // 光の筋用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_volumetricBuffer;
        // 光の筋用の影比較サンプラー
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_volumetricShadowSampler;
        // 画面空間AOを計算するPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_ambientOcclusionPixelShader;
        // AOを深度に応じてぼかすPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_ambientOcclusionBlurPixelShader;
        // 空描画用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_skyBuffer;
        // ブルームとFXAA用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_bloomBuffer;
        // 画面輪郭用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_screenOutlineBuffer;
        // レンズフレア用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_lensFlareBuffer;
        // 光条を広げるPS
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_lensFlareStreakPixelShader;

        // 借用する直前の光条処理結果
        ID3D11ShaderResourceView* m_lastStreakResource{};
        // 色補正用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_colorGradingBuffer;
        // 画面空間AO用の定数
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_ambientOcclusionBuffer;
        // 線形補間のCLAMPサンプラー
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
        // 深度比較と書き込みの無効状態
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthDisabled;
        // 両面描画用のラスタライザー
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    };
}
