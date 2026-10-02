#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // 各pragma行から一つを選び、選択キーワードをマクロ定義として渡す。
    enum class ShaderVariantKind
    {
        // 全選択肢を書き出す方式
        MultiCompile,
        // 使用キーワードで絞り込む方式
        ShaderFeature
    };

    struct ShaderVariantGroup final
    {
        // 組合せの書出し方式
        ShaderVariantKind kind{
            ShaderVariantKind::MultiCompile };
        // 空文字を含む宣言順の選択肢
        std::vector<std::string> keywords;
    };

    struct ShaderVariantDeclaration final
    {
        // 宣言順のバリアントグループ
        std::vector<ShaderVariantGroup> groups;
        // 空なら正常の宣言診断
        std::string error;

        // バリアントのグループ宣言が空か返す。
        [[nodiscard]] bool Empty() const noexcept
        {
            return groups.empty();
        }

        // グループの選択肢数を掛け合わせ、宣言なしなら1を返す。
        [[nodiscard]] std::size_t VariantCount() const noexcept;
    };

    // 読込と列挙に適用する組合せ上限
    inline constexpr std::size_t MaximumShaderVariants = 64;

    // HLSLのバリアント宣言を読み、誤りを診断に格納する(source: HLSLソース文字列)。
    [[nodiscard]] ShaderVariantDeclaration ParseShaderVariants(
        std::string_view source);

    // キーワード名の検証は行わないため、HLSLマクロ名として有効な文字列を渡す。
    class ShaderKeywordSet final
    {
    public:
        // 空のキーワード集合を作る。
        ShaderKeywordSet() = default;
        // 空文字と重複を除いてキーワードを整列する(keywords: 初期キーワード一覧)。
        explicit ShaderKeywordSet(
            std::vector<std::string> keywords);

        // 空文字と重複を除いてキーワードを追加する(keyword: 有効化するキーワード)。
        void Enable(std::string keyword);
        // 該当するキーワードを取り除く(keyword: 無効化するキーワード)。
        void Disable(std::string_view keyword);
        // キーワードの有効状態を設定する(keyword: 対象キーワード, enabled: 有効化フラグ)。
        void Set(std::string_view keyword, bool enabled);
        // キーワードが保存されているか返す(keyword: 確認するキーワード)。
        [[nodiscard]] bool IsEnabled(
            std::string_view keyword) const noexcept;

        // 整列済みで重複のないキーワード一覧を返す。
        [[nodiscard]] const std::vector<std::string>&
            Keywords() const noexcept
        {
            return m_keywords;
        }

        // 有効なキーワード集合が空か返す。
        [[nodiscard]] bool Empty() const noexcept
        {
            return m_keywords.empty();
        }

        // キーワードをプラス記号で連結したキャッシュキーを返す。
        [[nodiscard]] std::string Key() const;

        // 保存されたキーワード集合が一致するか返す(other: 比較するキーワード集合)。
        [[nodiscard]] bool operator==(
            const ShaderKeywordSet& other) const noexcept
        {
            return m_keywords == other.m_keywords;
        }

    private:
        // 整列済みで重複なしの有効集合
        std::vector<std::string> m_keywords;
    };

    // 各グループの宣言順で最初の要求キーワードを残す(declaration: バリアント宣言, requested: 要求キーワード集合)。
    [[nodiscard]] ShaderKeywordSet NormalizeKeywords(
        const ShaderVariantDeclaration& declaration,
        const ShaderKeywordSet& requested);

    // 上限内の組合せを列挙し、未宣言なら空キーワード1件を返す(declaration: バリアント宣言, usedKeywords: 絞込用の使用中一覧)。
    // 一覧の指定時はshader_featureだけ絞り、選択肢がなくなったグループには空選択肢を残す。
    [[nodiscard]] std::vector<ShaderKeywordSet>
        EnumerateShaderVariants(
            const ShaderVariantDeclaration& declaration,
            const std::vector<std::string>* usedKeywords
                = nullptr);
}
