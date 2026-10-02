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

    // シェーダー宣言で扱う描画API共通のステージ種別。
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
        // プログラムのステージ種別
        ShaderStage stage{};
        // 入口関数名
        std::string entryPoint;
        // コンパイル先の形式
        std::string target;
        // 入口未検出を許容するフラグ
        bool optional{ false };
    };

    // 描画API固有の状態への変換はパスを使う描画側で行う。
    struct RenderStateDesc final
    {
        // 深度比較方式
        std::string zTest{ "LessEqual" };
        // 深度書込フラグ
        bool zWrite{ true };
        // 描画を省く面の指定
        std::string cull{ "Back" };
        // 色とアルファの合成方式
        std::string blend{ "Opaque" };
    };

    // 描画経路は名前ではなくroleで選び、同じroleのパスも宣言順で実行する。
    enum class ShaderPassRole
    {
        Forward,
        Skinned,
        Instanced,
        Outline,
        SkinnedOutline,
        Occluded
    };

    // 描画パスの種別数
    inline constexpr std::size_t ShaderPassRoleCount = 6;

    struct ShaderPassDesc final
    {
        // 表示と診断に使うパス名
        std::string name;
        // 描画経路を選ぶ種別
        ShaderPassRole role{ ShaderPassRole::Forward };
        // パスのステージ記述一覧
        std::vector<ShaderStageDesc> stages;
        // 宣言された描画状態
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
        // 表示に使うプロパティ名
        std::string name;
        // プロパティの値の型
        std::string type;
        // 定数成分または追加画像番号
        // 空ならエディターが空き領域へ割り当てる。
        std::string target;
        // 未指定なら空の既定値JSON
        std::string defaultValue;
        // 任意の数値下限
        std::optional<double> minimum;
        // 任意の数値上限
        std::optional<double> maximum;
    };

    struct ShaderAssetDesc final
    {
        // 宣言形式の版番号
        int version{ 1 };
        // シェーダー資産の表示名
        std::string name;
        // シェーダー資産の用途
        ShaderAssetType type{};
        // 資産ルート相対のHLSLパス
        std::filesystem::path source;
        // 宣言順のプロパティ記述
        std::vector<ShaderPropertyDesc> properties;
        // 宣言順の描画パス記述
        std::vector<ShaderPassDesc> passes;
    };

    // 版1のシェーダー宣言を検証して出力する(jsonText: 入力JSON文字列, outDesc: 成功時に置換する記述, error: 出力診断)。
    // 失敗時は出力記述を保持し、成功時は診断を空にする。
    [[nodiscard]] bool ParseShaderAssetDesc(
        std::string_view jsonText,
        ShaderAssetDesc& outDesc,
        std::string& error);

    // 資産管理器から宣言を読んで検証する(assets: 借用する資産管理器, manifestPath: 宣言ファイルのパス, outDesc: 成功時に置換する記述, error: 出力診断)。
    // HLSLの相対パスは宣言ファイル基準にせず、資産ルート相対のまま保持する。
    [[nodiscard]] bool LoadShaderAssetDesc(
        AssetManager& assets,
        const std::filesystem::path& manifestPath,
        ShaderAssetDesc& outDesc,
        std::string& error);

    // 指定ステージの最初の記述を借用し、未検出なら空を返す(pass: 対象の描画パス, stage: 確認するステージ種別)。
    [[nodiscard]] const ShaderStageDesc* FindShaderStage(
        const ShaderPassDesc& pass,
        ShaderStage stage) noexcept;

    // 複合拡張子をASCIIの大小文字を区別せず判定する(path: 確認するパス)。
    [[nodiscard]] bool IsShaderManifestPath(
        const std::filesystem::path& path) noexcept;
}
