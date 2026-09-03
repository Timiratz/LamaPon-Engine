#include "LamaPon/Graphics/ShaderDiagnostics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace
{
    // コメントを空白へ置き換えます（消すと行番号がずれるため）。
    [[nodiscard]] std::string StripComments(
        const std::string_view source)
    {
        std::string result(source);
        const std::size_t size = result.size();
        for (std::size_t index = 0; index < size;)
        {
            if (index + 1 < size
                && result[index] == '/'
                && result[index + 1] == '/')
            {
                while (index < size && result[index] != '\n')
                {
                    result[index++] = ' ';
                }
                continue;
            }
            if (index + 1 < size
                && result[index] == '/'
                && result[index + 1] == '*')
            {
                while (index < size)
                {
                    const bool end = index + 1 < size
                        && result[index] == '*'
                        && result[index + 1] == '/';
                    if (result[index] != '\n')
                    {
                        result[index] = ' ';
                    }
                    ++index;
                    if (end)
                    {
                        if (index < size)
                        {
                            result[index++] = ' ';
                        }
                        break;
                    }
                }
                continue;
            }
            ++index;
        }
        return result;
    }

    [[nodiscard]] bool IsIdentifierCharacter(
        const char character) noexcept
    {
        return std::isalnum(
                static_cast<unsigned char>(character)) != 0
            || character == '_';
    }

    // 「名前」の直後が（空白を挟んで）'(' なら関数定義とみなします。
    [[nodiscard]] bool HasEntryPoint(
        const std::string& source,
        const std::string_view name)
    {
        std::size_t cursor = 0;
        while (true)
        {
            const auto found = source.find(name, cursor);
            if (found == std::string::npos)
            {
                return false;
            }
            cursor = found + name.size();

            const bool boundedLeft = found == 0
                || !IsIdentifierCharacter(source[found - 1]);
            std::size_t after = cursor;
            while (after < source.size()
                && std::isspace(
                    static_cast<unsigned char>(source[after]))
                    != 0)
            {
                ++after;
            }
            if (boundedLeft
                && after < source.size()
                && source[after] == '(')
            {
                return true;
            }
        }
    }

    [[nodiscard]] bool Contains(
        const std::string_view text,
        const std::string_view needle)
    {
        return text.find(needle) != std::string_view::npos;
    }

    // そのShaderが「何用に見えるか」を一言で。
    [[nodiscard]] std::string DescribeShaderKind(
        const LamaPon::ShaderEntryPoints& entryPoints)
    {
        if (entryPoints.compute
            && !entryPoints.vertex
            && !entryPoints.pixel)
        {
            return "これはCompute Shaderです（CSMainだけを持っています）。";
        }
        if (entryPoints.pixel
            && !entryPoints.vertex
            && !entryPoints.skinnedVertex)
        {
            return "これは2D用のShaderに見えます"
                "（PSMainだけを持っています）。";
        }
        if (entryPoints.vertex && entryPoints.pixel)
        {
            return "これは3DマテリアルのShaderです"
                "（VSMainとPSMainを持っています）。";
        }
        if (!entryPoints.Any())
        {
            return "このファイルにはエンジンが探す入口が"
                "1つもありません。";
        }
        return {};
    }

    // メッセージの最初の 'xxx' を取り出します。HLSLコンパイラは
    // 「error X3501: 'VSSkinnedMain': entrypoint not found」のように
    // 対象の名前を必ず引用符で書くので、どの入口／識別子で失敗したかが
    // これで分かります。
    [[nodiscard]] std::string_view FirstQuotedName(
        const std::string_view message)
    {
        const auto begin = message.find('\'');
        if (begin == std::string_view::npos)
        {
            return {};
        }
        const auto end = message.find('\'', begin + 1);
        if (end == std::string_view::npos)
        {
            return {};
        }
        return message.substr(begin + 1, end - begin - 1);
    }

    // エンジンが定数バッファで渡している名前。雛形からObjectBufferの
    // 宣言を写さずに使うと「未定義の識別子」で落ちるので、そのときだけ
    // 「これはエンジンが渡すものです」と言えます。
    [[nodiscard]] bool IsEngineProvidedName(
        const std::string_view name)
    {
        constexpr std::array<std::string_view, 16> names{
            "World",
            "ViewProjection",
            "WorldInverseTranspose",
            "MaterialColor",
            "CameraPosition",
            "CameraForward",
            "MaterialParameters",
            "CustomParameters",
            "MaterialTextureParameters",
            "EmissiveParameters",
            "TimeParameters",
            "Ambient",
            "LightCounts",
            "DirectionalLights",
            "PointLights",
            "SpotLights"
        };
        return std::ranges::find(names, name) != names.end();
    }

    [[nodiscard]] std::string RequirementFor(
        const LamaPon::ShaderUsage usage)
    {
        switch (usage)
        {
        case LamaPon::ShaderUsage::Material:
            return "3Dマテリアルには VSMain と PSMain の両方が要ります"
                "（スキニングモデルへ使うなら VSSkinnedMain と"
                " PSSkinnedMain も）。";
        case LamaPon::ShaderUsage::Sprite:
            return "スプライト／UI／パーティクルに割り当てられるのは"
                "ピクセルシェーダーだけです。入口の名前は PSMain で、"
                "引数は COLOR0 → TEXCOORD0 → SV_Position の順にします。";
        case LamaPon::ShaderUsage::ScreenEffect:
            return "画面全体のポストエフェクトには PSMain が要ります。";
        case LamaPon::ShaderUsage::Compute:
            return "Compute Shaderの入口は CSMain です"
                "（スレッドグループは8x8固定）。";
        }
        return {};
    }
}

