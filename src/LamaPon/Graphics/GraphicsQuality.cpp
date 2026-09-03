#include "LamaPon/Graphics/GraphicsQuality.h"

#include <algorithm>
#include <stdexcept>

namespace LamaPon
{
    GraphicsSettings GraphicsSettingsForPreset(
        const GraphicsQualityPreset preset) noexcept
    {
        switch (preset)
        {
        case GraphicsQualityPreset::Low:
        {
            GraphicsSettings settings{
                preset,
                0.65f,
                false,
                512,
                1,
                false,
                false,
                false,
                true,
                2,
                1
            };
            // 低スペック向けはVRAM節約を優先します。
            settings.runtimeTextureCompression = true;
            settings.ambientOcclusionSampleCount = 8;
            settings.depthOfFieldSampleCount = 10;
            settings.motionBlurSampleCount = 4;
            settings.automaticLodQuality = 0.55f;
            return settings;
        }
        case GraphicsQualityPreset::Medium:
        {
            GraphicsSettings settings{
                preset,
                0.85f,
                true,
                1024,
                2,
                true,
                true,
                true,
                true,
                4,
                2
            };
            settings.runtimeTextureCompression = true;
            settings.ambientOcclusionSampleCount = 12;
            settings.depthOfFieldSampleCount = 16;
            settings.motionBlurSampleCount = 6;
            settings.automaticLodQuality = 0.80f;
            return settings;
        }
        case GraphicsQualityPreset::Ultra:
        {
            GraphicsSettings settings{
                preset,
                1.0f,
                true,
                4096,
                4,
                true,
                true,
                true,
                true,
                8,
                4
            };
            settings.ambientOcclusionEnabled = true;
            settings.ambientOcclusionSampleCount = 24;
            settings.screenSpaceLensFlareEnabled = true;
            settings.depthOfFieldEnabled = true;
            settings.depthOfFieldSampleCount = 32;
            settings.motionBlurEnabled = true;
            settings.motionBlurSampleCount = 16;
            settings.autoExposureEnabled = true;
            settings.automaticLodQuality = 1.35f;
            return settings;
        }
        case GraphicsQualityPreset::Custom:
        case GraphicsQualityPreset::High:
        default:
        {
            GraphicsSettings settings{
                preset == GraphicsQualityPreset::Custom
                    ? GraphicsQualityPreset::Custom
                    : GraphicsQualityPreset::High,
                1.0f,
                true,
                2048,
                3,
                true,
                true,
                true,
                true,
                8,
                4
            };
            settings.ambientOcclusionEnabled = true;
            settings.screenSpaceLensFlareEnabled = true;
            settings.depthOfFieldEnabled = true;
            settings.motionBlurEnabled = true;
            settings.autoExposureEnabled = true;
            settings.automaticLodQuality = 1.0f;
            return settings;
        }
        }
    }

    GraphicsSettings ClampGraphicsSettings(
        GraphicsSettings settings) noexcept
    {
        // 1.0を超える値は高解像度で描いて縮小するスーパーサンプリング
        // （SSAA）です。ギザギザには一番よく効きますが、ピクセル数が
        // 倍率の2乗で増えるため重くなります（2.0なら4倍）。
        settings.renderScale = std::clamp(
            settings.renderScale,
            0.5f,
            2.0f);
        settings.ambientOcclusionSampleCount = std::clamp(
            settings.ambientOcclusionSampleCount,
            4u,
            32u);
        settings.depthOfFieldSampleCount = std::clamp(
            settings.depthOfFieldSampleCount,
            4u,
            64u);
        settings.motionBlurSampleCount = std::clamp(
            settings.motionBlurSampleCount,
            2u,
            32u);
        settings.automaticLodQuality = std::clamp(
            settings.automaticLodQuality,
            0.25f,
            2.0f);
        settings.shadowResolution = std::clamp(
            settings.shadowResolution,
            256u,
            8192u);
        settings.shadowCascadeLimit = std::clamp(
            settings.shadowCascadeLimit,
            1u,
            4u);
        settings.pointLightLimit = std::clamp(
            settings.pointLightLimit,
            0u,
            16u);
        settings.spotLightLimit = std::clamp(
            settings.spotLightLimit,
            0u,
            8u);
        if (settings.targetFrameRate != 0)
        {
            settings.targetFrameRate = std::clamp(
                settings.targetFrameRate,
                15u,
                1000u);
        }
        // 範囲外の値（古い保存データやC++からの直接代入）で
        // ライティングが無効になるより、既定へ倒します。
        if (settings.renderingPath != RenderingPath::Forward
            && settings.renderingPath
                != RenderingPath::ForwardPlus)
        {
            settings.renderingPath =
                RenderingPath::ForwardPlus;
        }
        return settings;
    }

    std::string_view GraphicsQualityPresetName(
        const GraphicsQualityPreset preset) noexcept
    {
        switch (preset)
        {
        case GraphicsQualityPreset::Low:
            return "Low";
        case GraphicsQualityPreset::Medium:
            return "Medium";
        case GraphicsQualityPreset::High:
            return "High";
        case GraphicsQualityPreset::Ultra:
            return "Ultra";
        case GraphicsQualityPreset::Custom:
            return "Custom";
        }
        return "High";
    }

    GraphicsQualityPreset
        GraphicsQualityPresetFromName(
            const std::string_view name)
    {
        if (name == "Low") return GraphicsQualityPreset::Low;
        if (name == "Medium") return GraphicsQualityPreset::Medium;
        if (name == "High") return GraphicsQualityPreset::High;
        if (name == "Ultra") return GraphicsQualityPreset::Ultra;
        if (name == "Custom") return GraphicsQualityPreset::Custom;
        throw std::invalid_argument(
            "Unknown graphics quality preset.");
    }

    std::string_view RenderingPathName(
        const RenderingPath path) noexcept
    {
        switch (path)
        {
        case RenderingPath::Forward:
            return "Forward";
        case RenderingPath::ForwardPlus:
            return "ForwardPlus";
        }
        return "ForwardPlus";
    }

    RenderingPath RenderingPathFromName(
        const std::string_view name)
    {
        if (name == "Forward")
        {
            return RenderingPath::Forward;
        }
        // プリセット名と違って投げません。読めない名前で起動ごと
        // 落とすより、既定へ倒して絵を出す方が親切なためです。
        return RenderingPath::ForwardPlus;
    }
}
