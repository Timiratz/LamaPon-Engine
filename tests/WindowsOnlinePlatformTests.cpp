#include "LamaPon/Online/WindowsOnlinePlatform.h"

#include <Windows.h>

#include <aclapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace
{
    using namespace std::chrono_literals;

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct FileHandle final
    {
        HANDLE value{ INVALID_HANDLE_VALUE };

        ~FileHandle()
        {
            Close();
        }

        void Close() noexcept
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
                value = INVALID_HANDLE_VALUE;
            }
        }
    };

    struct SavePause final
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered{};
        bool released{};
    };

    void PauseCredentialSaveBeforeReplace(void* const context) noexcept
    {
        try
        {
            auto& pause = *static_cast<SavePause*>(context);
            std::unique_lock lock(pause.mutex);
            if (pause.entered)
            {
                return;
            }
            pause.entered = true;
            pause.condition.notify_all();
            pause.condition.wait(
                lock,
                [&pause]
                {
                    return pause.released;
                });
        }
        catch (...)
        {
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
        constexpr std::string_view candidateToken =
            "candidate-refresh-token-must-not-be-committed";
        const auto requireOldCredential = [&](const char* message)
        {
            // 新しいinstanceで読み直し、process再起動後もcandidateでは
            // なく直前のcommitted値だけが見えることを確認します。
            LamaPon::Detail::WindowsRefreshTokenStore afterRestart(
                credentialPath,
                "game-41c81960",
                "production");
            const auto afterFailure = afterRestart.Load();
            Require(
                afterFailure.Loaded()
                    && afterFailure.refreshToken == refreshToken
                    && afterFailure.refreshToken != candidateToken
                    && !std::filesystem::exists(
                        credentialPath.wstring() + L".tmp"),
                message);
        };

        Require(
            !store.Save({}).succeeded,
            "An empty refresh token was accepted.");
        requireOldCredential(
            "Invalid Save changed the committed credential.");
        const std::string oversizedToken(8193, 'x');
        Require(
            !store.Save(oversizedToken).succeeded,
            "An oversized token replaced a valid credential.");
        requireOldCredential(
            "Oversized Save changed the committed credential.");

        for (const auto [failPoint, message] : {
                std::pair{
                    LamaPon::Detail::
                        WindowsRefreshTokenSaveTestFailPoint::Protection,
                    "Protection failure changed the committed credential." },
                std::pair{
                    LamaPon::Detail::
                        WindowsRefreshTokenSaveTestFailPoint::TemporaryWrite,
                    "Temporary-write failure changed the committed credential." },
                std::pair{
                    LamaPon::Detail::
                        WindowsRefreshTokenSaveTestFailPoint::TemporaryAcl,
                    "Temporary ACL failure changed the committed credential." },
                std::pair{
                    LamaPon::Detail::
                        WindowsRefreshTokenSaveTestFailPoint::Replace,
                    "Atomic-replace failure changed the committed credential." }
            })
        {
            LamaPon::Detail::SetWindowsRefreshTokenSaveTestFailPoint(
                failPoint);
            const auto failed = store.Save(candidateToken);
            LamaPon::Detail::SetWindowsRefreshTokenSaveTestFailPoint(
                LamaPon::Detail::
                    WindowsRefreshTokenSaveTestFailPoint::None);
            Require(!failed.succeeded, message);
            requireOldCredential(message);
        }

        auto lockPath = credentialPath;
        lockPath += L".lock";
        std::error_code fixtureError;
        constexpr DWORD AllowUnprivilegedCreate = 0x2u;

        auto usageLeasePath = credentialPath;
        usageLeasePath += L".session.lock";
        Require(
            store.AcquireUsageLease().succeeded
                && store.AcquireUsageLease().succeeded,
            "A credential usage lease was not idempotent.");
        LamaPon::Detail::WindowsRefreshTokenStore competingUsageStore(
            credentialPath,
            "game-41c81960",
            "production");
        Require(
            !competingUsageStore.AcquireUsageLease().succeeded
                && competingUsageStore.Load().refreshToken
                    == refreshToken,
            "Two stores acquired one usage lease or raw Load was changed.");
        store.ReleaseUsageLease();
        Require(
            competingUsageStore.AcquireUsageLease().succeeded,
            "A usage lease remained busy after explicit release.");
        competingUsageStore.ReleaseUsageLease();
        RequireRestrictedAcl(usageLeasePath);

        LamaPon::Detail::WindowsRefreshTokenStore destructorWaiter(
            credentialPath,
            "game-41c81960",
            "production");
        {
            LamaPon::Detail::WindowsRefreshTokenStore scopedUsageStore(
                credentialPath,
                "game-41c81960",
                "production");
            Require(
                scopedUsageStore.AcquireUsageLease().succeeded
                    && !destructorWaiter.AcquireUsageLease().succeeded,
                "A scoped usage lease did not exclude another store.");
        }
        Require(
            destructorWaiter.AcquireUsageLease().succeeded,
            "A store destructor did not release its usage lease.");
        destructorWaiter.ReleaseUsageLease();

        std::filesystem::remove(usageLeasePath, fixtureError);
        Require(
            !fixtureError,
            "Could not replace the credential usage-lock fixture.");
        const auto usageLockVictim =
            directory / L"usage-lock-hardlink-victim.bin";
        {
            std::ofstream output(
                usageLockVictim,
                std::ios::binary | std::ios::trunc);
            output << "usage-lock-victim-must-not-change";
            Require(
                static_cast<bool>(output),
                "Could not create the usage-lock victim.");
        }
        const auto usageLockVictimBytes = ReadBytes(usageLockVictim);
        Require(
            CreateHardLinkW(
                usageLeasePath.c_str(),
                usageLockVictim.c_str(),
                nullptr) != FALSE,
            "Could not create the usage-lock hard-link fixture.");
        Require(
            !store.AcquireUsageLease().succeeded
                && ReadBytes(usageLockVictim) == usageLockVictimBytes,
            "A hard-linked credential usage lock was accepted or modified.");
        std::filesystem::remove(usageLeasePath, fixtureError);
        Require(
            !fixtureError,
            "Could not remove the unsafe usage-lock fixture.");
        std::filesystem::remove(usageLockVictim, fixtureError);
        Require(
            !fixtureError,
            "Could not remove the usage-lock victim fixture.");

        const auto usageReparseVictim =
            directory / L"usage-lock-reparse-victim.bin";
        {
            std::ofstream output(
                usageReparseVictim,
                std::ios::binary | std::ios::trunc);
            output << "usage-reparse-victim-must-not-change";
            Require(
                static_cast<bool>(output),
                "Could not create the usage-lock reparse victim.");
        }
        const auto usageReparseVictimBytes =
            ReadBytes(usageReparseVictim);
        if (CreateSymbolicLinkW(
                usageLeasePath.c_str(),
                usageReparseVictim.c_str(),
                AllowUnprivilegedCreate) != FALSE)
        {
            Require(
                !store.AcquireUsageLease().succeeded
                    && ReadBytes(usageReparseVictim)
                        == usageReparseVictimBytes,
                "A reparse-point credential usage lock was accepted or modified.");
            std::filesystem::remove(usageLeasePath, fixtureError);
            Require(
                !fixtureError,
                "Could not remove the usage-lock reparse fixture.");
        }
        std::filesystem::remove(usageReparseVictim, fixtureError);
        Require(
            !fixtureError
                && store.AcquireUsageLease().succeeded,
            "A usage lease did not recover after an unsafe lock was removed.");
        store.ReleaseUsageLease();

        std::filesystem::remove(lockPath, fixtureError);
        Require(
            !fixtureError,
            "Could not replace the credential lock fixture.");
        const auto lockVictim = directory / L"lock-hardlink-victim.bin";
        {
            std::ofstream output(
                lockVictim,
                std::ios::binary | std::ios::trunc);
            output << "lock-victim-must-not-change";
            Require(
                static_cast<bool>(output),
                "Could not create the credential lock victim.");
        }
        const auto lockVictimBytes = ReadBytes(lockVictim);
        Require(
            CreateHardLinkW(
                lockPath.c_str(),
                lockVictim.c_str(),
                nullptr) != FALSE,
            "Could not create the credential lock hard-link fixture.");
        const auto unsafeLockLoad = store.Load();
        Require(
            unsafeLockLoad.status
                    == LamaPon::Detail::RefreshTokenLoadStatus::Unavailable
                && ReadBytes(lockVictim) == lockVictimBytes,
            "A hard-linked credential lock was accepted or modified.");
        std::filesystem::remove(lockPath, fixtureError);
        Require(!fixtureError, "Could not remove the unsafe lock fixture.");
        std::filesystem::remove(lockVictim, fixtureError);
        Require(!fixtureError, "Could not remove the lock victim fixture.");
        Require(
            store.Load().refreshToken == refreshToken,
            "Credential Load did not recover after removing an unsafe lock.");
        Require(
            std::filesystem::is_regular_file(lockPath),
            "Credential operations did not create a persistent lock file.");
        RequireRestrictedAcl(lockPath);
        FileHandle externalLock;
        externalLock.value = CreateFileW(
            lockPath.c_str(),
            GENERIC_READ,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        Require(
            externalLock.value != INVALID_HANDLE_VALUE,
            "Could not hold the credential operation lock.");
        LamaPon::Detail::WindowsRefreshTokenStore blockedStore(
            credentialPath,
            "game-41c81960",
            "production");
        const auto blockedSave = blockedStore.Save(candidateToken);
        const auto blockedLoad = blockedStore.Load();
        const auto blockedDelete = blockedStore.Delete();
        const auto bytesWhileBlocked = ReadBytes(credentialPath);
        externalLock.Close();
        Require(
            !blockedSave.succeeded
                && blockedLoad.status
                    == LamaPon::Detail::RefreshTokenLoadStatus::Unavailable
                && !blockedDelete.succeeded
                && bytesWhileBlocked == originalBytes,
            "A busy credential lock exposed or changed the final token.");

        const auto recoveredLoad = blockedStore.Load();
        Require(
            recoveredLoad.Loaded()
                && recoveredLoad.refreshToken == refreshToken
                && blockedStore.Save(candidateToken).succeeded
                && blockedStore.Load().refreshToken == candidateToken
                && blockedStore.Delete().succeeded
                && blockedStore.Load().status
                    == LamaPon::Detail::RefreshTokenLoadStatus::NotFound
                && blockedStore.Save(refreshToken).succeeded,
            "Credential operations did not recover after releasing the lock.");

        SavePause pause;
        LamaPon::Detail::SetWindowsRefreshTokenSaveBeforeReplaceHook(
            &PauseCredentialSaveBeforeReplace,
            &pause);
        LamaPon::Detail::WindowsRefreshTokenStore firstConcurrentStore(
            credentialPath,
            "game-41c81960",
            "production");
        LamaPon::Detail::WindowsRefreshTokenStore secondConcurrentStore(
            credentialPath,
            "game-41c81960",
            "production");
        LamaPon::Detail::OnlinePlatformResult firstConcurrentResult;
        std::thread firstSave(
            [&]
            {
                try
                {
                    firstConcurrentResult = firstConcurrentStore.Save(
                        "first-concurrent-candidate");
                }
                catch (...)
                {
                }
            });
        bool firstReachedReplace{};
        {
            std::unique_lock lock(pause.mutex);
            firstReachedReplace = pause.condition.wait_for(
                lock,
                3s,
                [&pause]
                {
                    return pause.entered;
                });
        }
        LamaPon::Detail::OnlinePlatformResult secondConcurrentResult;
        if (firstReachedReplace)
        {
            secondConcurrentResult = secondConcurrentStore.Save(
                "second-concurrent-candidate");
        }
        {
            std::scoped_lock lock(pause.mutex);
            pause.released = true;
        }
        pause.condition.notify_all();
        firstSave.join();
        LamaPon::Detail::SetWindowsRefreshTokenSaveBeforeReplaceHook(
            nullptr,
            nullptr);
        const auto concurrentFinal = store.Load();
        Require(
            firstReachedReplace
                && firstConcurrentResult.succeeded
                && !secondConcurrentResult.succeeded
                && concurrentFinal.Loaded()
                && concurrentFinal.refreshToken
                    == "first-concurrent-candidate"
                && store.Save(refreshToken).succeeded,
            "Concurrent credential stores committed another writer's candidate.");

        auto temporaryPath = credentialPath;
        temporaryPath += L".tmp";
        const auto temporaryVictim =
            directory / L"temporary-hardlink-victim.bin";
        {
            std::ofstream output(
                temporaryVictim,
                std::ios::binary | std::ios::trunc);
            output << "temporary-victim-must-not-change";
            Require(
                static_cast<bool>(output),
                "Could not create the credential temporary victim.");
        }
        const auto temporaryVictimBytes = ReadBytes(temporaryVictim);
        Require(
            CreateHardLinkW(
                temporaryPath.c_str(),
                temporaryVictim.c_str(),
                nullptr) != FALSE,
            "Could not create the credential temporary hard-link fixture.");
        Require(
            !store.Save(candidateToken).succeeded
                && ReadBytes(temporaryVictim) == temporaryVictimBytes
                && store.Load().refreshToken == refreshToken,
            "A hard-linked credential temporary file was followed or modified.");
        std::filesystem::remove(temporaryPath, fixtureError);
        Require(
            !fixtureError,
            "Could not remove the unsafe temporary fixture.");
        std::filesystem::remove(temporaryVictim, fixtureError);
        Require(
            !fixtureError,
            "Could not remove the temporary victim fixture.");

        const auto reparseTarget = directory / L"reparse-target";
        const auto reparseParent = directory / L"reparse-parent";
        std::filesystem::create_directories(reparseTarget, fixtureError);
        Require(
            !fixtureError,
            "Could not create the credential reparse target.");
        LamaPon::Detail::WindowsRefreshTokenStore realParentStore(
            reparseTarget / L"nested" / L"session.bin",
            "game-41c81960",
            "production");
        Require(
            realParentStore.Save(refreshToken).succeeded,
            "Could not prepare the credential reparse target.");
        if (CreateSymbolicLinkW(
                reparseParent.c_str(),
                reparseTarget.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY
                    | AllowUnprivilegedCreate) != FALSE)
        {
            LamaPon::Detail::WindowsRefreshTokenStore reparseStore(
                reparseParent / L"nested" / L"session.bin",
                "game-41c81960",
                "production");
            Require(
                reparseStore.Load().status
                        == LamaPon::Detail::RefreshTokenLoadStatus::Unavailable
                    && !reparseStore.Save(candidateToken).succeeded
                    && !reparseStore.Delete().succeeded
                    && !reparseStore.AcquireUsageLease().succeeded
                    && realParentStore.Load().refreshToken == refreshToken,
                "A reparse-point credential parent escaped path isolation.");
            std::filesystem::remove(reparseParent, fixtureError);
            Require(
                !fixtureError,
                "Could not remove the credential reparse fixture.");
        }
        std::filesystem::remove_all(reparseTarget, fixtureError);
        Require(
            !fixtureError,
            "Could not remove the credential reparse target.");

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
