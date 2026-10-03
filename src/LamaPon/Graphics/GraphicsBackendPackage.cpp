#include "LamaPon/Graphics/GraphicsBackendPackage.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/VersionCompare.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

namespace
{
    // 描画パッケージ設定の排他
    std::mutex g_backendPackageMutex;
    // パッケージ検索のアセット基点
    std::filesystem::path g_backendPackageAssetRoot;
    // 終了まで保持する描画DLL
    HMODULE g_backendPackageModule{};
    // 保持中の描画DLLの絶対パス
    std::filesystem::path g_loadedBackendLibrary;

    // 正規化後に親へ出ない相対パスか調べる(path: 検査するパス)。
    bool IsSafeRelativePath(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute()
            || path.has_root_name() || path.has_root_directory())
        {
            return false;
        }
        // 正規化したパスの構成要素
        for (const auto& component : path.lexically_normal())
        {
            if (component == L"..")
            {
                return false;
            }
        }
        return true;
    }

    // 正規化した候補が基点の内側にあるか調べる(directory: 基点フォルダー, candidate: 候補ファイル)。
    bool IsWithinDirectory(
        const std::filesystem::path& directory,
        const std::filesystem::path& candidate)
    {
        // ファイルパス操作のエラー
        std::error_code error;
        // 正規化した基点の文字列
        auto root = std::filesystem::weakly_canonical(
            directory,
            error).native();
        if (error)
        {
            return false;
        }
        // 正規化した候補の文字列
        auto file = std::filesystem::weakly_canonical(
            candidate,
            error).native();
        if (error || file.size() <= root.size()
            || _wcsnicmp(file.c_str(), root.c_str(), root.size()) != 0)
        {
            return false;
        }
        return root.ends_with(L'\\') || root.ends_with(L'/')
            || file[root.size()] == L'\\'
            || file[root.size()] == L'/';
    }

    // 失敗状態と説明を検査結果へまとめる(state: 失敗の区分, message: 結果の説明)。
    LamaPon::GraphicsBackendPackageInspection Failure(
        const LamaPon::GraphicsBackendPackageState state,
        std::string message)
    {
        // 描画パッケージの検査結果
        LamaPon::GraphicsBackendPackageInspection result;
        result.state = state;
        result.message = std::move(message);
        return result;
    }
}

namespace LamaPon
{
    GraphicsBackendPackageInspection InspectGraphicsBackendPackage(
        const std::filesystem::path& assetRoot,
        const RenderingApi api,
        const std::string_view currentEngineVersion)
    {
        if (api != RenderingApi::DirectX12Experimental)
        {
            // 描画パッケージの検査結果
            GraphicsBackendPackageInspection result;
            result.state = GraphicsBackendPackageState::BuiltIn;
            result.descriptor.api = RenderingApi::DirectX11;
            return result;
        }

        // 描画パッケージの配置先
        const auto packageDirectory = assetRoot / L"packages"
            / PathFromUtf8(DirectX12BackendPackageName);
        // パッケージ宣言のパス
        const auto manifestPath = packageDirectory / L"package.json";
        if (!std::filesystem::is_regular_file(manifestPath))
        {
            return Failure(
                GraphicsBackendPackageState::Missing,
                "DirectX 12バックエンドパッケージが未導入です。");
        }

        // 読み込むパッケージ宣言
        nlohmann::json manifest;
        try
        {
            // パッケージ宣言の入力ファイル
            std::ifstream input(manifestPath, std::ios::binary);
            // 読み込むパッケージ宣言
            input >> manifest;
        }
        catch (const std::exception&)
        {
            return Failure(
                GraphicsBackendPackageState::InvalidManifest,
                "DirectX 12バックエンドのpackage.jsonが不正です。");
        }

        try
        {
            // 宣言されたパッケージ名
            const auto name = manifest.value("name", std::string{});
            // 宣言されたパッケージ版
            const auto version = manifest.value("version", std::string{});
            // 必要なエンジンの最低版
            const auto minimumVersion = manifest.value(
                "minimumEngineVersion", std::string{});
            // 適用に必要な再起動の方式
            const auto activation = manifest.value(
                "activation", std::string{});
            if (name != DirectX12BackendPackageName || version.empty()
                || (activation != "Restart"
                    && activation != "RestartAndRebuild")
                || (!minimumVersion.empty()
                    && ParseVersionNumbers(minimumVersion).empty()))
            {
                return Failure(
                    GraphicsBackendPackageState::InvalidManifest,
                    "DirectX 12バックエンドのパッケージ情報が不正です。");
            }
            if (!minimumVersion.empty()
                && IsNewerVersion(currentEngineVersion, minimumVersion))
            {
                return Failure(
                    GraphicsBackendPackageState::IncompatibleEngine,
                    "DirectX 12バックエンドには新しいエンジンが必要です。");
            }

            if (!manifest.contains("graphicsBackend")
                || !manifest.at("graphicsBackend").is_object())
            {
                return Failure(
                    GraphicsBackendPackageState::InvalidManifest,
                    "graphicsBackend宣言がありません。");
            }
            // 描画基盤の宣言オブジェクト
            const auto& backend = manifest.at("graphicsBackend");
            if (backend.value("api", std::string{})
                != "DirectX12Experimental")
            {
                return Failure(
                    GraphicsBackendPackageState::InvalidManifest,
                    "graphicsBackend.apiがDirectX 12ではありません。");
            }
            // 宣言された描画ABI版
            const auto abiVersion = backend.value("abiVersion", 0u);
            if (abiVersion != GraphicsBackendPackageAbiVersion)
            {
                return Failure(
                    GraphicsBackendPackageState::IncompatibleAbi,
                    "DirectX 12バックエンドのABIバージョンが一致しません。");
            }

            // パッケージ内の描画DLLパス
            const auto runtimeRelative = PathFromUtf8(
                backend.value("runtimeLibrary", std::string{}));
            if (!IsSafeRelativePath(runtimeRelative)
                || runtimeRelative.extension() != L".dll")
            {
                return Failure(
                    GraphicsBackendPackageState::InvalidManifest,
                    "runtimeLibraryはパッケージ内のDLLを指定してください。");
            }
            // 描画DLLの正規化したパス
            const auto runtimeLibrary =
                (packageDirectory / runtimeRelative).lexically_normal();
            if (!std::filesystem::is_regular_file(runtimeLibrary))
            {
                return Failure(
                    GraphicsBackendPackageState::RuntimeMissing,
                    "DirectX 12バックエンドDLLが見つかりません。");
            }
            if (!IsWithinDirectory(packageDirectory, runtimeLibrary))
            {
                return Failure(
                    GraphicsBackendPackageState::InvalidManifest,
                    "runtimeLibraryがパッケージ外を参照しています。");
            }

            // 描画パッケージの検査結果
            GraphicsBackendPackageInspection result;
            result.state = GraphicsBackendPackageState::Ready;
            result.descriptor.api = RenderingApi::DirectX12Experimental;
            result.descriptor.abiVersion = abiVersion;
            result.descriptor.version = version;
            result.descriptor.packageDirectory = packageDirectory;
            result.descriptor.runtimeLibrary = runtimeLibrary;
            return result;
        }
        catch (const std::exception&)
        {
            return Failure(
                GraphicsBackendPackageState::InvalidManifest,
                "DirectX 12バックエンドのpackage.jsonが不正です。");
        }
    }

