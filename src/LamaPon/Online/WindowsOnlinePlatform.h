#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon::Detail
{
    enum class RefreshTokenLoadStatus : std::uint8_t
    {
        NotFound,
        Loaded,
        Unavailable,
        Corrupt
    };

    struct RefreshTokenLoadResult final
    {
        RefreshTokenLoadStatus status{
            RefreshTokenLoadStatus::Unavailable
        };
        std::string refreshToken;
        std::string errorCode;
        std::string errorMessage;

        [[nodiscard]] bool Loaded() const noexcept
        {
            return status == RefreshTokenLoadStatus::Loaded;
        }
    };

    struct OnlinePlatformResult final
    {
        bool succeeded{};
        std::string errorCode;
        std::string errorMessage;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return succeeded;
        }
    };

    class IRefreshTokenStore
    {
    public:
        virtual ~IRefreshTokenStore() = default;

        [[nodiscard]] virtual RefreshTokenLoadResult Load() = 0;
        [[nodiscard]] virtual OnlinePlatformResult Save(
            std::string_view refreshToken) = 0;
        [[nodiscard]] virtual OnlinePlatformResult Delete() = 0;
    };

    class IAuthorizationLauncher
    {
    public:
        virtual ~IAuthorizationLauncher() = default;

        [[nodiscard]] virtual OnlinePlatformResult Launch(
            std::string_view authorizationUrl,
            bool allowInsecureLoopback) = 0;
    };

    // テスト時は保存先をtest-outputへ固定して使います。本番は下の
    // factoryから生成し、資格情報をTEMPへフォールバックさせません。
    class WindowsRefreshTokenStore final : public IRefreshTokenStore
    {
    public:
        WindowsRefreshTokenStore(
            std::filesystem::path filePath,
            std::string gameId,
            std::string environmentId);

        [[nodiscard]] RefreshTokenLoadResult Load() override;
        [[nodiscard]] OnlinePlatformResult Save(
            std::string_view refreshToken) override;
        [[nodiscard]] OnlinePlatformResult Delete() override;

        [[nodiscard]] const std::filesystem::path& FilePath() const noexcept
        {
            return m_filePath;
        }

    private:
        std::filesystem::path m_filePath;
        std::vector<std::uint8_t> m_entropy;
        bool m_storageAvailable{};
    };

    // ShellExecuteWを直接呼ばずに検証できるよう、Windows APIと同じ
    // 引数を受ける境界を内部テストへ公開します。
    using ShellOpenFunction = std::function<std::intptr_t(
        void* ownerWindow,
        const wchar_t* operation,
        const wchar_t* file,
        const wchar_t* parameters,
        const wchar_t* directory,
        int showCommand)>;

    class WindowsAuthorizationLauncher final
        : public IAuthorizationLauncher
    {
    public:
        explicit WindowsAuthorizationLauncher(
            void* ownerWindow = nullptr,
            ShellOpenFunction shellOpen = {});

        [[nodiscard]] OnlinePlatformResult Launch(
            std::string_view authorizationUrl,
            bool allowInsecureLoopback) override;

    private:
        void* m_ownerWindow{};
        ShellOpenFunction m_shellOpen;
    };

    [[nodiscard]] std::unique_ptr<IRefreshTokenStore>
        MakeWindowsRefreshTokenStore(
            std::string gameId,
            std::string environmentId = "production");
    [[nodiscard]] std::unique_ptr<IAuthorizationLauncher>
        MakeWindowsAuthorizationLauncher(
            void* ownerWindow = nullptr);
}
