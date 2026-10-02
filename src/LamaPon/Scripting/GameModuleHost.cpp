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
    // Runtimeのファイル更新時刻を取得し不明ならfalseとする(writeTime: 時刻の出力)。
    [[nodiscard]] bool TryGetRuntimeWriteTime(
        std::filesystem::file_time_type& writeTime) noexcept
    {
        // 現在読み込み中のRuntime
        const HMODULE runtime =
            GetModuleHandleW(L"LamaPonRuntime.dll");
        if (runtime == nullptr)
        {
            return false;
        }
        // RuntimeDLLの取得先パス
        std::wstring path(MAX_PATH, L'\0');
        // 取得したDLLパスの文字数
        const DWORD length = GetModuleFileNameW(
            runtime,
            path.data(),
            static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return false;
        }
        path.resize(length);
        // ファイル操作のエラー
        std::error_code error;
        // Runtimeの更新時刻
        const auto time =
            std::filesystem::last_write_time(path, error);
        if (error)
        {
            return false;
        }
        writeTime = time;
        return true;
    }

    // Windowsエラーを改行末尾のない文字列にする(error: エラー番号)。
    std::string WindowsErrorMessage(const DWORD error)
    {
        if (error == ERROR_SUCCESS)
        {
            return {};
        }

        // Windowsが確保したエラー文
        char* buffer{};
        // エラー文の取得文字数
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
        // 整形したWindowsエラー文
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

    // 空・空白のみ・128バイト超の名前を拒否する(value: 任意のUTF8型名または表示名)。
    bool IsValidName(const char* value)
    {
        if (value == nullptr)
        {
            return false;
        }
        // 検査する借用名文字列
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
        // 採用する候補DLLの所有権
        void* handle{};
        // 候補DLL内の型記述
        const GameModuleDescriptor* descriptor{};
        // 候補DLLの読み込み用コピー
        std::filesystem::path shadowPath;
        // 候補元DLLの更新時刻
        std::filesystem::file_time_type writeTime{};
        // 候補DLLの複製表示名
        std::string moduleName;
        // 候補のスクリプト型名列
        std::vector<RegisteredNativeScript> components;
        // 候補のデータ型とスキーマ
        std::vector<RegisteredDataAssetType> dataAssets;
    };

    // 唯一の有効なDLL管理器
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
        // 通知中の変更を避ける登録控え
        const auto instances = m_instances;
        // 通知または検証する型と物体
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
        ReleaseNativeSearchDirectories();
        s_current = nullptr;
    }


    void GameModuleHost::SetNativeSearchDirectories(
        std::vector<std::filesystem::path> directories)
    {
        ReleaseNativeSearchDirectories();
        m_nativeSearchDirectories = std::move(directories);
        ApplyNativeSearchDirectories();
    }

    void GameModuleHost::ApplyNativeSearchDirectories()
    {
        // 依存DLLの探索フォルダー
        for (const auto& directory : m_nativeSearchDirectories)
        {
            // ファイル操作のエラー
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error))
            {
                continue;
            }
            // 正規化した探索先の絶対パス
            const auto absolute =
                std::filesystem::absolute(directory, error)
                    .lexically_normal();
            if (error)
            {
                continue;
            }
            // Windowsの探索登録
            if (auto* const cookie =
                AddDllDirectory(absolute.c_str()))
            {
                m_nativeSearchCookies.push_back(cookie);
            }
        }
    }

    void GameModuleHost::ReleaseNativeSearchDirectories() noexcept
    {
        // Windowsの探索登録
        for (auto* const cookie : m_nativeSearchCookies)
        {
            static_cast<void>(
                RemoveDllDirectory(
                    static_cast<DLL_DIRECTORY_COOKIE>(cookie)));
        }
        m_nativeSearchCookies.clear();
    }

    bool GameModuleHost::Load(std::filesystem::path modulePath)
    {
        // 正規化した元DLLパス
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
                // 通知中の変更を避ける登録控え
                const auto instances = m_instances;
                // 通知または検証する型と物体
                for (auto* component : instances)
                {
                    if (component != nullptr)
                    {
                        component->BeforeModuleUnload();
                    }
                }
                ReleaseLoadedModule();
                m_loadedWriteTime = {};
                // 通知または検証する型と物体
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

        // 採用前に検証するDLL候補
        Candidate candidate;
        if (!LoadCandidate(candidate))
        {
            return false;
        }

        // 通知中の変更を避ける登録控え
        const auto instances = m_instances;
        // 通知または検証する型と物体
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

        // 通知または検証する型と物体
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

        // ファイル操作のエラー
        std::error_code error;
        // 元DLLの現在更新時刻
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
        // 型記述列の検査番号
        for (std::size_t index = 0;
            index < m_descriptor->componentCount;
            ++index)
        {
            // 通知または検証する型と物体
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
        // 検索または検証するデータ型
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
        // ファイル操作のエラー
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

        // API番号が一致してもRuntimeより古いDLLは拒否し、現在のヘッダーでの再ビルドを求める。
        // 互換判定用のRuntime時刻
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

        // DLLコピーの作成先
        auto hotReloadDirectory = HotReloadDirectoryFor(m_modulePath);
        if (!EnsureDirectoryExists(hotReloadDirectory, error))
        {
            // プロジェクト内に作成できない場合はユーザーローカルへ切り替える。
            // ユーザーローカルの作成先
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
        // 古いDLLコピーの走査位置
        for (std::filesystem::directory_iterator iterator(
                hotReloadDirectory,
                error);
            !error
                && iterator
                    != std::filesystem::directory_iterator{};
            iterator.increment(error))
        {
            // 確認する古いコピー項目
            const auto& entry = *iterator;
            // 古いDLLコピーのファイル名
            const auto filename =
                entry.path().filename().wstring();
            // 対象DLLコピーの名前接頭辞
            const auto prefix =
                m_modulePath.stem().wstring()
                + L"-";
            if (entry.is_regular_file()
                && filename.starts_with(prefix)
                && entry.path().extension()
                    == m_modulePath.extension()
                && entry.path() != m_shadowPath)
            {
                // 古いコピーの削除エラー
                std::error_code removeError;
                std::filesystem::remove(
                    entry.path(),
                    removeError);
            }
        }
        error.clear();

        // コピー名に含める単調時刻
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

        // 探索登録がない場合は検索フラグを変えず、Windowsの既定探索順を維持する。
        // 読み込んだ候補DLL
        const HMODULE handle = m_nativeSearchCookies.empty()
            ? LoadLibraryW(candidate.shadowPath.c_str())
            : LoadLibraryExW(
                candidate.shadowPath.c_str(),
                nullptr,
                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                    | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        if (handle == nullptr)
        {
            m_lastError =
                "Game Module could not be loaded: "
                + WindowsErrorMessage(GetLastError());
            CleanupShadowCopy(candidate.shadowPath);
            return false;
        }
        candidate.handle = handle;

        // DLLから型記述を取得する関数
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

        // 登録済みスクリプト型名
        std::unordered_set<std::string> typeNames;
        candidate.moduleName =
            candidate.descriptor->moduleName;
        candidate.components.reserve(
            candidate.descriptor->componentCount);
        // 型記述列の検査番号
        for (std::size_t index = 0;
            index < candidate.descriptor->componentCount;
            ++index)
        {
            // 通知または検証する型と物体
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

        // 登録済みデータ型名
        std::unordered_set<std::string> dataAssetTypeNames;
        candidate.dataAssets.reserve(
            candidate.descriptor->dataAssetCount);
        // 型記述列の検査番号
        for (std::size_t index = 0;
            index < candidate.descriptor->dataAssetCount;
            ++index)
        {
            // 検索または検証するデータ型
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
        // ファイル操作のエラー
        std::error_code error;
        std::filesystem::remove(path, error);
    }
}
