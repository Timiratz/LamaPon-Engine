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

    // HLSLコンパイラのメッセージでは対象の入口／識別子が引用符で
    // 囲まれるため、最初の引用名を診断対象として抽出します。
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

    // 定数バッファ経由でエンジンが提供する識別子の一覧です。
    // ObjectBuffer宣言が無い状態で一覧中の名前が未定義になった場合に、
    // 組み込み識別子であることを診断へ補足します。
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
            return "3DマテリアルにはVSMainとPSMainが必要です"
                "（スキニングモデルではVSSkinnedMainと"
                "PSSkinnedMainも必要です）。";
        case LamaPon::ShaderUsage::Sprite:
            return "スプライト／UI／パーティクルに割り当てられるのは"
                "ピクセルシェーダーだけです。入口関数をPSMainとし、"
                "引数をCOLOR0、TEXCOORD0、SV_Positionの順に指定してください。";
        case LamaPon::ShaderUsage::ScreenEffect:
            return "画面全体のポストエフェクトにはPSMainが必要です。";
        case LamaPon::ShaderUsage::Compute:
            return "Compute Shaderの入口関数はCSMainです"
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

        // X1507はincludeファイルを開けない場合に発生するため、
        // ファイルの配置とエディターの更新方法を案内します。
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
                hint = "このシェーダーは3Dマテリアル用ですが、"
                    "割り当て先はスキニング（ボーン）モデルです。"
                    "VSSkinnedMainとPSSkinnedMainを追加してください。"
                    "雛形の該当する宣言を利用できます。";
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
            hint = "戻り値または引数にセマンティクス"
                "（例: : SV_Target）が付いていません。";
            if (usage == ShaderUsage::Sprite)
            {
                hint += " スプライト／UI／パーティクルの PSMain は"
                    "「COLOR0 → TEXCOORD0 → SV_Position の順の引数」と"
                    "「戻り値 : SV_Target」で記述します。"
                    "引数の順序が違うと値がずれて渡されます。";
            }
            else
            {
                hint += " ピクセルシェーダーの戻り値には : SV_Target、"
                    "頂点シェーダーが返す構造体にはSV_Positionが"
                    "必要です。";
            }
        }
        // X3004/X3000: 未定義名がエンジン提供の定数と一致する場合だけ、
        // cbuffer宣言の案内を追加します。それ以外は元の診断を保ちます。
        else if (Contains(compilerMessage, "X3004")
            || Contains(compilerMessage, "undeclared identifier"))
        {
            if (const auto name =
                    FirstQuotedName(compilerMessage);
                IsEngineProvidedName(name))
            {
                hint = "識別子「" + std::string{ name }
                    + "」はエンジンが定数バッファで渡す値です。"
                    "使用するにはシェーダー内でcbufferを宣言してください"
                    "（ObjectBufferはregister(b0)、ライティングは"
                    "register(b1)）。LamaPonLit.hlslの関連する宣言を"
                    "一式コピーしてください。一部だけコピーすると、"
                    "値の配置が一致しない場合があります。";
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
