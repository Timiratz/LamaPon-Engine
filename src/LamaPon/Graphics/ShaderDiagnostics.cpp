#include "LamaPon/Graphics/ShaderDiagnostics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string>

namespace
{
    // ソースのコメント部分を改行を残して空白へ置き換える(source: HLSLソース文字列)。
    [[nodiscard]] std::string StripComments(
        const std::string_view source)
    {
        // コメントを空白へ置換するソース
        std::string result(source);
        // ソース文字列のバイト数
        const std::size_t size = result.size();
        // ソース内の確認位置
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
                    // 複数行コメントの終端有無
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

    // 識別子に使う英数字か下線か判定する(character: 確認する文字)。
    [[nodiscard]] bool IsIdentifierCharacter(
        const char character) noexcept
    {
        return std::isalnum(
                static_cast<unsigned char>(character)) != 0
            || character == '_';
    }

    // 名前の直後に括弧を持つ入口候補を探す(source: コメント除去済みソース, name: 検出する入口名)。
    [[nodiscard]] bool HasEntryPoint(
        const std::string& source,
        const std::string_view name)
    {
        // 入口候補を探す開始位置
        std::size_t cursor = 0;
        while (true)
        {
            // 入口名の出現位置
            const auto found = source.find(name, cursor);
            if (found == std::string::npos)
            {
                return false;
            }
            cursor = found + name.size();

            // 名前の左端の識別子境界
            const bool boundedLeft = found == 0
                || !IsIdentifierCharacter(source[found - 1]);
            // 名前の後ろの確認位置
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

    // 指定文字列が含まれるか調べる(text: 検索元の文字列, needle: 検索する文字列)。
    [[nodiscard]] bool Contains(
        const std::string_view text,
        const std::string_view needle)
    {
        return text.find(needle) != std::string_view::npos;
    }

    // 入口候補の組合せから用途の説明を作る(entryPoints: 検出済みの入口候補)。
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

    // 診断内の最初の単一引用符の内容を借用する(message: コンパイラの診断文)。
    [[nodiscard]] std::string_view FirstQuotedName(
        const std::string_view message)
    {
        // 先頭の単一引用符位置
        const auto begin = message.find('\'');
        if (begin == std::string_view::npos)
        {
            return {};
        }
        // 対応する閉じ単一引用符の位置
        const auto end = message.find('\'', begin + 1);
        if (end == std::string_view::npos)
        {
            return {};
        }
        return message.substr(begin + 1, end - begin - 1);
    }

    // 定数バッファが提供する既知の識別子か判定する(name: 確認する識別子)。
    [[nodiscard]] bool IsEngineProvidedName(
        const std::string_view name)
    {
        // 定数バッファの既知の識別子
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

    // 割当用途に必要な入口の説明を返す(usage: シェーダーの割当用途)。
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
    std::optional<ShaderDiagnosticLocation>
        ParseShaderDiagnosticLocation(const std::string_view message)
    {
        // 位置表記を探す開始位置
        std::size_t searchFrom{};
        while (searchFrom < message.size())
        {
            // 位置表記の開き括弧位置
            const auto open = message.find('(', searchFrom);
            if (open == std::string_view::npos)
            {
                return std::nullopt;
            }
            // 位置表記の閉じ括弧位置
            const auto close = message.find(')', open + 1);
            if (close == std::string_view::npos)
            {
                return std::nullopt;
            }
            // 行番号と列番号の区切り位置
            const auto comma = message.find(',', open + 1);
            // 行番号文字列の終了位置
            const auto lineEnd = comma != std::string_view::npos
                    && comma < close
                ? comma
                : close;
            // 1始まりの解析した行番号
            std::uint32_t line{};
            // 行番号を表す文字列
            const auto lineText = message.substr(
                open + 1,
                lineEnd - open - 1);
            // 行番号の数値変換結果
            const auto lineResult = std::from_chars(
                lineText.data(),
                lineText.data() + lineText.size(),
                line);
            if (lineResult.ec != std::errc{}
                || lineResult.ptr != lineText.data() + lineText.size()
                || line == 0)
            {
                searchFrom = open + 1;
                continue;
            }

            // 1始まりの解析した列番号
            std::uint32_t column{ 1 };
            if (comma != std::string_view::npos && comma < close)
            {
                // 列番号を表す文字列
                const auto columnText = message.substr(
                    comma + 1,
                    close - comma - 1);
                // 列番号の数値変換結果
                const auto columnResult = std::from_chars(
                    columnText.data(),
                    columnText.data() + columnText.size(),
                    column);
                if (columnResult.ec != std::errc{}
                    || columnResult.ptr
                        != columnText.data() + columnText.size()
                    || column == 0)
                {
                    column = 1;
                }
            }

            // 直前の改行位置
            const auto newline = message.rfind('\n', open);
            // ファイルパスの開始位置
            auto pathStart = newline == std::string_view::npos
                ? std::size_t{}
                : newline + 1;
            // 前置き説明はコロンと空白で除き、Windowsのドライブ区切りは保持する。
            // 前置き説明の最終区切り位置
            const auto prefix = message.rfind(": ", open);
            if (prefix != std::string_view::npos
                && prefix + 2 > pathStart)
            {
                pathStart = prefix + 2;
            }
            while (pathStart < open
                && std::isspace(static_cast<unsigned char>(
                    message[pathStart])) != 0)
            {
                ++pathStart;
            }
            // ファイルパスの終了位置
            auto pathEnd = open;
            while (pathEnd > pathStart
                && std::isspace(static_cast<unsigned char>(
                    message[pathEnd - 1])) != 0)
            {
                --pathEnd;
            }
            if (pathEnd == pathStart)
            {
                searchFrom = close + 1;
                continue;
            }

            return ShaderDiagnosticLocation{
                std::filesystem::path{ std::string{ message.substr(
                    pathStart,
                    pathEnd - pathStart) } },
                line,
                column
            };
        }
        return std::nullopt;
    }

    ShaderEntryPoints ParseShaderEntryPoints(
        const std::string_view source)
    {
        // コメント除去済みのソース
        const auto stripped = StripComments(source);
        // 検出したシェーダー入口候補
        ShaderEntryPoints entryPoints;
        entryPoints.vertex = HasEntryPoint(stripped, "VSMain");
        entryPoints.pixel = HasEntryPoint(stripped, "PSMain");
        entryPoints.skinnedVertex =
            HasEntryPoint(stripped, "VSSkinnedMain");
        entryPoints.skinnedPixel =
            HasEntryPoint(stripped, "PSSkinnedMain");
        entryPoints.geometry = HasEntryPoint(stripped, "GSMain");
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
        // 元の診断へ添える説明
        std::string hint;

        // includeの解決失敗には配置と更新方法の説明を添える。
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
        // 入口不足には用途に合う入口名を案内する。
        else if (Contains(compilerMessage, "X3501")
            || Contains(compilerMessage, "entrypoint not found"))
        {
            // 検出したシェーダー入口候補
            const auto entryPoints =
                ParseShaderEntryPoints(source);
            // 診断に引用された未検出名
            const auto missing =
                FirstQuotedName(compilerMessage);
            // 骨格用入口の不足有無
            const bool missingSkinned =
                missing == "VSSkinnedMain"
                || missing == "PSSkinnedMain";
            // 3Dの入口があっても骨格用入口が不足する場合は区別して案内する。
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
                // 入口候補から推定した用途
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
        // セマンティクス不足には用途に合う引数と戻り値の規約を添える。
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
        // 既知の組込識別子が未定義の場合だけ定数バッファの説明を添える。
        else if (Contains(compilerMessage, "X3004")
            || Contains(compilerMessage, "undeclared identifier"))
        {
            // 診断に引用された未定義名
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
