#pragma once

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/FrameDebugger.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon::Detail
{
    // 空なら空文字、指定時は汎用区切りのUTF-8パスを返します(path: 表示するパス)。
    [[nodiscard]] inline std::string FrameDebugPathLabel(
        const std::filesystem::path& path)
    {
        return path.empty()
            ? std::string{}
            : PathToUtf8(std::filesystem::path{ path.generic_wstring() });
    }

    // 有効数字4桁で数値を文字列化します(value: 表示する数値)。
    [[nodiscard]] inline std::string FrameDebugNumber(const double value)
    {
        // 数値の表示文字バッファー
        char text[32]{};
        std::snprintf(text, sizeof(text), "%.4g", value);
        return text;
    }

    // 空でない項目をカンマ区切りで追加します(text: 追加先の要約, item: 追加する項目)。
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

    // カリング方式の表示名を返します(mode: カリング方式)。
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

    // 材質・シェーダー・画像の表示名を組み立てます(materialAsset: 材質パス, shader: シェーダーパスで空は標準Lit, albedo: 基本色画像パス)。
    [[nodiscard]] inline std::string FrameDebugMaterialLabel(
        const std::filesystem::path& materialAsset,
        const std::filesystem::path& shader,
        const std::filesystem::path& albedo)
    {
        // 材質とシェーダーの表示要約
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

    // 画像描画の説明を設定してtrueを返します(description: 出力先, geometry: 形状の説明, texture: 画像パス, sortOrder: 描画順序)。
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
