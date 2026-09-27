#pragma once

#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Scene/SceneTransition.h"

#include <nlohmann/json.hpp>
#include <algorithm>

namespace LamaPonSceneShowcase
{
    inline constexpr auto PresetType = "SceneTransition.Preset";
    inline constexpr auto DefaultPresetPath =
        "packages/scene-transition-showcase/presets/IrisGold.asset.json";

    // DataAssetはLoadDataAssetから取得します。型の違うアセットは実行せず、
    // 欠けた項目・範囲外の値にはランタイムと同じ互換・補正処理を使います。
    [[nodiscard]] inline bool ReadPreset(
        const LamaPon::DataAsset& asset,
        LamaPon::SceneTransitionSettings& settings)
    {
        if (asset.TypeName() != PresetType || asset.IsEmpty())
        {
            return false;
        }
        auto document = nlohmann::json::parse(
            asset.SerializeToJson(), nullptr, false);
        if (!document.is_object() || !document.contains("values")
            || !document.at("values").is_object())
        {
            return false;
        }
        auto& values = document.at("values");
        // DataAssetは整数もdoubleで保持するため、整数項目だけ型を戻します。
        if (values.contains("divisions") && values.at("divisions").is_number())
        {
            values["divisions"] = static_cast<int>(std::clamp(
                values.at("divisions").get<double>(), 1.0, 64.0));
        }
        settings = LamaPon::SceneTransitionFromJson(values);
        return true;
    }
}