namespace LamaPon
{
    ShaderEntryPoints ParseShaderEntryPoints(
        const std::string_view source)
    {
        const auto stripped = StripComments(source);
        ShaderEntryPoints entryPoints;
        entryPoints.vertex = HasEntryPoint(stripped, "VSMain");
        entryPoints.pixel = HasEntryPoint(stripped, "PSMain");
        entryPoints.skinnedVertex =
            HasEntryPoint(stripped, "VSSkinnedMain");
        entryPoints.skinnedPixel =
            HasEntryPoint(stripped, "PSSkinnedMain");
        entryPoints.hull = HasEntryPoint(stripped, "HSMain");
        entryPoints.domain = HasEntryPoint(stripped, "DSMain");
        entryPoints.compute = HasEntryPoint(stripped, "CSMain");
        return entryPoints;
    }

    std::string ExplainShaderError(
        const std::string_view compilerMessage,
        const std::string_view source,
        const ShaderUsage usage)
    {
        std::string hint;

        // X1507: includeしたファイルを開けなかった。
        // 配布漏れで全プロジェクトが開けなくなったことがあるので、
        // 真っ先に見当がつくようにしています。
        if (Contains(compilerMessage, "X1507")
            || Contains(
                compilerMessage,
                "failed to open source file"))
        {
            hint = "#includeしたファイルが見つかりません。"
                "同じ assets/shaders フォルダーにその名前の"
                "ファイルがあるか確かめてください。"
                "エンジン同梱のものなら、プロジェクトを新しい"
                "エディターで開き直すと揃います。";
        }
        // X3501: 探した入口がなかった。
        else if (Contains(compilerMessage, "X3501")
            || Contains(compilerMessage, "entrypoint not found"))
        {
            const auto entryPoints =
                ParseShaderEntryPoints(source);
            const auto missing =
                FirstQuotedName(compilerMessage);
            const bool missingSkinned =
                missing == "VSSkinnedMain"
                || missing == "PSSkinnedMain";
            // 3D用としては正しく書けていて、スキニング（ボーン）
            // モデルへ割り当てただけ、という場合。ここを一般論で
            // 済ませると「VSMainならあるのに」と混乱します。
            if (missingSkinned
                && entryPoints.vertex
                && entryPoints.pixel)
            {
                hint = "このShaderは3Dマテリアルとしては書けて"
                    "いますが、割り当て先が**スキニング（ボーン）"
                    "モデル**です。VSSkinnedMain と PSSkinnedMain を"
                    "足すと、このモデルにも使えます"
                    "（雛形の該当部分をそのまま写せます）。";
            }
            else
            {
                auto kind = DescribeShaderKind(entryPoints);
                if (!kind.empty())
                {
                    kind += " ";
                }
                hint = kind + RequirementFor(usage);
            }
            if (usage == ShaderUsage::Material
                && entryPoints.hull
                && entryPoints.domain
                && entryPoints.vertex)
            {
                hint += " なお、テセレーション（HSMain／DSMain）が"
                    "効くのは、四角パッチに割れる形状"
                    "（Plane・Cube）のMesh Rendererだけです。";
            }
        }
        // X3506/X3502: 戻り値や引数にセマンティクスが無い。
        // 初めて書くときにいちばん多い形です。
        else if (Contains(compilerMessage, "X3506")
            || Contains(compilerMessage, "X3502")
            || Contains(compilerMessage, "missing semantics"))
        {
            hint = "戻り値か引数にセマンティクス（`: SV_Target` の"
                "ような役割の指定）が付いていません。";
            if (usage == ShaderUsage::Sprite)
            {
                hint += " スプライト／UI／パーティクルの PSMain は"
                    "「COLOR0 → TEXCOORD0 → SV_Position の順の引数」と"
                    "「戻り値 : SV_Target」で書きます"
                    "（**引数の順番が違うと、エラーも出ずに値が"
                    "1本ずつずれます**）。";
            }
            else
            {
                hint += " ピクセルシェーダーの戻り値は `: SV_Target`、"
                    "頂点シェーダーが返す構造体には `SV_Position` が"
                    "要ります。";
            }
        }
        // X3004/X3000: 未定義の識別子。名前がエンジンの渡すものと
        // 一致するときだけ説明します（一般の綴り間違いに一般論を
        // 足しても、本当の原因から目をそらせるだけなので）。
        else if (Contains(compilerMessage, "X3004")
            || Contains(compilerMessage, "undeclared identifier"))
        {
            if (const auto name =
                    FirstQuotedName(compilerMessage);
                IsEngineProvidedName(name))
            {
                hint = "`" + std::string{ name }
                    + "` はエンジンが定数バッファで渡している値です。"
                    "使うには、そのShaderの中で cbuffer の宣言を"
                    "しておく必要があります"
                    "（ObjectBuffer は `register(b0)`、ライティングは"
                    " `register(b1)`）。LamaPonLit.hlsl から"
                    "**丸ごと**写してください。"
                    "一部だけ写すと、エラーも出ないまま値が"
                    "ずれて読まれます。";
            }
        }

        if (hint.empty())
        {
            return std::string{ compilerMessage };
        }
        return hint
            + "\n\n"
            + std::string{ compilerMessage };
    }
}
