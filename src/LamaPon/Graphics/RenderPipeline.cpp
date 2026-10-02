#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Graphics/RenderTarget.h"

namespace
{
    // 深度パスを設定し、終了時に通常描画へ戻すスコープ。
    struct DepthPassScope final
    {
        // スコープ中に借用する描画デバイス
        LamaPon::GraphicsDevice& graphics;

        // 深度パスを切り替える(device: 借用する描画デバイス, kind: 設定する深度パス種別)。
        DepthPassScope(
            LamaPon::GraphicsDevice& device,
            const LamaPon::DepthPassKind kind) noexcept
            : graphics(device)
        {
            graphics.SetDepthPass(kind);
        }

        // 元の種別によらず深度パスをNoneへ戻す。
        ~DepthPassScope() noexcept
        {
            graphics.SetDepthPass(
                LamaPon::DepthPassKind::None);
        }

        // 深度パススコープのコピーを禁止する。
        DepthPassScope(const DepthPassScope&) = delete;
        // 深度パススコープのコピー代入を禁止する。
        DepthPassScope& operator=(
            const DepthPassScope&) = delete;
    };
}

namespace LamaPon
{
    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const BloomSettings& bloom,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric,
        const TemporalAntiAliasingFrame& temporal,
        const PostProcessHook& afterToneMapping)
    {
        RunPostProcess(
            graphics,
            target,
            bloom,
            ScreenSpaceLensFlareSettings{},
            colorGrading,
            volumetric,
            temporal,
            afterToneMapping);
    }

    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const BloomSettings& bloom,
        const ScreenSpaceLensFlareSettings& lensFlare,
        const ColorGradingSettings& colorGrading,
        const VolumetricLightFrame& volumetric,
        const TemporalAntiAliasingFrame& temporal,
        const PostProcessHook& afterToneMapping)
    {
        // 省略項目を既定にしたフレーム入力
        PostProcessFrame frame{};
        frame.bloom = bloom;
        frame.lensFlare = lensFlare;
        frame.colorGrading = colorGrading;
        frame.volumetric = volumetric;
        frame.temporal = temporal;
        RunPostProcess(
            graphics,
            target,
            frame,
            afterToneMapping);
    }

    DepthPrepassResult RunDepthPrepass(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& projection,
        const AmbientOcclusionSettings& ambientOcclusion,
        const ScreenSpaceReflectionSettings&
            screenSpaceReflection,
        const DepthPrepassHook& drawDepthOnly)
    {
        // 深度と遮蔽の利用可能性
        DepthPrepassResult result{};
        // 現在の描画品質設定
        const auto& settings = graphics.Settings();
        // SSAOとSSRで深度を共有し、必要なら一回だけ描画する。
        // 品質とSceneが要求する遮蔽
        const bool occlusionWanted =
            ambientOcclusion.enabled
            && settings.ambientOcclusionEnabled;
        // Sceneが要求する画面空間反射
        const bool reflectionWanted =
            screenSpaceReflection.enabled;
        if (!target.IsValid()
            || !drawDepthOnly
            || (!occlusionWanted && !reflectionWanted))
        {
            return result;
        }

        // 不透明ジオメトリを深度専用の描画先へ描く。
        {
            // 描画区間の計測スコープ
            GpuProfiler::SectionScope section{
                graphics.Gpu(),
                "深度プリパス"
            };
            graphics.BindOffscreenTargetDepthOnly(target);
            // 深度パスをNoneへ戻すスコープ
            const DepthPassScope depthScope{
                graphics,
                DepthPassKind::Prepass };
            drawDepthOnly();
        }
        result.depthAvailable = true;

        if (!occlusionWanted)
        {
            // SSRだけの要求なら遮蔽の解決は省く。
            return result;
        }

        // 色を変更せず、半解像度と深度対応ブラーで遮蔽を求める。
        {
            // 描画区間の計測スコープ
            GpuProfiler::SectionScope section{
                graphics.Gpu(),
                "SSAO"
            };
            result.ambientOcclusionResolved =
                graphics.ResolveOffscreenTargetAmbientOcclusion(
                    target,
                    ambientOcclusion,
                    projection,
                    settings.ambientOcclusionSampleCount);
        }
        return result;
    }

    void RunPostProcess(
        GraphicsDevice& graphics,
        RenderTarget& target,
        const PostProcessFrame& frame,
        const PostProcessHook& afterToneMapping)
    {
        if (!target.IsValid())
        {
            return;
        }
        // 全API・全描画経路で同じ順序を使い、SSAOとSSRは照明描画前に済ませる。
        // ポスト処理全体の計測スコープ
        const GpuProfiler::SectionScope postScope{
            graphics.Gpu(),
            "ポスト処理" };

        // 現在の描画品質設定
        const auto& settings = graphics.Settings();


        // 指定地点で任意の追加効果を実行する(point: 適用する地点)。
        const auto inject =
            [&afterToneMapping, &target](
                const ScreenEffectPoint point)
        {
            if (afterToneMapping)
            {
                afterToneMapping(target, point);
            }
        };

        // 3D描画直後のHDRへ追加効果を適用する。
        inject(ScreenEffectPoint::BeforePostProcess);

        // 履歴に後続の効果を重ねないよう、TAAは最初のHDRへ適用する。
        graphics.ApplyOffscreenTargetTemporalAntiAliasing(
            target,
            frame.temporal.settings,
            frame.temporal.inputs);
        // 初回も次回用の履歴が必要なため、適用成功ではなく有効設定で履歴を保存する。
        if (frame.temporal.settings.enabled)
        {
            graphics.CaptureOffscreenTargetTemporalHistory(
                target,
                frame.temporal.inputs.viewProjection);
        }

        // 散乱光はブルームとトーンマップの対象となるHDRへ加算する。
        graphics.ApplyOffscreenTargetVolumetricLight(
            target,
            frame.volumetric.settings,
            frame.volumetric.inputs);

        // 履歴判定をぼける前に済ませ、被写界深度はTAA後・ブルーム前に適用する。
        // 品質の許可を反映した被写界深度
        auto effectiveDepthOfField = frame.depthOfField.settings;
        effectiveDepthOfField.enabled =
            effectiveDepthOfField.enabled
            && settings.depthOfFieldEnabled;
        graphics.ApplyOffscreenTargetDepthOfField(
            target,
            effectiveDepthOfField,
            frame.depthOfField.projection,
            settings.depthOfFieldSampleCount);

        // カメラブラーは被写界深度の後、ブルーム前のHDRへ適用する。
        // 品質の許可を反映したブラー
        auto effectiveMotionBlur = frame.motionBlur.settings;
        effectiveMotionBlur.enabled =
            effectiveMotionBlur.enabled
            && settings.motionBlurEnabled;
        graphics.ApplyOffscreenTargetMotionBlur(
            target,
            effectiveMotionBlur,
            frame.motionBlur.inverseViewProjection,
            frame.motionBlur.viewProjection,
            settings.motionBlurSampleCount);

        // ブルーム抽出前のHDRへ追加効果を適用する。
        inject(ScreenEffectPoint::BeforeBloom);

        // 品質の許可を反映したブルーム
        auto effectiveBloom = frame.bloom;
        effectiveBloom.enabled =
            effectiveBloom.enabled
            && settings.bloomEnabled;
        graphics.ApplyOffscreenTargetBloom(
            target,
            effectiveBloom);

        // フレアはブルーム後のHDRへ加え、トーンマップで明るさを調整する。
        // 品質の許可を反映したフレア
        auto effectiveLensFlare = frame.lensFlare;
        effectiveLensFlare.enabled =
            effectiveLensFlare.enabled
            && settings.screenSpaceLensFlareEnabled;
        graphics.ApplyOffscreenTargetScreenSpaceLensFlare(
            target,
            effectiveLensFlare);

        // 露出測定の対象となる最終HDRへ追加効果を適用する。
        inject(ScreenEffectPoint::BeforeToneMapping);

        // ブルームなどを含む最終HDRから自動露出を求め、手動露出へ加算する。
        // 品質の許可を反映した自動露出
        auto effectiveAutoExposure = frame.autoExposure.settings;
        effectiveAutoExposure.enabled =
            effectiveAutoExposure.enabled
            && settings.autoExposureEnabled;
        // 自動露出を合成する色調整設定
        auto effectiveColorGrading = frame.colorGrading;
        effectiveColorGrading.autoExposureStops =
            graphics.UpdateOffscreenTargetAutoExposure(
                target,
                effectiveAutoExposure,
                frame.autoExposure.deltaSeconds);

        graphics.ApplyOffscreenTargetToneMapping(
            target,
            effectiveColorGrading);

        // 輪郭はトーンマップ後に重ね、最後のFXAAで線も平滑化する。
        graphics.ApplyOffscreenTargetScreenOutline(
            target,
            frame.screenOutline.settings,
            frame.screenOutline.projection);

        // トーンマップ後のLDRへ追加効果を適用する。
        inject(ScreenEffectPoint::AfterToneMapping);

        // 色が確定した最後にFXAAを適用する。
        if (settings.antiAliasingEnabled)
        {
            graphics.ApplyOffscreenTargetFXAA(target);
        }
    }
}
