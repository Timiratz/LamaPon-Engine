#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace LamaPon
{
    class EditorExtensionRegistry;

    class EditorExtensionModuleManager final
    {
    public:
        // 登録中の外部モジュールを解除してDLLを解放します。
        ~EditorExtensionModuleManager();

        // 外部モジュールと登録callbackの共有を禁止します。
        EditorExtensionModuleManager(
            const EditorExtensionModuleManager&) = delete;
        // 外部モジュールと登録callbackの共有を禁止します。
        EditorExtensionModuleManager& operator=(
            const EditorExtensionModuleManager&) = delete;

        // 空の外部モジュール管理器を作ります。
        EditorExtensionModuleManager() = default;

        // assets/packagesのEditor拡張DLLを検査して登録します(assetRoot: assetsフォルダー, registry: 拡張登録先)。
        [[nodiscard]] std::vector<std::string> LoadInstalled(
            const std::filesystem::path& assetRoot,
            EditorExtensionRegistry& registry);
        // 登録callbackを解除してロードしたDLLを解放します。
        void Shutdown() noexcept;

    private:
        struct ModuleState;

        // 登録callbackが参照するロード済みモジュール
        std::vector<std::shared_ptr<ModuleState>> m_modules;
        // 登録先の拡張一覧
        EditorExtensionRegistry* m_registry{};
    };
}
