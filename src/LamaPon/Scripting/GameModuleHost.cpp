#include "LamaPon/Scripting/GameModuleHost.h"

#include "LamaPon/Components/NativeScriptComponent.h"
#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace
{
    // このランタイム（LamaPonRuntime.dll）のビルド時刻を返します。
    // 静的リンクなどで取得できない場合はfalseを返し、時刻による
    // 互換性判定を省略します。
    [[nodiscard]] bool TryGetRuntimeWriteTime(
        std::filesystem::file_time_type& writeTime) noexcept
    {
        const HMODULE runtime =
            GetModuleHandleW(L"LamaPonRuntime.dll");
        if (runtime == nullptr)
        {
            return false;
        }
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length = GetModuleFileNameW(
            runtime,
            path.data(),
            static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return false;
        }
        path.resize(length);
        std::error_code error;
        const auto time =
            std::filesystem::last_write_time(path, error);
        if (error)
        {
            return false;
        }
        writeTime = time;
        return true;
    }

    std::string WindowsErrorMessage(const DWORD error)
    {
        if (error == ERROR_SUCCESS)
        {
            return {};
        }

        char* buffer{};
        const DWORD size = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER
                | FORMAT_MESSAGE_FROM_SYSTEM
                | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            0,
            reinterpret_cast<char*>(&buffer),
            0,
            nullptr);
        std::string result = size != 0 && buffer != nullptr
            ? std::string(buffer, size)
            : "Windows error " + std::to_string(error);
        if (buffer != nullptr)
        {
            LocalFree(buffer);
        }
        while (!result.empty()
            && (result.back() == '\r'
                || result.back() == '\n'
                || result.back() == ' '))
        {
            result.pop_back();
        }
        return result;
    }

    bool IsValidName(const char* value)
    {
        if (value == nullptr)
        {
            return false;
        }
        const std::string_view text(value);
        return !text.empty()
            && text.size() <= 128
            && text.find_first_not_of(" \t\r\n")
                != std::string_view::npos;
    }
}

namespace LamaPon
{
    struct GameModuleHost::Candidate final
    {
        void* handle{};
        const GameModuleDescriptor* descriptor{};
        std::filesystem::path shadowPath;
        std::filesystem::file_time_type writeTime{};
        std::string moduleName;
        std::vector<RegisteredNativeScript> components;
        std::vector<RegisteredDataAssetType> dataAssets;
    };

    GameModuleHost* GameModuleHost::s_current{};

    GameModuleHost::GameModuleHost()
    {
        if (s_current != nullptr)
        {
            throw std::logic_error(
                "Only one GameModuleHost can be active.");
        }
        s_current = this;
    }

    GameModuleHost::~GameModuleHost()
    {
        const auto instances = m_instances;
        for (auto* component : instances)
        {
            if (component != nullptr)
            {
                component->BeforeModuleUnload();
                component->m_host = nullptr;
            }
        }
        m_instances.clear();
        ReleaseLoadedModule();
        s_current = nullptr;
    }

    bool GameModuleHost::Load(std::filesystem::path modulePath)
    {
        const auto requestedPath =
            std::filesystem::absolute(
            std::move(modulePath)).lexically_normal();
        m_lastError.clear();
        m_modulePath = requestedPath;
        if (!std::filesystem::is_regular_file(
                requestedPath))
        {
            if (IsLoaded())
            {
                const auto instances = m_instances;
                for (auto* component : instances)
                {
                    if (component != nullptr)
                    {
                        component->BeforeModuleUnload();
                    }
                }
                ReleaseLoadedModule();
                m_loadedWriteTime = {};
                for (auto* component : instances)
                {
                    if (component != nullptr)
                    {
                        component->AfterModuleLoad();
                    }
                }
            }
            return false;
        }
        return Reload();
    }

    bool GameModuleHost::Reload()
    {
        if (m_modulePath.empty())
        {
            m_lastError = "Game Module path is empty.";
            return false;
        }

        Candidate candidate;
        if (!LoadCandidate(candidate))
        {
            return false;
        }

        const auto instances = m_instances;
        for (auto* component : instances)
        {
            if (component != nullptr)
            {
                component->BeforeModuleUnload();
            }
        }

        ReleaseLoadedModule();
        m_moduleHandle = candidate.handle;
        m_descriptor = candidate.descriptor;
        m_shadowPath = std::move(candidate.shadowPath);
        m_loadedWriteTime = candidate.writeTime;
        m_moduleName = std::move(candidate.moduleName);
        m_registeredComponents =
            std::move(candidate.components);
        m_registeredDataAssets =
            std::move(candidate.dataAssets);
        m_lastError.clear();

        for (auto* component : instances)
        {
            if (component != nullptr)
            {
                component->AfterModuleLoad();
            }
        }
        return true;
    }

