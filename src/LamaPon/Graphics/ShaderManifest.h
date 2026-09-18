#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class AssetManager;

    // Shader Manifestが表現できるプログラマブルステージです。
    // GPU API固有の型を持たせず、D3D11以外のバックエンドでも
    // 同じ定義を読み取れる形にしています。
    enum class ShaderStage
    {
        Vertex,
        Pixel,
        Geometry,
        Hull,
        Domain,
        Compute
    };

    struct ShaderStageDesc final
    {
        ShaderStage stage{};
        std::string entryPoint;
        std::string target;
        bool optional{ false };
    };

    // MVPでは描画状態を宣言どおり保持します。D3D11の状態への変換は
    // 実際にPassを使用する描画側の責務です。
    struct RenderStateDesc final
    {
        std::string zTest{ "LessEqual" };
        bool zWrite{ true };
        std::string cull{ "Back" };
        std::string blend{ "Opaque" };
    };

    // pass名は表示・診断用、roleは描画経路を選ぶための独立した値です。
    // 名前から役割を推測しないため、自由なpass名を保ったまま同じroleを
    // 複数回、宣言順に実行できます。
    enum class ShaderPassRole
    {
        Forward,
        Skinned,
        Instanced,
        Outline,
        SkinnedOutline,
        Occluded
    };

    inline constexpr std::size_t ShaderPassRoleCount = 6;

    struct ShaderPassDesc final
    {
        std::string name;
        ShaderPassRole role{ ShaderPassRole::Forward };
        std::vector<ShaderStageDesc> stages;
        RenderStateDesc renderState;
    };

    enum class ShaderAssetType
    {
        Material,
        ScreenEffect,
        Compute
    };

    struct ShaderPropertyDesc final
    {
        std::string name;
        std::string type;
        // CustomParametersの成分（"0.x"／"1.rgb"）または追加
        // テクスチャslot（"t7"〜"t10"）。Phase 1のManifestとの
        // 後方互換のため、省略時は空文字列のまま保持します。
        std::string target;
        // defaultの正規化されたJSON表現です。未指定時は空文字列に
        // なります。JSONライブラリの型を公開APIへ漏らさず、MVPで
        // 値の種類と内容を保持できるようにしています。
        std::string defaultValue;
        std::optional<double> minimum;
        std::optional<double> maximum;
    };

    struct ShaderAssetDesc final
    {
        int version{ 1 };
        std::string name;
        ShaderAssetType type{};
        std::filesystem::path source;
        std::vector<ShaderPropertyDesc> properties;
        std::vector<ShaderPassDesc> passes;
    };

    // JSONテキストを読み取り、version 1のShader Manifestとして
    // 検証します。失敗時はoutDescを変更せず、errorへ原因を返します。
    [[nodiscard]] bool ParseShaderAssetDesc(
        std::string_view jsonText,
        ShaderAssetDesc& outDesc,
        std::string& error);

    // AssetManager経由で.lamashader.jsonを読み取ります。これにより
    // 展開済みassets/と配布用アーカイブを同じ経路で扱えます。
    // sourceの相対パスはAssetManagerのルート相対のまま保持します。
    [[nodiscard]] bool LoadShaderAssetDesc(
        AssetManager& assets,
        const std::filesystem::path& manifestPath,
        ShaderAssetDesc& outDesc,
        std::string& error);

    [[nodiscard]] const ShaderStageDesc* FindShaderStage(
        const ShaderPassDesc& pass,
        ShaderStage stage) noexcept;

    // 複合拡張子を大文字小文字を区別せず判定します。
    [[nodiscard]] bool IsShaderManifestPath(
        const std::filesystem::path& path) noexcept;
}
