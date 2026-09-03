#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // Shaderバリアント（#pragma multi_compileで指定する組み合わせ）。
    //
    // 1本のシェーダーの中でif分岐を書くと、使わない枝の分もGPUが
    // レジスタを確保し続けるため、実際には通らない機能のぶんまで
    // 遅くなります。バリアントは「キーワードの組み合わせごとに
    // 別々のシェーダーとしてコンパイルしてしまう」やり方で、
    // 分岐そのものを消します。
    //
    //   #pragma multi_compile _ FOG_ON
    //   #pragma shader_feature _ DETAIL_ON
    //
    // 1行が1グループで、そこから必ず1つが選ばれます。`_`は
    // 「どのキーワードも立てない」組み合わせを表します。
    // 選ばれたキーワードは `#define FOG_ON 1` として渡ります。
    //
    // multi_compileとshader_featureの違いは**書き出し時だけ**です。
    // multi_compileは全組み合わせを同梱し、shader_featureは
    // 実際にどれかのマテリアルが使っている組み合わせだけを同梱
    // します。エディターでの
    // 挙動は同じです。
    enum class ShaderVariantKind
    {
        // 常に全組み合わせを用意します。
        MultiCompile,
        // 使われている組み合わせだけを書き出しへ含めます。
        ShaderFeature
    };

    struct ShaderVariantGroup final
    {
        ShaderVariantKind kind{
            ShaderVariantKind::MultiCompile };
        // このグループの選択肢。`_`は空文字列として入ります
        // （「どれも立てない」を表します）。
        std::vector<std::string> keywords;
    };

    struct ShaderVariantDeclaration final
    {
        std::vector<ShaderVariantGroup> groups;
        // 宣言の誤り（空なら問題なし）。Inspectorへ出します。
        std::string error;

        [[nodiscard]] bool Empty() const noexcept
        {
            return groups.empty();
        }

        // 全組み合わせの数。グループの選択肢数の掛け算です。
        [[nodiscard]] std::size_t VariantCount() const noexcept;
    };

    // 組み合わせの上限。これを超える宣言は読み取り時に弾きます。
    //
    // 組み合わせが増えすぎるのを防ぐための制限です。multi_compileを1行足すたびに
    // 組み合わせは倍になり、
    // 10行書けば1024通り、そのすべてが書き出しのコンパイル時間と
    // 配布サイズになります。後から気付くと直すのが大変なので、
    // 最初から止めます。
    inline constexpr std::size_t MaximumShaderVariants = 64;

    // HLSLソースから#pragma multi_compile / shader_featureを読みます。
    [[nodiscard]] ShaderVariantDeclaration ParseShaderVariants(
        std::string_view source);

    // 有効なキーワードの集合。並びは常に整列されているので、
    // そのままキャッシュのキーや比較に使えます。
    class ShaderKeywordSet final
    {
    public:
        ShaderKeywordSet() = default;
        explicit ShaderKeywordSet(
            std::vector<std::string> keywords);

        void Enable(std::string keyword);
        void Disable(std::string_view keyword);
        void Set(std::string_view keyword, bool enabled);
        [[nodiscard]] bool IsEnabled(
            std::string_view keyword) const noexcept;

        [[nodiscard]] const std::vector<std::string>&
            Keywords() const noexcept
        {
            return m_keywords;
        }

        [[nodiscard]] bool Empty() const noexcept
        {
            return m_keywords.empty();
        }

        // キャッシュのキーやログに使う一意な文字列。
        // 例: "FOG_ON+SHADOWS_HIGH"、何も無ければ空文字列。
        [[nodiscard]] std::string Key() const;

        [[nodiscard]] bool operator==(
            const ShaderKeywordSet& other) const noexcept
        {
            return m_keywords == other.m_keywords;
        }

    private:
        // 常に整列・重複なし。
        std::vector<std::string> m_keywords;
    };

    // 宣言に無いキーワードを落とし、各グループから高々1つだけを
    // 残します。マテリアルの保存値が古いシェーダー由来でも、
    // 存在しないキーワードでコンパイルしないようにするためです。
    [[nodiscard]] ShaderKeywordSet NormalizeKeywords(
        const ShaderVariantDeclaration& declaration,
        const ShaderKeywordSet& requested);

    // 宣言から全組み合わせを作ります。上限を超える場合は空を返します。
    //
    // usedKeywordsを渡すと、shader_featureのグループは「そこに
    // 入っているキーワード」と「立てない(_)」だけへ絞ります
    // 使用中の組み合わせだけを残します。multi_compileの
    // グループは常に全部作ります。
    //
    // 絞るのは安全側です。取りこぼした組み合わせは実行時に
    // コンパイルされるだけで、動かなくなることはありません。
    [[nodiscard]] std::vector<ShaderKeywordSet>
        EnumerateShaderVariants(
            const ShaderVariantDeclaration& declaration,
            const std::vector<std::string>* usedKeywords
                = nullptr);
}
