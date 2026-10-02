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
        // 登録スクリプトの型名
        std::string typeName;
        // 登録スクリプトの表示名
        std::string displayName;
    };

    // 型名とスキーマはDLLから複製して保持する。
    struct RegisteredDataAssetType final
    {
        // 登録データアセットの型名
        std::string typeName;
        // 新規作成メニューの表示名
        std::string displayName;
        // 複製した型付き入力スキーマ
        std::string schemaJson;
    };

    // 読み込み・再読込・インスタンス登録はメインスレッドで直列化する。
    class GameModuleHost final
    {
    public:
        // 同時に1件だけ有効なDLL管理器を登録する。
        GameModuleHost();
        // 登録インスタンスを停止してDLLと探索先を解放する。
        ~GameModuleHost();

        // DLL管理器の複製を禁止する。
        GameModuleHost(const GameModuleHost&) = delete;
        // DLL管理器のコピー代入を禁止する。
        GameModuleHost& operator=(const GameModuleHost&) = delete;

        // 対象DLLを指定し、通常ファイルがなければ現在のDLLも解除する(modulePath: 対象DLLパス)。
        bool Load(std::filesystem::path modulePath);
        // 候補検証のfalseでは旧DLLとインスタンスを維持する。
        // 候補を検証してから旧インスタンスを停止しDLLを入れ替える。
        bool Reload();
        // 0.5秒間隔で更新時刻の変化を調べる(deltaTime: 今回の経過秒数)。
        void PollHotReload(float deltaTime);

        // DLL読み込み前に依存DLLの探索先を登録する(directories: SDKなどの配置フォルダー)。
        void SetNativeSearchDirectories(
            std::vector<std::filesystem::path> directories);

        // DLLの読み込み状態を返す。
        [[nodiscard]] bool IsLoaded() const noexcept
        {
            return m_moduleHandle != nullptr;
        }
        // 現在の読み込み要求先を参照する。
        [[nodiscard]] const std::filesystem::path&
            ModulePath() const noexcept
        {
            return m_modulePath;
        }
        // 現在のDLLから複製した表示名を参照する。
        [[nodiscard]] const std::string&
            ModuleName() const noexcept
        {
            return m_moduleName;
        }
        // 直前の読み込みエラーを参照する。
        [[nodiscard]] const std::string&
            LastError() const noexcept
        {
            return m_lastError;
        }
        // 複製して保持するスクリプト型一覧を参照する。
        [[nodiscard]] const std::vector<RegisteredNativeScript>&
            RegisteredComponents() const noexcept
        {
            return m_registeredComponents;
        }
        // DLL内の型記述参照はLoad・Reload・管理器破棄までに限る。
        // DLL内の型記述を返し見つからなければ空とする(typeName: 検索する型名)。
        [[nodiscard]] const NativeScriptTypeDescriptor*
            FindComponent(std::string_view typeName) const noexcept;
        // 複製して保持するデータ型一覧を参照する。
        [[nodiscard]] const std::vector<RegisteredDataAssetType>&
            RegisteredDataAssets() const noexcept
        {
            return m_registeredDataAssets;
        }
        // 登録済みデータ型を返し見つからなければ空とする(typeName: 検索する型名)。
        [[nodiscard]] const RegisteredDataAssetType*
            FindDataAssetType(
                std::string_view typeName) const noexcept;

        // 現在有効な管理器を返し未作成なら空とする。
        [[nodiscard]] static GameModuleHost* Current() noexcept;

        // ローカルのDLLコピー先を選ぶ(modulePath: 元のDLLパス)。
        [[nodiscard]] static std::filesystem::path
            HotReloadDirectoryFor(
                const std::filesystem::path& modulePath);

    private:
        friend class NativeScriptComponent;

        struct Candidate;

        // 重複を除いて借用インスタンスを登録する(component: 登録中に存続させる借用対象)。
        void RegisterInstance(NativeScriptComponent& component);
        // 借用インスタンスと対応する管理器参照を解除する(component: 登録解除する対象)。
        void UnregisterInstance(NativeScriptComponent& component) noexcept;
        // 更新時刻とAPI・型記述を検証して候補を読み込む(candidate: 候補DLLの資源出力)。
        [[nodiscard]] bool LoadCandidate(Candidate& candidate);
        // DLLを解放し記述参照と複製一覧・読み込み用コピーを破棄する。
        void ReleaseLoadedModule() noexcept;
        // 空でないDLLコピーを削除しファイルエラーを抑止する(path: 削除対象のコピー)。
        void CleanupShadowCopy(
            const std::filesystem::path& path) noexcept;
        // 存在する探索フォルダーをWindowsへ登録する。
        void ApplyNativeSearchDirectories();
        // Windowsへ登録した全探索先を解除する。
        void ReleaseNativeSearchDirectories() noexcept;

        // 唯一の有効なDLL管理器
        static GameModuleHost* s_current;

        // 要求された元DLLのパス
        std::filesystem::path m_modulePath;
        // 読み込み中のDLLコピー
        std::filesystem::path m_shadowPath;
        // 採用済みDLLの更新時刻
        std::filesystem::file_time_type m_loadedWriteTime{};
        // 所有する読み込み済みDLL
        void* m_moduleHandle{};
        // DLL内の型記述の借用参照
        const GameModuleDescriptor* m_descriptor{};
        // 複製したDLL表示名
        std::string m_moduleName;
        // 直前の読み込み失敗理由
        std::string m_lastError;
        // 複製したスクリプト型一覧
        std::vector<RegisteredNativeScript> m_registeredComponents;
        // 複製したデータ型一覧
        std::vector<RegisteredDataAssetType> m_registeredDataAssets;
        // 登録中の借用インスタンス
        std::vector<NativeScriptComponent*> m_instances;
        // 依存DLLの探索フォルダー
        std::vector<std::filesystem::path> m_nativeSearchDirectories;

        // 解除するWindows探索登録
        std::vector<void*> m_nativeSearchCookies;
        // 更新時刻照会までの経過秒数
        float m_pollAccumulator{};
    };
}
