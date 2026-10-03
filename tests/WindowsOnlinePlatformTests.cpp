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

    // Require(condition: 判定結果, message: 失敗理由) は失敗時に例外を送出する。
    void Require(const bool condition, const char* message)
    {
        // 条件不成立ならテストを失敗させる。
        if (!condition)
        {
            // 失敗理由を例外で呼び出し元へ伝える。
            throw std::runtime_error(message);
        }
    }

    struct FileHandle final
    {
        // Windows APIのハンドル
        HANDLE value{ INVALID_HANDLE_VALUE };

        // ~FileHandle() は保持中のOSハンドルを解放する。
        ~FileHandle()
        {
            Close();
        }

        // Close() は有効なOSハンドルを閉じる。
        void Close() noexcept
        {
            // 有効なハンドルだけを解放する。
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
                value = INVALID_HANDLE_VALUE;
            }
        }
    };

    struct SavePause final
    {
        // 待機状態の排他制御
        std::mutex mutex;
        // 待機と再開の通知
        std::condition_variable condition;
        // 停止地点への到達状態
        bool entered{};
        // 保存処理の再開状態
        bool released{};
    };

    // PauseCredentialSaveBeforeReplace(context: 保存停止状態) は置換前の保存を待機させる。
    void PauseCredentialSaveBeforeReplace(void* const context) noexcept
    {
        // 保存フックの例外を封じて停止処理を行う。
        try
        {
            // 保存フックの共有状態
            auto& pause = *static_cast<SavePause*>(context);
            // 停止状態の排他ロック
            std::unique_lock lock(pause.mutex);
            // 停止済みなら二重に待機しない。
            if (pause.entered)
            {
                // 二重到達時は処理を終了する。
                return;
            }
            pause.entered = true;
            pause.condition.notify_all();
            pause.condition.wait(
                lock,
                [&pause]
                {
                    // 再開通知を待つ条件
                    return pause.released;
                });
        }
        // noexcept契約を保つため例外を抑止する。
        catch (...)
        {
        }
    }

    // ReadBytes(path: 読み取り対象) はファイル全体をバイト列で返す。
    [[nodiscard]] std::vector<std::uint8_t> ReadBytes(
        const std::filesystem::path& path)
    {
        // バイナリ入力ストリーム
        std::ifstream input(path, std::ios::binary);
        // 入力を開けなければテストを失敗させる。
        if (!input)
        {
            // 読込失敗を呼び出し元へ伝える。
            throw std::runtime_error("Could not read credential fixture.");
        }
        // 読み取った全バイトを返す。
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    // OverwriteBytes(path: 上書き対象, bytes: 新しい内容) は既存ファイルを書き換える。
    void OverwriteBytes(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes)
    {
        // 上書き先ハンドル
        const HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_HIDDEN,
            nullptr);
        // ファイルを開けなければ失敗とする。
        if (file == INVALID_HANDLE_VALUE)
        {
            // 上書き失敗を呼び出し元へ伝える。
            throw std::runtime_error("Could not alter credential fixture.");
        }
        // 書込開始位置
        LARGE_INTEGER beginning{};
        // 実際に書き込んだバイト数
        DWORD written{};
        // 上書き処理の成否
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
        // 上書き処理が失敗した場合を検出する。
        if (!succeeded)
        {
            // 書込失敗を呼び出し元へ伝える。
            throw std::runtime_error("Could not alter credential fixture.");
        }
    }

    // RequireRestrictedAcl(path: 検査対象) は広範な主体を許すACLを拒否する。
    void RequireRestrictedAcl(const std::filesystem::path& path)
    {
        // 対象ファイルのACL
        PACL acl{};
        // ACLのセキュリティ記述子
        PSECURITY_DESCRIPTOR descriptor{};
        // ACL取得APIの戻り値
        const DWORD result = GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            &acl,
            nullptr,
            &descriptor);
        // ACL取得結果が不完全なら検査を中断する。
        if (result != ERROR_SUCCESS || descriptor == nullptr || acl == nullptr)
        {
            // 取得済みのセキュリティ記述子を解放する。
            if (descriptor != nullptr)
            {
                LocalFree(descriptor);
            }
            // ACL検査の失敗を呼び出し元へ伝える。
            throw std::runtime_error("Could not inspect credential ACL.");
        }

        // セキュリティ記述子の制御属性
        SECURITY_DESCRIPTOR_CONTROL control{};
        // 制御属性の版数
        DWORD revision{};
        // ACLの制限状態
        bool restricted = GetSecurityDescriptorControl(
                descriptor,
                &control,
                &revision) != FALSE
            && (control & SE_DACL_PROTECTED) != 0;
        // ACLのACE件数情報
        ACL_SIZE_INFORMATION information{};
        restricted = restricted
            && GetAclInformation(
                acl,
                &information,
                sizeof(information),
                AclSizeInformation) != FALSE;
        // index: ACE検査位置
        for (DWORD index = 0;
            restricted && index < information.AceCount;
            ++index)
        {
            // 取得したACEへのポインター
            void* rawAce{};
            // ACEを取得できなければ制限を満たさない。
            if (GetAce(acl, index, &rawAce) == FALSE)
            {
                restricted = false;
                // ACE検査を終了する。
                break;
            }
            // ACE共通ヘッダー
            const auto* header = static_cast<ACE_HEADER*>(rawAce);
            // 許可ACE以外を含むACLは拒否する。
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            {
                restricted = false;
                // ACE検査を終了する。
                break;
            }
            // アクセス許可ACE
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
            // ACEが許可する主体の識別子
            PSID sid = &ace->SidStart;
            // 広範な主体が含まれる場合は制限を解除する。
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

    // FactoryCredentialPath(gameId: ゲームID, environmentId: 環境ID) は資格情報パスを返す。
    [[nodiscard]] std::filesystem::path FactoryCredentialPath(
        const std::string& gameId,
        const std::string& environmentId)
    {
        // ファクトリーが生成したストア
        auto store = LamaPon::Detail::MakeWindowsRefreshTokenStore(
            gameId,
            environmentId);
        // Windows実装のストア
        const auto* windowsStore = dynamic_cast<
            LamaPon::Detail::WindowsRefreshTokenStore*>(store.get());
        Require(
            windowsStore != nullptr && !windowsStore->FilePath().empty(),
            "The Windows credential factory did not resolve LOCALAPPDATA.");
        // 生成済みストアの資格情報ファイルを返す。
        return windowsStore->FilePath();
    }

    // IsLowerHexPathComponent(path: 検査対象) は末尾が64桁の小文字16進数か判定する。
    [[nodiscard]] bool IsLowerHexPathComponent(
        const std::filesystem::path& path)
    {
        // ファイル名のワイド文字列
        const auto value = path.filename().wstring();
        // 64文字のファイル名かを返す。
        return value.size() == 64
            && std::ranges::all_of(
                value,
                [](const wchar_t character)
                {
                    // 小文字16進数か判定する各文字
                    return (character >= L'0' && character <= L'9')
                        || (character >= L'a' && character <= L'f');
                });
    }
}

