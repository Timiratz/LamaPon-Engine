#pragma once

#include "LamaPon/Graphics/ShaderRenderState.h"
#include "LamaPon/Graphics/ShaderVariants.h"

#include <d3dcommon.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace LamaPon
{
    class AssetManager;

    // 依存を照合してHLSLをコンパイルまたはキャッシュから取得する(assets: 借用する資産管理器, path: HLSLソースパス, entryPoint: 入口関数名, target: コンパイル先の形式, defines: 整列済みマクロ一覧)。
    // ソース不在なら索引を使い、コンパイル失敗または書出し先への保存失敗は例外で返す。
    // 通常の保存失敗は結果を返し、入口不足だけを依存情報付きの失敗記録へ残す。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob>
        CompileShaderCached(
            AssetManager& assets,
            const std::filesystem::path& path,
            const char* entryPoint,
            const char* target,
            const std::vector<std::string>& defines = {});

    // 同じマクロ集合の依存ファイルの状態を識別値へまとめる(assets: 借用する資産管理器, path: HLSLソースパス, defines: 整列済みマクロ一覧)。
    // 未記録・アーカイブ・取得失敗は0とし、通常はパス・存在・更新時刻・サイズを照合する。
    [[nodiscard]] std::uint64_t ShaderSourceDependencyRevision(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::vector<std::string>& defines = {}) noexcept;

    // 既知の入口を試してCPU側のコンパイル結果を用意する(assets: 借用する資産管理器, path: HLSLソースパス, keywords: 整列済みマクロ一覧)。
    // GPU資源を作らず標準例外を破棄するため、通常ファイルの読込時だけワーカーで使う。
    void WarmShaderCache(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::vector<std::string>& keywords);

    // 指定先の未確定索引とメタデータを既存索引へ追記する(directory: 事前コンパイルの保存先)。
    // 保存失敗は例外で返し、取り出した未確定索引を待機集合へは戻さない。
    void WriteShaderCacheIndex(
        const std::filesystem::path& directory);

    // 通常キャッシュの保存先を返し、解決不能なら空を返す。
    [[nodiscard]] std::filesystem::path ShaderCacheDirectory();

    // 読込用ディレクトリと索引を追加する(directory: キャッシュの検索先)。
    void AddShaderCacheSearchDirectory(
        std::filesystem::path directory);
    // 読込用ディレクトリ・索引・メタデータを消去する。
    void ClearShaderCacheSearchDirectories();

    // ソースを含まない配布物の描画宣言を復元する(assets: 借用する資産管理器, path: HLSLソースパス, renderState: 任意の出力描画状態, variants: 任意の出力バリアント宣言)。
    [[nodiscard]] bool LoadPrecompiledShaderMetadata(
        AssetManager& assets,
        const std::filesystem::path& path,
        ShaderRenderState* renderState,
        ShaderVariantDeclaration* variants);

    // 入口名とコンパイル形式の組。
    struct ShaderEntryPoint final
    {
        // コンパイルする入口関数名
        const char* entryPoint;
        // コンパイル先の形式
        const char* target;
    };

    // エンジンが探索する既知の入口と形式の一覧を返す。
    [[nodiscard]] const std::vector<ShaderEntryPoint>&
        KnownShaderEntryPoints();

    // 既知の入口とバリアントを事前コンパイルして成功数を返す(assets: 借用する資産管理器, path: HLSLソースパス, destinationDirectory: キャッシュ出力先, defines: 固定の追加マクロ一覧, usedKeywords: 任意の使用キーワード一覧)。
    std::uint32_t PrecompileShader(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        const std::vector<std::string>& defines = {},
        const std::vector<std::string>* usedKeywords = nullptr);

    // 指定された入口を一回ずつ事前コンパイルし成功数を返す(assets: 借用する資産管理器, path: HLSLソースパス, destinationDirectory: キャッシュ出力先, entryPoints: 入口と形式の借用一覧, defines: 整列済みマクロ一覧, error: 任意の出力診断)。
    // バリアントは展開せず、入口名と形式の文字列は呼出し中保持し、保存失敗の例外は伝播させる。
    std::uint32_t PrecompileShader(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        std::span<const ShaderEntryPoint> entryPoints,
        const std::vector<std::string>& defines,
        std::string* error = nullptr);

    // 指定入口でHLSLのバリアントを展開して成功数を返す(assets: 借用する資産管理器, path: HLSLソースパス, destinationDirectory: キャッシュ出力先, entryPoints: 入口と形式の借用一覧, defines: 固定の追加マクロ一覧, usedKeywords: 任意の使用キーワード一覧, error: 任意の出力診断)。
    std::uint32_t PrecompileShaderVariants(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        std::span<const ShaderEntryPoint> entryPoints,
        const std::vector<std::string>& defines = {},
        const std::vector<std::string>* usedKeywords = nullptr,
        std::string* error = nullptr);

    // 前回のリセットからの累積計測。
    struct ShaderCompileStats final
    {
        // 実コンパイルの試行回数
        std::uint32_t compiledCount{};
        // 成功と失敗のキャッシュ命中数
        std::uint32_t cacheHitCount{};
        // 実コンパイルの累積ミリ秒
        double compileMilliseconds{};
        // 内容キャッシュ読込のミリ秒
        double cacheReadMilliseconds{};
    };

    // コンパイルとキャッシュ利用の累積計測を取得する。
    [[nodiscard]] ShaderCompileStats ShaderCompileStatistics() noexcept;
    // コンパイルとキャッシュ利用の累積計測を消去する。
    void ResetShaderCompileStatistics() noexcept;

    // 次の実コンパイル後に一回だけ実行する試験用処理を設定する(hook: 依存記録直後のコールバック)。
    // コンパイル中の保存を再現する回帰試験だけで設定する。
    void SetShaderCompileCompletionHookForTesting(
        std::function<void()> hook);

    // 依存記録と通常キャッシュの保存ディレクトリを消去する。
    void ClearShaderCache();
    // 通常キャッシュの失敗記録と対応依存情報を削除し成功数を返す。
    std::size_t ClearShaderCacheFailures();
    // 通常のキャッシュ読込と保存を切り替える(enabled: 利用フラグ)。
    // 事前コンパイルの保存とソース不在時の索引読込には適用しない。
    void SetShaderCacheEnabled(bool enabled) noexcept;
    // 通常のキャッシュ利用設定を返す。
    [[nodiscard]] bool IsShaderCacheEnabled() noexcept;
}
