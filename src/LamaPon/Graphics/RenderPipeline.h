#pragma once

#include "LamaPon/Graphics/EnvironmentRenderer.h"

#include <DirectXMath.h>

#include <cstdint>
#include <functional>

namespace LamaPon
{
    class GraphicsDevice;
    class RenderTarget;
    struct AmbientOcclusionSettings;
    struct BloomSettings;
    struct ScreenOutlineSettings;
    struct ScreenSpaceLensFlareSettings;
    struct ScreenSpaceReflectionSettings;
    struct ColorGradingSettings;

    // 深度専用パスへ切り替えた状態で呼ばれ、不透明ジオメトリを描画します。
    using DepthPrepassHook = std::function<void()>;

    // ポスト処理の途中へScreenEffectを適用する追加パスです。
    //
    // 決められた4地点すべてで呼ばれ、指定地点に対応するエフェクトだけを
    // GraphicsDeviceが適用します。
    // 定義はGraphicsDevice.hにあり、依存を減らすため前方宣言します。
    enum class ScreenEffectPoint : std::uint8_t;

    using PostProcessHook =
        std::function<void(RenderTarget&, ScreenEffectPoint)>;

    // SSAOまたはSSRが必要な場合に深度プリパスを実行します。
    // ambientOcclusionResolvedがtrueならAOをLitEffectへ渡せます。
    // depthAvailableはSSR用の深度が利用可能かを示し、SSAO無効時も
    // trueになり得ます。どちらも不要な場合は両方falseです。
    // 呼び出し後はtarget.Bind()でメインパスの描画先を復元します。
    // projectionにはこれから描く画面の射影行列を渡します。
    struct DepthPrepassResult final
    {
        bool ambientOcclusionResolved{};
        bool depthAvailable{};
    };

    [[nodiscard]] DepthPrepassResult RunDepthPrepass(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& projection,
        const AmbientOcclusionSettings& ambientOcclusion,
        const ScreenSpaceReflectionSettings&
            screenSpaceReflection,
        const DepthPrepassHook& drawDepthOnly);

    // ボリュメトリックライトに必要な、シーン側しか知らない情報。
    // 影付きの平行光源が無いフレームでは enabled が立たないので、
    // ポスト処理側は何もしません。
    struct VolumetricLightFrame final
    {
        VolumetricLightSettings settings{};
        EnvironmentRenderer::VolumetricInputs inputs{};
    };

    // TAAに必要な、シーン側しか知らない情報（今と前フレームの行列）。
    struct TemporalAntiAliasingFrame final
    {
        TemporalAntiAliasingSettings settings{};
        EnvironmentRenderer::TemporalInputs inputs{};
    };

    // 被写界深度に必要な、シーン側しか知らない情報。
    //
    // 射影行列を運ぶためにあります。深度を「カメラからの距離」へ戻さ
    // ないとピントが合う範囲を決められず、その変換には描いたときの
    // 射影が必要です。ビューごとに違う（Scene Viewとカメラプレビューは
    // 同じフレームで別の射影を使う）ので、共有せずに毎回運びます。
    struct DepthOfFieldFrame final
    {
        DepthOfFieldSettings settings{};
        // この絵を描いたときの射影行列（TAAのずらしを含んだもの＝
        // 深度バッファと噛み合う方）。
        DirectX::XMFLOAT4X4 projection{};
    };

    // モーションブラーに必要な、シーン側しか知らない情報。
    //
    // 前フレームの行列そのものはRenderTargetが持ちます（ビューごとに
    // 別なので、シーンで1つ持つとエディターの2ビューが踏み合います）。
    // 現在フレームの逆ビュー射影と次回用ビュー射影だけを保持します。
    struct MotionBlurFrame final
    {
        MotionBlurSettings settings{};
        // 深度からワールド位置へ戻す逆ビュー射影。TAAと同じく
        // ずらしを含まないもの。
        DirectX::XMFLOAT4X4 inverseViewProjection{};
        // 次フレームの「前フレーム」として控えるビュー射影
        // （ずらし無し）。
        DirectX::XMFLOAT4X4 viewProjection{};
    };

    // 自動露出に必要な、シーン側しか知らない情報。
    struct AutoExposureFrame final
    {
        AutoExposureSettings settings{};
        // 順応に使う経過時間（秒）。停止中のScene Viewでも露出が収束するよう、
        // timeScaleの影響を受けない実時間を渡します。
        float deltaSeconds{};
    };

    // ポスト処理へ渡すものを1つにまとめた入れ物。
    //
    // すべての描画経路へ同じポスト処理情報を渡せるよう、引数を集約します。
    // フィールドを足すときは必ず末尾へ。
    struct PostProcessFrame final
    {
        BloomSettings bloom{};
        struct ScreenOutlineFrame final
        {
            ScreenOutlineSettings settings{};
            DirectX::XMFLOAT4X4 projection{};
        } screenOutline{};
        ScreenSpaceLensFlareSettings lensFlare{};
        DepthOfFieldFrame depthOfField{};
        MotionBlurFrame motionBlur{};
        AutoExposureFrame autoExposure{};
        ColorGradingSettings colorGrading{};
        VolumetricLightFrame volumetric{};
        TemporalAntiAliasingFrame temporal{};
    };

    // シーンを描き終えた1枚へ、ポスト処理を順番にかけます。
    //
    // Scene View、Game View、カメラプレビュー、名前付きレンダー
    // テクスチャ、ゲーム実行時で共通の描画順を定義します。描画パスを
    // 追加するときは、この関数を更新します。
    //
    // Scene設定と品質設定の照合を行うため、Sceneの値を変更せずに渡します。
    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric = {},
        const TemporalAntiAliasingFrame& temporal = {},
        const PostProcessHook& afterToneMapping = {});

    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric = {},
        const TemporalAntiAliasingFrame& temporal = {},
        const PostProcessHook& afterToneMapping = {});

    // すべてのポスト処理情報を受け取る共通の入口です。
    // Scene::PostProcessFrameData()が返すPostProcessFrameを受け取ります。
    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const PostProcessFrame& frame,
        const PostProcessHook& afterToneMapping = {});
}
