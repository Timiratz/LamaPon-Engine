#pragma once

#include "LamaPon/Scripting/GameModule.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class NativeScriptComponent;

    struct RegisteredNativeScript final
    {
        std::string typeName;
        std::string displayName;
    };

    // Game Moduleが宣言したデータアセットの型です。スキーマは
    // Inspectorが入力欄を描くために使うので、文字列で保持します
    // （DLLを外した後も表示が壊れないよう、コピーして持ちます）。
    struct RegisteredDataAssetType final
    {
        std::string typeName;
        std::string displayName;
        std::string schemaJson;
    };

    class GameModuleHost final
    {
    public:
        GameModuleHost();
        ~GameModuleHost();

        GameModuleHost(const GameModuleHost&) = delete;
        GameModuleHost& operator=(const GameModuleHost&) = delete;

        bool Load(std::filesystem::path modulePath);
        bool Reload();
        void PollHotReload(float deltaTime);

        [[nodiscard]] bool IsLoaded() const noexcept
        {
            return m_moduleHandle != nullptr;
        }
        [[nodiscard]] const std::filesystem::path&
            ModulePath() const noexcept
        {
            return m_modulePath;
        }
        [[nodiscard]] const std::string&
            ModuleName() const noexcept
        {
            return m_moduleName;
        }
        [[nodiscard]] const std::string&
            LastError() const noexcept
        {
            return m_lastError;
        }
        [[nodiscard]] const std::vector<RegisteredNativeScript>&
            RegisteredComponents() const noexcept
        {
            return m_registeredComponents;
        }
        [[nodiscard]] const NativeScriptTypeDescriptor*
            FindComponent(std::string_view typeName) const noexcept;
        [[nodiscard]] const std::vector<RegisteredDataAssetType>&
            RegisteredDataAssets() const noexcept
        {
            return m_registeredDataAssets;
        }
        // 型名からデータアセットの宣言を引きます。見つからない
        // ときはnullptr（型が消えた・モジュール未読込）です。
        [[nodiscard]] const RegisteredDataAssetType*
            FindDataAssetType(
                std::string_view typeName) const noexcept;

        [[nodiscard]] static GameModuleHost* Current() noexcept;

        // シャドウコピーの置き場所。ネットワーク/WebDAV上のモジュールは
        // 共有側へ書けない・ロードできないことがあるため、ユーザー
        // ローカルへ退避します（テストから直接検証できるよう公開）。
        [[nodiscard]] static std::filesystem::path
            HotReloadDirectoryFor(
                const std::filesystem::path& modulePath);

    private:
        friend class NativeScriptComponent;

        struct Candidate;

        void RegisterInstance(NativeScriptComponent& component);
        void UnregisterInstance(NativeScriptComponent& component) noexcept;
        [[nodiscard]] bool LoadCandidate(Candidate& candidate);
        void ReleaseLoadedModule() noexcept;
        void CleanupShadowCopy(
            const std::filesystem::path& path) noexcept;

        static GameModuleHost* s_current;

        std::filesystem::path m_modulePath;
        std::filesystem::path m_shadowPath;
        std::filesystem::file_time_type m_loadedWriteTime{};
        void* m_moduleHandle{};
        const GameModuleDescriptor* m_descriptor{};
        std::string m_moduleName;
        std::string m_lastError;
        std::vector<RegisteredNativeScript> m_registeredComponents;
        std::vector<RegisteredDataAssetType> m_registeredDataAssets;
        std::vector<NativeScriptComponent*> m_instances;
        float m_pollAccumulator{};
    };
}
