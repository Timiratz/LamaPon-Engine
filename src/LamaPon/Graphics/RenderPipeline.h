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

    // 深度プリパスで描く中身（不透明ジオメトリ）を呼ぶ側から渡します。
    // GraphicsDeviceが深度専用パスへ切り替えた状態で呼ばれるので、
    // 中では通常の3D描画をそのまま行ってください。
    using DepthPrepassHook = std::function<void()>;

    // ポスト処理の途中へ差し込む追加パスです。ゲーム実行時の
    // 画面エフェクト（ScreenEffect）がここへ入ります。
    //
    // **決められた4地点すべてで呼ばれます。** どこで何をするかは
    // 受け取った側が地点を見て決めてください（GraphicsDeviceは
    // その地点に指定されたエフェクトだけをかけます）。
    // 定義は GraphicsDevice.h。重いヘッダを引き込まないよう、
    // ここでは前方宣言だけにしています。
    enum class ScreenEffectPoint : std::uint8_t;

    using PostProcessHook =
        std::function<void(RenderTarget&, ScreenEffectPoint)>;

    // 深度プリパスを走らせ、SSAOをライティングより前に解決します。
    //
    // 戻り値がtrueなら、target.AmbientOcclusionShaderResourceView()
    // をLitEffectへ渡せます（Litシェーダーが環境光／IBL項にだけ
    // 掛けます）。falseなら遮蔽なしとして扱ってください。
    //
    // SSAOが無効なときは何もせずfalseを返します。プリパスはジオメトリを
    // もう1回描くコストがあるので、SSAOを使わない構成では走らせません
    // （早期Zの効果だけを狙って常に走らせるのは今後の課題）。
    //
    // 呼んだ後は描画先とビューポートが変わっているので、呼ぶ側は
    // メインパスの前にtarget.Bind()で描画先を戻してください。
    // projectionはこれから描く絵の射影行列です。
    // プリパスの結果。depthAvailableは「深度が使える状態になった」で、
    // SSRが読むのはこちらです（SSAOを切っていてもSSRのために走る
    // ことがあるため、2つを分けています）。
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
    // ここへ入れるのは今のフレームの2本だけです。
    struct MotionBlurFrame final
    {
        MotionBlurSettings settings{};
        // 深度からワールド位置へ戻す逆ビュー射影。TAAと同じく
        // **ずらしを含まない**もの。
        DirectX::XMFLOAT4X4 inverseViewProjection{};
        // 次フレームの「前フレーム」として控えるビュー射影
        // （ずらし無し）。
        DirectX::XMFLOAT4X4 viewProjection{};
    };

    // 自動露出に必要な、シーン側しか知らない情報。
    struct AutoExposureFrame final
    {
        AutoExposureSettings settings{};
        // 順応に使う経過時間（秒）。**timeScaleの影響を受けない実時間**
        // を渡してください。停止中のScene Viewでも露出が落ち着いて
        // ほしいので、ポーズで止めない方を選んでいます。
        float deltaSeconds{};
    };

    // ポスト処理へ渡すものを1つにまとめた入れ物。
    //
    // まとめた理由は、パスを足すたびにRunPostProcessの引数が増えて
    // 5経路すべての呼び出しを直す必要があったからです（並びを1箇所へ
    // 集約したのと同じ動機で、引数も1箇所へ集めました）。
    // **フィールドを足すときは必ず末尾へ。**
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
    // 並びの定義はこの関数だけにあります。Scene View、Game View、
    // カメラプレビュー、名前付きレンダーテクスチャ、ゲーム実行時の
    // 5経路すべてがここを通るので、パスを足すときはここだけを
    // 直してください。以前は同じ並びが5箇所へ複製されていて、
    // 新しいパスを足すと1箇所だけ抜ける事故が起きやすい形でした。
    //
    // Sceneの設定と品質設定の突き合わせもここで行うので、呼ぶ側は
    // Sceneが持っている値をそのまま渡してください。
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

    // 本来の入口。上の2つは足りないぶんを既定値で埋めてここへ流します。
    // Sceneが組み立てたPostProcessFrameをそのまま渡してください
    // （Scene::PostProcessFrameData()）。
    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const PostProcessFrame& frame,
        const PostProcessHook& afterToneMapping = {});
}
