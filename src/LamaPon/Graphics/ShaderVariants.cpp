#include "LamaPon/Graphics/ShaderVariants.h"

#include <algorithm>
#include <string>
#include <cctype>

namespace
{
    // 改行以外の空白文字か判定する(value: 確認する文字)。
    [[nodiscard]] bool IsSpace(const char value) noexcept
    {
        return value == ' '
            || value == '\t'
            || value == '\r'
            || value == '\f'
            || value == '\v';
    }

    // 前後の空白を除いた借用文字列を返す(text: 入力文字列)。
    [[nodiscard]] std::string_view TrimSpace(
        std::string_view text) noexcept
    {
        while (!text.empty() && IsSpace(text.front()))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && IsSpace(text.back()))
        {
            text.remove_suffix(1);
        }
        return text;
    }

    // HLSLマクロ名に使える英数字か下線か判定する(value: 確認する文字)。
    [[nodiscard]] bool IsKeywordCharacter(
        const char value) noexcept
    {
        return value == '_'
            || (value >= '0' && value <= '9')
            || (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z');
    }

    // 先頭が数字でないHLSLマクロ名か判定する(keyword: 確認するキーワード)。
    [[nodiscard]] bool IsValidKeyword(
        const std::string_view keyword) noexcept
    {
        if (keyword.empty())
        {
            return false;
        }
        // 先頭の数字はHLSLマクロ名として無効とする。
        if (keyword.front() >= '0' && keyword.front() <= '9')
        {
            return false;
        }
        return std::all_of(
            keyword.begin(),
            keyword.end(),
            IsKeywordCharacter);
    }
}

namespace LamaPon
{
    std::size_t
        ShaderVariantDeclaration::VariantCount() const noexcept
    {
        // バリアントの組合せ数
        std::size_t count = 1;
        // 対象のバリアントグループ
        for (const auto& group : groups)
        {
            if (group.keywords.empty())
            {
                continue;
            }
            count *= group.keywords.size();
        }
        return count;
    }

    ShaderVariantDeclaration ParseShaderVariants(
        const std::string_view source)
    {
        // 解析中の宣言と診断
        ShaderVariantDeclaration declaration;
        // 次に読むソース位置
        std::size_t offset = 0;
        while (offset <= source.size())
        {
            // 行末改行のソース位置
            const auto lineEnd = source.find('\n', offset);
            // 前後の空白を除いた入力行
            const auto line = TrimSpace(source.substr(
                offset,
                lineEnd == std::string_view::npos
                    ? std::string_view::npos
                    : lineEnd - offset));
            offset = lineEnd == std::string_view::npos
                ? source.size() + 1
                : lineEnd + 1;

            if (line.empty() || line.front() != '#')
            {
                continue;
            }
            // 未処理のpragma引数
            auto rest = TrimSpace(line.substr(1));
            if (rest.rfind("pragma", 0) != 0)
            {
                continue;
            }
            rest = TrimSpace(rest.substr(6));

            // バリアントの書出し方式
            ShaderVariantKind kind{};
            if (rest.rfind("multi_compile", 0) == 0)
            {
                kind = ShaderVariantKind::MultiCompile;
                rest = TrimSpace(rest.substr(13));
            }
            else if (rest.rfind("shader_feature", 0) == 0)
            {
                kind = ShaderVariantKind::ShaderFeature;
                rest = TrimSpace(rest.substr(14));
            }
            else
            {
                continue;
            }

            // pragma引数から行末コメントを除く。
            // 行末コメントの開始位置
            if (const auto comment = rest.find("//");
                comment != std::string_view::npos)
            {
                rest = TrimSpace(rest.substr(0, comment));
            }

            // 対象のバリアントグループ
            ShaderVariantGroup group;
            group.kind = kind;
            // 選択肢の開始位置
            std::size_t tokenStart = 0;
            while (tokenStart <= rest.size())
            {
                // 選択肢の終了位置
                auto tokenEnd = tokenStart;
                while (tokenEnd < rest.size()
                    && !IsSpace(rest[tokenEnd]))
                {
                    ++tokenEnd;
                }
                // 解析中の選択肢文字列
                const auto token =
                    rest.substr(tokenStart, tokenEnd - tokenStart);
                if (!token.empty())
                {
                    if (token == "_")
                    {
                        // 下線だけの選択肢は未定義を表す空文字として保持する。
                        group.keywords.emplace_back();
                    }
                    else if (IsValidKeyword(token))
                    {
                        group.keywords.emplace_back(token);
                    }
                    else
                    {
                        declaration.error =
                            "キーワードに使えない文字が"
                            "含まれています: "
                            + std::string(token);
                        return declaration;
                    }
                }
                if (tokenEnd >= rest.size())
                {
                    break;
                }
                tokenStart = tokenEnd + 1;
            }

            // 二つ未満の選択肢を持つ行は診断を出さずに除外する。
            if (group.keywords.size() < 2)
            {
                continue;
            }
            declaration.groups.push_back(std::move(group));
        }

        if (declaration.VariantCount() > MaximumShaderVariants)
        {
            declaration.error =
                "バリアントの組み合わせが多すぎます（"
                + std::to_string(declaration.VariantCount())
                + "通り、上限は"
                + std::to_string(MaximumShaderVariants)
                + "通り）。multi_compileを1行足すたびに"
                "組み合わせは倍になります。";
            declaration.groups.clear();
        }
        return declaration;
    }

