#include "LamaPon/Graphics/ShaderVariants.h"

#include <algorithm>
#include <string>
#include <cctype>

namespace
{
    [[nodiscard]] bool IsSpace(const char value) noexcept
    {
        return value == ' '
            || value == '\t'
            || value == '\r'
            || value == '\f'
            || value == '\v';
    }

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

    // キーワードとして使える文字か（HLSLのマクロ名になるため）。
    [[nodiscard]] bool IsKeywordCharacter(
        const char value) noexcept
    {
        return value == '_'
            || (value >= '0' && value <= '9')
            || (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z');
    }

    [[nodiscard]] bool IsValidKeyword(
        const std::string_view keyword) noexcept
    {
        if (keyword.empty())
        {
            return false;
        }
        // 先頭が数字のものはマクロ名にできません。
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
        std::size_t count = 1;
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
        ShaderVariantDeclaration declaration;
        std::size_t offset = 0;
        while (offset <= source.size())
        {
            const auto lineEnd = source.find('\n', offset);
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
            auto rest = TrimSpace(line.substr(1));
            if (rest.rfind("pragma", 0) != 0)
            {
                continue;
            }
            rest = TrimSpace(rest.substr(6));

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

            // 行末コメントは切り落とします。
            if (const auto comment = rest.find("//");
                comment != std::string_view::npos)
            {
                rest = TrimSpace(rest.substr(0, comment));
            }

            ShaderVariantGroup group;
            group.kind = kind;
            std::size_t tokenStart = 0;
            while (tokenStart <= rest.size())
            {
                auto tokenEnd = tokenStart;
                while (tokenEnd < rest.size()
                    && !IsSpace(rest[tokenEnd]))
                {
                    ++tokenEnd;
                }
                const auto token =
                    rest.substr(tokenStart, tokenEnd - tokenStart);
                if (!token.empty())
                {
                    if (token == "_")
                    {
                        // 「どれも立てない」枠。
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

            // 選択肢が1つしかないグループは組み合わせを増やさない
            // ので、宣言としては無意味です。書き間違いの可能性が
            // 高いので落とします（エラーにはしません）。
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
        std::string key;
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
        ShaderKeywordSet result;
        for (const auto& group : declaration.groups)
        {
            // グループの中で最初に見つかった有効なものだけを残します。
            // 2つ以上立っていると、どちらの#defineも渡って
            // シェーダー側の想定が崩れるためです。
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
        std::vector<ShaderKeywordSet> variants;
        if (declaration.VariantCount() > MaximumShaderVariants)
        {
            return variants;
        }
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

        // 何も宣言が無くても「キーワード無し」の1本は必要です。
        variants.emplace_back();
        for (const auto& group : declaration.groups)
        {
            // shader_featureは「実際に使われているものだけ」へ
            // 絞ります。multi_compileは常に全部です。
            std::vector<std::string> choices;
            bool hasEmpty = false;
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
            // 絞った結果が空でも「立てない」だけは残します
            // （そのグループを使っていない、という組み合わせ）。
            if (hasEmpty || choices.empty())
            {
                choices.insert(choices.begin(), std::string{});
            }

            std::vector<ShaderKeywordSet> expanded;
            expanded.reserve(variants.size() * choices.size());
            for (const auto& base : variants)
            {
                for (const auto& keyword : choices)
                {
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