// main() はWindows資格情報の保存と認証URLの安全性を検証する。
int main()
{
    // テスト用出力ディレクトリ
    const auto directory =
        std::filesystem::current_path()
        / "test-output"
        / "online-credentials";
    // トップレベルでテスト例外を捕捉する。
    try
    {
        // 出力先の後始末エラー
        std::error_code cleanupError;
        std::filesystem::remove_all(directory, cleanupError);
        Require(
            !cleanupError,
            "Could not prepare online credential test output.");

        // ハッシュ衝突を検査する識別子組
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
        // 生成済み資格情報パス
        std::vector<std::filesystem::path> factoryPaths;
        // ゲームIDと環境IDの組
        for (const auto& [gameId, environmentId] : pathNamespaces)
        {
            // 識別子組から生成したパス
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

        // テスト用資格情報ファイル
        const auto credentialPath = directory / "session.bin";
        // 暗号化保存する秘密値
        constexpr std::string_view refreshToken =
            "refresh-secret-that-must-never-appear-in-the-file";
        // 主に検証する資格情報ストア
        LamaPon::Detail::WindowsRefreshTokenStore store(
            credentialPath,
            "game-41c81960",
            "production");
        Require(
            store.Load().status
                == LamaPon::Detail::RefreshTokenLoadStatus::NotFound,
            "A missing credential was not reported as NotFound.");

        // 初回保存の結果
        const auto saved = store.Save(refreshToken);
        Require(
            saved.succeeded
                && std::filesystem::is_regular_file(credentialPath)
                && !std::filesystem::exists(
                    credentialPath.wstring() + L".tmp"),
            "The refresh token was not saved atomically.");
        RequireRestrictedAcl(credentialPath);
        RequireRestrictedAcl(credentialPath.parent_path());

        // 保存直後の暗号化データ
        const auto originalBytes = ReadBytes(credentialPath);
        Require(
            !originalBytes.empty()
                && std::search(
                    originalBytes.begin(),
                    originalBytes.end(),
                    refreshToken.begin(),
                    refreshToken.end()) == originalBytes.end(),
            "The refresh token appeared in the stored DPAPI envelope.");

        // 保存内容の読込結果
        const auto loaded = store.Load();
        Require(
            loaded.Loaded()
                && loaded.refreshToken == refreshToken,
            "The DPAPI-protected refresh token did not round-trip.");

        // 再生成した資格情報ストア
        LamaPon::Detail::WindowsRefreshTokenStore reopened(
            credentialPath,
            "game-41c81960",
            "production");
        Require(
            reopened.Load().refreshToken == refreshToken,
            "A recreated credential store could not restore the token.");

        // 別ゲーム識別子のストア
        LamaPon::Detail::WindowsRefreshTokenStore wrongGame(
            credentialPath,
            "another-game",
            "production");
        // 異なるゲーム識別子での読込結果
        const auto wrongEntropy = wrongGame.Load();
        Require(
            wrongEntropy.status
                    == LamaPon::Detail::RefreshTokenLoadStatus::Corrupt
                && wrongEntropy.refreshToken.empty()
                && wrongEntropy.errorMessage.find(refreshToken)
                    == std::string::npos,
            "A token was accepted with a different game entropy.");

        // 別環境識別子のストア
        LamaPon::Detail::WindowsRefreshTokenStore wrongEnvironment(
            credentialPath,
            "game-41c81960",
            "staging");
        Require(
            wrongEnvironment.Load().status
                == LamaPon::Detail::RefreshTokenLoadStatus::Corrupt,
            "A token was accepted with different environment entropy.");

        // 改変後の暗号化データ
        auto tampered = originalBytes;
        tampered.back() ^= 0x5au;
        OverwriteBytes(credentialPath, tampered);
        // 改変データの読込結果
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
        // 保存失敗試験用の秘密値
        constexpr std::string_view candidateToken =
            "candidate-refresh-token-must-not-be-committed";
        // requireOldCredential(message: 失敗理由) は直前の資格情報が保持されたことを確認する。
        const auto requireOldCredential = [&](const char* message)
        {
            // 再起動後の資格情報ストア
            LamaPon::Detail::WindowsRefreshTokenStore afterRestart(
                credentialPath,
                "game-41c81960",
                "production");
            // 保存失敗後の読込結果
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
        // サイズ上限を超える秘密値
        const std::string oversizedToken(8193, 'x');
        Require(
            !store.Save(oversizedToken).succeeded,
            "An oversized token replaced a valid credential.");
        requireOldCredential(
            "Oversized Save changed the committed credential.");

        // 失敗注入点と期待理由
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
            // 失敗注入時の保存結果
            const auto failed = store.Save(candidateToken);
            LamaPon::Detail::SetWindowsRefreshTokenSaveTestFailPoint(
                LamaPon::Detail::
                    WindowsRefreshTokenSaveTestFailPoint::None);
            Require(!failed.succeeded, message);
            requireOldCredential(message);
        }

        // 資格情報操作用ロックパス
        auto lockPath = credentialPath;
        lockPath += L".lock";
        // テスト資材操作のエラー
        std::error_code fixtureError;
        // シンボリックリンク作成属性
        constexpr DWORD AllowUnprivilegedCreate = 0x2u;

        // 資格情報使用中を示すロックパス
        auto usageLeasePath = credentialPath;
        usageLeasePath += L".session.lock";
        Require(
            store.AcquireUsageLease().succeeded
                && store.AcquireUsageLease().succeeded,
            "A credential usage lease was not idempotent.");
        // 使用権を競合するストア
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

        // 破棄時の解放確認用ストア
        LamaPon::Detail::WindowsRefreshTokenStore destructorWaiter(
            credentialPath,
            "game-41c81960",
            "production");
        {
            // スコープ内で使用権を持つストア
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
        // ハードリンク先の保護対象
        const auto usageLockVictim =
            directory / L"usage-lock-hardlink-victim.bin";
        {
            // 保護対象の初期内容を書き込むストリーム
            std::ofstream output(
                usageLockVictim,
                std::ios::binary | std::ios::trunc);
            output << "usage-lock-victim-must-not-change";
            Require(
                static_cast<bool>(output),
                "Could not create the usage-lock victim.");
        }
        // 保護対象の初期バイト列
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

        // リパースリンク先の保護対象
        const auto usageReparseVictim =
            directory / L"usage-lock-reparse-victim.bin";
        {
            // 保護対象の初期内容を書き込むストリーム
            std::ofstream output(
                usageReparseVictim,
                std::ios::binary | std::ios::trunc);
            output << "usage-reparse-victim-must-not-change";
            Require(
                static_cast<bool>(output),
                "Could not create the usage-lock reparse victim.");
        }
        // 保護対象の初期バイト列
        const auto usageReparseVictimBytes =
            ReadBytes(usageReparseVictim);
        // シンボリックリンクを作成できた場合に危険パスを試す。
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
        // ハードリンク先の保護対象
        const auto lockVictim = directory / L"lock-hardlink-victim.bin";
        {
            // 保護対象の初期内容を書き込むストリーム
            std::ofstream output(
                lockVictim,
                std::ios::binary | std::ios::trunc);
            output << "lock-victim-must-not-change";
            Require(
                static_cast<bool>(output),
                "Could not create the credential lock victim.");
        }
        // 保護対象の初期バイト列
        const auto lockVictimBytes = ReadBytes(lockVictim);
        Require(
            CreateHardLinkW(
                lockPath.c_str(),
                lockVictim.c_str(),
                nullptr) != FALSE,
            "Could not create the credential lock hard-link fixture.");
        // 危険なロックファイルの読込結果
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
        // 外部プロセス相当が保持するロック
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
        // ロック競合を検証するストア
        LamaPon::Detail::WindowsRefreshTokenStore blockedStore(
            credentialPath,
            "game-41c81960",
            "production");
        // ロック保持中の保存結果
        const auto blockedSave = blockedStore.Save(candidateToken);
        // ロック保持中の読込結果
        const auto blockedLoad = blockedStore.Load();
        // ロック保持中の削除結果
        const auto blockedDelete = blockedStore.Delete();
        // 競合中の資格情報ファイル内容
        const auto bytesWhileBlocked = ReadBytes(credentialPath);
        externalLock.Close();
        Require(
            !blockedSave.succeeded
                && blockedLoad.status
                    == LamaPon::Detail::RefreshTokenLoadStatus::Unavailable
                && !blockedDelete.succeeded
                && bytesWhileBlocked == originalBytes,
            "A busy credential lock exposed or changed the final token.");

        // ロック解放後の読込結果
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

        // 同時保存を制御する共有状態
        SavePause pause;
        LamaPon::Detail::SetWindowsRefreshTokenSaveBeforeReplaceHook(
            &PauseCredentialSaveBeforeReplace,
            &pause);
        // 先に保存を始めるストア
        LamaPon::Detail::WindowsRefreshTokenStore firstConcurrentStore(
            credentialPath,
            "game-41c81960",
            "production");
        // 後から保存するストア
        LamaPon::Detail::WindowsRefreshTokenStore secondConcurrentStore(
            credentialPath,
            "game-41c81960",
            "production");
        // 先行保存の結果
        LamaPon::Detail::OnlinePlatformResult firstConcurrentResult;
        // 先行保存を実行するスレッド
        std::thread firstSave(
            [&]
            {
                // 先行保存スレッドの例外を外へ漏らさない。
                try
                {
                    firstConcurrentResult = firstConcurrentStore.Save(
                        "first-concurrent-candidate");
                }
                // テストスレッドの例外を記録せず終了する。
                catch (...)
                {
                }
            });
        // 先行保存が置換待ちへ到達した状態
        bool firstReachedReplace{};
        {
            // 待機状態を読むための排他ロック
            std::unique_lock lock(pause.mutex);
            firstReachedReplace = pause.condition.wait_for(
                lock,
                3s,
                [&pause]
                {
                    // 停止地点への到達状態を待つ条件。
                    return pause.entered;
                });
        }
        // 後続保存の結果
        LamaPon::Detail::OnlinePlatformResult secondConcurrentResult;
        // 先行保存が置換待ちなら後続保存を競合させる。
        if (firstReachedReplace)
        {
            secondConcurrentResult = secondConcurrentStore.Save(
                "second-concurrent-candidate");
        }
        {
            // 保存再開状態の排他ロック
            std::scoped_lock lock(pause.mutex);
            pause.released = true;
        }
        pause.condition.notify_all();
        firstSave.join();
        LamaPon::Detail::SetWindowsRefreshTokenSaveBeforeReplaceHook(
            nullptr,
            nullptr);
        // 競合終了後に保存されている値
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

        // 資格情報の一時ファイルパス
        auto temporaryPath = credentialPath;
        temporaryPath += L".tmp";
        // ハードリンク先の保護対象
        const auto temporaryVictim =
            directory / L"temporary-hardlink-victim.bin";
        {
            // 保護対象の初期内容を書き込むストリーム
            std::ofstream output(
                temporaryVictim,
                std::ios::binary | std::ios::trunc);
            output << "temporary-victim-must-not-change";
            Require(
                static_cast<bool>(output),
                "Could not create the credential temporary victim.");
        }
        // 保護対象の初期バイト列
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

        // リパースリンク先の実体ディレクトリ
        const auto reparseTarget = directory / L"reparse-target";
        // 検査対象となる親リンクパス
        const auto reparseParent = directory / L"reparse-parent";
        std::filesystem::create_directories(reparseTarget, fixtureError);
        Require(
            !fixtureError,
            "Could not create the credential reparse target.");
        // 実体パスを使う基準ストア
        LamaPon::Detail::WindowsRefreshTokenStore realParentStore(
            reparseTarget / L"nested" / L"session.bin",
            "game-41c81960",
            "production");
        Require(
            realParentStore.Save(refreshToken).succeeded,
            "Could not prepare the credential reparse target.");
        // リンクを作成できた場合に経由パスの分離を検証する。
        if (CreateSymbolicLinkW(
                reparseParent.c_str(),
                reparseTarget.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY
                    | AllowUnprivilegedCreate) != FALSE)
        {
            // リパース経由で開くストア
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
            // 残存する一時ファイルの書込ストリーム
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
        // 一時ファイル削除失敗を含む結果
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

        // 不正識別子を拒否した状態
        bool invalidIdentifierRejected{};
        // 不正な識別子の拒否動作を検証する。
        try
        {
            // パス形式の識別子を渡すストア
            LamaPon::Detail::WindowsRefreshTokenStore invalid(
                credentialPath,
                "../escape",
                "production");
        }
        // 不正識別子を示す例外だけを期待する。
        catch (const std::invalid_argument&)
        {
            invalidIdentifierRejected = true;
        }
        Require(
            invalidIdentifierRejected,
            "A path-like game identifier was accepted.");

        // 起動関数の呼出回数
        std::size_t shellCalls{};
        // 起動関数に渡された操作名
        std::wstring openedOperation;
        // 起動関数に渡されたURL
        std::wstring openedUrl;
        // 起動関数に渡された所有者
        void* openedOwner{};
        // 起動引数がnullだった状態
        bool nullParameters{};
        // 作業ディレクトリがnullだった状態
        bool nullDirectory{};
        // 起動時の表示指定
        int openedShowCommand{};
        // 起動関数の戻り値
        std::intptr_t shellResult{ 33 };
        // shellOpen(owner: 所有者, operation: 操作, file: 起動対象, parameters: 引数, workingDirectory: 作業場所, showCommand: 表示方法) は起動関数の引数を記録する。
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
            // 起動APIの戻り値を呼び出し元へ返す。
            return shellResult;
        };
        // 起動ウィンドウの識別子
        void* const owner = reinterpret_cast<void*>(0x1234);
        // Windows認証URL起動器
        LamaPon::Detail::WindowsAuthorizationLauncher launcher(
            owner,
            shellOpen);
        // テストする認証URL
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

        // unsafeUrl: 拒否対象URL
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
        // URL起動失敗の結果
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
        // 全検証の成功をプロセス終了コードで示す。
        return 0;
    }
    // exception: テスト失敗の原因を表示する。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        // 失敗をプロセス終了コードで示す。
        return 1;
    }
}