    void GameModuleHost::PollHotReload(
        const float deltaTime)
    {
        m_pollAccumulator += deltaTime;
        if (m_pollAccumulator < 0.5f
            || m_modulePath.empty())
        {
            return;
        }
        m_pollAccumulator = 0.0f;

        std::error_code error;
        const auto writeTime =
            std::filesystem::last_write_time(
                m_modulePath,
                error);
        if (!error && writeTime != m_loadedWriteTime)
        {
            static_cast<void>(Reload());
        }
    }

    const NativeScriptTypeDescriptor*
        GameModuleHost::FindComponent(
            const std::string_view typeName) const noexcept
    {
        if (m_descriptor == nullptr)
        {
            return nullptr;
        }
        for (std::size_t index = 0;
            index < m_descriptor->componentCount;
            ++index)
        {
            const auto& component =
                m_descriptor->components[index];
            if (component.typeName != nullptr
                && typeName == component.typeName)
            {
                return &component;
            }
        }
        return nullptr;
    }

    const RegisteredDataAssetType*
        GameModuleHost::FindDataAssetType(
            const std::string_view typeName) const noexcept
    {
        for (const auto& dataAsset : m_registeredDataAssets)
        {
            if (dataAsset.typeName == typeName)
            {
                return &dataAsset;
            }
        }
        return nullptr;
    }

    GameModuleHost* GameModuleHost::Current() noexcept
    {
        return s_current;
    }

    std::filesystem::path GameModuleHost::HotReloadDirectoryFor(
        const std::filesystem::path& modulePath)
    {
        // ネットワークやWebDAV上ではシャドウコピーとLoadLibraryが
        // 失敗する場合があります。リモートのモジュールはユーザーの
        // ローカル領域へコピーしてから読み込みます。
        if (!UsesNetworkDrive(modulePath))
        {
            return modulePath.parent_path() / L".lamapon-hot-reload";
        }
        return LocalEngineCachePath(L"HotReload")
            / PathCacheKey(modulePath.parent_path());
    }

    void GameModuleHost::RegisterInstance(
        NativeScriptComponent& component)
    {
        if (std::ranges::find(
                m_instances,
                &component) == m_instances.end())
        {
            m_instances.push_back(&component);
            component.m_host = this;
        }
    }

    void GameModuleHost::UnregisterInstance(
        NativeScriptComponent& component) noexcept
    {
        std::erase(m_instances, &component);
        if (component.m_host == this)
        {
            component.m_host = nullptr;
        }
    }

