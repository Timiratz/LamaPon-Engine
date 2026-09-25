#pragma once

// 描画Componentがフレームデバッガー向けの説明文を組み立てるための
// Runtime内部の補助です。Game Module SDKの公開APIではありません。
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/FrameDebugger.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon::Detail
{
    [[nodiscard]] inline std::string FrameDebugPathLabel(
        const std::filesystem::path& path)
    {
        return path.empty()
            ? std::string{}
            : PathToUtf8(std::filesystem::path{ path.generic_wstring() });
    }

    // 1.000000のような末尾の0を出さずに数値を表示します。
    [[nodiscard]] inline std::string FrameDebugNumber(const double value)
    {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%.4g", value);
        return text;
    }

    // 要約の項目を「, 」区切りで足します。
    inline void AppendFrameDebugItem(
        std::string& text,
        const std::string_view item)
    {
        if (item.empty())
        {
            return;
        }
        if (!text.empty())
        {
            text += ", ";
        }
        text += item;
    }

    [[nodiscard]] inline std::string_view FrameDebugCullLabel(
        const ShaderCullMode mode) noexcept
    {
        switch (mode)
        {
        case ShaderCullMode::Front:
            return "表面カリング";
        case ShaderCullMode::None:
            return "両面描画";
        case ShaderCullMode::Back:
            break;
        }
        return "裏面カリング";
    }

    // マテリアルアセット、Shader、ベース色テクスチャの順に、
    // 指定されているものだけを並べます。
    [[nodiscard]] inline std::string FrameDebugMaterialLabel(
        const std::filesystem::path& materialAsset,
        const std::filesystem::path& shader,
        const std::filesystem::path& albedo)
    {
        std::string label;
        if (!materialAsset.empty())
        {
            AppendFrameDebugItem(
                label,
                "Material: " + FrameDebugPathLabel(materialAsset));
        }
        AppendFrameDebugItem(
            label,
            shader.empty()
                ? std::string{ "Shader: LamaPon Lit" }
                : "Shader: " + FrameDebugPathLabel(shader));
        if (!albedo.empty())
        {
            AppendFrameDebugItem(
                label,
                "Texture: " + FrameDebugPathLabel(albedo));
        }
        return label;
    }

    // 画像1枚で描く2D/UI描画の説明です。
    [[nodiscard]] inline bool DescribeTexturedDraw(
        FrameDebugDrawDescription& description,
        std::string geometry,
        const std::filesystem::path& texture,
        const int sortOrder)
    {
        description.geometry = std::move(geometry);
        description.material = texture.empty()
            ? std::string{ "Texture: （なし）" }
            : "Texture: " + FrameDebugPathLabel(texture);
        description.state = "並び順 " + std::to_string(sortOrder);
        return true;
    }
}
