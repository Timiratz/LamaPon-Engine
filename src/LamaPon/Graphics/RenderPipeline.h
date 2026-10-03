#pragma once

#include "LamaPon/Graphics/EnvironmentSettings.h"

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

    // 深度専用の状態で不透明ジオメトリを同期描画するフック。
    using DepthPrepassHook = std::function<void()>;

    // 四つの適用地点で追加効果を選択するための地点種別。
    enum class ScreenEffectPoint : std::uint8_t;

    // 描画先と適用地点を渡して追加効果を同期実行するフック。
    using PostProcessHook =
        std::function<void(RenderTarget&, ScreenEffectPoint)>;

    // 深度プリパスと画面空間遮蔽の利用可能性。
    struct DepthPrepassResult final
    {
        // 照明へ渡せる遮蔽の解決有無
        bool ambientOcclusionResolved{};
        // SSRなどへ渡せる深度の有無
        bool depthAvailable{};
    };

    // SSAOまたはSSRが必要なら深度を一回描画する(graphics: 描画デバイス, target: 対象の描画先, projection: 深度と一致する射影, ambientOcclusion: 遮蔽設定, screenSpaceReflection: 反射設定, drawDepthOnly: 不透明深度の描画処理)。
    // 呼出後の主描画先はBindOffscreenTargetで復元し、終了時は深度パス種別をNoneに戻す。
    // フック例外は伝播するため、例外時の描画先の復元も呼出側で行う。
    [[nodiscard]] DepthPrepassResult RunDepthPrepass(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& projection,
        const AmbientOcclusionSettings& ambientOcclusion,
        const ScreenSpaceReflectionSettings&
            screenSpaceReflection,
        const DepthPrepassHook& drawDepthOnly);

    // Sceneが構築し、影付き方向光がある場合に有効となる散乱光のフレーム入力。
    struct VolumetricLightFrame final
    {
        // 散乱光の設定
        VolumetricLightSettings settings{};
        // 影・光・カメラの入力
        VolumetricLightInputs inputs{};
    };

    // Sceneが構築する現在の行列と設定で、過去の履歴は描画先ごとに保持する。
    struct TemporalAntiAliasingFrame final
    {
        // 時間的平滑化の設定
        TemporalAntiAliasingSettings settings{};
        // 現在のビュー射影と逆行列
        TemporalAntiAliasingInputs inputs{};
    };

    // ビューごとに実際の深度と一致する射影を運ぶ被写界深度の入力。
    struct DepthOfFieldFrame final
    {
        // 被写界深度の設定
        DepthOfFieldSettings settings{};
        // 深度と一致する射影行列
        DirectX::XMFLOAT4X4 projection{};
    };

    // 現在の行列をSceneが渡し、前フレームの行列は描画先ごとに保持するカメラブラー入力。
    struct MotionBlurFrame final
    {
        // カメラブラーの設定
        MotionBlurSettings settings{};
        // ずらしを除く逆ビュー射影
        DirectX::XMFLOAT4X4 inverseViewProjection{};
        // 履歴用のずらしを除くビュー射影
        DirectX::XMFLOAT4X4 viewProjection{};
    };

    // Sceneが構築する自動露出の入力。
    struct AutoExposureFrame final
    {
        // 自動露出の設定
        AutoExposureSettings settings{};
        // 停止中も進む実経過秒
        float deltaSeconds{};
    };

    // 全描画経路で共有するポスト処理の入力で、項目追加は位置指定初期化を保つため末尾へ置く。
    struct PostProcessFrame final
    {
        // ブルームの設定
        BloomSettings bloom{};
        // 画面輪郭の設定と射影の入力
        struct ScreenOutlineFrame final
        {
            // 画面輪郭の設定
            ScreenOutlineSettings settings{};
            // 深度と一致する射影行列
            DirectX::XMFLOAT4X4 projection{};
        } screenOutline{};
        // レンズフレアの設定
        ScreenSpaceLensFlareSettings lensFlare{};
        // 被写界深度のフレーム入力
        DepthOfFieldFrame depthOfField{};
        // カメラブラーのフレーム入力
        MotionBlurFrame motionBlur{};
        // 自動露出のフレーム入力
        AutoExposureFrame autoExposure{};
        // トーンマップと色調整の設定
        ColorGradingSettings colorGrading{};
        // 散乱光のフレーム入力
        VolumetricLightFrame volumetric{};
        // 時間的平滑化のフレーム入力
        TemporalAntiAliasingFrame temporal{};
    };


    // フレアを無効として共通のポスト処理へ渡す(graphics: 描画デバイス, target: 対象の描画先, bloom: ブルーム設定, colorGrading: 色調整設定, volumetric: 散乱光の入力, temporal: 時間的平滑化の入力, afterToneMapping: 四地点で呼ぶ任意の処理)。
    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric = {},
        const TemporalAntiAliasingFrame& temporal = {},
        const PostProcessHook& afterToneMapping = {});

    // 指定の効果をフレーム入力へまとめてポスト処理を実行する(graphics: 描画デバイス, target: 対象の描画先, bloom: ブルーム設定, lensFlare: フレア設定, colorGrading: 色調整設定, volumetric: 散乱光の入力, temporal: 時間的平滑化の入力, afterToneMapping: 四地点で呼ぶ任意の処理)。
    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric = {},
        const TemporalAntiAliasingFrame& temporal = {},
        const PostProcessHook& afterToneMapping = {});


    // 品質とScene設定を照合して共通の順序でポスト処理を行う(graphics: 描画デバイス, target: 対象の描画先, frame: Sceneのフレーム入力, afterToneMapping: 四地点で呼ぶ任意の処理)。
    // 無効な描画先は無処理とし、フック例外は伝播して後続処理を実行しない。
    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const PostProcessFrame& frame,
        const PostProcessHook& afterToneMapping = {});
}
