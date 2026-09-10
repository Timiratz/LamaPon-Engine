#include "LamaPon/Online/WindowsOnlinePlatform.h"

#include <Windows.h>

#include <aclapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] std::vector<std::uint8_t> ReadBytes(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error("Could not read credential fixture.");
        }
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    void OverwriteBytes(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes)
    {
        const HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_HIDDEN,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Could not alter credential fixture.");
        }
        LARGE_INTEGER beginning{};
        DWORD written{};
        const bool succeeded = SetFilePointerEx(
                file,
                beginning,
                nullptr,
                FILE_BEGIN) != FALSE
            && (bytes.empty()
                || (WriteFile(
                        file,
                        bytes.data(),
                        static_cast<DWORD>(bytes.size()),
                        &written,
                        nullptr) != FALSE
                    && written == bytes.size()))
            && SetEndOfFile(file) != FALSE
            && FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
        if (!succeeded)
        {
            throw std::runtime_error("Could not alter credential fixture.");
        }
    }

    void RequireRestrictedAcl(const std::filesystem::path& path)
    {
        PACL acl{};
        PSECURITY_DESCRIPTOR descriptor{};
        const DWORD result = GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            &acl,
            nullptr,
            &descriptor);
        if (result != ERROR_SUCCESS || descriptor == nullptr || acl == nullptr)
        {
            if (descriptor != nullptr)
            {
                LocalFree(descriptor);
            }
            throw std::runtime_error("Could not inspect credential ACL.");
        }

        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision{};
        bool restricted = GetSecurityDescriptorControl(
                descriptor,
                &control,
                &revision) != FALSE
            && (control & SE_DACL_PROTECTED) != 0;
        ACL_SIZE_INFORMATION information{};
        restricted = restricted
            && GetAclInformation(
                acl,
                &information,
                sizeof(information),
                AclSizeInformation) != FALSE;
        for (DWORD index = 0;
            restricted && index < information.AceCount;
            ++index)
        {
            void* rawAce{};
            if (GetAce(acl, index, &rawAce) == FALSE)
            {
                restricted = false;
                break;
            }
            const auto* header = static_cast<ACE_HEADER*>(rawAce);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            {
                restricted = false;
                break;
            }
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
            PSID sid = &ace->SidStart;
            if (IsWellKnownSid(sid, WinWorldSid)
                || IsWellKnownSid(sid, WinBuiltinUsersSid)
                || IsWellKnownSid(sid, WinAuthenticatedUserSid))
            {
                restricted = false;
            }
        }
        LocalFree(descriptor);
        Require(
            restricted,
            "Credential file ACL allowed a broad Windows principal.");
    }

    [[nodiscard]] std::filesystem::path FactoryCredentialPath(
        const std::string& gameId,
        const std::string& environmentId)
    {
        auto store = LamaPon::Detail::MakeWindowsRefreshTokenStore(
            gameId,
            environmentId);
        const auto* windowsStore = dynamic_cast<
            LamaPon::Detail::WindowsRefreshTokenStore*>(store.get());
        Require(
            windowsStore != nullptr && !windowsStore->FilePath().empty(),
            "The Windows credential factory did not resolve LOCALAPPDATA.");
        return windowsStore->FilePath();
    }

    [[nodiscard]] bool IsLowerHexPathComponent(
        const std::filesystem::path& path)
    {
        const auto value = path.filename().wstring();
        return value.size() == 64
            && std::ranges::all_of(
                value,
                [](const wchar_t character)
                {
                    return (character >= L'0' && character <= L'9')
                        || (character >= L'a' && character <= L'f');
                });
    }
}