    ShaderKeywordSet::ShaderKeywordSet(
        std::vector<std::string> keywords)
        : m_keywords(std::move(keywords))
    {
        std::sort(m_keywords.begin(), m_keywords.end());
        // 空文字のキーワードを除く(keyword: 対象キーワード)。
        m_keywords.erase(
            std::remove_if(
                m_keywords.begin(),
                m_keywords.end(),
                [](const std::string& keyword)
                {
                    return keyword.empty();
                }),
            m_keywords.end());
        m_keywords.erase(
            std::unique(m_keywords.begin(), m_keywords.end()),
            m_keywords.end());
    }

    void ShaderKeywordSet::Enable(std::string keyword)
    {
        if (keyword.empty())
        {
            return;
        }
        // キーワード集合内の対象位置
        const auto position = std::lower_bound(
            m_keywords.begin(),
            m_keywords.end(),
            keyword);
        if (position != m_keywords.end()
            && *position == keyword)
        {
            return;
        }
        m_keywords.insert(position, std::move(keyword));
    }

    void ShaderKeywordSet::Disable(
        const std::string_view keyword)
    {
        // キーワード集合内の対象位置
        const auto position = std::find(
            m_keywords.begin(),
            m_keywords.end(),
            keyword);
        if (position != m_keywords.end())
        {
            m_keywords.erase(position);
        }
    }

    void ShaderKeywordSet::Set(
        const std::string_view keyword,
        const bool enabled)
    {
        if (enabled)
        {
            Enable(std::string(keyword));
        }
        else
        {
            Disable(keyword);
        }
    }

    bool ShaderKeywordSet::IsEnabled(
        const std::string_view keyword) const noexcept
    {
        return std::binary_search(
            m_keywords.begin(),
            m_keywords.end(),
            keyword);
    }

    std::string ShaderKeywordSet::Key() const
    {
        // 連結するキャッシュキー
        std::string key;
        // 確認または追加するキーワード
        for (const auto& keyword : m_keywords)
        {
            if (!key.empty())
            {
                key.push_back('+');
            }
            key.append(keyword);
        }
        return key;
    }

    ShaderKeywordSet NormalizeKeywords(
        const ShaderVariantDeclaration& declaration,
        const ShaderKeywordSet& requested)
    {
        // 正規化したキーワード集合
        ShaderKeywordSet result;
        // 対象のバリアントグループ
        for (const auto& group : declaration.groups)
        {
            // 同じグループでは宣言順で最初の要求キーワードだけを残す。
            // 確認または追加するキーワード
            for (const auto& keyword : group.keywords)
            {
                if (!keyword.empty()
                    && requested.IsEnabled(keyword))
                {
                    result.Enable(keyword);
                    break;
                }
            }
        }
        return result;
    }

    std::vector<ShaderKeywordSet> EnumerateShaderVariants(
        const ShaderVariantDeclaration& declaration,
        const std::vector<std::string>* usedKeywords)
    {
        // 列挙中のキーワード組合せ
        std::vector<ShaderKeywordSet> variants;
        if (declaration.VariantCount() > MaximumShaderVariants)
        {
            return variants;
        }
        // 一覧が指定された場合だけ使用有無を調べる(keyword: 対象キーワード)。
        const auto isUsed =
            [usedKeywords](const std::string& keyword)
        {
            if (usedKeywords == nullptr)
            {
                return true;
            }
            return std::find(
                usedKeywords->begin(),
                usedKeywords->end(),
                keyword) != usedKeywords->end();
        };

        // 宣言がなくても未定義の組合せを一件残す。
        variants.emplace_back();
        // 対象のバリアントグループ
        for (const auto& group : declaration.groups)
        {
            // 使用一覧の指定時はshader_featureだけ選択肢を絞る。
            // 対象グループの有効な選択肢
            std::vector<std::string> choices;
            // 空の選択肢の宣言有無
            bool hasEmpty = false;
            // 確認または追加するキーワード
            for (const auto& keyword : group.keywords)
            {
                if (keyword.empty())
                {
                    hasEmpty = true;
                    continue;
                }
                if (group.kind
                        == ShaderVariantKind::MultiCompile
                    || isUsed(keyword))
                {
                    choices.push_back(keyword);
                }
            }
            // 元の空選択肢または空になったグループに未定義の選択肢を残す。
            if (hasEmpty || choices.empty())
            {
                choices.insert(choices.begin(), std::string{});
            }

            // 一段展開したキーワード組合せ
            std::vector<ShaderKeywordSet> expanded;
            expanded.reserve(variants.size() * choices.size());
            // 展開元のキーワード組合せ
            for (const auto& base : variants)
            {
                // 確認または追加するキーワード
                for (const auto& keyword : choices)
                {
                    // 選択肢を加えたキーワード集合
                    auto next = base;
                    if (!keyword.empty())
                    {
                        next.Enable(keyword);
                    }
                    expanded.push_back(std::move(next));
                }
            }
            variants = std::move(expanded);
        }
        return variants;
    }
}