    void SetGraphicsBackendPackageAssetRoot(
        std::filesystem::path assetRoot)
    {
        // 描画パッケージ設定の排他保持
        std::scoped_lock lock(g_backendPackageMutex);
        g_backendPackageAssetRoot = std::move(assetRoot);
    }

    GraphicsBackendPackageInspection ActivateGraphicsBackendPackage(
        const RenderingApi api,
        const std::string_view currentEngineVersion)
    {
        // 描画パッケージ設定の排他保持
        std::scoped_lock lock(g_backendPackageMutex);
        if (g_backendPackageAssetRoot.empty())
        {
            return Failure(
                GraphicsBackendPackageState::Missing,
                "描画バックエンドのパッケージルートが未指定です。");
        }
        // 描画パッケージの検査結果
        auto inspection = InspectGraphicsBackendPackage(
            g_backendPackageAssetRoot,
            api,
            currentEngineVersion);
        if (inspection.state != GraphicsBackendPackageState::Ready)
        {
            return inspection;
        }

        // ファイルパス操作のエラー
        std::error_code error;
        // 要求された描画DLLの絶対パス
        const auto requestedLibrary = std::filesystem::weakly_canonical(
            inspection.descriptor.runtimeLibrary,
            error);
        if (error)
        {
            inspection.state = GraphicsBackendPackageState::RuntimeMissing;
            inspection.message =
                "DirectX 12バックエンドDLLを解決できません。";
            return inspection;
        }
        if (g_backendPackageModule != nullptr)
        {
            if (requestedLibrary == g_loadedBackendLibrary)
            {
                return inspection;
            }
            inspection.state = GraphicsBackendPackageState::InvalidManifest;
            inspection.message =
                "別の描画バックエンドDLLが既にロードされています。";
            return inspection;
        }

        // ロードした描画DLL
        const HMODULE module = LoadLibraryExW(
            requestedLibrary.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module == nullptr)
        {
            inspection.state = GraphicsBackendPackageState::RuntimeMissing;
            inspection.message =
                "DirectX 12バックエンドDLLをロードできません。";
            return inspection;
        }

        using AbiVersionFunction = std::uint32_t (*)();
        using ApiNameFunction = const char* (*)();
        // ABI版を取得する関数
        const auto abiVersionFunction = reinterpret_cast<
            AbiVersionFunction>(GetProcAddress(
                module,
                "LamaPonGraphicsBackendAbiVersion"));
        // 描画API名を取得する関数
        const auto apiNameFunction = reinterpret_cast<ApiNameFunction>(
            GetProcAddress(module, "LamaPonGraphicsBackendApi"));
        if (abiVersionFunction == nullptr || apiNameFunction == nullptr)
        {
            FreeLibrary(module);
            inspection.state = GraphicsBackendPackageState::IncompatibleAbi;
            inspection.message =
                "DirectX 12バックエンドのABI entry pointがありません。";
            return inspection;
        }
        // DLLが宣言する描画API名
        const char* const apiName = apiNameFunction();
        if (abiVersionFunction() != GraphicsBackendPackageAbiVersion
            || apiName == nullptr
            || std::string_view{ apiName } != "DirectX12Experimental")
        {
            FreeLibrary(module);
            inspection.state = GraphicsBackendPackageState::IncompatibleAbi;
            inspection.message =
                "DirectX 12バックエンドDLLのABIが一致しません。";
            return inspection;
        }

        g_backendPackageModule = module;
        g_loadedBackendLibrary = requestedLibrary;
        return inspection;
    }
}
