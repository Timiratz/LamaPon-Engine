#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Graphics/RenderTarget.h"

namespace
{
    // 深度専用パスのRAII。例外が出ても必ず元へ戻します。
    struct DepthPassScope final
    {
        LamaPon::GraphicsDevice& graphics;

        DepthPassScope(
            LamaPon::GraphicsDevice& device,
            const LamaPon::DepthPassKind kind) noexcept
            : graphics(device)
        {
            graphics.SetDepthPass(kind);
        }

        ~DepthPassScope() noexcept
        {
            graphics.SetDepthPass(
                LamaPon::DepthPassKind::None);
        }

        DepthPassScope(const DepthPassScope&) = delete;
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
        DepthPrepassResult result{};
        const auto& settings = graphics.Settings();
        // SSAOとSSRのどちらかが要求していれば実行します。どちらも
        // 同じ深度を使うため、プリパスは1回だけ実行します。
        const bool occlusionWanted =
            ambientOcclusion.enabled
            && settings.ambientOcclusionEnabled;
        const bool reflectionWanted =
            screenSpaceReflection.enabled;
        if (!target.IsValid()
            || !drawDepthOnly
            || (!occlusionWanted && !reflectionWanted))
        {
            return result;
        }

        // 深度だけを描画先にして、不透明ジオメトリをもう1回描きます。
        // ピクセルシェーダーが外れるので、自作Shaderのオブジェクトも
        // そのまま安全に深度へ載ります。
        {
            GpuProfiler::SectionScope section{
                graphics.Gpu(),
                "深度プリパス"
            };
            graphics.BindOffscreenTargetDepthOnly(target);
            const DepthPassScope depthScope{
                graphics,
                DepthPassKind::Prepass };
            drawDepthOnly();
        }
        result.depthAvailable = true;

        if (!occlusionWanted)
        {
            // SSRのためだけに走った場合はここで終わりです。
            return result;
        }