int main()
{
    const auto directory =
        std::filesystem::current_path()
        / "test-output"
        / "online-credentials";
    try
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(directory, cleanupError);
        Require(
            !cleanupError,
            "Could not prepare online credential test output.");

        const std::pair<std::string, std::string> pathNamespaces[] = {
            { ".", "production" },
            { "..", "production" },
            { "game", "production" },
            { "Game", "production" },
            { "game.", "production" },
            { "CON", "production" },
            { "game", "." },
            { "game", ".." },
            { "game", "Production" },
            { "game", "production." },
            { "game", "CON" }
        };
        std::vector<std::filesystem::path> factoryPaths;
        for (const auto& [gameId, environmentId] : pathNamespaces)
        {
            const auto path = FactoryCredentialPath(
                gameId,
                environmentId);
            Require(
                path.filename() == L"session.bin"
                    && path.parent_path().parent_path().filename()
                        == L"Online"
                    && IsLowerHexPathComponent(path.parent_path()),
                "A factory credential path did not use one lower-hex hash component.");
            Require(
                std::ranges::find(factoryPaths, path)
                    == factoryPaths.end(),
                "Distinct credential identifiers aliased to one Windows path.");
            factoryPaths.push_back(path);
        }
        Require(
            FactoryCredentialPath("game", "production")
                == FactoryCredentialPath("game", "production"),
            "A credential namespace did not produce a stable path hash.");

        const auto credentialPath = directory / "session.bin";
        constexpr std::string_view refreshToken =
            "refresh-secret-that-must-never-appear-in-the-file";
        LamaPon::Detail::WindowsRefreshTokenStore store(
            credentialPath,
            "game-41c81960",
            "production");
        Require(
            store.Load().status
                == LamaPon::Detail::RefreshTokenLoadStatus::NotFound,
            "A missing credential was not reported as NotFound.");

        const auto saved = store.Save(refreshToken);
        Require(
            saved.succeeded
                && std::filesystem::is_regular_file(credentialPath)
                && !std::filesystem::exists(
                    credentialPath.wstring() + L".tmp"),
            "The refresh token was not saved atomically.");
        RequireRestrictedAcl(credentialPath);
        RequireRestrictedAcl(credentialPath.parent_path());

        const auto originalBytes = ReadBytes(credentialPath);
        Require(
            !originalBytes.empty()
                && std::search(
                    originalBytes.begin(),
                    originalBytes.end(),
                    refreshToken.begin(),
                    refreshToken.end()) == originalBytes.end(),
            "The refresh token appeared in the stored DPAPI envelope.");

        const auto loaded = store.Load();
        Require(
            loaded.Loaded()
                && loaded.refreshToken == refreshToken,
            "The DPAPI-protected refresh token did not round-trip.");

        LamaPon::Detail::WindowsRefreshTokenStore reopened(
            credentialPath,
            "game-41c81960",
            "production");
        Require(
            reopened.Load().refreshToken == refreshToken,
            "A recreated credential store could not restore the token.");

        LamaPon::Detail::WindowsRefreshTokenStore wrongGame(
            credentialPath,
            "another-game",
            "production");
        const auto wrongEntropy = wrongGame.Load();
        Require(
            wrongEntropy.status
                    == LamaPon::Detail::RefreshTokenLoadStatus::Corrupt
                && wrongEntropy.refreshToken.empty()
                && wrongEntropy.errorMessage.find(refreshToken)
                    == std::string::npos,
            "A token was accepted with a different game entropy.");

        LamaPon::Detail::WindowsRefreshTokenStore wrongEnvironment(
            credentialPath,
            "game-41c81960",
            "staging");
        Require(
            wrongEnvironment.Load().status
                == LamaPon::Detail::RefreshTokenLoadStatus::Corrupt,
            "A token was accepted with different environment entropy.");

        auto tampered = originalBytes;
        tampered.back() ^= 0x5au;
        OverwriteBytes(credentialPath, tampered);
        const auto tamperedResult = store.Load();
        Require(
            tamperedResult.status
                    == LamaPon::Detail::RefreshTokenLoadStatus::Corrupt
                && tamperedResult.refreshToken.empty(),
            "A modified DPAPI blob was accepted.");

        OverwriteBytes(
            credentialPath,
            std::vector<std::uint8_t>(
                originalBytes.begin(),
                originalBytes.begin() + 8));
        Require(
            store.Load().status
                == LamaPon::Detail::RefreshTokenLoadStatus::Corrupt,
            "A truncated credential envelope was accepted.");

        OverwriteBytes(credentialPath, originalBytes);
        const std::string oversizedToken(8193, 'x');
        Require(
            !store.Save(oversizedToken).succeeded
                && store.Load().refreshToken == refreshToken,
            "An oversized token replaced a valid credential.");

        auto temporaryPath = credentialPath;
        temporaryPath += L".tmp";
        {
            std::ofstream leftover(
                temporaryPath,
                std::ios::binary | std::ios::trunc);
            leftover << "protected-blob-residue";
            Require(
                static_cast<bool>(leftover),
                "Could not create a credential temporary-file fixture.");
        }
        Require(
            store.Delete().succeeded
                && !std::filesystem::exists(credentialPath)
                && !std::filesystem::exists(temporaryPath)
                && store.Delete().succeeded,
            "Credential deletion did not remove final and temporary files.");

        Require(
            store.Save(refreshToken).succeeded,
            "Could not prepare a partial-delete fixture.");
        Require(
            std::filesystem::create_directory(
                temporaryPath,
                cleanupError)
                && !cleanupError,
            "Could not create an undeletable temporary-path fixture.");
        const auto partialDelete = store.Delete();
        Require(
            !partialDelete.succeeded
                && !std::filesystem::exists(credentialPath)
                && std::filesystem::is_directory(temporaryPath),
            "A temporary-file deletion failure was hidden after deleting final.");
        std::filesystem::remove(temporaryPath, cleanupError);
        Require(
            !cleanupError
                && store.Delete().succeeded
                && store.Load().status
                    == LamaPon::Detail::RefreshTokenLoadStatus::NotFound,
            "Credential deletion did not recover after temporary cleanup.");

        bool invalidIdentifierRejected{};
        try
        {
            LamaPon::Detail::WindowsRefreshTokenStore invalid(
                credentialPath,
                "../escape",
                "production");
        }
        catch (const std::invalid_argument&)
        {
            invalidIdentifierRejected = true;
        }
        Require(
            invalidIdentifierRejected,
            "A path-like game identifier was accepted.");

        std::size_t shellCalls{};
        std::wstring openedOperation;
        std::wstring openedUrl;
        void* openedOwner{};
        bool nullParameters{};
        bool nullDirectory{};
        int openedShowCommand{};
        std::intptr_t shellResult{ 33 };
        auto shellOpen = [&](
            void* owner,
            const wchar_t* operation,
            const wchar_t* file,
            const wchar_t* parameters,
            const wchar_t* workingDirectory,
            const int showCommand)
        {
            ++shellCalls;
            openedOwner = owner;
            openedOperation = operation == nullptr ? L"" : operation;
            openedUrl = file == nullptr ? L"" : file;
            nullParameters = parameters == nullptr;
            nullDirectory = workingDirectory == nullptr;
            openedShowCommand = showCommand;
            return shellResult;
        };
        void* const owner = reinterpret_cast<void*>(0x1234);
        LamaPon::Detail::WindowsAuthorizationLauncher launcher(
            owner,
            shellOpen);
        constexpr std::string_view authorizationUrl =
            "https://discord.com/oauth2/authorize?client_id=42&state=super-secret-state";
        Require(
            launcher.Launch(authorizationUrl, false).succeeded
                && shellCalls == 1
                && openedOwner == owner
                && openedOperation == L"open"
                && openedUrl
                    == L"https://discord.com/oauth2/authorize?client_id=42&state=super-secret-state"
                && nullParameters
                && nullDirectory
                && openedShowCommand == SW_SHOWNORMAL,
            "A safe authorization URL was not opened as a URL-only target.");

        for (const std::string_view unsafeUrl : {
                "http://discord.com/oauth2/authorize",
                "file:///C:/Windows/System32/calc.exe",
                "javascript:alert(1)",
                "https://user:password@discord.com/oauth2/authorize",
                "https://@discord.com/oauth2/authorize",
                "https://discord.com/oauth2/authorize#secret",
                "https://discord.com/\\evil",
                "https://discord.com/oauth2/authorize\nX-Test: value",
                "http://localhost.example.com/oauth2/authorize" })
        {
            Require(
                !launcher.Launch(unsafeUrl, false).succeeded,
                "An unsafe authorization URL reached ShellExecute.");
        }
        Require(
            shellCalls == 1,
            "ShellExecute was called for a rejected authorization URL.");

        Require(
            launcher.Launch(
                "http://127.0.0.1:8123/oauth/callback?state=dev",
                true).succeeded
                && shellCalls == 2,
            "An explicitly enabled loopback URL was rejected.");
        Require(
            !launcher.Launch(
                "http://localhost.example.com/oauth/callback",
                true).succeeded
                && shellCalls == 2,
            "A remote host disguised as loopback was accepted.");

        shellResult = 31;
        const auto launchFailure = launcher.Launch(
            authorizationUrl,
            false);
        Require(
            !launchFailure.succeeded
                && launchFailure.errorCode
                    == "authorization_launch_failed"
                && launchFailure.errorMessage.find("super-secret-state")
                    == std::string::npos,
            "A ShellExecute failure leaked the authorization URL.");

        std::filesystem::remove_all(directory, cleanupError);
        Require(
            !cleanupError,
            "Could not clean online credential test output.");
        std::cout
            << "Windows online platform tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
