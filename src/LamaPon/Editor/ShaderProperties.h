#pragma once

#include "LamaPon/Graphics/ShaderManifest.h"

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
        // 定数配列番号・textureならt7起点
        std::size_t parameterIndex{};
        // 有効成分の番号・xからwは0から3
        std::array<std::size_t, 4> components{};
        // 有効な成分の数
        std::size_t componentCount{ 1 };
        // Inspectorの入力形式
        ShaderPropertyKind kind{ ShaderPropertyKind::Float };
        // Inspectorに表示する入力名
        std::string name;
        // 範囲付き数値入力の下限
        float minimum{ 0.0f };
        // 範囲付き数値入力の上限
        float maximum{ 1.0f };
        // 有効な入力範囲が指定されたか
        bool hasRange{};
        // 宣言された復元用の既定値
        std::optional<std::array<float, 4>> defaultValue;
    };

    struct ShaderProperties final
    {
        // 宣言が存在し名前付きUIを使うか
        bool declared{};
        // 解釈できたプロパティの入力情報
        std::vector<ShaderPropertyField> fields;
        // Inspectorへ表示する宣言エラー
        std::string error;
    };

    // 変更と操作終了を別々に集め、Inspectorは確定frameだけをUndoへ追加する。
    struct ShaderPropertyEditResult final
    {
        // 値が変更されたframeがあるか
        bool changed{};
        // Undoへ確定するframeがあるか
        bool committed{};

        // 変更と確定の通知を同じ編集結果へ集める(valueChanged: このframeで値が変わったか, editCommitted: このframeで操作が確定したか)。
        void Observe(
            const bool valueChanged,
            const bool editCommitted) noexcept
        {
            changed = changed || valueChanged;
            committed = committed || editCommitted;
        }
    };

    // HLSL内の最初のプロパティ配列を読み入力情報と解釈エラーを返す(shaderSource: 読み取るHLSL文書)。
    [[nodiscard]] ShaderProperties ParseShaderProperties(
        std::string_view shaderSource);

    // 明示targetを先に予約し省略項目を宣言順に自動配置する(properties: Manifestのプロパティ宣言一覧)。
    [[nodiscard]] ShaderProperties ConvertShaderManifestProperties(
        const std::vector<ShaderPropertyDesc>& properties);

    // HLSLを読みプロパティを解釈し読込できなければ未宣言として返す(shaderPath: 読み取るHLSL文書のパス)。
    [[nodiscard]] ShaderProperties LoadShaderProperties(
        const std::filesystem::path& shaderPath);
}
