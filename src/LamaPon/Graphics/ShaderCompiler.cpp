#include "LamaPon/Graphics/ShaderCompiler.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/ShaderRenderState.h"
#include "LamaPon/Graphics/ShaderVariants.h"

#include <nlohmann/json.hpp>

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // 通常キャッシュの利用フラグ
    std::atomic<bool> g_cacheEnabled{ true };
    // 検索先の排他制御
    std::mutex g_searchMutex;
    // 追加検索先は通常の保存先として使わない。
    // キャッシュの読込検索先一覧
    std::vector<std::filesystem::path> g_searchDirectories;
    // 書出し中は呼出しスレッドだけ保存先を変え、キャッシュ読込を省く。
    // スレッド内の書出し保存先
    thread_local std::filesystem::path g_writeOverride;
    // スレッド内の強制コンパイル
    thread_local bool g_forceCompile{};

    class ScopedPrecompileContext final
    {
    public:
        // 呼出しスレッドだけの事前コンパイル先を切り替える(destination: キャッシュ出力先)。
        explicit ScopedPrecompileContext(
            const std::filesystem::path& destination)
            : m_previousWriteOverride(g_writeOverride)
            , m_previousForceCompile(g_forceCompile)
        {
            g_writeOverride = destination;
            g_forceCompile = true;
        }

        // 事前コンパイル前の保存先と強制コンパイル設定を復元する。
        ~ScopedPrecompileContext()
        {
            g_forceCompile = m_previousForceCompile;
            g_writeOverride = std::move(m_previousWriteOverride);
        }

        // スレッド設定の二重復元を防ぐためコピーを禁止する。
        ScopedPrecompileContext(const ScopedPrecompileContext&) = delete;
        // スレッド設定の二重復元を防ぐためコピー代入を禁止する。
        ScopedPrecompileContext& operator=(
            const ScopedPrecompileContext&) = delete;

    private:
        // 復元するスレッド内保存先
        std::filesystem::path m_previousWriteOverride;
        // 復元する強制コンパイル設定
        bool m_previousForceCompile{};
    };

    class ShaderCacheWriteFailure final : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };
    struct ShaderSourceMetadata final
    {
        // ソース由来の描画状態
        LamaPon::ShaderRenderState renderState;
        // ソース由来のバリアント宣言
        LamaPon::ShaderVariantDeclaration variants;
    };

    struct PendingShaderCacheIndex final
    {
        // 検索識別値と内容識別値の組
        std::vector<std::pair<std::string, std::string>> entries;
        // ソース別の描画宣言
        std::unordered_map<std::string, ShaderSourceMetadata>
            sourceMetadata;
    };

    // 並行する書出しの待機索引を正規化した出力先別に保持する。
    // 未確定索引の排他制御
    std::mutex g_pendingMutex;
    // 書出し先別の未確定索引
    std::unordered_map<std::string, PendingShaderCacheIndex>
        g_pendingByDestination;

    // ソースを含まない配布物の検索索引を保持する。
    // 読込済み索引の排他制御
    std::mutex g_indexMutex;
    // 検索識別値と内容識別値の索引
    std::unordered_map<std::string, std::string> g_cacheIndex;
    // 読込済みのソース別描画宣言
    std::unordered_map<std::string, ShaderSourceMetadata>
        g_sourceMetadata;
    // 結果と索引の保存排他制御
    std::mutex g_cacheFileWriteMutex;

    struct LiveCompileDependencies final
    {
        // マクロ集合の識別値
        std::string defineKey;
        // ソースとincludeと不在候補
        std::vector<std::filesystem::path> files;
    };

    struct LiveShaderDependencies final
    {
        // ソース内容のSHA-256
        std::string sourceHash;
        // コンパイル識別値別の依存記録
        std::unordered_map<std::string, LiveCompileDependencies>
            compileIdentities;
    };

    // 依存の再読込監視はディスクキャッシュと独立して保持する。
    // 依存記録の排他制御
    std::mutex g_liveDependencyMutex;
    // ソース識別値別の依存記録
    std::unordered_map<std::string, LiveShaderDependencies>
        g_liveDependencies;

    // 索引用のUTF-8パスを小文字へ変換する(path: 変換するパス文字列)。
    [[nodiscard]] std::string NormalizeIndexedAssetPath(
        std::string path)
    {
        // 文字を小文字に変換する(character: 確認するUTF-8の一バイト)。
        std::ranges::transform(
            path,
            path.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return path;
    }

    // 出力先を可能なら絶対化し索引識別値へ正規化する(directory: キャッシュ出力先)。
    [[nodiscard]] std::string PendingDestinationKey(
        std::filesystem::path directory)
    {
        // ファイル操作のエラー結果
        std::error_code error;
        // 絶対化した出力先
        auto absolute = std::filesystem::absolute(directory, error);
        if (!error)
        {
            directory = std::move(absolute);
        }
        return NormalizeIndexedAssetPath(
            LamaPon::PathToUtf8(directory.lexically_normal()));
    }

    // ソース不在時の検索用識別値を作る(relativePath: 資産ルート相対パス, entryPoint: 入口関数名, target: コンパイル先の形式, defines: 整列済みマクロ一覧)。
    [[nodiscard]] std::string MakeIndexKey(
        const std::string& relativePath,
        const char* entryPoint,
        const char* target,
        const std::vector<std::string>& defines)
    {
        // 相対パスと入口の検索識別子
        std::string key = relativePath;
        key.push_back('|');
        key.append(entryPoint != nullptr ? entryPoint : "");
        key.push_back('|');
        key.append(target != nullptr ? target : "");
        key.push_back('|');
        // マクロは呼出し側で整列して渡す。
        // 識別に含めるマクロ名
        for (const auto& define : defines)
        {
            key.append(define);
            key.push_back('+');
        }
        return key;
    }

    // 長さ付きマクロ列から依存検索用識別値を作る(defines: 整列済みマクロ一覧)。
    [[nodiscard]] std::string MakeDefineKey(
        const std::vector<std::string>& defines)
    {
        // 長さ付きマクロ列の識別子
        std::string key;
        // 識別に含めるマクロ名
        for (const auto& define : defines)
        {
            key.append(std::to_string(define.size()));
            key.push_back(':');
            key.append(define);
            key.push_back(';');
        }
        return key;
    }

    // 解決済みソースパスから依存管理用識別値を作る(assets: 借用する資産管理器, path: HLSLソースパス)。
    [[nodiscard]] std::string MakeLiveSourceKey(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path)
    {
        return NormalizeIndexedAssetPath(
            LamaPon::PathToUtf8(
                assets.ResolvePath(path).lexically_normal()));
    }

    // 入口・形式・マクロ列を長さ付き識別値にまとめる(entryPoint: 入口関数名, target: コンパイル先の形式, defineKey: マクロ集合の識別値)。
    [[nodiscard]] std::string MakeLiveCompileIdentity(
        const char* const entryPoint,
        const char* const target,
        const std::string_view defineKey)
    {
        // 入口とマクロ列の依存管理識別子
        std::string key;
        // 長さ付きの文字列を識別値へ加える(value: 加える文字列)。
        const auto append = [&key](const std::string_view value)
        {
            key.append(std::to_string(value.size()));
            key.push_back(':');
            key.append(value);
            key.push_back(';');
        };
        append(entryPoint != nullptr
            ? std::string_view{ entryPoint }
            : std::string_view{});
        append(target != nullptr
            ? std::string_view{ target }
            : std::string_view{});
        append(defineKey);
        return key;
    }
    // 累積計測の排他制御
    std::mutex g_statisticsMutex;
    // コンパイルの累積計測
    LamaPon::ShaderCompileStats g_statistics{};
    // 試験用処理の排他制御
    std::mutex g_compileCompletionHookMutex;
    // 次の実コンパイル後の処理
    std::function<void()> g_compileCompletionHook;

    // 待機中の試験用処理を取り出してロック外で一回実行する。
    void RunShaderCompileCompletionHook()
    {
        // 一回実行する試験用処理
        std::function<void()> hook;
        {
            // 共有状態を保護するロック
            const std::lock_guard<std::mutex> lock(
                g_compileCompletionHookMutex);
            hook = std::move(g_compileCompletionHook);
            g_compileCompletionHook = {};
        }
        if (hook)
        {
            hook();
        }
    }

    // キャッシュ形式を変更するときは版番号を更新する。
    // 内容キャッシュ形式の版番号
    constexpr int CacheFormatVersion = 2;


    // SHA-256を十六進文字列にし、計算失敗なら空を返す(data: 入力バイト列, size: 入力バイト数)。
    [[nodiscard]] std::string HashBytes(
        const std::uint8_t* data,
        const std::size_t size)
    {
        // SHA-256のアルゴリズム資源
        BCRYPT_ALG_HANDLE algorithm{};
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0)))
        {
            return {};
        }
        // SHA-256の32バイト結果
        std::array<std::uint8_t, 32> digest{};
        // SHA-256の計算結果
        const NTSTATUS status = BCryptHash(
            algorithm,
            nullptr,
            0,
            const_cast<PUCHAR>(data),
            static_cast<ULONG>(size),
            digest.data(),
            static_cast<ULONG>(digest.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!BCRYPT_SUCCESS(status))
        {
            return {};
        }
        // 小文字の十六進数字表
        static constexpr char Digits[] = "0123456789abcdef";
        // SHA-256の十六進文字列
        std::string text;
        text.reserve(digest.size() * 2);
        // 十六進数へ変換する一バイト
        for (const auto byte : digest)
        {
            text.push_back(Digits[byte >> 4]);
            text.push_back(Digits[byte & 0x0F]);
        }
        return text;
    }

    // バイト配列のSHA-256を十六進文字列にする(data: 入力バイト配列)。
    [[nodiscard]] std::string HashBytes(
        const std::vector<std::uint8_t>& data)
    {
        return HashBytes(data.data(), data.size());
    }

    // 索引用の資産相対パスを返し、失敗時は入力を正規化する(assets: 借用する資産管理器, path: 対象ファイルのパス)。
    [[nodiscard]] std::string RelativeToAssetRoot(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path)
    {
        // 資産ルートのパス
        const auto& root = assets.AssetRoot();
        if (!root.empty())
        {
            // ファイル操作のエラー結果
            std::error_code error;
            // 資産ルートからの相対パス
            const auto relative = std::filesystem::relative(
                path,
                root,
                error);
            if (!error
                && !relative.empty()
                && relative.native().rfind(L"..", 0) != 0)
            {
                return NormalizeIndexedAssetPath(
                    LamaPon::PathToUtf8(
                        relative.lexically_normal()));
            }
        }
        return NormalizeIndexedAssetPath(
            LamaPon::PathToUtf8(path.lexically_normal()));
    }

    // 実際に開いたファイルと不在候補をコンパイル別に記録する(assets: 借用する資産管理器, sourcePath: HLSLソースパス, entryPoint: 入口関数名, target: コンパイル先の形式, defines: 整列済みマクロ一覧, source: HLSLの内容バイト列, dependencies: 開いたパスと内容ハッシュ, missingDependencies: 不在だった候補のパス一覧)。
    void RememberLiveShaderDependencies(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& sourcePath,
        const char* const entryPoint,
        const char* const target,
        const std::vector<std::string>& defines,
        const std::vector<std::uint8_t>& source,
        const std::vector<std::pair<std::string, std::string>>&
            dependencies,
        const std::vector<std::filesystem::path>& missingDependencies = {})
    {
        // 依存管理用のソース識別値
        const auto sourceKey = MakeLiveSourceKey(assets, sourcePath);
        // マクロ集合の識別値
        const auto defineKey = MakeDefineKey(defines);
        // コンパイル単位の識別値
        const auto identity = MakeLiveCompileIdentity(
            entryPoint,
            target,
            defineKey);

        // 記録する依存ファイル一覧
        std::vector<std::filesystem::path> files;
        files.reserve(
            dependencies.size() + missingDependencies.size() + 1);
        files.push_back(
            assets.ResolvePath(sourcePath).lexically_normal());
        // dependency: 依存パス、hash: 内容の識別値
        for (const auto& [dependency, hash] : dependencies)
        {
            static_cast<void>(hash);
            files.push_back(
                assets.ResolvePath(LamaPon::PathFromUtf8(dependency))
                    .lexically_normal());
        }
        // 依存候補のファイルパス
        for (const auto& dependency : missingDependencies)
        {
            files.push_back(
                assets.ResolvePath(dependency).lexically_normal());
        }
        // 正規化したパス順に並べる(left: 左の依存パス, right: 右の依存パス)。
        std::ranges::sort(
            files,
            [](const auto& left, const auto& right)
            {
                return NormalizeIndexedAssetPath(
                    LamaPon::PathToUtf8(left))
                    < NormalizeIndexedAssetPath(
                        LamaPon::PathToUtf8(right));
            });
        // 正規化したパスの重複を除く(left: 左の依存パス, right: 右の依存パス)。
        files.erase(
            std::unique(
                files.begin(),
                files.end(),
                [](const auto& left, const auto& right)
                {
                    return NormalizeIndexedAssetPath(
                        LamaPon::PathToUtf8(left))
                        == NormalizeIndexedAssetPath(
                            LamaPon::PathToUtf8(right));
                }),
            files.end());

        // ソース内容のSHA-256
        const auto sourceHash = HashBytes(source);
        // 共有状態を保護するロック
        const std::lock_guard<std::mutex> lock(
            g_liveDependencyMutex);
        // 更新するソースの依存記録
        auto& tracked = g_liveDependencies[sourceKey];
        if (tracked.sourceHash != sourceHash)
        {
            tracked.sourceHash = sourceHash;
            tracked.compileIdentities.clear();
        }
        tracked.compileIdentities.insert_or_assign(
            identity,
            LiveCompileDependencies{
                defineKey,
                std::move(files)
            });
    }

    // ソースの描画状態とバリアントを出力先別に記録する(assets: 借用する資産管理器, path: HLSLソースパス, destinationDirectory: キャッシュ出力先)。
    void RecordShaderSourceMetadata(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory)
    {
        // 索引用の資産ルート相対パス
        const auto relativePath = RelativeToAssetRoot(assets, path);
        // 書出し先を正規化した識別値
        const auto destinationKey =
            PendingDestinationKey(destinationDirectory);
        if (!assets.FileExists(path))
        {
            return;
        }

        {
            // 未確定索引を保護するロック
            const std::lock_guard<std::mutex> lock(g_pendingMutex);
            // 出力先の未確定索引の位置
            const auto pending =
                g_pendingByDestination.find(destinationKey);
            if (pending != g_pendingByDestination.end()
                && pending->second.sourceMetadata.contains(
                    relativePath))
            {
                return;
            }
        }

        try
        {
            // 読み込んだHLSL内容バイト列
            const auto source = assets.ReadFileBytesFresh(path);
            // 描画宣言を調べるソース文字列
            const std::string_view text{
                reinterpret_cast<const char*>(source.data()),
                source.size()
            };
            // 保存する描画宣言
            ShaderSourceMetadata metadata;
            metadata.renderState =
                LamaPon::ParseShaderRenderState(text);
            metadata.variants = LamaPon::ParseShaderVariants(text);
            // 未確定索引を保護するロック
            const std::lock_guard<std::mutex> lock(g_pendingMutex);
            g_pendingByDestination[destinationKey]
                .sourceMetadata.try_emplace(
                relativePath,
                std::move(metadata));
        }
        catch (const std::exception&)
        {
            // メタデータ抽出の失敗は本体のコンパイル診断へ委ねる。
        }
    }

    // 配布用の描画状態とバリアントをJSONにする(path: 資産ルート相対パス, metadata: 保存する描画宣言)。
    [[nodiscard]] nlohmann::json SerializeShaderSourceMetadata(
        const std::string& path,
        const ShaderSourceMetadata& metadata)
    {
        // 保存するバリアントのJSON列
        nlohmann::json groups = nlohmann::json::array();
        // JSONへ保存するバリアント宣言
        for (const auto& group : metadata.variants.groups)
        {
            groups.push_back({
                { "kind", static_cast<int>(group.kind) },
                { "keywords", group.keywords }
            });
        }
        return {
            { "version", 1 },
            { "path", path },
            { "renderState", {
                { "blend", static_cast<int>(
                    metadata.renderState.blend) },
                { "cull", static_cast<int>(
                    metadata.renderState.cull) },
                { "depthWrite", metadata.renderState.depthWrite },
                { "depthTest", metadata.renderState.depthTest },
                { "declared", metadata.renderState.declared }
            } },
            { "variants", {
                { "groups", std::move(groups) },
                { "error", metadata.variants.error }
            } }
        };
    }

    // 配布用JSONを検証して描画宣言へ戻す(document: 保存されたJSON文書, path: 出力するソースパス, metadata: 出力する描画宣言)。
    [[nodiscard]] bool DeserializeShaderSourceMetadata(
        const nlohmann::json& document,
        std::string& path,
        ShaderSourceMetadata& metadata)
    {
        if (!document.is_object()
            || document.value("version", 0) != 1
            || !document.contains("path")
            || !document["path"].is_string()
            || !document.contains("renderState")
            || !document["renderState"].is_object()
            || !document.contains("variants")
            || !document["variants"].is_object())
        {
            return false;
        }

        // 保存された描画状態JSON
        const auto& state = document["renderState"];
        // 保存された合成方式の数値
        const int blend = state.value("blend", -1);
        // 保存されたカリングの数値
        const int cull = state.value("cull", -1);
        if (blend < static_cast<int>(
                LamaPon::ShaderBlendMode::Opaque)
            || blend > static_cast<int>(
                LamaPon::ShaderBlendMode::Premultiplied)
            || cull < static_cast<int>(
                LamaPon::ShaderCullMode::Back)
            || cull > static_cast<int>(
                LamaPon::ShaderCullMode::None)
            || !state.contains("depthWrite")
            || !state["depthWrite"].is_boolean()
            || !state.contains("depthTest")
            || !state["depthTest"].is_boolean()
            || !state.contains("declared")
            || !state["declared"].is_boolean())
        {
            return false;
        }

        // 検証中の描画宣言
        ShaderSourceMetadata parsed;
        parsed.renderState.blend =
            static_cast<LamaPon::ShaderBlendMode>(blend);
        parsed.renderState.cull =
            static_cast<LamaPon::ShaderCullMode>(cull);
        parsed.renderState.depthWrite =
            state["depthWrite"].get<bool>();
        parsed.renderState.depthTest =
            state["depthTest"].get<bool>();
        parsed.renderState.declared =
            state["declared"].get<bool>();

        // 保存されたバリアントJSON
        const auto& variants = document["variants"];
        if (!variants.contains("groups")
            || !variants["groups"].is_array())
        {
            return false;
        }
        parsed.variants.error = variants.value(
            "error",
            std::string{});
        // 解析するグループJSON
        for (const auto& value : variants["groups"])
        {
            if (!value.is_object()
                || !value.contains("kind")
                || !value["kind"].is_number_integer()
                || !value.contains("keywords")
                || !value["keywords"].is_array())
            {
                return false;
            }
            // 保存された宣言種別の数値
            const int kind = value["kind"].get<int>();
            if (kind < static_cast<int>(
                    LamaPon::ShaderVariantKind::MultiCompile)
                || kind > static_cast<int>(
                    LamaPon::ShaderVariantKind::ShaderFeature))
            {
                return false;
            }
            // JSONから復元するバリアント宣言
            LamaPon::ShaderVariantGroup group;
            group.kind =
                static_cast<LamaPon::ShaderVariantKind>(kind);
            try
            {
                group.keywords = value["keywords"]
                    .get<std::vector<std::string>>();
            }
            catch (const std::exception&)
            {
                return false;
            }
            parsed.variants.groups.push_back(std::move(group));
        }

        path = NormalizeIndexedAssetPath(
            document["path"].get<std::string>());
        if (path.empty())
        {
            return false;
        }
        metadata = std::move(parsed);
        return true;
    }

    // インクルードの内容ハッシュと不在候補を記録する。
    class RecordingInclude final : public ID3DInclude
    {
    public:
        // 依存を記録するインクルード読込を初期化する(assets: 借用するアセット管理, rootShader: 主ソースのパス)。
        RecordingInclude(
            LamaPon::AssetManager& assets,
            std::filesystem::path rootShader)
            : m_assets(assets)
            , m_rootShader(std::move(rootShader))
        {
        }

        // インクルードを開きCloseまで内容を保持する(includeType: 探索種別, fileName: 参照名, parentData: 親の内容ポインター, data: 内容の出力先, bytes: バイト数の出力先)。
        HRESULT Open(
            const D3D_INCLUDE_TYPE includeType,
            const LPCSTR fileName,
            const LPCVOID parentData,
            LPCVOID* data,
            UINT* bytes) override
        {
            if (fileName == nullptr
                || data == nullptr
                || bytes == nullptr)
            {
                return E_INVALIDARG;
            }

            // 探索の基準ディレクトリー
            std::filesystem::path parent =
                m_rootShader.parent_path();
            if (includeType == D3D_INCLUDE_LOCAL
                && parentData != nullptr)
            {
                // 登録済みの索引または親情報
                const auto found =
                    m_parentDirectories.find(parentData);
                if (found != m_parentDirectories.end())
                {
                    parent = found->second;
                }
            }

            // 探索・照合する依存パス
            auto path =
                (parent / std::filesystem::path(fileName))
                    .lexically_normal();
            // 親から見たインクルード候補
            const auto localCandidate = path;
            if (!m_assets.FileExists(path))
            {
                path = m_assets.ResolvePath(
                    std::filesystem::path(fileName))
                    .lexically_normal();
                // ローカル候補の出現で優先先が変わるため、不在も依存に残す。
                if (path != localCandidate)
                {
                    m_missingDependencies.push_back(localCandidate);
                }
            }
            if (!m_assets.FileExists(path))
            {
                if (path == localCandidate)
                {
                    m_missingDependencies.push_back(localCandidate);
                }
                else
                {
                    m_missingDependencies.push_back(path);
                }
                return E_FAIL;
            }

            try
            {
                // 読み込んだインクルードの内容
                auto source = m_assets.ReadFileBytesFresh(path);
                // 配布先でも依存を照合できるよう、アセットルート相対で記録する。
                m_dependencies.emplace_back(
                    RelativeToAssetRoot(m_assets, path),
                    HashBytes(source));
                // 内容バッファーの所有先
                auto storage =
                    std::make_unique<
                        std::vector<std::uint8_t>>(
                            std::move(source));
                // コンパイラーへ渡す内容ポインター
                const void* pointer = storage->data();
                *data = pointer;
                *bytes = static_cast<UINT>(storage->size());
                m_parentDirectories[pointer] =
                    path.parent_path();
                m_sources[pointer] = std::move(storage);
                return S_OK;
            }
            catch (...)
            {
                return E_FAIL;
            }
        }

        // 開いた内容と親ディレクトリー記録を解放する(data: Openで取得した内容ポインター)。
        HRESULT Close(const LPCVOID data) override
        {
            m_parentDirectories.erase(data);
            m_sources.erase(data);
            return S_OK;
        }

        // 開いた順の相対パスと内容ハッシュ一覧を借用する。
        [[nodiscard]] const std::vector<
            std::pair<std::string, std::string>>&
            Dependencies() const noexcept
        {
            return m_dependencies;
        }

        // 出現すると探索結果が変わる不在パス一覧を借用する。
        [[nodiscard]] const std::vector<std::filesystem::path>&
            MissingDependencies() const noexcept
        {
            return m_missingDependencies;
        }

    private:
        // 借用するアセット管理
        LamaPon::AssetManager& m_assets;
        // 主ソースのパス
        std::filesystem::path m_rootShader;
        // 開いた内容と親ディレクトリー
        std::unordered_map<
            const void*,
            std::filesystem::path> m_parentDirectories;
        // Closeまで所有する内容バッファー
        std::unordered_map<
            const void*,
            std::unique_ptr<std::vector<std::uint8_t>>>
            m_sources;
        // 開いた順の相対パスとハッシュ
        std::vector<std::pair<std::string, std::string>>
            m_dependencies;
        // 探索時に不在だった候補パス
        std::vector<std::filesystem::path>
            m_missingDependencies;
    };

    // ビルド構成に応じたコンパイルフラグを返す。
    [[nodiscard]] UINT CompileFlags() noexcept
    {
        // コンパイルフラグ
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG
            | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        return flags;
    }

    // 対象がgs_で始まるか判定する(target: シェーダープロファイル)。
    [[nodiscard]] bool IsGeometryShaderTarget(
        const std::string_view target) noexcept
    {
        return target.size() >= 3
            && std::tolower(static_cast<unsigned char>(target[0])) == 'g'
            && std::tolower(static_cast<unsigned char>(target[1])) == 's'
            && target[2] == '_';
    }

    // ジオメトリーシェーダーの三角形入力を検証し、不適合時に例外を送出する(byteCode: 検査するバイトコード, entryPoint: 診断用の入口名)。
    void ValidateGeometryShaderInput(
        ID3DBlob* const byteCode,
        const std::string_view entryPoint)
    {
        // シェーダーのリフレクション
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
        // リフレクション作成結果
        const auto reflectionResult = D3DReflect(
            byteCode->GetBufferPointer(),
            byteCode->GetBufferSize(),
            IID_ID3D11ShaderReflection,
            &reflection);
        if (FAILED(reflectionResult))
        {
            throw std::runtime_error(
                "Could not inspect geometry shader entry '"
                + std::string{ entryPoint }
                + "' while preparing the export.");
        }

        // 入力プリミティブの記述
        D3D11_SHADER_DESC shaderDescription{};
        if (FAILED(reflection->GetDesc(&shaderDescription)))
        {
            throw std::runtime_error(
                "Could not read geometry shader entry '"
                + std::string{ entryPoint }
                + "' while preparing the export.");
        }
        if (shaderDescription.InputPrimitive
            != D3D_PRIMITIVE_TRIANGLE)
        {
            throw std::runtime_error(
                "Geometry shader entry '"
                + std::string{ entryPoint }
                + "' must take triangle input; LamaPon material "
                  "renderers do not submit point or line input.");
        }
    }


    // 平文または暗号化キャッシュを読み、読込・復号失敗時は空のoptionalを返す(path: 保存ファイル)。
    [[nodiscard]] std::optional<std::vector<std::uint8_t>>
        ReadCacheFile(const std::filesystem::path& path)
    {
        // キャッシュ読込ストリーム
        std::ifstream input(
            LamaPon::ExtendedLengthPath(path),
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            return std::nullopt;
        }
        // キャッシュ末尾のバイト位置
        const auto end = input.tellg();
        if (end < 0)
        {
            return std::nullopt;
        }
        // 読み込んだキャッシュの内容
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(end));
        input.seekg(0);
        if (!bytes.empty())
        {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!input)
            {
                return std::nullopt;
            }
        }
        if (LamaPon::Crypto::IsSealed(
                bytes.data(),
                bytes.size()))
        {
            return LamaPon::Crypto::Unseal(
                bytes.data(),
                bytes.size(),
                LamaPon::Crypto::ArchiveKey());
        }
        return bytes;
    }

    // キャッシュを文字列として読み、読込失敗時は空文字列を返す(path: 保存ファイル)。
    [[nodiscard]] std::string ReadCacheText(
        const std::filesystem::path& path)
    {
        // 読み込んだキャッシュの内容
        const auto bytes = ReadCacheFile(path);
        if (!bytes)
        {
            return {};
        }
        return std::string(bytes->begin(), bytes->end());
    }

    // 依存の内容と不在状態を照合する(assets: アセット管理, dependencyFile: 依存一覧ファイル, matchedDependencies: 成功時だけ代入する任意の一覧出力)。
    [[nodiscard]] bool DependenciesMatch(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& dependencyFile,
        std::vector<std::pair<std::string, std::string>>*
            matchedDependencies = nullptr)
    {
        // 依存一覧の内容
        const auto manifest = ReadCacheFile(dependencyFile);
        if (!manifest)
        {
            return false;
        }
        // 照合済みの依存一覧
        std::vector<std::pair<std::string, std::string>> matched;
        // 依存一覧の読込ストリーム
        std::istringstream input(
            std::string(manifest->begin(), manifest->end()));
        // 依存一覧の一行
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            // ハッシュとパスの区切り
            const auto separator = line.find(' ');
            if (separator == std::string::npos)
            {
                return false;
            }
            // 期待するハッシュまたは不在印
            const std::string expected =
                line.substr(0, separator);
            // 照合・記録する依存パス
            const auto dependency = line.substr(separator + 1);
            // 探索・照合する依存パス
            const auto path = assets.ResolvePath(
                LamaPon::PathFromUtf8(
                    dependency));
            if (expected == "-")
            {
                if (assets.FileExists(path))
                {
                    return false;
                }
                matched.emplace_back(dependency, expected);
                continue;
            }
            if (!assets.FileExists(path))
            {
                return false;
            }
            try
            {
                if (HashBytes(assets.ReadFileBytesFresh(path))
                    != expected)
                {
                    return false;
                }
            }
            catch (...)
            {
                return false;
            }
            matched.emplace_back(dependency, expected);
        }
        if (matchedDependencies != nullptr)
        {
            *matchedDependencies = std::move(matched);
        }
        return true;
    }


    // 入口不在の診断だけを失敗キャッシュの対象とする(details: コンパイラー診断)。
    [[nodiscard]] bool ShouldRememberFailure(
        const std::string& details)
    {
        return details.find("X3501") != std::string::npos
            || details.find("entrypoint not found")
                != std::string::npos;
    }


    // 索引に対応する最初の有効なバイトコードを読む(indexKey: ソースと入口の識別子, directories: 探索順のディレクトリー一覧)。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob>
        LoadFromIndexInDirectories(
            const std::string& indexKey,
            const std::vector<std::filesystem::path>&
                directories)
    {
        // バイトコードの内容識別子
        std::string key;
        {
            // キャッシュ索引の排他制御
            const std::lock_guard<std::mutex> lock(
                g_indexMutex);
            // 登録済みの索引または親情報
            const auto found = g_cacheIndex.find(indexKey);
            if (found == g_cacheIndex.end())
            {
                return {};
            }
            key = found->second;
        }
        // キャッシュ探索先
        for (const auto& directory : directories)
        {
            // バイトコードの保存先
            const auto byteCodePath =
                directory / (key + ".cso");
            // ファイル操作のエラー
            std::error_code error;
            if (!std::filesystem::exists(byteCodePath, error))
            {
                continue;
            }
            // 読み込んだキャッシュの内容
            const auto bytes = ReadCacheFile(byteCodePath);
            if (!bytes || bytes->empty())
            {
                continue;
            }
            // 読み込んだバイトコード
            Microsoft::WRL::ComPtr<ID3DBlob> blob;
            if (FAILED(D3DCreateBlob(
                    static_cast<SIZE_T>(bytes->size()),
                    blob.ReleaseAndGetAddressOf())))
            {
                continue;
            }
            std::memcpy(
                blob->GetBufferPointer(),
                bytes->data(),
                bytes->size());
            return blob;
        }
        return {};
    }

    // 書込完了後に保存先を置換する(destination: 保存先, data: 書込内容, size: 内容のバイト数)。
    // 同じ保存先への並行書込は、呼出側で排他制御する。
    [[nodiscard]] bool WriteFileAtomically(
        const std::filesystem::path& destination,
        const void* data,
        const std::size_t size)
    {
        // MAX_PATH制限を避ける拡張長の保存先
        const auto longDestination =
            LamaPon::ExtendedLengthPath(destination);
        // 置換前に書き終えるための一時保存先
        auto temporary = longDestination;
        temporary += L".tmp";
        {
            // 一時保存先への書込ストリーム
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return false;
            }
            output.write(
                static_cast<const char*>(data),
                static_cast<std::streamsize>(size));
            output.flush();
            if (!output)
            {
                output.close();
                // 除去失敗の記録先
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            output.close();
            if (!output)
            {
                // 除去失敗の記録先
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
        }
        if (!MoveFileExW(
                temporary.c_str(),
                longDestination.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            // ファイル操作のエラー
            std::error_code error;
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }


    // 依存一覧と結果を保存する(cacheDirectory: 保存先フォルダー, dependencyPath: 依存一覧の保存先, writePath: 結果の保存先, assets: アセット管理, source: 主ソースの内容, sourcePath: 主ソースのパス, dependencies: 読込済み依存一覧, missingDependencies: 不在の依存一覧, payload: 結果の内容, payloadSize: 結果のバイト数)。
    [[nodiscard]] bool WriteCacheEntry(
        const std::filesystem::path& cacheDirectory,
        const std::filesystem::path& dependencyPath,
        const std::filesystem::path& writePath,
        LamaPon::AssetManager& assets,
        const std::vector<std::uint8_t>& source,
        const std::filesystem::path& sourcePath,
        const std::vector<std::pair<std::string, std::string>>&
            dependencies,
        const std::vector<std::filesystem::path>&
            missingDependencies,
        const void* payload,
        const std::size_t payloadSize)
    {
        if (writePath.empty() || cacheDirectory.empty())
        {
            return false;
        }
        // キャッシュファイル書込の排他制御
        const std::lock_guard<std::mutex> writeLock(
            g_cacheFileWriteMutex);
        // ファイル操作のエラー
        std::error_code error;
        if (!LamaPon::EnsureDirectoryExists(
                LamaPon::ExtendedLengthPath(cacheDirectory),
                error))
        {
            return false;
        }
        // 反対の結果の保存先
        auto other = writePath;
        other.replace_extension(
            writePath.extension() == L".cso"
                ? L".fail"
                : L".cso");
        // 古い結果と新しい依存一覧の組合せを防ぐため、結果を先に除去する。
        std::filesystem::remove(
            LamaPon::ExtendedLengthPath(writePath), error);
        if (error)
        {
            return false;
        }
        std::filesystem::remove(
            LamaPon::ExtendedLengthPath(other), error);
        if (error)
        {
            return false;
        }
        // 途中終了でも依存の古い結果を使わせないため、依存一覧を先に保存する。
        // 依存一覧の内容
        std::string manifest;
        manifest.append(HashBytes(source));
        manifest.push_back(' ');
        manifest.append(RelativeToAssetRoot(assets, sourcePath));
        manifest.push_back('\n');
        // dependencyName: 相対依存パス, hash: 内容ハッシュ
        for (const auto& [dependencyName, hash] : dependencies)
        {
            manifest.append(hash);
            manifest.push_back(' ');
            manifest.append(dependencyName);
            manifest.push_back('\n');
        }
        // 照合・記録する依存パス
        for (const auto& dependency : missingDependencies)
        {
            manifest.append("- ");
            manifest.append(RelativeToAssetRoot(assets, dependency));
            manifest.push_back('\n');
        }
        if (!WriteFileAtomically(
                dependencyPath,
                manifest.data(),
                manifest.size())
            || !WriteFileAtomically(
                writePath,
                payload,
                payloadSize))
        {
            // 保存に失敗した組は両方を除去する。
            std::filesystem::remove(
                LamaPon::ExtendedLengthPath(dependencyPath), error);
            std::filesystem::remove(
                LamaPon::ExtendedLengthPath(writePath), error);
            return false;
        }

        return true;
    }
}

namespace LamaPon
{
    std::filesystem::path ShaderCacheDirectory()
    {
        // ユーザー別データ保存先の取得領域
        std::wstring localAppData(32768, L'\0');
        // 取得した保存先の文字数
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        // キャッシュの基準ディレクトリー
        std::filesystem::path root;
        if (length > 0 && length < localAppData.size())
        {
            localAppData.resize(length);
            root = localAppData;
        }
        else
        {
            // ファイル操作のエラー
            std::error_code error;
            root = std::filesystem::temp_directory_path(error);
            if (error)
            {
                return {};
            }
        }
        return root / L"LamaPon" / L"shader-cache";
    }

    namespace
    {
        // 登録済みの探索先と通常キャッシュからバイトコードを読む(assets: アセット管理, path: 主ソースのパス, entryPoint: 入口名, target: プロファイル, defines: 順序を含む追加マクロ一覧)。
        [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob>
            LoadFromIndex(
                AssetManager& assets,
                const std::filesystem::path& path,
                const char* entryPoint,
                const char* target,
                const std::vector<std::string>& defines)
        {
            // キャッシュの探索先一覧
            std::vector<std::filesystem::path> directories;
            {
                // キャッシュ探索先の排他制御
                const std::lock_guard<std::mutex> lock(
                    g_searchMutex);
                directories = g_searchDirectories;
            }
            directories.push_back(ShaderCacheDirectory());
            // 索引から取得したバイトコード
            auto blob = LoadFromIndexInDirectories(
                MakeIndexKey(
                    RelativeToAssetRoot(assets, path),
                    entryPoint,
                    target,
                    defines),
                directories);
            if (blob)
            {
                // 統計の排他制御
                const std::lock_guard<std::mutex> lock(
                    g_statisticsMutex);
                ++g_statistics.cacheHitCount;
            }
            return blob;
        }
    }

    void WriteShaderCacheIndex(
        const std::filesystem::path& directory)
    {
        if (directory.empty())
        {
            return;
        }
        // 保存待ちの索引項目
        std::vector<std::pair<std::string, std::string>> pendingIndex;
        // 保存待ちのソース情報
        std::unordered_map<std::string, ShaderSourceMetadata>
            pendingSourceMetadata;
        {
            // 保存待ちの索引の排他制御
            const std::lock_guard<std::mutex> lock(g_pendingMutex);
            // 保存先に対応する待機情報
            const auto pending = g_pendingByDestination.find(
                PendingDestinationKey(directory));
            if (pending == g_pendingByDestination.end())
            {
                return;
            }
            pendingIndex = std::move(pending->second.entries);
            pendingSourceMetadata =
                std::move(pending->second.sourceMetadata);
            g_pendingByDestination.erase(pending);
        }
        // 保存済みの索引を残し、結果の書込と同じ排他制御で追記する。
        // キャッシュファイル書込の排他制御
        const std::lock_guard<std::mutex> writeLock(
            g_cacheFileWriteMutex);
        // 既存索引と追記内容
        std::string text = ReadCacheText(directory / L"index.txt");
        if (!text.empty() && text.back() != '\n')
        {
            text.push_back('\n');
        }
        // indexKey: ソースと入口の識別子, cacheKey: 内容識別子
        for (const auto& [indexKey, cacheKey] : pendingIndex)
        {
            text.append(cacheKey);
            text.push_back(' ');
            text.append(indexKey);
            text.push_back('\n');
        }
        // 出力順を整える相対パス一覧
        std::vector<std::string> metadataPaths;
        metadataPaths.reserve(pendingSourceMetadata.size());
        // path: 相対ソースパス, metadata: 保存待ちのソース情報
        for (const auto& [path, metadata] :
            pendingSourceMetadata)
        {
            static_cast<void>(metadata);
            metadataPaths.push_back(path);
        }
        std::sort(metadataPaths.begin(), metadataPaths.end());
        // 処理する相対ソースパス
        for (const auto& path : metadataPaths)
        {
            // 登録済みのソース情報
            const auto found = pendingSourceMetadata.find(path);
            if (found == pendingSourceMetadata.end())
            {
                continue;
            }
            text.append("@metadata ");
            text.append(SerializeShaderSourceMetadata(
                path,
                found->second).dump());
            text.push_back('\n');
        }
        // ファイル操作のエラー
        std::error_code error;
        if (LamaPon::EnsureDirectoryExists(
                LamaPon::ExtendedLengthPath(directory), error))
        {
            if (!WriteFileAtomically(
                    directory / L"index.txt",
                    text.data(),
                    text.size()))
            {
                throw ShaderCacheWriteFailure(
                    "Could not write the precompiled shader cache "
                    "index: "
                    + LamaPon::PathToUtf8(
                        directory / L"index.txt"));
            }
        }
        else
        {
            throw ShaderCacheWriteFailure(
                "Could not create the precompiled shader cache "
                "directory: "
                + LamaPon::PathToUtf8(directory));
        }
    }

    void AddShaderCacheSearchDirectory(
        std::filesystem::path directory)
    {
        if (directory.empty())
        {
            return;
        }
        // キャッシュ探索先の排他制御
        const std::lock_guard<std::mutex> lock(g_searchMutex);
        // 登録済みの探索先
        for (const auto& existing : g_searchDirectories)
        {
            if (existing == directory)
            {
                return;
            }
        }
        // キャッシュ索引のパス
        const auto indexPath = directory / L"index.txt";
        g_searchDirectories.push_back(std::move(directory));
        // 索引があれば読み込みます（ソースを外した配布物用）。
        // 読み込んだ索引の内容
        const auto indexText = ReadCacheText(indexPath);
        if (indexText.empty())
        {
            return;
        }
        // 索引の読込ストリーム
        std::istringstream input(indexText);
        // 索引とソース情報の排他制御
        const std::lock_guard<std::mutex> indexLock(
            g_indexMutex);
        // 索引の一行
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            // ソース情報行の識別接頭辞
            constexpr std::string_view metadataPrefix =
                "@metadata ";
            if (line.rfind(metadataPrefix, 0) == 0)
            {
                try
                {
                    // ソース情報のJSON
                    const auto document = nlohmann::json::parse(
                        line.begin()
                            + static_cast<std::ptrdiff_t>(
                                metadataPrefix.size()),
                        line.end());
                    // 処理する相対ソースパス
                    std::string path;
                    // 読み込むソース情報
                    ShaderSourceMetadata metadata;
                    if (DeserializeShaderSourceMetadata(
                            document,
                            path,
                            metadata))
                    {
                        g_sourceMetadata.insert_or_assign(
                            std::move(path),
                            std::move(metadata));
                    }
                }
                catch (const std::exception&)
                {
                    // 壊れたmetadata行だけを無視し、DXBC索引は読み続けます。
                }
                continue;
            }
            // 内容識別子と索引の区切り
            const auto separator = line.find(' ');
            if (separator == std::string::npos)
            {
                continue;
            }
            // ソースと入口の識別子
            auto indexKey = line.substr(separator + 1);
            // 相対パス部分の終端
            const auto pathSeparator = indexKey.find('|');
            if (pathSeparator != std::string::npos)
            {
                // 処理する相対ソースパス
                auto path = NormalizeIndexedAssetPath(
                    indexKey.substr(0, pathSeparator));
                indexKey.replace(0, pathSeparator, path);
            }
            g_cacheIndex.insert_or_assign(
                std::move(indexKey),
                line.substr(0, separator));
        }
    }

    void ClearShaderCacheSearchDirectories()
    {
        // キャッシュ探索先の排他制御
        const std::lock_guard<std::mutex> lock(g_searchMutex);
        g_searchDirectories.clear();
        // 索引とソース情報の排他制御
        const std::lock_guard<std::mutex> indexLock(g_indexMutex);
        g_cacheIndex.clear();
        g_sourceMetadata.clear();
    }

    bool LoadPrecompiledShaderMetadata(
        AssetManager& assets,
        const std::filesystem::path& path,
        ShaderRenderState* const renderState,
        ShaderVariantDeclaration* const variants)
    {
        // 検索する相対ソースパス
        const auto key = RelativeToAssetRoot(assets, path);
        // 索引とソース情報の排他制御
        const std::lock_guard<std::mutex> lock(g_indexMutex);
        // 登録済みのソース情報
        const auto found = g_sourceMetadata.find(key);
        if (found == g_sourceMetadata.end())
        {
            return false;
        }
        if (renderState != nullptr)
        {
            *renderState = found->second.renderState;
        }
        if (variants != nullptr)
        {
            *variants = found->second.variants;
        }
        return true;
    }

    const std::vector<ShaderEntryPoint>&
        KnownShaderEntryPoints()
    {
        // 事前コンパイルする既知の入口一覧
        static const std::vector<ShaderEntryPoint> entries{
            { "VSMain", "vs_5_0" },
            { "PSMain", "ps_5_0" },
            { "VSSkinnedMain", "vs_5_0" },
            { "PSSkinnedMain", "ps_5_0" },
            { "VSInstancedMain", "vs_5_0" },
            { "GSMain", "gs_5_0" },
            { "HSMain", "hs_5_0" },
            { "DSMain", "ds_5_0" },
            { "VSOutline", "vs_5_0" },
            { "VSSkinnedOutline", "vs_5_0" },
            { "PSOutline", "ps_5_0" },
            { "PSOccluded", "ps_5_0" },
            { "PSSky", "ps_5_0" },
            { "PSBloom", "ps_5_0" },
            { "PSScreenOutline", "ps_5_0" },
            { "PSScreenSpaceLensFlare", "ps_5_0" },
            { "PSLensFlareStreak", "ps_5_0" },
            { "PSToneMap", "ps_5_0" },
            { "PSFXAA", "ps_5_0" },
            { "PSCopy", "ps_5_0" },
            { "PSCopyMirrorX", "ps_5_0" },
            { "PSTemporalAntiAliasing", "ps_5_0" },
            { "PSVolumetricLight", "ps_5_0" },
            { "PSDepthOfFieldPrepare", "ps_5_0" },
            { "PSDepthOfFieldBlur", "ps_5_0" },
            { "PSDepthOfFieldComposite", "ps_5_0" },
            { "PSMotionBlur", "ps_5_0" },
            { "PSLuminance", "ps_5_0" },
            { "PSAmbientOcclusion", "ps_5_0" },
            { "PSAmbientOcclusionBlur", "ps_5_0" },
            { "PSPrefilterEnvironment", "ps_5_0" },
            { "PSIrradiance", "ps_5_0" },
            { "PSReflectionDepthLinearize", "ps_5_0" },
            { "PSReflectionDepthDownsample", "ps_5_0" },
            { "CSMain", "cs_5_0" }
        };
        return entries;
    }

    std::uint32_t PrecompileShader(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        const std::vector<std::string>& defines,
        const std::vector<std::string>* usedKeywords)
    {
        return PrecompileShaderVariants(
            assets,
            path,
            destinationDirectory,
            std::span<const ShaderEntryPoint>{
                KnownShaderEntryPoints() },
            defines,
            usedKeywords);
    }

    std::uint32_t PrecompileShaderVariants(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        const std::span<const ShaderEntryPoint> entryPoints,
        const std::vector<std::string>& defines,
        const std::vector<std::string>* usedKeywords,
        std::string* const error)
    {
        if (error != nullptr)
        {
            error->clear();
        }
        if (destinationDirectory.empty())
        {
            return 0;
        }
        // 書き出すキーワードの組合せ一覧
        std::vector<ShaderKeywordSet> variants;
        try
        {
            // バリアントを調べる主ソース
            const auto source = assets.ReadFileBytesFresh(path);
            // ソースから読んだバリアント宣言
            const auto declaration = ParseShaderVariants(
                std::string_view{
                    reinterpret_cast<const char*>(source.data()),
                    source.size() });
            // 削減前のバリアント数
            const auto total =
                EnumerateShaderVariants(declaration).size();
            variants = EnumerateShaderVariants(
                declaration,
                usedKeywords);
            // 未使用のshader_featureを除いた数を記録する。
            if (variants.size() < total)
            {
                Logger::Instance().Info(
                    "shader_feature stripped "
                    + std::to_string(total - variants.size())
                    + " unused variant(s) of "
                    + PathToUtf8(path)
                    + " ("
                    + std::to_string(variants.size())
                    + " kept).");
            }
        }
        catch (const std::exception&)
        {
            variants.clear();
        }
        if (variants.empty())
        {
            // 宣言が無い（または上限超過で無効）シェーダーはキーワード無しの1本だけ。
            variants.emplace_back();
        }

        // 成功した入口の累積数
        std::uint32_t succeeded{};
        // 書き出すキーワードの組合せ
        for (const auto& variant : variants)
        {
            // 固定マクロとバリアントの一覧
            auto keywords = defines;
            // 追加するバリアントのマクロ
            for (const auto& keyword : variant.Keywords())
            {
                keywords.push_back(keyword);
            }
            // 組合せの最初の失敗診断
            std::string variantError;
            succeeded += PrecompileShader(
                assets,
                path,
                destinationDirectory,
                entryPoints,
                keywords,
                &variantError);
            if (error != nullptr
                && error->empty()
                && !variantError.empty())
            {
                *error = std::move(variantError);
            }
        }
        return succeeded;
    }

    std::uint32_t PrecompileShader(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::filesystem::path& destinationDirectory,
        const std::span<const ShaderEntryPoint> entryPoints,
        const std::vector<std::string>& defines,
        std::string* const error)
    {
        if (error != nullptr)
        {
            error->clear();
        }
        if (destinationDirectory.empty())
        {
            return 0;
        }

        RecordShaderSourceMetadata(
            assets,
            path,
            destinationDirectory);
        // この呼出中の保存先と強制コンパイル
        const ScopedPrecompileContext context(destinationDirectory);
        // 成功した入口の累積数
        std::uint32_t succeeded{};
        // コンパイルする入口とプロファイル
        for (const auto& entry : entryPoints)
        {
            try
            {
                // コンパイル済みバイトコード
                const auto byteCode = CompileShaderCached(
                    assets,
                    path,
                    entry.entryPoint,
                    entry.target,
                    defines);
                if (IsGeometryShaderTarget(entry.target))
                {
                    ValidateGeometryShaderInput(
                        byteCode.Get(),
                        entry.entryPoint);
                }
                ++succeeded;
            }
            catch (const ShaderCacheWriteFailure&)
            {
                // 書込失敗は配布処理を中止できるよう上位へ伝える。
                throw;
            }
            // 最初の診断に保存する例外
            catch (const std::exception& exception)
            {
                // 入口ごとのコンパイル失敗を許容して探索を続ける。
                if (error != nullptr && error->empty())
                {
                    *error = exception.what();
                }
            }
        }
        return succeeded;
    }

    void WarmShaderCache(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::vector<std::string>& keywords)
    {
        // コンパイルする入口とプロファイル
        for (const auto& entry : KnownShaderEntryPoints())
        {
            try
            {
                static_cast<void>(CompileShaderCached(
                    assets,
                    path,
                    entry.entryPoint,
                    entry.target,
                    keywords));
            }
            catch (const std::exception&)
            {
                // 入口ごとのコンパイル失敗を許容して探索を続ける。
            }
        }
    }

    ShaderCompileStats ShaderCompileStatistics() noexcept
    {
        // コンパイル統計の排他制御
        const std::lock_guard<std::mutex> lock(
            g_statisticsMutex);
        return g_statistics;
    }

    void ResetShaderCompileStatistics() noexcept
    {
        // コンパイル統計の排他制御
        const std::lock_guard<std::mutex> lock(
            g_statisticsMutex);
        g_statistics = {};
    }

    void SetShaderCompileCompletionHookForTesting(
        std::function<void()> hook)
    {
        // テスト用完了処理の排他制御
        const std::lock_guard<std::mutex> lock(
            g_compileCompletionHookMutex);
        g_compileCompletionHook = std::move(hook);
    }

    void ClearShaderCache()
    {
        {
            // 実行時の依存記録の排他制御
            const std::lock_guard<std::mutex> lock(
                g_liveDependencyMutex);
            g_liveDependencies.clear();
        }
        // 通常キャッシュのディレクトリー
        const auto directory = ShaderCacheDirectory();
        if (directory.empty())
        {
            return;
        }
        // キャッシュ除去のエラー
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::uint64_t ShaderSourceDependencyRevision(
        AssetManager& assets,
        const std::filesystem::path& path,
        const std::vector<std::string>& defines) noexcept
    {
        try
        {
            if (path.empty() || assets.IsArchived())
            {
                return 0;
            }

            // 照合する依存ファイル一覧
            std::vector<std::filesystem::path> files;
            // 主ソースの識別パス
            const auto sourceKey = MakeLiveSourceKey(assets, path);
            // 追加マクロ一覧の識別子
            const auto defineKey = MakeDefineKey(defines);
            {
                // 実行時の依存記録の排他制御
                const std::lock_guard<std::mutex> lock(
                    g_liveDependencyMutex);
                // 主ソースの依存記録
                const auto shader = g_liveDependencies.find(sourceKey);
                if (shader == g_liveDependencies.end())
                {
                    return 0;
                }
                // identity: コンパイル条件の識別子, dependencies: 記録した依存情報
                for (const auto& [identity, dependencies] :
                    shader->second.compileIdentities)
                {
                    static_cast<void>(identity);
                    if (dependencies.defineKey == defineKey)
                    {
                        files.insert(
                            files.end(),
                            dependencies.files.begin(),
                            dependencies.files.end());
                    }
                }
            }
            if (files.empty())
            {
                return 0;
            }

            // 比較用の小文字パスを返す(file: 比較対象のパス)。
            const auto normalizedPath = [](const auto& file)
            {
                return NormalizeIndexedAssetPath(
                    PathToUtf8(file.lexically_normal()));
            };
            // 正規化パスで昇順に比較する(left: 左のパス, right: 右のパス)。
            std::ranges::sort(
                files,
                [&](const auto& left, const auto& right)
                {
                    return normalizedPath(left) < normalizedPath(right);
                });
            // 正規化後に同じパスか判定する(left: 左のパス, right: 右のパス)。
            files.erase(
                std::unique(
                    files.begin(),
                    files.end(),
                    [&](const auto& left, const auto& right)
                    {
                        return normalizedPath(left)
                            == normalizedPath(right);
                    }),
                files.end());

            // 内容は再コンパイル時に読み、監視時はパス・存在・時刻・サイズを照合する。
            // 依存状態の累積リビジョン
            std::uint64_t revision = 14695981039346656037ull;
            // リビジョンへ内容を加算する(data: 加算する内容, size: 内容のバイト数)。
            const auto append = [&](const void* data, const std::size_t size)
            {
                // 加算する内容のバイト列
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                // 加算するバイトの位置
                for (std::size_t index = 0; index < size; ++index)
                {
                    revision ^= bytes[index];
                    revision *= 1099511628211ull;
                }
            };
            // 照合する依存ファイル
            for (const auto& file : files)
            {
                // 比較用に正規化したパス
                const auto name = normalizedPath(file);
                append(name.data(), name.size());
                // パスと状態を区切るゼロ
                const std::uint8_t separator{};
                append(&separator, sizeof(separator));

                // ファイル情報取得・除去のエラー
                std::error_code error;
                // 依存ファイルの存在有無
                const bool exists = std::filesystem::exists(file, error);
                // 不在・存在・取得失敗の識別値
                const std::uint8_t state = error
                    ? 2u
                    : (exists ? 1u : 0u);
                append(&state, sizeof(state));
                if (!error && exists)
                {
                    // 依存ファイルの更新時刻
                    const auto timestamp =
                        std::filesystem::last_write_time(file, error);
                    // 取得失敗時はゼロとする更新時刻
                    const auto ticks = error
                        ? std::filesystem::file_time_type::duration::rep{}
                        : timestamp.time_since_epoch().count();
                    append(&ticks, sizeof(ticks));
                    error.clear();
                    // 取得したファイルサイズ
                    const auto size = std::filesystem::file_size(file, error);
                    // 取得失敗時はゼロとするファイルサイズ
                    const std::uintmax_t bytes = error ? 0u : size;
                    append(&bytes, sizeof(bytes));
                }
            }
            return revision == 0 ? 1 : revision;
        }
        catch (...)
        {
            return 0;
        }
    }

    std::size_t ClearShaderCacheFailures()
    {
        // 通常キャッシュのディレクトリー
        const auto directory = ShaderCacheDirectory();
        if (directory.empty())
        {
            return 0;
        }
        // ファイル情報取得・除去のエラー
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
        {
            return 0;
        }
        // 除去に成功した失敗キャッシュの数
        std::size_t removed = 0;
        // 除去する失敗キャッシュ一覧
        std::vector<std::filesystem::path> failures;
        // キャッシュ内のファイル
        for (const auto& entry :
            std::filesystem::directory_iterator(
                directory,
                error))
        {
            if (entry.path().extension() == L".fail")
            {
                failures.push_back(entry.path());
            }
        }
        // 除去する失敗キャッシュのパス
        for (const auto& failure : failures)
        {
            // 対応する依存情報のパス
            auto dependencies = failure;
            dependencies.replace_extension(L".deps");
            if (std::filesystem::remove(failure, error))
            {
                ++removed;
            }
            std::filesystem::remove(dependencies, error);
        }
        return removed;
    }

    void SetShaderCacheEnabled(const bool enabled) noexcept
    {
        g_cacheEnabled.store(enabled);
    }

    bool IsShaderCacheEnabled() noexcept
    {
        return g_cacheEnabled.load();
    }

    Microsoft::WRL::ComPtr<ID3DBlob> CompileShaderCached(
        AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target,
        const std::vector<std::string>& defines)
    {
        if (!assets.FileExists(path))
        {
            // ソース不在時に索引から取得するバイトコード
            if (auto blob = LoadFromIndex(
                    assets,
                    path,
                    entryPoint,
                    target,
                    defines))
            {
                return blob;
            }
            if (assets.IsArchived())
            {
                // 診断に表示する追加マクロ一覧
                std::string keywordList;
                // 追加するマクロ名
                for (const auto& define : defines)
                {
                    if (!keywordList.empty())
                    {
                        keywordList.push_back('+');
                    }
                    keywordList.append(define);
                }
                throw std::runtime_error(
                    "Precompiled shader cache entry was not found in "
                    "the source-stripped archive: "
                    + RelativeToAssetRoot(assets, path)
                    + " (entry '"
                    + (entryPoint != nullptr ? entryPoint : "")
                    + "', target '"
                    + (target != nullptr ? target : "")
                    + "', keywords '" + keywordList
                    + "'). Re-export the game so this shader "
                    "variant is included.");
            }
            throw std::runtime_error(
                "Shader file was not found: "
                + PathToUtf8(path));
        }
        // コンパイルする主ソースの内容
        const auto source = assets.ReadFileBytesFresh(path);
        // 診断に表示する主ソースのパス
        const auto sourceName = PathToUtf8(path);
        // コンパイルフラグ
        const UINT flags = CompileFlags();

        // 主ソースとコンパイル条件の内容識別子
        std::string key;
        // キャッシュ結果の保存先
        std::filesystem::path cacheDirectory;
        // 再利用するバイトコードのパス
        std::filesystem::path byteCodePath;
        // 再利用する依存一覧のパス
        std::filesystem::path dependencyPath;
        // 事前コンパイル中はキャッシュ無効設定より成果物の保存を優先する。
        // キャッシュの読込・保存の利用有無
        const bool useCache = g_forceCompile
            || g_cacheEnabled.load();
        if (useCache)
        {
            // 内容識別子を作るソースと条件
            std::string material;
            material.reserve(source.size() + 128);
            material.append(
                reinterpret_cast<const char*>(source.data()),
                source.size());
            material.append("\n#lamapon-shader-cache v");
            material.append(
                std::to_string(CacheFormatVersion));
            material.append("\nentry=");
            material.append(
                entryPoint != nullptr ? entryPoint : "");
            material.append("\ntarget=");
            material.append(target != nullptr ? target : "");
            material.append("\nflags=");
            material.append(std::to_string(flags));
            // 追加マクロもコンパイル条件の一部として識別する。
            material.append("\ndefines=");
            // 追加するマクロ名
            for (const auto& define : defines)
            {
                material.append(define);
                material.push_back(';');
            }
            key = HashBytes(
                reinterpret_cast<const std::uint8_t*>(
                    material.data()),
                material.size());
            cacheDirectory = g_writeOverride.empty()
                ? ShaderCacheDirectory()
                : g_writeOverride;
        }

        // 再利用する失敗記録のパス
        std::filesystem::path failurePath;

        // バイトコードの書込先
        std::filesystem::path writeByteCodePath;
        // 依存一覧の書込先
        std::filesystem::path writeDependencyPath;
        // 失敗記録の書込先
        std::filesystem::path writeFailurePath;
        if (!key.empty() && !cacheDirectory.empty())
        {
            writeByteCodePath = cacheDirectory / (key + ".cso");
            writeDependencyPath =
                cacheDirectory / (key + ".deps");
            writeFailurePath = cacheDirectory / (key + ".fail");
            // 依存が一致した候補だけを選び、事前コンパイル中は既存結果を再利用しない。
            // キャッシュの探索先一覧
            std::vector<std::filesystem::path> readDirectories;
            {
                // キャッシュ探索先の排他制御
                const std::lock_guard<std::mutex> lock(
                    g_searchMutex);
                readDirectories = g_searchDirectories;
            }
            readDirectories.push_back(cacheDirectory);
            if (g_forceCompile)
            {
                readDirectories.clear();
            }
            // 再利用候補で照合済みの依存一覧
            std::vector<std::pair<std::string, std::string>>
                cachedDependencies;
            // 検査するキャッシュ探索先
            for (const auto& directory : readDirectories)
            {
                // 候補の依存一覧のパス
                const auto candidateDeps =
                    directory / (key + ".deps");
                // 候補のバイトコードのパス
                const auto candidateByteCode =
                    directory / (key + ".cso");
                // 候補の失敗記録のパス
                const auto candidateFailure =
                    directory / (key + ".fail");
                // 候補の存在確認エラー
                std::error_code exists;
                if (!std::filesystem::exists(
                        candidateDeps,
                        exists))
                {
                    continue;
                }
                // 候補から照合する依存一覧
                std::vector<std::pair<std::string, std::string>>
                    candidateDependencies;
                if (!DependenciesMatch(
                        assets,
                        candidateDeps,
                        &candidateDependencies))
                {
                    continue;
                }
                cachedDependencies = std::move(candidateDependencies);
                byteCodePath = candidateByteCode;
                failurePath = candidateFailure;
                dependencyPath = candidateDeps;
                break;
            }
            if (!dependencyPath.empty())
            {
                RememberLiveShaderDependencies(
                    assets,
                    path,
                    entryPoint,
                    target,
                    defines,
                    source,
                    cachedDependencies);
            }
            // キャッシュ読込・コンパイル開始時刻
            const auto started =
                std::chrono::steady_clock::now();
            // キャッシュの確認・除去エラー
            std::error_code error;
            // 入口不在の記録を再利用し、同じ入口の繰返し探索を省く。
            if (std::filesystem::exists(failurePath, error))
            {
                // 保存済みの失敗診断
                const auto stored = ReadCacheText(failurePath);
                if (!stored.empty())
                {
                    if (!ShouldRememberFailure(stored))
                    {
                        // 保存対象外の古い失敗は、除去に失敗しても再利用しない。
                        std::filesystem::remove(
                            failurePath,
                            error);
                        std::filesystem::remove(
                            dependencyPath,
                            error);
                    }
                    else
                    {
                        // キャッシュ結果を読む経過ミリ秒
                        const std::chrono::duration<
                            double,
                            std::milli> elapsed =
                            std::chrono::steady_clock::now()
                            - started;
                        {
                            // コンパイル統計の排他制御
                            const std::lock_guard<std::mutex>
                                lock(g_statisticsMutex);
                            ++g_statistics.cacheHitCount;
                            g_statistics.cacheReadMilliseconds
                                += elapsed.count();
                        }
                        throw std::runtime_error(stored);
                    }
                }
            }
            if (std::filesystem::exists(byteCodePath, error))
            {
                // 読み込んだバイトコード
                const auto bytes = ReadCacheFile(byteCodePath);
                if (bytes && !bytes->empty())
                {
                    // キャッシュ結果のバイトコード
                    Microsoft::WRL::ComPtr<ID3DBlob> blob;
                    if (SUCCEEDED(D3DCreateBlob(
                            static_cast<SIZE_T>(bytes->size()),
                            blob.ReleaseAndGetAddressOf())))
                    {
                        std::memcpy(
                            blob->GetBufferPointer(),
                            bytes->data(),
                            bytes->size());
                        // キャッシュ結果を読む経過ミリ秒
                        const std::chrono::duration<
                            double,
                            std::milli> elapsed =
                            std::chrono::steady_clock::now()
                            - started;
                        // コンパイル統計の排他制御
                        const std::lock_guard<std::mutex>
                            lock(g_statisticsMutex);
                        ++g_statistics.cacheHitCount;
                        g_statistics.cacheReadMilliseconds
                            += elapsed.count();
                        return blob;
                    }
                }
            }
        }

        // 追加マクロ一覧
        std::vector<D3D_SHADER_MACRO> macros;
        macros.reserve(defines.size() + 1);
        // 追加するマクロ名
        for (const auto& define : defines)
        {
            macros.push_back({ define.c_str(), "1" });
        }
        // D3D_SHADER_MACROはヌルの要素を終端に置く。
        macros.push_back({ nullptr, nullptr });

        // 依存を記録するインクルード読込
        RecordingInclude includeHandler{ assets, path };
        // コンパイルしたバイトコード
        Microsoft::WRL::ComPtr<ID3DBlob> shader;
        // コンパイラーの診断バッファー
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // キャッシュ読込・コンパイル開始時刻
        const auto started = std::chrono::steady_clock::now();
        // コンパイルの成否
        const HRESULT result = D3DCompile(
            source.data(),
            source.size(),
            sourceName.c_str(),
            defines.empty() ? nullptr : macros.data(),
            &includeHandler,
            entryPoint,
            target,
            flags,
            0,
            shader.ReleaseAndGetAddressOf(),
            errors.ReleaseAndGetAddressOf());
        RememberLiveShaderDependencies(
            assets,
            path,
            entryPoint,
            target,
            defines,
            source,
            includeHandler.Dependencies(),
            includeHandler.MissingDependencies());
        RunShaderCompileCompletionHook();
        // 依存記録と完了処理を含む経過ミリ秒
        const std::chrono::duration<double, std::milli>
            elapsed =
                std::chrono::steady_clock::now() - started;
        if (FAILED(result))
        {
            // コンパイラーからの失敗診断
            const std::string details = errors
                ? std::string{
                    static_cast<const char*>(
                        errors->GetBufferPointer()),
                    errors->GetBufferSize()
                }
                : "Unknown shader compiler error.";
            // ソースと入口を含む失敗診断
            const std::string message =
                "Failed to compile shader "
                + sourceName
                + " ("
                + (entryPoint != nullptr ? entryPoint : "")
                + "): "
                + details;
            {
                // コンパイル統計の排他制御
                const std::lock_guard<std::mutex> lock(
                    g_statisticsMutex);
                ++g_statistics.compiledCount;
                g_statistics.compileMilliseconds
                    += elapsed.count();
            }
            // 入口不在の失敗だけを保存し、その他の診断は再コンパイルで再確認する。
            if (ShouldRememberFailure(details))
            {
                static_cast<void>(WriteCacheEntry(
                    cacheDirectory,
                    writeDependencyPath,
                    writeFailurePath,
                    assets,
                    source,
                    path,
                    includeHandler.Dependencies(),
                    includeHandler.MissingDependencies(),
                    message.data(),
                    message.size()));
            }
            throw std::runtime_error(message);
        }

        {
            // コンパイル統計の排他制御
            const std::lock_guard<std::mutex> lock(
                g_statisticsMutex);
            ++g_statistics.compiledCount;
            g_statistics.compileMilliseconds
                += elapsed.count();
        }
        // 成功時に記録するコンパイル結果
        std::ostringstream message;
        message.precision(1);
        message << std::fixed
            << "シェーダーをコンパイルしました: "
            << sourceName
            << " ("
            << (entryPoint != nullptr ? entryPoint : "")
            << ") "
            << elapsed.count()
            << "ms";
        Logger::Instance().Info(message.str());

        // コンパイル結果の保存成否
        const bool cacheWritten = WriteCacheEntry(
            cacheDirectory,
            writeDependencyPath,
            writeByteCodePath,
            assets,
            source,
            path,
            includeHandler.Dependencies(),
            includeHandler.MissingDependencies(),
            shader->GetBufferPointer(),
            shader->GetBufferSize());
        if (!g_writeOverride.empty())
        {
            if (!cacheWritten || key.empty())
            {
                throw ShaderCacheWriteFailure(
                    "Could not write a precompiled shader cache "
                    "entry for " + sourceName + " ("
                    + (entryPoint != nullptr ? entryPoint : "")
                    + ") to "
                    + LamaPon::PathToUtf8(g_writeOverride));
            }
            // 保存待ちの索引の排他制御
            const std::lock_guard<std::mutex> lock(g_pendingMutex);
            g_pendingByDestination[
                PendingDestinationKey(g_writeOverride)]
                .entries.emplace_back(
                MakeIndexKey(
                    RelativeToAssetRoot(assets, path),
                    entryPoint,
                    target,
                    defines),
                key);
        }
        return shader;
    }
}