        // 深度から遮蔽を求めます（半解像度＋深度を見るブラー）。
        // 結果はtargetの中に残り、カラーには触りません。
        {
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
        if (graphics.ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            // D3D12は移植済みのBloomだけをHDRのうちに適用します。
            // トーンマップとカラーグレーディングは最終合成で行い、
            // TAA・SSAO・FXAAなど未移植のpassは安全に無処理とします。
            auto effectiveBloom = frame.bloom;
            effectiveBloom.enabled =
                effectiveBloom.enabled
                && graphics.Settings().bloomEnabled;
            graphics.ApplyOffscreenTargetBloom(
                target,
                effectiveBloom);
            return;
        }

        // 5つの描画経路が通る位置で計測し、エディタービューポートの
        // Bloom、トーンマップ、FXAAもGPU時間の内訳へ含めます。
        const GpuProfiler::SectionScope postScope{
            graphics.Gpu(),
            "ポスト処理" };

        const auto& settings = graphics.Settings();

        // 差し込み地点は4つです。位置の意味はここにしかありません
        // ので、増やすときはこの関数の中だけを直してください
        // （5経路すべてがここを通ります）。
        const auto inject =
            [&afterToneMapping, &target](
                const ScreenEffectPoint point)
        {
            if (afterToneMapping)
            {
                afterToneMapping(target, point);
            }
        };

        // 3Dを描き終えた素のHDR。
        inject(ScreenEffectPoint::BeforePostProcess);

        // TAA（時間的アンチエイリアス）は一番先です。以降のパスは
        // 完成した色を前提にしているので、混ぜるのは素の絵のうちに
        // 済ませます。Bloomの後で混ぜると、前フレームのBloomが
        // さらに滲んで輪郭が二重になります。
        graphics.ApplyOffscreenTargetTemporalAntiAliasing(
            target,
            frame.temporal.settings,
            frame.temporal.inputs);
        // 最初のフレームは混ぜる履歴が無くても、次のフレーム用の
        // 履歴は作る必要があります。そのため適用結果ではなく設定の
        // enabledだけで判定します。また、後続のHDR処理が乗る前の
        // この位置で控え、TAAが解決した色だけを履歴に残します。
        if (frame.temporal.settings.enabled)
        {
            graphics.CaptureOffscreenTargetTemporalHistory(
                target,
                frame.temporal.inputs.viewProjection);
        }

        // ボリュメトリックライト（光の筋）は、深度と影を読むうえに
        // 光を足す処理なので、Bloomより前・HDRのうちにかけます。
        // これで明るい筋がBloomで滲み、トーンマップも通ります。
        graphics.ApplyOffscreenTargetVolumetricLight(
            target,
            frame.volumetric.settings,
            frame.volumetric.inputs);

        // 被写界深度はレンズの中で起きるので、Bloomより前・HDRのうちに
        // かけます。ぼかした後の絵に対してBloomが滲むのが正しい順序で、
        // 逆にすると「ぼけているのに輪郭だけ光っている」絵になります。
        // TAAより後なのも意図的です。TAAの近傍クランプはぼける前の
        // 鋭い絵で判定させないと、履歴を捨てる基準が緩くなって
        // 動きの残像が残ります。
        auto effectiveDepthOfField = frame.depthOfField.settings;
        effectiveDepthOfField.enabled =
            effectiveDepthOfField.enabled
            && settings.depthOfFieldEnabled;
        graphics.ApplyOffscreenTargetDepthOfField(
            target,
            effectiveDepthOfField,
            frame.depthOfField.projection,
            settings.depthOfFieldSampleCount);

        // モーションブラーは被写界深度の後です。光はレンズ（ぼけ）を
        // 通ってからセンサーへ届き、ブレはそのセンサーが開いている
        // 時間で起きるので、この順が実際の並びです。Bloomより前・
        // HDRのうちにかけるのも同じ理由です。
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

        // Bloomの手前。ここで足した明るさは滲みます。
        inject(ScreenEffectPoint::BeforeBloom);

        auto effectiveBloom = frame.bloom;
        effectiveBloom.enabled =
            effectiveBloom.enabled
            && settings.bloomEnabled;
        graphics.ApplyOffscreenTargetBloom(
            target,
            effectiveBloom);

        // Screen Space Lens FlareはBloom後のHDRへかけます。Bloomの
        // 柔らかな光も光学系へ入るため、光源の周囲に自然なゴーストが
        // 付きます。その後トーンマップへ通すので、明るさも馴染みます。
        auto effectiveLensFlare = frame.lensFlare;
        effectiveLensFlare.enabled =
            effectiveLensFlare.enabled
            && settings.screenSpaceLensFlareEnabled;
        graphics.ApplyOffscreenTargetScreenSpaceLensFlare(
            target,
            effectiveLensFlare);

        // トーンマップの手前。まだHDRなので、ここで足した明るさも
        // 自動露出の測定に入ります。
        inject(ScreenEffectPoint::BeforeToneMapping);

        // 自動露出はトーンマップの直前です。測るのは「これから
        // トーンマップに通す絵」でなければならず、Bloomや光の筋で
        // 足された明るさも含めた最終のHDRがここにあります。
        //
        // 返るのは露出への補正（段数）で、手動の露出へ足します。
        // つまり手動側は自動の上に乗る「補正値」として働くので、
        // 両方同時に使えます。
        auto effectiveAutoExposure = frame.autoExposure.settings;
        effectiveAutoExposure.enabled =
            effectiveAutoExposure.enabled
            && settings.autoExposureEnabled;
        auto effectiveColorGrading = frame.colorGrading;
        effectiveColorGrading.autoExposureStops =
            graphics.UpdateOffscreenTargetAutoExposure(
                target,
                effectiveAutoExposure,
                frame.autoExposure.deltaSeconds);

        graphics.ApplyOffscreenTargetToneMapping(
            target,
            effectiveColorGrading);

        // トーンマップ後の画面へ輪郭を重ねます。深度だけを読むので、
        // UIが合成される前に置けば3Dだけへ適用できます。FXAAは最後に
        // かかるため、輪郭線の階段も一緒に平滑化されます。
        graphics.ApplyOffscreenTargetScreenOutline(
            target,
            frame.screenOutline.settings,
            frame.screenOutline.projection);

        // 既定の位置。トーンマップ後のLDRです。
        inject(ScreenEffectPoint::AfterToneMapping);

        // FXAAは輪郭を見て平すので、色が確定した最後にかけます。
        if (settings.antiAliasingEnabled)
        {
            graphics.ApplyOffscreenTargetFXAA(target);
        }
    }
}
