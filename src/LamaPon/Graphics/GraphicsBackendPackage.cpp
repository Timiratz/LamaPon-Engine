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
    std::mutex g_backendPackageMutex;
    std::filesystem::path g_backendPackageAssetRoot;
    HMODULE g_backendPackageModule{};
    std::filesystem::path g_loadedBackendLibrary;

    bool IsSafeRelativePath(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute()
            || path.has_root_name() || path.has_root_directory())
        {
            return false;
        }
        for (const auto& component : path.lexically_normal())
        {
            if (component == L"..")
            {
                return false;
            }
        }
        return true;
    }

    bool IsWithinDirectory(
        const std::filesystem::path& directory,
        const std::filesystem::path& candidate)
    {
        std::error_code error;
        auto root = std::filesystem::weakly_canonical(
            directory,
            error).native();
        if (error)
        {
            return false;
        }
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

    LamaPon::GraphicsBackendPackageInspection Failure(
        const LamaPon::GraphicsBackendPackageState state,
        std::string message)
    {
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
            GraphicsBackendPackageInspection result;
            result.state = GraphicsBackendPackageState::BuiltIn;
            result.descriptor.api = RenderingApi::DirectX11;
            return result;
        }

        const auto packageDirectory = assetRoot / L"packages"
            / PathFromUtf8(DirectX12BackendPackageName);
        const auto manifestPath = packageDirectory / L"package.json";
        if (!std::filesystem::is_regular_file(manifestPath))
        {
            return Failure(
                GraphicsBackendPackageState::Missing,
                "DirectX 12バックエンドパッケージが未導入です。");
        }

        nlohmann::json manifest;
        try
        {
            std::ifstream input(manifestPath, std::ios::binary);
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
            const auto name = manifest.value("name", std::string{});
            const auto version = manifest.value("version", std::string{});
            const auto minimumVersion = manifest.value(
                "minimumEngineVersion", std::string{});
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
            const auto& backend = manifest.at("graphicsBackend");
            if (backend.value("api", std::string{})
                != "DirectX12Experimental")
            {
                return Failure(
                    GraphicsBackendPackageState::InvalidManifest,
                    "graphicsBackend.apiがDirectX 12ではありません。");
            }
            const auto abiVersion = backend.value("abiVersion", 0u);
            if (abiVersion != GraphicsBackendPackageAbiVersion)
            {
                return Failure(
                    GraphicsBackendPackageState::IncompatibleAbi,
                    "DirectX 12バックエンドのABIバージョンが一致しません。");
            }

            const auto runtimeRelative = PathFromUtf8(
                backend.value("runtimeLibrary", std::string{}));
            if (!IsSafeRelativePath(runtimeRelative)
                || runtimeRelative.extension() != L".dll")
            {
                return Failure(
                    GraphicsBackendPackageState::InvalidManifest,
                    "runtimeLibraryはパッケージ内のDLLを指定してください。");
            }
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
        std::scoped_lock lock(g_backendPackageMutex);
        g_backendPackageAssetRoot = std::move(assetRoot);
    }

    GraphicsBackendPackageInspection ActivateGraphicsBackendPackage(
        const RenderingApi api,
        const std::string_view currentEngineVersion)
    {
        std::scoped_lock lock(g_backendPackageMutex);
        if (g_backendPackageAssetRoot.empty())
        {
            return Failure(
                GraphicsBackendPackageState::Missing,
                "描画バックエンドのパッケージルートが未指定です。");
        }
        auto inspection = InspectGraphicsBackendPackage(
            g_backendPackageAssetRoot,
            api,
            currentEngineVersion);
        if (inspection.state != GraphicsBackendPackageState::Ready)
        {
            return inspection;
        }

        std::error_code error;
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
        const auto abiVersionFunction = reinterpret_cast<
            AbiVersionFunction>(GetProcAddress(
                module,
                "LamaPonGraphicsBackendAbiVersion"));
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
