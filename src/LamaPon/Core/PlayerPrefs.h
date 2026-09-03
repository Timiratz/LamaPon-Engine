#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
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

        void Load();
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
        struct Implementation;
        std::unique_ptr<Implementation> m_implementation;
    };

    [[nodiscard]] std::filesystem::path
        UserDataDirectory(std::string_view applicationName);

    // いま使えるPlayerPrefs。Applicationが起動時に自分のものを登録し、
    // C++ Script（`LamaPon::Script`のSave/Load系）がここから読み書き
    // します。**Scriptから設定値を保存できる唯一の入口**です。
    //
    // **実体はPlayerPrefs.cppに1つだけ**あります。ヘッダでinline
    // staticにするとEXE側とDLL側で別々の実体になり、保存が無言で
    // 消えます（ActivePhysicsSettingsと同じ理由）。
    //
    // Applicationを立てないツール（CLIなど）ではnullptrのままです。
    // 呼ぶ側はnullptrを許容してください（保存は無視、読み出しは
    // 既定値）。
    [[nodiscard]] PlayerPrefs* ActivePlayerPrefs() noexcept;
    void SetActivePlayerPrefs(PlayerPrefs* prefs) noexcept;
}
