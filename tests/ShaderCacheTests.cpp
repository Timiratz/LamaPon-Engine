#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include <objbase.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

// Shader依存変更の検出と失敗キャッシュの再利用防止を検証する。
namespace
{
    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗にする。
    void Require(const bool condition, const char* message)
    {
        // 検査条件の不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // Contains(text: 検索対象, needle: 検索文字列): 対象に文字列が含まれるか調べる。
    [[nodiscard]] bool Contains(
        const std::string& text,
        const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    class TemporaryDirectory final
    {
    public:
        // TemporaryDirectory(): キャッシュ検査用の一時領域を作る。
        TemporaryDirectory()
        {
            // 一時パス衝突の回避値
            const auto unique =
                std::chrono::steady_clock::now()
                    .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonShaderCacheTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(m_path);
        }

        // 一時ディレクトリと内容を削除する。
        ~TemporaryDirectory()
        {
            // 削除失敗を例外にしない受け皿
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        // TemporaryDirectory(other: 複製元): 一時領域の複製を禁止する。
        TemporaryDirectory(const TemporaryDirectory&) = delete;
        // operator=(other: 複製元): 一時領域の代入を禁止する。
        TemporaryDirectory& operator=(
            const TemporaryDirectory&) = delete;

        // Path(): 作成した一時ディレクトリの場所を返す。
        [[nodiscard]] const std::filesystem::path&
            Path() const noexcept
        {
            return m_path;
        }

    private:
        // キャッシュ検査用一時ディレクトリ
        std::filesystem::path m_path;
    };

    // WriteFile(path: 出力先, contents: ファイル内容): 親フォルダーを作ってテキストを保存する。
    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(path.parent_path());
        // 作成するshaderファイル
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << contents;
        // shader書き込みの失敗を検出する。
        if (!output)
        {
            throw std::runtime_error(
                "Could not write the test shader.");
        }
    }

    // ReadFile(path: 入力元): ファイル全体を文字列として読む。
    [[nodiscard]] std::string ReadFile(
        const std::filesystem::path& path)
    {
        // 読み込むキャッシュファイル
        std::ifstream input(path, std::ios::binary);
        // ファイル内容の蓄積先
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

    // UniqueMarker(name: ケース名): キャッシュ衝突を避けるshaderコメントを作る。
    [[nodiscard]] std::string UniqueMarker(const char* name)
    {
        // 実行内で一意な識別時刻
        const auto unique = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        return "// " + std::string(name) + "-"
            + std::to_string(unique) + "\n";
    }

    // CompileMessage(assets: 資産管理, path: shaderパス, entryPoint: 入口名, target: shader種別): 成否の診断文を返す。
    [[nodiscard]] std::string CompileMessage(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target)
    {
        // shaderコンパイルを試す。
        try
        {
            // キャッシュ経由で生成されたshader
            const auto blob = LamaPon::CompileShaderCached(
                assets,
                path,
                entryPoint,
                target);
            Require(
                blob && blob->GetBufferSize() > 0,
                "a successful compile must return bytecode");
            return {};
        }
        // exception: コンパイル失敗の診断を返す。
        catch (const std::exception& exception)
        {
            return exception.what();
        }
    }

    // CacheEntries(extension: キャッシュ拡張子): 共有キャッシュ内の該当ファイルを列挙する。
    [[nodiscard]] std::set<std::filesystem::path>
        CacheEntries(const std::wstring& extension)
    {
        // 該当キャッシュのパス集合
        std::set<std::filesystem::path> entries;
        // 共有shaderキャッシュの場所
        const auto directory = LamaPon::ShaderCacheDirectory();
        // ディレクトリ走査の失敗情報
        std::error_code error;
        // キャッシュ保存先が存在しない場合を扱う。
        if (!std::filesystem::is_directory(directory, error))
        {
            return entries;
        }
        // キャッシュ内のファイルを調べる。
        for (const auto& entry :
            std::filesystem::directory_iterator(
                directory,
                error))
        {
            // 拡張子が一致する項目だけ収集する。
            if (entry.path().extension() == extension)
            {
                entries.insert(entry.path());
            }
        }
        return entries;
    }

    // NewCacheEntry(before: 既存項目, extension: 拡張子): 新規追加されたキャッシュを返す。
    [[nodiscard]] std::filesystem::path NewCacheEntry(
        const std::set<std::filesystem::path>& before,
        const std::wstring& extension)
    {
        // 見つかった新規項目
        std::filesystem::path found;
        // 既存集合にないキャッシュを探す。
        for (const auto& entry : CacheEntries(extension))
        {
            // 走査前になかった項目を見つける。
            if (!before.contains(entry))
            {
                Require(
                    found.empty(),
                    "exactly one cache entry must be added");
                found = entry;
            }
        }
        return found;
    }

    // このテストが共有キャッシュへ残したものを片付けます。
    // キーは実行ごとに変わるので、放っておくと実行のたびに溜まります。
    class CacheLitterGuard final
    {
    public:
        // CacheLitterGuard(): 共有shaderキャッシュの開始状態を記録する。
        CacheLitterGuard()
            : m_before(Snapshot())
        {
        }

        // このテストが追加したキャッシュだけを削除する。
        ~CacheLitterGuard()
        {
            // キャッシュ削除失敗を受け取る先
            std::error_code error;
            // 開始時点になかったキャッシュを調べる。
            for (const auto& entry : Snapshot())
            {
                // テストが作成した項目だけ削除する。
                if (!m_before.contains(entry))
                {
                    std::filesystem::remove(entry, error);
                }
            }
        }

        // CacheLitterGuard(other: 複製元): 後始末ガードの複製を禁止する。
        CacheLitterGuard(const CacheLitterGuard&) = delete;
        // operator=(other: 複製元): 後始末ガードの代入を禁止する。
        CacheLitterGuard& operator=(
            const CacheLitterGuard&) = delete;

    private:
        // Snapshot(): shaderキャッシュの全形式を収集する。
        [[nodiscard]] static std::set<std::filesystem::path>
            Snapshot()
        {
            // 収集したキャッシュパス
            std::set<std::filesystem::path> entries;
            // 成功・依存・失敗キャッシュの拡張子
            for (const auto* extension :
                { L".cso", L".deps", L".fail" })
            {
                entries.merge(CacheEntries(extension));
            }
            return entries;
        }

        // ガード生成時点のキャッシュ集合
        std::set<std::filesystem::path> m_before;
    };
}

// main(): Shader依存更新、失敗記録、キャッシュ削除を検証する。
int main()
{
    // AssetManagerはアセットの走査でWICを使うため、COMが要ります。
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // COM終了処理が必要か
    const bool uninitialize = SUCCEEDED(comResult);

    // AssetManager解放後にCoUninitializeするため、終了処理はtryの外で行う。
    // テスト失敗の終了状態
    int status = 0;
    // shader共有キャッシュへの追加を削除するガード
    try
    {
        // テスト開始前後の共有キャッシュ差分
        CacheLitterGuard litter;
        // shader素材を置くテスト用一時領域
        TemporaryDirectory root;
        // shaderキャッシュ検査用の資産管理
        LamaPon::AssetManager assets(nullptr, nullptr);
        assets.SetAssetRoot(root.Path());
        Require(
            LamaPon::IsShaderCacheEnabled(),
            "the disk cache must be on for these checks");

        // includeだけの変更も依存記録で検出し、古いbytecodeを返さない。
        // shader本体は同一のままなので、.deps照合が必要。
        {
            // キャッシュ衝突を避ける識別コメント
            const auto marker = UniqueMarker("stale-bytecode");
            // 依存includeを読むshader
            const auto shader = root.Path() / L"stale.hlsl";
            // 更新を検査する依存include
            const auto include = root.Path() / L"stale.hlsli";
            WriteFile(
                include,
                "float4 Value() { return float4(0, 0, 0, 1); }\n");
            WriteFile(
                shader,
                marker
                + "#include \"stale.hlsli\"\n"
                "float4 VSMain() : SV_Position"
                " { return Value(); }\n");
            Require(
                CompileMessage(
                    assets,
                    L"stale.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "the first compile must succeed");

            // 依存includeを不正HLSLへ差し替える。
            WriteFile(
                include,
                "float4 Value() { return not_a_function(); }\n");
            Require(
                !CompileMessage(
                    assets,
                    L"stale.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "a broken include must not return cached bytecode");
        }

        // 隣接includeが無い間はasset-root版へfallbackする。
        // 隣接版の追加で依存優先順位を再解決する。
        {
            // キャッシュ衝突を避ける識別コメント
            const auto marker = UniqueMarker("include-shadow");
            // 隣接include解決を検査するshader
            const auto shader = root.Path()
                / L"nested"
                / L"shadow.hlsl";
            // asset-rootから読み込むfallback include
            const auto fallback = root.Path() / L"shadow.hlsli";
            // 優先順位の高い隣接include
            const auto local = root.Path()
                / L"nested"
                / L"shadow.hlsli";
            WriteFile(
                fallback,
                "float4 Value() { return float4(1, 1, 1, 1); }\n");
            WriteFile(
                shader,
                marker
                + "#include \"shadow.hlsli\"\n"
                "float4 VSMain() : SV_Position"
                " { return Value(); }\n");
            Require(
                CompileMessage(
                    assets,
                    L"nested/shadow.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "the asset-root include fallback must compile");
            // fallback依存を含む初回revision
            const auto fallbackRevision =
                LamaPon::ShaderSourceDependencyRevision(
                    assets,
                    L"nested/shadow.hlsl");
            Require(
                fallbackRevision != 0,
                "the fallback include dependencies must be tracked");

            WriteFile(
                local,
                "float4 Value() { return not_a_function(); }\n");
            Require(
                LamaPon::ShaderSourceDependencyRevision(
                    assets,
                    L"nested/shadow.hlsl")
                    != fallbackRevision,
                "creating a higher-priority local include must change the live revision");
            Require(
                !CompileMessage(
                    assets,
                    L"nested/shadow.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "a new local include must shadow the cached root fallback");
        }

        // include修正後は失敗キャッシュを再利用せず成功する。
        {
            // キャッシュ衝突を避ける識別コメント
            const auto marker = UniqueMarker("stale-failure");
            // 不正から修正するshader
            const auto shader = root.Path() / L"retry.hlsl";
            // 失敗後に修正する依存include
            const auto include = root.Path() / L"retry.hlsli";
            WriteFile(
                include,
                "float4 Value() { return not_a_function(); }\n");
            WriteFile(
                shader,
                marker
                + "#include \"retry.hlsli\"\n"
                "float4 VSMain() : SV_Position"
                " { return Value(); }\n");
            Require(
                !CompileMessage(
                    assets,
                    L"retry.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "a broken include must fail the first time");

            WriteFile(
                include,
                "float4 Value() { return float4(1, 1, 1, 1); }\n");
            Require(
                CompileMessage(
                    assets,
                    L"retry.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "fixing the include must let the compile through");
        }

        // .failは入口名欠落だけ保存し、構文エラーは保存しない。
        {
            // キャッシュ衝突を避ける識別コメント
            const auto marker = UniqueMarker("unremembered");
            // 構文エラーのキャッシュ動作を検査するshader
            const auto shader = root.Path() / L"syntax.hlsl";
            WriteFile(
                shader,
                marker
                + "float4 VSMain() : SV_Position"
                " { return not_a_function(); }\n");
            // 構文エラー前の失敗キャッシュ集合
            const auto before = CacheEntries(L".fail");
            Require(
                !CompileMessage(
                    assets,
                    L"syntax.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "a syntax error must fail");
            Require(
                NewCacheEntry(before, L".fail").empty(),
                "only missing-entry-point failures may be remembered");

            // 構文修正後に再コンパイルが成功する。
            WriteFile(
                shader,
                marker
                + "float4 VSMain() : SV_Position"
                " { return float4(1, 1, 1, 1); }\n");
            Require(
                CompileMessage(
                    assets,
                    L"syntax.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "fixing a syntax error must let the compile through");
        }

        // allowlist外の古い失敗記録は読み込み時に破棄する。
        {
            // キャッシュ衝突を避ける識別コメント
            const auto marker = UniqueMarker("poisoned-failure");
            // 入口名不一致を検査するshader
            const auto shader = root.Path() / L"poisoned.hlsl";
            // includeを持たない入口名エラーを使い、依存関係を一定に保ちます。
            WriteFile(
                shader,
                marker
                + "float4 PSMain() : SV_Target { return 1; }\n");
            // 失敗生成前の共有キャッシュ集合
            const auto before = CacheEntries(L".fail");
            // 実際の入口名エラー
            const auto realFailure = CompileMessage(
                assets,
                L"poisoned.hlsl",
                "VSMain",
                "vs_5_0");
            Require(
                Contains(realFailure, "X3501"),
                "a missing entry point must fail with X3501");
            // 生成された入口名エラー記録
            const auto failurePath =
                NewCacheEntry(before, L".fail");
            Require(
                !failurePath.empty(),
                "a missing entry point must be remembered");

            // allowlist外の古い失敗記録を配置する。
            WriteFile(
                failurePath,
                "Failed to compile shader other-project.hlsl"
                " (VSMain): other-project.hlsl(4,10-35):"
                " error X1507: failed to open source file:"
                " 'LamaPonScreenDepth.hlsli'\n");
            // 失敗記録を破棄した後の再コンパイル結果
            const auto replayed = CompileMessage(
                assets,
                L"poisoned.hlsl",
                "VSMain",
                "vs_5_0");
            Require(
                !Contains(replayed, "X1507"),
                "a failure outside the allowlist must not be replayed");
            Require(
                Contains(replayed, "X3501"),
                "the shader must be compiled again for the real error");
            Require(
                !Contains(ReadFile(failurePath), "X1507"),
                "the stale entry must be replaced, not kept");
        }

        // セーフモードは失敗記録だけを削除し、bytecodeを残す。
        {
            // キャッシュ衝突を避ける識別コメント
            const auto marker = UniqueMarker("discard");
            // 成功するshader
            const auto good = root.Path() / L"good.hlsl";
            // 失敗記録を作るshader
            const auto bad = root.Path() / L"bad.hlsl";
            WriteFile(
                good,
                marker
                + "float4 VSMain() : SV_Position"
                " { return float4(1, 1, 1, 1); }\n");
            WriteFile(
                bad,
                marker
                + "float4 PSMain() : SV_Target { return 1; }\n");

            // 初回コンパイル前の成功bytecode一覧
            const auto beforeByteCode = CacheEntries(L".cso");
            Require(
                CompileMessage(
                    assets,
                    L"good.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "the good shader must compile");
            // 成功shaderのbytecodeキャッシュ
            const auto byteCodePath =
                NewCacheEntry(beforeByteCode, L".cso");
            Require(
                !byteCodePath.empty(),
                "a success must be remembered");

            // 失敗shaderのコンパイル前一覧
            const auto beforeFailure = CacheEntries(L".fail");
            Require(
                !CompileMessage(
                    assets,
                    L"bad.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "the bad shader must fail");
            // 生成された失敗キャッシュ
            const auto failurePath =
                NewCacheEntry(beforeFailure, L".fail");
            Require(
                !failurePath.empty(),
                "a missing entry point must be remembered");

            Require(
                LamaPon::ClearShaderCacheFailures() > 0,
                "discarding must report what it removed");
            Require(
                !std::filesystem::exists(failurePath),
                "the remembered failure must be gone");
            Require(
                std::filesystem::exists(byteCodePath),
                "the compiled bytecode must survive");
        }

        std::cout << "Shader cache checks passed." << std::endl;
    }
    // 例外(exception: shader検査失敗情報)を標準エラーへ出力する。
    catch (const std::exception& exception)
    {
        std::cerr << "Shader cache check failed: "
                  << exception.what() << std::endl;
        status = 1;
    }

    // COM初期化に成功した場合だけ終了処理する。
    if (uninitialize)
    {
        CoUninitialize();
    }
    return status;
}
