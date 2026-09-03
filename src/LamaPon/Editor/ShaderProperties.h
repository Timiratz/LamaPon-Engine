#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // カスタムShaderのパラメーターに名前と範囲を付けるための宣言です。
    // HLSLの中へ次のブロックを書くと、エディターが読み取って
    // Inspectorへ名前付きのUIを出します。
    //
    //   /* LAMAPON_PROPERTIES
    //   [
    //     { "target": "0.x",   "type": "float", "name": "影のしきい値",
    //       "min": 0, "max": 1, "default": 0.5 },
    //     { "target": "1.rgb", "type": "color", "name": "影の色",
    //       "default": [0.2, 0.25, 0.4] }
    //   ]
    //   */
    //
    // 解釈はエディターだけで行い、シェーダーのコンパイル方法や
    // 実行時の動作には一切影響しません。宣言が無いShaderは従来どおり
    // 生のfloat4を8本編集するUIになります。
    enum class ShaderPropertyKind
    {
        // 1成分のスライダー（min/maxがあれば範囲付き）。
        Float,
        // 3成分または4成分のカラーピッカー。
        Color,
        // 0/1として格納するチェックボックス。
        Boolean,
        // 2〜4成分をまとめて数値入力。
        Vector,
        // 追加テクスチャの割り当て枠（targetは"t7"のように書きます）。
        Texture
    };

    struct ShaderPropertyField final
    {
        // CustomParameters[parameterIndex] のどこへ入れるか。
        // Textureのときは追加テクスチャの番号（t7が0番）です。
        std::size_t parameterIndex{};
        // 使用する成分（0=x, 1=y, 2=z, 3=w）。componentCount個ぶん有効。
        std::array<std::size_t, 4> components{};
        std::size_t componentCount{ 1 };
        ShaderPropertyKind kind{ ShaderPropertyKind::Float };
        std::string name;
        float minimum{ 0.0f };
        float maximum{ 1.0f };
        bool hasRange{};
        // 「既定値に戻す」で書き込む値（宣言があるときだけ）。
        std::optional<std::array<float, 4>> defaultValue;
    };

    struct ShaderProperties final
    {
        // 宣言ブロックがあったか（無ければ従来のUIを使います）。
        bool declared{};
        std::vector<ShaderPropertyField> fields;
        // 宣言が壊れている場合の理由（Inspectorへ表示します）。
        std::string error;
    };

    // HLSLのテキストから宣言ブロックを読み取ります。例外は投げません。
    [[nodiscard]] ShaderProperties ParseShaderProperties(
        std::string_view shaderSource);

    // ファイルから読み取ります。読めない場合は declared=false を返します。
    [[nodiscard]] ShaderProperties LoadShaderProperties(
        const std::filesystem::path& shaderPath);
}
