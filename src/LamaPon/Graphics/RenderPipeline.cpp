#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Graphics/RenderTarget.h"

namespace
{
    // GPU区間のRAII。途中でreturnしても閉じ忘れないようにします。
    struct GpuSectionScope final
    {
        LamaPon::GraphicsDevice& graphics;

        GpuSectionScope(
            LamaPon::GraphicsDevice& device,
            const char* const name) noexcept
            : graphics(device)
        {
            graphics.Gpu().BeginSection(name);
        }

        ~GpuSectionScope() noexcept
        {
            graphics.Gpu().EndSection();
        }

        GpuSectionScope(const GpuSectionScope&) = delete;
        GpuSectionScope& operator=(
            const GpuSectionScope&) = delete;
    };

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
        // SSAOとSSRのどちらかが要求していれば走らせます。どちらも
        // 深度を読むので、プリパス自体は1回で足ります。
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
        graphics.Gpu().BeginSection("深度プリパス");
        target.BindDepthOnly(graphics.Context());
        {
            const DepthPassScope depthScope{
                graphics,
                DepthPassKind::Prepass };
            drawDepthOnly();
        }
        graphics.Gpu().EndSection();
        result.depthAvailable = true;

        if (!occlusionWanted)
        {
            // SSRのためだけに走った場合はここで終わりです。
            return result;
        }

        // 深度から遮蔽を求めます（半解像度＋深度を見るブラー）。
        // 結果はtargetの中に残り、カラーには触りません。
        graphics.Gpu().BeginSection("SSAO");
        result.ambientOcclusionResolved =
            target.ResolveAmbientOcclusion(
                graphics.Environment(),
                ambientOcclusion,
                projection,
                settings.ambientOcclusionSampleCount);
        graphics.Gpu().EndSection();
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

        // 計測をここへ置いているのは、5経路すべてがこの関数を通る
        // からです。以前はGraphicsDevice側にしか区間が無く、
        // エディタービューポートのBloom・トーンマップ・FXAAが
        // まるごと計測外でした（GPU合計と内訳が合わない原因）。
        const GpuSectionScope postScope{
            graphics,
            "ポスト処理" };

        const auto& settings = graphics.Settings();

        // 差し込み地点は4つです。**位置の意味はここにしかありません**
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
        target.ApplyTemporalAntiAliasing(
            graphics.Environment(),
            graphics.Context(),
            frame.temporal.settings,
            frame.temporal.inputs);

        // ボリュメトリックライト（光の筋）は、深度と影を読むうえに
        // 光を足す処理なので、Bloomより前・HDRのうちにかけます。
        // これで明るい筋がBloomで滲み、トーンマップも通ります。
        target.ApplyVolumetricLight(
            graphics.Environment(),
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
        target.ApplyDepthOfField(
            graphics.Environment(),
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
        target.ApplyMotionBlur(
            graphics.Environment(),
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
        target.ApplyBloom(
            graphics.Environment(),
            effectiveBloom);

        // Screen Space Lens FlareはBloom後のHDRへかけます。Bloomの
        // 柔らかな光も光学系へ入るため、光源の周囲に自然なゴーストが
        // 付きます。その後トーンマップへ通すので、明るさも馴染みます。
        auto effectiveLensFlare = frame.lensFlare;
        effectiveLensFlare.enabled =
            effectiveLensFlare.enabled
            && settings.screenSpaceLensFlareEnabled;
        target.ApplyScreenSpaceLensFlare(
            graphics.Environment(),
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
            target.UpdateAutoExposure(
                graphics.Environment(),
                graphics.Context(),
                effectiveAutoExposure,
                frame.autoExposure.deltaSeconds);

        target.ApplyToneMapping(
            graphics.Environment(),
            effectiveColorGrading);

        // トーンマップ後の画面へ輪郭を重ねます。深度だけを読むので、
        // UIが合成される前に置けば3Dだけへ適用できます。FXAAは最後に
        // かかるため、輪郭線の階段も一緒に平滑化されます。
        target.ApplyScreenOutline(
            graphics.Environment(),
            frame.screenOutline.settings,
            frame.screenOutline.projection);

        // 既定の位置。トーンマップ後のLDRです。
        inject(ScreenEffectPoint::AfterToneMapping);

        // FXAAは輪郭を見て平すので、色が確定した最後にかけます。
        if (settings.antiAliasingEnabled)
        {
            target.ApplyFXAA(
                graphics.Environment());
        }
    }
}