    bool GameModuleHost::LoadCandidate(
        Candidate& candidate)
    {
        std::error_code error;
        candidate.writeTime =
            std::filesystem::last_write_time(
                m_modulePath,
                error);
        if (error)
        {
            m_lastError =
                "Game Module timestamp could not be read: "
                + error.message();
            return false;
        }

        // Runtimeより古いGame Moduleは、同じAPI番号でも公開クラスの
        // レイアウトが一致しない可能性があります。読み込み前に更新時刻を
        // 比較し、再ビルドが必要な場合は手順を案内します。
        std::filesystem::file_time_type runtimeWriteTime{};
        if (TryGetRuntimeWriteTime(runtimeWriteTime)
            && candidate.writeTime < runtimeWriteTime)
        {
            m_lastError =
                "Game Module is older than this engine build, "
                "so it may not match the current headers. "
                "Rebuild it (save a .cpp in the editor, "
                "or run: LamaPonCli build --project <dir>).";
            return false;
        }

        auto hotReloadDirectory = HotReloadDirectoryFor(m_modulePath);
        if (!EnsureDirectoryExists(hotReloadDirectory, error))
        {
            // 読み取り専用や権限不足でプロジェクト内へ作成できない場合は、
            // ユーザーのローカル領域へ切り替えて再試行します。
            const auto fallback =
                LocalEngineCachePath(L"HotReload")
                / PathCacheKey(m_modulePath.parent_path());
            if (fallback != hotReloadDirectory)
            {
                hotReloadDirectory = fallback;
                EnsureDirectoryExists(hotReloadDirectory, error);
            }
        }
        if (error)
        {
            m_lastError =
                "Hot-reload directory could not be created: "
                + error.message();
            return false;
        }
        for (std::filesystem::directory_iterator iterator(
                hotReloadDirectory,
                error);
            !error
                && iterator
                    != std::filesystem::directory_iterator{};
            iterator.increment(error))
        {
            const auto& entry = *iterator;
            const auto filename =
                entry.path().filename().wstring();
            const auto prefix =
                m_modulePath.stem().wstring()
                + L"-";
            if (entry.is_regular_file()
                && filename.starts_with(prefix)
                && entry.path().extension()
                    == m_modulePath.extension()
                && entry.path() != m_shadowPath)
            {
                std::error_code removeError;
                std::filesystem::remove(
                    entry.path(),
                    removeError);
            }
        }
        error.clear();

        const auto uniqueValue = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        candidate.shadowPath =
            hotReloadDirectory
            / (m_modulePath.stem().wstring()
                + L"-"
                + std::to_wstring(uniqueValue)
                + m_modulePath.extension().wstring());
        std::filesystem::copy_file(
            m_modulePath,
            candidate.shadowPath,
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (error)
        {
            m_lastError =
                "Game Module could not be copied for hot reload: "
                + error.message();
            return false;
        }

        const HMODULE handle =
            LoadLibraryW(candidate.shadowPath.c_str());
        if (handle == nullptr)
        {
            m_lastError =
                "Game Module could not be loaded: "
                + WindowsErrorMessage(GetLastError());
            CleanupShadowCopy(candidate.shadowPath);
            return false;
        }
        candidate.handle = handle;

        const auto getDescriptor =
            reinterpret_cast<
                GetGameModuleDescriptorFunction>(
                GetProcAddress(
                    handle,
                    "LamaPonGetGameModule"));
        if (getDescriptor == nullptr)
        {
            m_lastError =
                "Game Module does not export LamaPonGetGameModule.";
            FreeLibrary(handle);
            candidate.handle = nullptr;
            CleanupShadowCopy(candidate.shadowPath);
            return false;
        }

        candidate.descriptor = getDescriptor();
        // APIバージョンの不一致は再ビルドで解消できるため、
        // 一般的なモジュール破損と分けて必要な操作を案内します。
        if (candidate.descriptor != nullptr
            && candidate.descriptor->apiVersion
                != GameModuleApiVersion)
        {
            m_lastError =
                "Game Module was built for API version "
                + std::to_string(
                    candidate.descriptor->apiVersion)
                + " but this engine requires "
                + std::to_string(GameModuleApiVersion)
                + ". Rebuild it (save a .cpp in the editor, "
                  "or run: LamaPonCli build --project <dir>).";
            FreeLibrary(handle);
            candidate.handle = nullptr;
            CleanupShadowCopy(candidate.shadowPath);
            return false;
        }
        if (candidate.descriptor == nullptr
            || !IsValidName(
                candidate.descriptor->moduleName)
            || candidate.descriptor->componentCount > 256
            || (candidate.descriptor->componentCount != 0
                && candidate.descriptor->components == nullptr)
            || candidate.descriptor->dataAssetCount > 256
            || (candidate.descriptor->dataAssetCount != 0
                && candidate.descriptor->dataAssets == nullptr))
        {
            m_lastError =
                "Game Module descriptor is invalid.";
            FreeLibrary(handle);
            candidate.handle = nullptr;
            CleanupShadowCopy(candidate.shadowPath);
            return false;
        }

        std::unordered_set<std::string> typeNames;
        candidate.moduleName =
            candidate.descriptor->moduleName;
        candidate.components.reserve(
            candidate.descriptor->componentCount);
        for (std::size_t index = 0;
            index < candidate.descriptor->componentCount;
            ++index)
        {
            const auto& component =
                candidate.descriptor->components[index];
            if (!IsValidName(component.typeName)
                || !IsValidName(component.displayName)
                || component.create == nullptr
                || component.destroy == nullptr
                || !typeNames.emplace(
                    component.typeName).second)
            {
                m_lastError =
                    "Game Module contains an invalid or duplicate component descriptor.";
                FreeLibrary(handle);
                candidate.handle = nullptr;
                CleanupShadowCopy(candidate.shadowPath);
                return false;
            }
            candidate.components.push_back(
                {
                    component.typeName,
                    component.displayName
                });
        }

        std::unordered_set<std::string> dataAssetTypeNames;
        candidate.dataAssets.reserve(
            candidate.descriptor->dataAssetCount);
        for (std::size_t index = 0;
            index < candidate.descriptor->dataAssetCount;
            ++index)
        {
            const auto& dataAsset =
                candidate.descriptor->dataAssets[index];
            if (!IsValidName(dataAsset.typeName)
                || !IsValidName(dataAsset.displayName)
                || !dataAssetTypeNames.emplace(
                    dataAsset.typeName).second)
            {
                m_lastError =
                    "Game Module contains an invalid or duplicate data asset descriptor.";
                FreeLibrary(handle);
                candidate.handle = nullptr;
                CleanupShadowCopy(candidate.shadowPath);
                return false;
            }
            candidate.dataAssets.push_back(
                {
                    dataAsset.typeName,
                    dataAsset.displayName,
                    dataAsset.schemaJson != nullptr
                        ? dataAsset.schemaJson
                        : ""
                });
        }
        return true;
    }

    void GameModuleHost::ReleaseLoadedModule() noexcept
    {
        if (m_moduleHandle != nullptr)
        {
            FreeLibrary(
                static_cast<HMODULE>(m_moduleHandle));
            m_moduleHandle = nullptr;
        }
        m_descriptor = nullptr;
        m_moduleName.clear();
        m_registeredComponents.clear();
        m_registeredDataAssets.clear();
        CleanupShadowCopy(m_shadowPath);
        m_shadowPath.clear();
    }

    void GameModuleHost::CleanupShadowCopy(
        const std::filesystem::path& path) noexcept
    {
        if (path.empty())
        {
            return;
        }
        std::error_code error;
        std::filesystem::remove(path, error);
    }
}
