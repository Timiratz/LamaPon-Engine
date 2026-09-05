#pragma once

#include "LamaPon/Graphics/EnvironmentSettings.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>

namespace LamaPon
{
    class AssetManager;
    class RenderTarget;

    class EnvironmentRenderer final
    {
    public:
        EnvironmentRenderer(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);

        // 空に描く太陽。朝昼夜モード（SkySettings::sunDriven）の
        // ときにSceneが渡します。
        struct SkySun final
        {
            // 太陽へ向かう向き（Directional Lightの進行方向の逆）。
            DirectX::XMFLOAT3 directionToSun{ 0.0f, 1.0f, 0.0f };
            // 色×強さ。
            DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
            // 角半径（ラジアン）。本物の太陽は0.53度＝0.00465。
            float angularRadius{ 0.004625f };
        };

        void DrawSky(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const SkySettings& settings,
            ID3D11ShaderResourceView* cubemap = nullptr,
            const SkySun* sun = nullptr);
        void ApplyBloom(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const BloomSettings& settings);
        void ApplyScreenOutline(
            ID3D11ShaderResourceView* source,
            ID3D11ShaderResourceView* depth,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        // 筋を1/4解像度で作ります。ストライドを4倍ずつ広げながら
        // ping-pongで3回書き戻すので、レンダーターゲットとSRVを
        // 2組受け取ります。仕上がった側のSRVは
        // LastLensFlareStreakResource()で取れます。
        void BuildLensFlareStreaks(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* firstTarget,
            ID3D11ShaderResourceView* firstResource,
            ID3D11RenderTargetView* secondTarget,
            ID3D11ShaderResourceView* secondResource,
            std::uint32_t width,
            std::uint32_t height,
            const ScreenSpaceLensFlareSettings& settings);
        [[nodiscard]] ID3D11ShaderResourceView*
            LastLensFlareStreakResource() const noexcept
        {
            return m_lastStreakResource;
        }

        // streakはBuildLensFlareStreaksが作った筋です。nullptrなら
        // 筋なしで合成します。
        void ApplyScreenSpaceLensFlare(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const ScreenSpaceLensFlareSettings& settings,
            ID3D11ShaderResourceView* streak = nullptr);
        void ApplyToneMapping(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const ColorGradingSettings& settings);
        void ApplyFXAA(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height);
        // SSAO。3パスに分かれています。
        // (1)深度から遮蔽を求める（半解像度のRチャンネルへ）
        // (2)深度を見るブラーでザラつきを消す（半解像度）
        // (3)フル解像度のカラーへ掛ける
        // 分けているのはブラーを挟むためです。falseを返したときは
        // 何も描いていないので、呼び出し側は合成を進めないでください。
        [[nodiscard]] bool RenderAmbientOcclusion(
            ID3D11ShaderResourceView* depth,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        void BlurAmbientOcclusion(
            ID3D11ShaderResourceView* occlusion,
            ID3D11ShaderResourceView* depth,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height);
        // ボリュメトリックライト（光の筋）。カメラからのレイに沿って
        // カスケードシャドウを引き、光が届いている区間を積みます。
        // 影付きの平行光源が要るので、揃っていなければ何もしません。
        // 戻り値がtrueなら描画先を入れ替えています。
        struct VolumetricInputs final
        {
            ID3D11ShaderResourceView* depth{};
            ID3D11ShaderResourceView* cascadeShadow{};
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            DirectX::XMFLOAT3 cameraPosition{};
            // 光源から出る向き。
            DirectX::XMFLOAT3 lightDirection{};
            DirectX::XMFLOAT3 lightColor{ 1.0f, 1.0f, 1.0f };
            std::array<DirectX::XMFLOAT4X4, 4>
                cascadeViewProjections{};
            std::uint32_t cascadeCount{};
            float shadowBias{ 0.002f };
            float shadowResolution{ 2048.0f };
        };
        // TAA（時間的アンチエイリアス）の解決に必要な入力。
        struct TemporalInputs final
        {
            ID3D11ShaderResourceView* history{};
            ID3D11ShaderResourceView* depth{};
            // 今のフレームの逆ビュー射影。ずらしを含まないもの
            // を渡してください。ずらし込みで復元すると履歴を読む
            // 位置が毎フレーム動き、輪郭がちらつきます。
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // 今のフレームのビュー射影（ずらし無し）。次フレームの
            // 参照用にRenderTargetが控えます。
            DirectX::XMFLOAT4X4 viewProjection{};
            // 以下はRenderTargetが自分の状態から埋めます。ビューごとに
            // 履歴が別なので、行列もビューごとに持つ必要があります
            // （エディターはシーンビューとゲームビューを同じフレームで
            // 描くため、共有すると互いに踏み合って再投影が壊れます）。
            DirectX::XMFLOAT4X4 previousViewProjection{};
            bool previousValid{};
        };
        bool ApplyTemporalAntiAliasing(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const TemporalAntiAliasingSettings& settings,
            const TemporalInputs& inputs);

        bool ApplyVolumetricLight(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const VolumetricLightSettings& settings,
            const VolumetricInputs& inputs);

        // 被写界深度（DoF）に必要な、呼び出し側しか知らない情報。
        struct DepthOfFieldInputs final
        {
            // メインパスが書いた深度。ポスト処理の時点では描画先から
            // 外れているのでそのまま読めます（深度プリパスは不要）。
            ID3D11ShaderResourceView* depth{};
            // この絵を描いたときの射影行列。深度をカメラからの距離へ
            // 戻すのに使います。正投影では復元できないため、その
            // ときは何もしません。
            DirectX::XMFLOAT4X4 projection{};
            // 半解像度の作業用2枚。(1)の書き出し先→(2)の読み元、
            // (2)の書き出し先→(3)の読み元、という受け渡しに使います。
            ID3D11RenderTargetView* prepareTarget{};
            ID3D11ShaderResourceView* prepareResource{};
            ID3D11RenderTargetView* blurTarget{};
            ID3D11ShaderResourceView* blurResource{};
            std::uint32_t halfWidth{};
            std::uint32_t halfHeight{};
            // ぼけのサンプル数（品質設定から）。
            std::uint32_t sampleCount{ 22 };
        };
        // 被写界深度（DoF）。3パスに分かれています。
        // (1)半解像度へ色とCoC（ぼけの大きさ）を書き出す
        // (2)半解像度で円形にぼかす
        // (3)フル解像度でCoCの大きさに応じて元の絵と混ぜる
        // 戻り値がtrueのときだけdestinationへ描いています。falseなら
        // 呼び出し側は入れ替えを行わないでください。
        [[nodiscard]] bool ApplyDepthOfField(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const DepthOfFieldSettings& settings,
            const DepthOfFieldInputs& inputs);

        // モーションブラーに必要な、呼び出し側しか知らない情報。
        struct MotionBlurInputs final
        {
            ID3D11ShaderResourceView* depth{};
            // 今のフレームの逆ビュー射影。TAAと同じくずらしを
            // 含まないものを渡してください（含めると毎フレーム
            // 半画素ぶんの偽の速度が出ます）。
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // 前フレームのビュー射影（ずらし無し）。ビューごとに
            // 別なのでRenderTargetが控えます。
            DirectX::XMFLOAT4X4 previousViewProjection{};
            bool previousValid{};
            // ブレの線に沿って何回サンプルするか（品質設定から）。
            std::uint32_t sampleCount{ 8 };
        };
        // モーションブラー。深度と前フレームの行列が要ります。前
        // フレームの行列がまだ無い最初のフレームは何もしません。
        // 戻り値がtrueのときだけdestinationへ描いています。
        [[nodiscard]] bool ApplyMotionBlur(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t width,
            std::uint32_t height,
            const MotionBlurSettings& settings,
            const MotionBlurInputs& inputs);

        // 自動露出の明るさ測定。輝度の対数を1/4解像度へ書き、続けて
        // ミップ連鎖を生成します。いちばん小さいミップ（1x1）が
        // 画面全体の対数平均になるので、呼び出し側はそれをCPUへ
        // 読み出して露出を決めます。
        //
        // resourceは連鎖の生成に必要です（RTVはミップ0だけを指す
        // ので、GenerateMipsへは全ミップを見るSRVを渡します）。
        void RenderLuminance(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            ID3D11ShaderResourceView* resource,
            std::uint32_t width,
            std::uint32_t height);

        void Copy(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t destinationWidth,
            std::uint32_t destinationHeight);
        // 左右反転コピー（リフレクションプローブのベイク用。
        // 理由はLamaPonEnvironment.hlslのPSCopyMirrorXを参照）。
        void CopyMirroredX(
            ID3D11ShaderResourceView* source,
            ID3D11RenderTargetView* destination,
            std::uint32_t destinationWidth,
            std::uint32_t destinationHeight);

        // IBLの事前フィルタ結果（split-sum近似）。
        struct PrefilteredEnvironment final
        {
            // ミップごとに粗さを上げてGGX畳み込みした
            // スペキュラキューブマップ。
            ID3D11ShaderResourceView* specular{};
            // コサイン畳み込みした拡散用の放射照度キューブ。
            ID3D11ShaderResourceView* irradiance{};
            // specularの最終ミップ番号（粗さ→ミップ変換用）。
            float specularMaximumMip{};
        };

        // ソースキューブマップの事前フィルタ結果を返します。
        // 同じソースなら生成済みを再利用します（生成は1回だけ）。
        // cacheKeyが0以外なら、生成のかわりにディスクの環境
        // キャッシュを引き、外れたら生成して保存します（鍵は
        // 呼ぶ側がソースの内容から作ります）。
        [[nodiscard]] PrefilteredEnvironment
            GetPrefilteredEnvironment(
                ID3D11ShaderResourceView* source,
                std::uint64_t cacheKey = 0);

        // 呼び出し側が所有する事前フィルタ結果。リフレクション
        // プローブのように「プローブごとに1組」を持ちたい場合に
        // 使います（上のキャッシュはスカイ用の1組だけ）。
        struct OwnedPrefilteredEnvironment final
        {
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView> specular;
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView> irradiance;
            float specularMaximumMip{};

            [[nodiscard]] bool IsValid() const noexcept
            {
                return specular != nullptr
                    && irradiance != nullptr;
            }
        };
        // includeSpecular=falseでスペキュラの畳み込みを飛ばします
        // （照度しか使わないGIベイク用。resultのspecularは空になり、
        // IsValid()は偽になるので、irradianceだけを見てください）。
        [[nodiscard]] OwnedPrefilteredEnvironment
            CreatePrefilteredEnvironment(
                ID3D11ShaderResourceView* source,
                bool includeSpecular = true);

        // SSRのHi-Z用の深度ピラミッドを作ります（ミップ0で深度→
        // ビュー距離、以降は2x2の最小値）。RenderTargetが持っている
        // ピラミッドへ書き込みます。描画先とビューポートは退避して
        // 戻すので、フレームの途中で呼べます。
        void BuildReflectionDepthPyramid(
            RenderTarget& target,
            float projectionZ,
            float projectionW);

    private:
        void BuildPrefilteredEnvironment(
            ID3D11ShaderResourceView* source,
            std::uint64_t cacheKey);
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_reflectionLinearizePixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_reflectionDownsamplePixelShader;
        struct SkyConstants final
        {
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            DirectX::XMFLOAT4 cameraPosition{};
            DirectX::XMFLOAT4 topColor{};
            DirectX::XMFLOAT4 horizonColor{};
            DirectX::XMFLOAT4 groundColor{};
            // x=キューブマップ使用, y/z/w=予約
            DirectX::XMFLOAT4 options{};
            // xyz=太陽へ向かう向き, w=角半径（ラジアン）。
            DirectX::XMFLOAT4 sunDirection{};
            // rgb=太陽の色×強さ, w=0より大きければ空に描く。
            DirectX::XMFLOAT4 sunDiskColor{};
        };

        struct BloomConstants final
        {
            DirectX::XMFLOAT2 texelSize{};
            float threshold{};
            float intensity{};
            float radius{};
            DirectX::XMFLOAT3 padding{};
        };

        struct ScreenOutlineConstants final
        {
            // rgb=線の色, w=強さ
            DirectX::XMFLOAT4 color{};
            // x=太さ（画素）, y=深度しきい値,
            // z=法線しきい値, w=予約
            DirectX::XMFLOAT4 parameters{};
            // x=projection._33, y=projection._43,
            // z=1/projection._11, w=1/projection._22
            DirectX::XMFLOAT4 projection{};
            // xy=1/画面サイズ, zw=画面サイズ
            DirectX::XMFLOAT4 texel{};
        };

        struct LensFlareConstants final
        {
            // xy=1/画面サイズ, z=しきい値, w=全体の強さ
            DirectX::XMFLOAT4 primary{};
            // x=ゴースト, y=ハロー, z=色分散, w=筋の強さ
            DirectX::XMFLOAT4 secondary{};
            // x=筋の長さ, y/z/w=予約
            DirectX::XMFLOAT4 tertiary{};
            // x=タップ間隔, y=方向の本数, z=1本目の角度,
            // w=1なら最初の回。
            DirectX::XMFLOAT4 streakPass{};
        };

        struct ColorGradingConstants final
        {
            DirectX::XMFLOAT4 primary{};
            DirectX::XMFLOAT4 secondary{};
        };

        struct AmbientOcclusionConstants final
        {
            // x=1/幅, y=1/高さ, z=半径, w=強さ
            DirectX::XMFLOAT4 parameters{};
            // x=projection._33, y=projection._43,
            // z=1/projection._11, w=1/projection._22
            DirectX::XMFLOAT4 projection{};
            // x=サンプル数, y/z/w=予約
            DirectX::XMFLOAT4 quality{};
        };

        // 並びはLamaPonEnvironment.hlslのVolumetricBufferと
        // 一致させてください。
        // 並びはLamaPonEnvironment.hlslのTemporalBufferと
        // 一致させてください。
        struct TemporalConstants final
        {
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            DirectX::XMFLOAT4X4 previousViewProjection{};
            // x=履歴を残す比率, y=近傍クランプの緩さ,
            // z=1/幅, w=1/高さ
            DirectX::XMFLOAT4 parameters{};
        };

        // 並びはLamaPonEnvironment.hlslのDepthOfFieldBufferと
        // 一致させてください。
        struct DepthOfFieldConstants final
        {
            // x=ピントの合う距離, y=ピントの合う幅, z=ぼけの強さ,
            // w=ぼけ半径の上限（フル解像度の画素）
            DirectX::XMFLOAT4 parameters{};
            // x=projection._33, y=projection._43, z/w=予約
            DirectX::XMFLOAT4 projection{};
            // x=1/幅, y=1/高さ（そのパスの解像度）, z=サンプル数,
            // w=予約
            DirectX::XMFLOAT4 texel{};
        };

        // 並びはLamaPonEnvironment.hlslのMotionBlurBufferと
        // 一致させてください。
        struct MotionBlurConstants final
        {
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            DirectX::XMFLOAT4X4 previousViewProjection{};
            // x=ブレの強さ, y=伸ばす最大の長さ（画素）,
            // z=サンプル数, w=予約
            DirectX::XMFLOAT4 parameters{};
            // x=1/幅, y=1/高さ, z/w=予約
            DirectX::XMFLOAT4 texel{};
        };

        // 並びはLamaPonEnvironment.hlslのLuminanceBufferと
        // 一致させてください。
        struct LuminanceConstants final
        {
            // x=1/幅, y=1/高さ（測定先の解像度）, z/w=予約
            DirectX::XMFLOAT4 texel{};
        };

        struct VolumetricConstants final
        {
            DirectX::XMFLOAT4X4 inverseViewProjection{};
            // xyz=カメラ位置, w=最大距離
            DirectX::XMFLOAT4 cameraPosition{};
            // xyz=光の向き, w=サンプル数
            DirectX::XMFLOAT4 lightDirection{};
            // rgb=光の色×強度, w=前方散乱
            DirectX::XMFLOAT4 lightColor{};
            std::array<DirectX::XMFLOAT4X4, 4> cascades{};
            // x=カスケード数, y=バイアス, z=1/解像度, w=予約
            DirectX::XMFLOAT4 shadowParameters{};
        };

        // DoFの3パスに共通する描画。t0=元画像、t2=深度、
        // t6=半解像度の作業用（不要なものはnullptr）。終了時に
        // それらを外すので、直後に同じテクスチャを描画先にできます。
        void DrawDepthOfFieldPass(
            ID3D11PixelShader* pixelShader,
            ID3D11ShaderResourceView* source,
            ID3D11ShaderResourceView* depth,
            ID3D11ShaderResourceView* work,
            ID3D11RenderTargetView* destination,
            float width,
            float height);

        // SSAOの3パスに共通する描画。
        void DrawAmbientOcclusionPass(
            ID3D11PixelShader* pixelShader,
            ID3D11ShaderResourceView* source,
            ID3D11ShaderResourceView* depth,
            ID3D11RenderTargetView* destination,
            float width,
            float height);

        struct PrefilterConstants final
        {
            // x=キューブ面, y=粗さ, z=ソース解像度, w=予約
            DirectX::XMFLOAT4 parameters{};
        };

        ID3D11Device* m_device{};
        ID3D11DeviceContext* m_context{};
        // 事前フィルタのキャッシュ（ソースが変わったら再生成）。
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_prefilterSource;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_prefilteredSpecular;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_prefilteredIrradiance;
        float m_prefilteredMaximumMip{};
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_prefilterPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_irradiancePixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_prefilterBuffer;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_skyPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_bloomPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_screenOutlinePixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_lensFlarePixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_toneMapPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_fxaaPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_copyPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_copyMirrorPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_volumetricPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_temporalPixelShader;
        // 被写界深度の3パス。
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_depthOfFieldPreparePixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_depthOfFieldBlurPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_depthOfFieldCompositePixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_depthOfFieldBuffer;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_motionBlurPixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_motionBlurBuffer;
        // 自動露出の明るさ測定（対数輝度）。
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_luminancePixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_luminanceBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_temporalBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_volumetricBuffer;
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_volumetricShadowSampler;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_ambientOcclusionPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_ambientOcclusionBlurPixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_skyBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_bloomBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_screenOutlineBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_lensFlareBuffer;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_lensFlareStreakPixelShader;
        // 直前のBuildLensFlareStreaksで仕上がった側のSRV。
        ID3D11ShaderResourceView* m_lastStreakResource{};
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_colorGradingBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_ambientOcclusionBuffer;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthDisabled;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    };
}
