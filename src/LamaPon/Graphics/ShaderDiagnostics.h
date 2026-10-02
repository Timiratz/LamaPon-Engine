#pragma once

#include "LamaPon/Core/Api.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace LamaPon
{
    struct ShaderDiagnosticLocation final
    {
        // エラーが発生したソース
        std::filesystem::path path;
        // 1から始まるエラー行番号
        std::uint32_t line{ 1 };
        // 1から始まるエラー列番号
        std::uint32_t column{ 1 };
    };

    // 診断文の最初の有効な位置を読み、未検出なら空を返す(message: コンパイラの診断文)。
    [[nodiscard]] LAMAPON_API std::optional<ShaderDiagnosticLocation>
        ParseShaderDiagnosticLocation(std::string_view message);

    // ソースから検出したシェーダー入口の有無を保持します。
    struct ShaderEntryPoints final
    {
        // VSMainの有無
        bool vertex{};
        // PSMainの有無
        bool pixel{};
        // VSSkinnedMainの有無
        bool skinnedVertex{};
        // PSSkinnedMainの有無
        bool skinnedPixel{};
        // GSMainの有無
        bool geometry{};
        // HSMainの有無
        bool hull{};
        // DSMainの有無
        bool domain{};
        // CSMainの有無
        bool compute{};

        // 対応するシェーダー入口が一つでもあるか返す。
        [[nodiscard]] bool Any() const noexcept
        {
            return vertex || pixel || skinnedVertex
                || skinnedPixel || geometry || hull || domain
                || compute;
        }
    };

    // コメントを除いたHLSLから既知の入口名を検出する(source: HLSLソース文字列)。
    // 定義と呼出しを区別しない文字列検出なので、診断の補助に限る。
    [[nodiscard]] ShaderEntryPoints ParseShaderEntryPoints(
        std::string_view source);

    // 必要な入口を判定するための割り当て用途です。
    enum class ShaderUsage
    {
        // 3Dマテリアル（Mesh Renderer／Model Renderer）
        Material,
        // スプライト／UI／パーティクル
        Sprite,
        // 画面全体のポストエフェクト
        ScreenEffect,
        // 自作Compute Shader
        Compute
    };

    // 用途に応じた説明を元の診断へ添える(compilerMessage: コンパイラの診断文, source: HLSLソース文字列, usage: シェーダーの割当用途)。
    // 説明を作れない場合は元の診断だけを返し、作れる場合も位置表記を保持する。
    [[nodiscard]] std::string ExplainShaderError(
        std::string_view compilerMessage,
        std::string_view source,
        ShaderUsage usage);
}
