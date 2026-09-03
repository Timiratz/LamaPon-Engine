#pragma once

#include <string>
#include <string_view>

namespace LamaPon
{
    // そのHLSLが持っている入口。コンパイルせずにソースから読みます
    // （割り当てた直後、コンパイルが失敗した理由を説明するため）。
    struct ShaderEntryPoints final
    {
        bool vertex{};          // VSMain
        bool pixel{};           // PSMain
        bool skinnedVertex{};   // VSSkinnedMain
        bool skinnedPixel{};    // PSSkinnedMain
        bool hull{};            // HSMain
        bool domain{};          // DSMain
        bool compute{};         // CSMain

        [[nodiscard]] bool Any() const noexcept
        {
            return vertex || pixel || skinnedVertex
                || skinnedPixel || hull || domain || compute;
        }
    };

    // コメントは先に取り除きます。取り除かないと、雛形の
    // 「Entry points must remain VSMain and PSMain (...)」のような
    // 説明文まで入口として数えてしまいます。
    [[nodiscard]] ShaderEntryPoints ParseShaderEntryPoints(
        std::string_view source);

    // そのShaderがどこへ割り当てられたか。必要な入口が用途ごとに
    // 違うので、説明を選ぶのに使います。
    enum class ShaderUsage
    {
        Material,       // 3Dマテリアル（Mesh Renderer／Model Renderer）
        Sprite,         // スプライト／UI／パーティクル
        ScreenEffect,   // 画面全体のポストエフェクト
        Compute         // 自作Compute Shader
    };

    // コンパイルエラーの前に、原因の見当を日本語で足します。
    //
    // なぜ要るか: HLSLコンパイラは「error X3501: 'VSMain':
    // entrypoint not found」としか言いません。初めての人には、
    // それが「2D用のShaderを3Dマテリアルへ入れた」という意味だと
    // 読み取れません。ソースを見れば何用のShaderかは分かるので、
    // そこまで書いて返します。
    //
    // 見当がつかないときは元のメッセージをそのまま返します。
    // 元のメッセージは必ず末尾に残します（行番号が要る場面がある）。
    [[nodiscard]] std::string ExplainShaderError(
        std::string_view compilerMessage,
        std::string_view source,
        ShaderUsage usage);
}
