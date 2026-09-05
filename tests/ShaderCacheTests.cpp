// シェーダーのディスクキャッシュが依存ファイルの変更を検出し、
// 無効な失敗結果を再利用しないことを検証します。
// キャッシュキーに含まれない#includeの内容は.depsとの照合で判定します。

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

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] bool Contains(
        const std::string& text,
        const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            const auto unique =
                std::chrono::steady_clock::now()
                    .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonShaderCacheTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(
            const TemporaryDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path&
            Path() const noexcept
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output)
        {
            throw std::runtime_error(
                "Could not write the test shader.");
        }
    }

    [[nodiscard]] std::string ReadFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream contents;
        contents << input.rdbuf();
        return contents.str();
    }

    // 共有キャッシュ内の既存データと衝突しないキーをケースごとに生成します。
    [[nodiscard]] std::string UniqueMarker(const char* name)
    {
        const auto unique = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        return "// " + std::string(name) + "-"
            + std::to_string(unique) + "\n";
    }

    // コンパイルして、失敗したらそのメッセージを返します。
    // 成功したら空文字列です。
    [[nodiscard]] std::string CompileMessage(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* entryPoint,
        const char* target)
    {
        try
        {
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
        catch (const std::exception& exception)
        {
            return exception.what();
        }
    }

    [[nodiscard]] std::set<std::filesystem::path>
        CacheEntries(const std::wstring& extension)
    {
        std::set<std::filesystem::path> entries;
        const auto directory = LamaPon::ShaderCacheDirectory();
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
        {
            return entries;
        }
        for (const auto& entry :
            std::filesystem::directory_iterator(
                directory,
                error))
        {
            if (entry.path().extension() == extension)
            {
                entries.insert(entry.path());
            }
        }
        return entries;
    }

    // directoryへ新しく増えた1件を返します。キャッシュのキーは
    // 内部の実装なので、テストからは「増えたもの」として捕まえます。
    [[nodiscard]] std::filesystem::path NewCacheEntry(
        const std::set<std::filesystem::path>& before,
        const std::wstring& extension)
    {
        std::filesystem::path found;
        for (const auto& entry : CacheEntries(extension))
        {
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

    // このテストが共有キャッシュへ残したものを片付けます。キーは
    // 実行ごとに変わるので、放っておくと実行のたびに溜まります。
    class CacheLitterGuard final
    {
    public:
        CacheLitterGuard()
            : m_before(Snapshot())
        {
        }

        ~CacheLitterGuard()
        {
            std::error_code error;
            for (const auto& entry : Snapshot())
            {
                if (!m_before.contains(entry))
                {
                    std::filesystem::remove(entry, error);
                }
            }
        }

        CacheLitterGuard(const CacheLitterGuard&) = delete;
        CacheLitterGuard& operator=(
            const CacheLitterGuard&) = delete;

    private:
        [[nodiscard]] static std::set<std::filesystem::path>
            Snapshot()
        {
            std::set<std::filesystem::path> entries;
            for (const auto* extension :
                { L".cso", L".deps", L".fail" })
            {
                entries.merge(CacheEntries(extension));
            }
            return entries;
        }

        std::set<std::filesystem::path> m_before;
    };
}

int main()
{
    // AssetManagerはアセットの走査でWICを使うため、COMが要ります。
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);

    // AssetManagerが持つCOMオブジェクトはCoUninitializeより先に
    // 解放しなければならないので、後始末はtryを抜けてから行います。
    int status = 0;
    try
    {
        CacheLitterGuard litter;
        TemporaryDirectory root;
        LamaPon::AssetManager assets(nullptr, nullptr);
        assets.SetAssetRoot(root.Path());
        Require(
            LamaPon::IsShaderCacheEnabled(),
            "the disk cache must be on for these checks");

        // includeした側だけを直したとき、古いバイトコードを
        // 返さないこと。本体の中身は変わらないのでキーは同じです。
        // 変更は.depsの照合で検出します。
        {
            const auto marker = UniqueMarker("stale-bytecode");
            const auto shader = root.Path() / L"stale.hlsl";
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

            // includeだけを壊します。
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

        // include修正後は失敗結果を再利用せず、コンパイルが成功すること。
        {
            const auto marker = UniqueMarker("stale-failure");
            const auto shader = root.Path() / L"retry.hlsl";
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

        // 保存可能な失敗は「入口が無い」場合だけに限定します。
        // 構文エラーのような他の失敗は、.failを作らない。
        //
        // 未知の失敗を保存しないよう、保存可能な失敗を許可リストで限定します。
        {
            const auto marker = UniqueMarker("unremembered");
            const auto shader = root.Path() / L"syntax.hlsl";
            WriteFile(
                shader,
                marker
                + "float4 VSMain() : SV_Position"
                " { return not_a_function(); }\n");
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

            // 修正後は失敗キャッシュに妨げられずコンパイルが成功すること。
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

        // 保存対象外の失敗が既存キャッシュにあっても、読み込み時に破棄すること。
        //
        // 既存の共有キャッシュに残る、現在は保存対象外の失敗も拒否します。
        {
            const auto marker = UniqueMarker("poisoned-failure");
            const auto shader = root.Path() / L"poisoned.hlsl";
            // includeを持たない入口名エラーを使い、依存関係を一定に保ちます。
            WriteFile(
                shader,
                marker
                + "float4 PSMain() : SV_Target { return 1; }\n");
            const auto before = CacheEntries(L".fail");
            const auto realFailure = CompileMessage(
                assets,
                L"poisoned.hlsl",
                "VSMain",
                "vs_5_0");
            Require(
                Contains(realFailure, "X3501"),
                "a missing entry point must fail with X3501");
            const auto failurePath =
                NewCacheEntry(before, L".fail");
            Require(
                !failurePath.empty(),
                "a missing entry point must be remembered");

            // 現在は保存対象外となる既存形式の失敗記録を用意します。
            WriteFile(
                failurePath,
                "Failed to compile shader other-project.hlsl"
                " (VSMain): other-project.hlsl(4,10-35):"
                " error X1507: failed to open source file:"
                " 'LamaPonScreenDepth.hlsli'\n");
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

        // セーフモードでは失敗結果だけを破棄できること。
        // バイトコードは残さないと、次の起動が全部コンパイルから
        // になります。
        {
            const auto marker = UniqueMarker("discard");
            const auto good = root.Path() / L"good.hlsl";
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

            const auto beforeByteCode = CacheEntries(L".cso");
            Require(
                CompileMessage(
                    assets,
                    L"good.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "the good shader must compile");
            const auto byteCodePath =
                NewCacheEntry(beforeByteCode, L".cso");
            Require(
                !byteCodePath.empty(),
                "a success must be remembered");

            const auto beforeFailure = CacheEntries(L".fail");
            Require(
                !CompileMessage(
                    assets,
                    L"bad.hlsl",
                    "VSMain",
                    "vs_5_0").empty(),
                "the bad shader must fail");
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
    catch (const std::exception& exception)
    {
        std::cerr << "Shader cache check failed: "
                  << exception.what() << std::endl;
        status = 1;
    }

    if (uninitialize)
    {
        CoUninitialize();
    }
    return status;
}
