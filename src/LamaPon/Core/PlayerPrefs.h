#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class PersistenceProfiles;
    namespace Detail
    {
        class LocalPersistenceDocuments;
        class OnlinePersistenceCoordinator;
    }

    enum class PlayerPrefType
    {
        Integer,
        Number,
        Boolean,
        String
    };

    class PlayerPrefs final
    {
    public:
        explicit PlayerPrefs(std::filesystem::path filePath);
        ~PlayerPrefs();

        PlayerPrefs(const PlayerPrefs&) = delete;
        PlayerPrefs& operator=(const PlayerPrefs&) = delete;

        // 現在のファイルを読み直します。読み込みに失敗した場合は、
        // ファイルパス・値・dirty状態を保ち、load failureだけを記録
        // して元ファイルを保護します。
        void Load();
        void Reload();

        // Load/Reloadで現在のファイルを読めなかった後は、破損・未来
        // version・ACLエラー等の元ファイルを自動保存で上書きしない
        // ようSave()をfail-closedにします。外部でファイルを修復した
        // 後はRecoverAfterLoadFailure()、内容を明示的に破棄するときは
        // ResetAfterLoadFailure()を呼んで解除してください。
        [[nodiscard]] bool HasLoadFailure() const noexcept;
        void RecoverAfterLoadFailure();
        void ResetAfterLoadFailure();

        // 保存先を切り替え、新しいファイルの値を読み込みます。
        // 未保存の変更がある間は切り替えを拒否します。先にSave()か
        // Reload()を明示的に呼んでください。読み込み失敗時は現在の
        // プロファイルを保つため、アカウント切り替えにも使えます。
        void Rebind(std::filesystem::path filePath);
        void Save();
        [[nodiscard]] bool IsDirty() const noexcept;
        [[nodiscard]] const std::filesystem::path&
            FilePath() const noexcept;

        [[nodiscard]] bool HasKey(std::string_view key) const;
        [[nodiscard]] std::vector<std::string> Keys() const;
        [[nodiscard]] PlayerPrefType TypeOf(std::string_view key) const;
        void DeleteKey(std::string_view key);
        void DeleteAll() noexcept;

        void SetInteger(std::string key, std::int64_t value);
        [[nodiscard]] std::int64_t GetInteger(
            std::string_view key,
            std::int64_t defaultValue = 0) const;
        void SetNumber(std::string key, double value);
        [[nodiscard]] double GetNumber(
            std::string_view key,
            double defaultValue = 0.0) const;
        void SetBoolean(std::string key, bool value);
        [[nodiscard]] bool GetBoolean(
            std::string_view key,
            bool defaultValue = false) const;
        void SetString(std::string key, std::string value);
        [[nodiscard]] std::string GetString(
            std::string_view key,
            std::string defaultValue = {}) const;

        [[nodiscard]] std::string SerializeToJson() const;

    private:
        friend class PersistenceProfiles;
        friend class Detail::LocalPersistenceDocuments;
        friend class Detail::OnlinePersistenceCoordinator;

        // PersistenceProfilesが、検証済みの状態をfinal rename後に
        // 例外なしで公開するための内部トランザクション操作です。
        void RelocateBinding(
            std::filesystem::path filePath) noexcept;
        void SwapLoadedState(PlayerPrefs& other) noexcept;
        [[nodiscard]] bool AcquireBindingLease(
            const void* owner) noexcept;
        [[nodiscard]] bool ReleaseBindingLease(
            const void* owner) noexcept;
        [[nodiscard]] bool IsBindingLeased() const noexcept;
        [[nodiscard]] bool BindingLeaseOwnedBy(
            const void* owner) const noexcept;
        // LocalPersistenceDocumentsがsecure/strict readerの同一snapshotを
        // disk再openなしでprepared stateへ変換する内部seamです。
        void LoadValidatedSnapshot(
            std::string_view fullDocument,
            bool missing);
        void ApplyRemoteDocumentAtomically(std::string_view fullDocument);
        void DeleteRemoteDocumentAtomically();

        struct Implementation;
        std::unique_ptr<Implementation> m_implementation;
    };

    [[nodiscard]] std::filesystem::path
        UserDataDirectory(std::string_view applicationName);

    // いま使えるPlayerPrefs。Applicationが起動時に自分のものを登録し、
    // C++ Script（LamaPon::ScriptのSave/Load系）がここから読み書き
    // します。Scriptから設定値を保存できる唯一の入口です。
    //
    // 実体はPlayerPrefs.cppに1つだけあります。ヘッダでinline
    // staticにするとEXE側とDLL側で別々の実体になり、保存が無言で
    // 消えます（ActivePhysicsSettingsと同じ理由）。
    //
    // Applicationを立てないツール（CLIなど）ではnullptrのままです。
    // 呼ぶ側はnullptrを許容してください（保存は無視、読み出しは
    // 既定値）。
    [[nodiscard]] PlayerPrefs* ActivePlayerPrefs() noexcept;
    void SetActivePlayerPrefs(PlayerPrefs* prefs) noexcept;
}
