#include "LamaPon/Online/CloudSaveJournal.h"

#include <Windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using Journal = LamaPon::Detail::CloudSaveJournal;

    constexpr std::string_view AccountKey =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    constexpr std::string_view LevelHash =
        "fUV07UsUNLX3A0GWwak_tdwTcH_rWlJ1CqBLNiwLRwY";
    constexpr std::string_view EmptyHash =
        "RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o";
    constexpr std::string_view MutationOne =
        "123e4567-e89b-42d3-a456-426614174000";
    constexpr std::string_view MutationTwo =
        "123e4567-e89b-42d3-b456-426614174001";
    constexpr std::string_view MutationThree =
        "123e4567-e89b-42d3-8456-426614174002";

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::string Sha256LowerHex(const std::string_view value)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        Require(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0u) >= 0,
            "Journal test SHA-256 provider failed.");
        std::array<std::uint8_t, 32u> digest{};
        const auto status = BCryptHash(
            algorithm,
            nullptr,
            0u,
            value.empty()
                ? nullptr
                : reinterpret_cast<PUCHAR>(
                    const_cast<char*>(value.data())),
            static_cast<ULONG>(value.size()),
            digest.data(),
            static_cast<ULONG>(digest.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0u);
        Require(status >= 0, "Journal test SHA-256 failed.");
        constexpr char Digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2u);
        for (const auto byte : digest)
        {
            result.push_back(Digits[(byte >> 4u) & 0x0fu]);
            result.push_back(Digits[byte & 0x0fu]);
        }
        return result;
    }

    template<class Function>
    bool Throws(Function&& function)
    {
        try
        {
            std::forward<Function>(function)();
            return false;
        }
        catch (...)
        {
            return true;
        }
    }

    bool IsOwnedByCurrentUser(const std::filesystem::path& path)
    {
        HANDLE token{};
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
        {
            return false;
        }
        DWORD bytes{};
        GetTokenInformation(token, TokenUser, nullptr, 0u, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER
            || bytes < sizeof(TOKEN_USER))
        {
            CloseHandle(token);
            return false;
        }
        std::vector<std::uint8_t> tokenUser(bytes);
        if (GetTokenInformation(
                token,
                TokenUser,
                tokenUser.data(),
                bytes,
                &bytes) == FALSE)
        {
            CloseHandle(token);
            return false;
        }
        CloseHandle(token);
        const auto currentUser =
            reinterpret_cast<const TOKEN_USER*>(tokenUser.data())->User.Sid;
        PSID owner{};
        PSECURITY_DESCRIPTOR descriptor{};
        const auto result = GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &owner,
            nullptr,
            nullptr,
            nullptr,
            &descriptor);
        const bool matches = result == ERROR_SUCCESS
            && descriptor != nullptr
            && owner != nullptr
            && EqualSid(owner, currentUser) != FALSE;
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return matches;
    }

    std::filesystem::path TestRoot()
    {
        const auto root = std::filesystem::absolute(
            std::filesystem::current_path()
            / L"test-output"
            / L"cloud-save-journal").lexically_normal();
        Require(
            root.parent_path().filename() == L"test-output",
            "Journal test cleanup root escaped test-output.");
        return root;
    }

    void ResetTestRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(TestRoot(), error);
        Require(!error, "Journal test cleanup failed.");
        std::filesystem::create_directories(TestRoot(), error);
        Require(!error, "Journal test root creation failed.");
    }

    std::filesystem::path TrustedPath(const std::string_view name)
    {
        return TestRoot()
            / std::filesystem::path(
                std::u8string(
                    reinterpret_cast<const char8_t*>(name.data()),
                    reinterpret_cast<const char8_t*>(name.data() + name.size())))
            / L"GuestDisplayName";
    }

    LamaPon::PersistenceProfilePaths Profile(
        const std::filesystem::path& trusted,
        const std::string_view key = AccountKey)
    {
        const auto root = trusted.parent_path()
            / L"OnlineProfiles"
            / std::filesystem::path(
                std::u8string(
                    reinterpret_cast<const char8_t*>(key.data()),
                    reinterpret_cast<const char8_t*>(key.data() + key.size())));
        return {
            root,
            root / L"PlayerPrefs.json",
            root / L"Saves",
            std::string(key),
            false
        };
    }

    std::unique_ptr<Journal> MakeJournal(
        const std::filesystem::path& trusted,
        const std::string_view backend = "https://online.example.test/tenant-a/")
    {
        return std::make_unique<Journal>(
            trusted,
            Profile(trusted),
            "journal-game",
            "staging",
            std::string(backend));
    }

    std::vector<std::uint8_t> LevelContent()
    {
        return { '{', '"', 'l', 'e', 'v', 'e', 'l', '"', ':', '7', '}' };
    }

    std::vector<std::uint8_t> EmptyContent()
    {
        return { '{', '}' };
    }

    LamaPon::CloudSaveSnapshot LiveSnapshot(
        LamaPon::CloudSaveResource resource,
        std::string etag,
        std::vector<std::uint8_t> content,
        const std::string_view hash)
    {
        return {
            std::move(resource),
            std::move(etag),
            false,
            std::move(content),
            std::string(hash)
        };
    }

    LamaPon::CloudSaveSnapshot Tombstone(
        LamaPon::CloudSaveResource resource,
        std::string etag)
    {
        return {
            std::move(resource),
            std::move(etag),
            true,
            {},
            {}
        };
    }

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Require(input.good(), "Journal fixture could not be read.");
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    void WriteText(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(output.good(), "Journal fixture could not be opened.");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        Require(output.good(), "Journal fixture could not be written.");
    }

    std::filesystem::path Suffix(
        const std::filesystem::path& path,
        const std::wstring_view suffix)
    {
        auto result = path;
        result += suffix;
        return result;
    }

    void RemoveExact(const std::filesystem::path& path)
    {
        std::error_code error;
        (void)std::filesystem::remove(path, error);
        Require(!error, "Journal fixture cleanup failed.");
    }

    void TestWriteAheadRoundTripAndConflict()
    {
        const auto trusted = TrustedPath("roundtrip");
        const auto profile = Profile(trusted);
        Require(
            !std::filesystem::exists(profile.rootDirectory),
            "Account fixture unexpectedly existed.");

        std::filesystem::path journalPath;
        {
            auto journal = MakeJournal(trusted);
            journalPath = journal->FilePath();
            Require(
                journal->Generation() == 0u
                    && !std::filesystem::exists(journalPath)
                    && !std::filesystem::exists(profile.rootDirectory)
                    && journalPath.parent_path().parent_path().filename()
                        == L"OnlineState",
                "Journal construction created account persistence.");
            journal->QueuePut(
                LamaPon::CloudSaveResource::Preferences(),
                LevelContent(),
                MutationOne);
            Require(
                journal->Generation() == 1u
                    && std::filesystem::exists(journalPath)
                    && !std::filesystem::exists(profile.rootDirectory),
                "Write-ahead journal was not durably separated from account data.");
        }

        {
            auto journal = MakeJournal(trusted);
            const auto pending = journal->Pending(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                journal->Generation() == 1u
                    && pending
                    && pending->mutationId == MutationOne
                    && !pending->baseEtag
                    && pending->content == LevelContent()
                    && pending->sha256 == LevelHash
                    && journal->Dispatchable().size() == 1u,
                "Pending mutation did not survive restart exactly.");
            journal->RecordConflict(
                LamaPon::CloudSaveResource::Preferences(),
                MutationOne,
                LiveSnapshot(
                    LamaPon::CloudSaveResource::Preferences(),
                    "\"remote-1\"",
                    EmptyContent(),
                    EmptyHash));
        }

        {
            auto journal = MakeJournal(trusted);
            const auto pending = journal->Pending(
                LamaPon::CloudSaveResource::Preferences());
            const auto conflict = journal->Conflict(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                pending && pending->mutationId == MutationOne
                    && conflict && conflict->etag == "\"remote-1\""
                    && journal->Dispatchable().empty()
                    && journal->Resources().size() == 1u,
                "Conflict or its immutable pending mutation was not restored.");
            const auto generation = journal->Generation();
            Require(
                Throws([&]
                {
                    journal->ResolveConflict(
                        LamaPon::CloudSaveResource::Preferences(),
                        MutationTwo,
                        LamaPon::Detail::CloudSaveConflictResolution::RetryLocal,
                        MutationThree);
                })
                    && journal->Generation() == generation,
                "A stale conflict UI mutation id changed the journal.");
            journal->ResolveConflict(
                LamaPon::CloudSaveResource::Preferences(),
                MutationOne,
                LamaPon::Detail::CloudSaveConflictResolution::RetryLocal,
                MutationTwo);
            const auto retried = journal->Pending(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                retried && retried->mutationId == MutationTwo
                    && retried->baseEtag == "\"remote-1\""
                    && retried->content == LevelContent()
                    && !journal->Conflict(
                        LamaPon::CloudSaveResource::Preferences())
                    && journal->Dispatchable().size() == 1u,
                "RetryLocal changed content or failed to advance CAS metadata.");
            journal->RecordSuccess(
                LamaPon::CloudSaveResource::Preferences(),
                MutationTwo,
                LiveSnapshot(
                    LamaPon::CloudSaveResource::Preferences(),
                    "\"accepted-2\"",
                    LevelContent(),
                    LevelHash));
        }

        {
            auto journal = MakeJournal(trusted);
            const auto baseline = journal->Baseline(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                baseline && baseline->etag == "\"accepted-2\""
                    && baseline->content == LevelContent()
                    && !journal->HasPending(
                        LamaPon::CloudSaveResource::Preferences()),
                "Mutation success was not retained as a full baseline.");
        }

        const auto persisted = ReadText(journalPath);
        const auto document = Json::parse(persisted);
        Require(
            document.contains("parentGeneration")
                && document.contains("parentChecksum")
                && document.contains("checksum")
                && persisted.find("journal-game") == std::string::npos
                && persisted.find("staging") == std::string::npos
                && persisted.find("online.example.test") == std::string::npos
                && persisted.find("token") == std::string::npos
                && persisted.find("discord") == std::string::npos,
            "Journal schema omitted its chain or exposed namespace/identity data.");
    }

    void TestDeleteUseRemoteAndGlobalMutationIds()
    {
        const auto trusted = TrustedPath("delete");
        auto journal = MakeJournal(trusted);
        const auto slot = LamaPon::CloudSaveResource::SaveSlot("Slot-A");
        journal->RecordBaseline(Tombstone(slot, "\"old\""));
        journal->QueueDelete(slot, MutationOne, "\"old\"");
        const auto before = journal->Generation();
        Require(
            Throws([&]
            {
                journal->QueuePut(
                    LamaPon::CloudSaveResource::Preferences(),
                    EmptyContent(),
                    MutationOne);
            })
                && journal->Generation() == before,
            "A mutation id was reused by a different resource.");
        journal->RecordConflict(
            slot,
            MutationOne,
            LiveSnapshot(slot, "\"remote\"", EmptyContent(), EmptyHash));
        journal.reset();

        journal = MakeJournal(trusted);
        Require(
            journal->Pending(
                LamaPon::CloudSaveResource::SaveSlot("slot-a"))
                && journal->Conflict(
                    LamaPon::CloudSaveResource::SaveSlot("slot-a")),
            "Case-insensitive slot identity was not restored.");
        journal->ResolveConflict(
            LamaPon::CloudSaveResource::SaveSlot("slot-a"),
            MutationOne,
            LamaPon::Detail::CloudSaveConflictResolution::UseRemote);
        const auto baseline = journal->Baseline(slot);
        Require(
            baseline && !baseline->deleted
                && baseline->etag == "\"remote\""
                && !journal->Pending(slot),
            "UseRemote did not finalize the retained full remote snapshot.");
    }

    void TestGenerationCasBlocksStaleInstance()
    {
        const auto trusted = TrustedPath("cas");
        auto first = MakeJournal(trusted);
        auto stale = MakeJournal(trusted);
        first->RecordBaseline(Tombstone(
            LamaPon::CloudSaveResource::SaveSlot("first"),
            "\"one\""));
        Require(
            Throws([&]
            {
                stale->RecordBaseline(Tombstone(
                    LamaPon::CloudSaveResource::SaveSlot("stale"),
                    "\"two\""));
            }),
            "A stale journal instance bypassed generation CAS.");
        Require(
            Throws([&] { (void)stale->Dispatchable(); })
                && Throws([&] { (void)stale->Generation(); }),
            "A stale instance remained usable after an ambiguous CAS failure.");
        auto reopened = MakeJournal(trusted);
        Require(
            reopened->Resources().size() == 1u
                && reopened->Resources()[0].slot == "first",
            "A CAS loser changed persistent state.");
    }

    void TestOperatingSystemLockFailsClosed()
    {
        const auto trusted = TrustedPath("os-lock");
        auto journal = MakeJournal(trusted);
        const auto lockPath =
            journal->FilePath().parent_path() / L"CloudSaveJournal.lock";
        Require(
            LamaPon::Detail::IsCloudSaveJournalLockExclusiveForTesting(
                lockPath),
            "Two production journal locks were acquired concurrently.");
        const auto lock = CreateFileW(
            lockPath.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(lock != INVALID_HANDLE_VALUE, "Journal OS lock fixture failed.");
        const bool rejected = Throws([&]
        {
            journal->QueuePut(
                LamaPon::CloudSaveResource::Preferences(),
                EmptyContent(),
                MutationOne);
        });
        CloseHandle(lock);
        Require(
            rejected && journal->Generation() == 0u
                && journal->Resources().empty(),
            "An initial lock contention permanently blocked a clean instance.");
        journal->QueuePut(
            LamaPon::CloudSaveResource::Preferences(),
            EmptyContent(),
            MutationOne);
        auto reopened = MakeJournal(trusted);
        Require(
            reopened->Generation() == 1u
                && reopened->Pending(
                    LamaPon::CloudSaveResource::Preferences()),
            "A journal operation could not retry after lock contention ended.");
    }

    void TestHardLinksAreRejectedBeforeMutation()
    {
        const auto trusted = TrustedPath("hard-links");
        auto journal = MakeJournal(trusted);
        const auto finalPath = journal->FilePath();
        const auto writingPath = Suffix(finalPath, L".writing");
        Require(
            IsOwnedByCurrentUser(finalPath.parent_path().parent_path())
                && IsOwnedByCurrentUser(finalPath.parent_path())
                && IsOwnedByCurrentUser(
                    finalPath.parent_path() / L"CloudSaveJournal.lock"),
            "A newly created restricted journal object has an unsafe owner.");
        const auto writeVictim = TestRoot() / L"hard-link-write-victim.txt";
        constexpr std::string_view WriteVictimText = "must-not-be-truncated";
        WriteText(writeVictim, WriteVictimText);
        Require(
            CreateHardLinkW(
                writingPath.c_str(),
                writeVictim.c_str(),
                nullptr) != FALSE,
            "Journal write hard-link fixture could not be created.");
        Require(
            Throws([&]
            {
                journal->QueuePut(
                    LamaPon::CloudSaveResource::Preferences(),
                    EmptyContent(),
                    MutationOne);
            })
                && ReadText(writeVictim) == WriteVictimText,
            "A hard-linked write target was modified before validation.");
        RemoveExact(writingPath);
        RemoveExact(writeVictim);
        journal->QueuePut(
            LamaPon::CloudSaveResource::Preferences(),
            EmptyContent(),
            MutationOne);
        Require(
            IsOwnedByCurrentUser(finalPath),
            "A newly published journal has an unsafe owner.");
        journal.reset();

        const auto candidateAlias = Suffix(finalPath, L".candidate-alias");
        const auto finalDocument = ReadText(finalPath);
        Require(
            CreateHardLinkW(
                candidateAlias.c_str(),
                finalPath.c_str(),
                nullptr) != FALSE,
            "Journal candidate hard-link fixture could not be created.");
        Require(
            Throws([&] { (void)MakeJournal(trusted); })
                && ReadText(finalPath) == finalDocument,
            "A hard-linked journal candidate was accepted or modified.");
        RemoveExact(candidateAlias);

        const auto lockPath =
            finalPath.parent_path() / L"CloudSaveJournal.lock";
        RemoveExact(lockPath);
        const auto lockVictim = TestRoot() / L"hard-link-lock-victim.txt";
        constexpr std::string_view LockVictimText = "must-remain-a-data-file";
        WriteText(lockVictim, LockVictimText);
        Require(
            CreateHardLinkW(
                lockPath.c_str(),
                lockVictim.c_str(),
                nullptr) != FALSE,
            "Journal lock hard-link fixture could not be created.");
        Require(
            Throws([&] { (void)MakeJournal(trusted); })
                && ReadText(lockVictim) == LockVictimText,
            "A hard-linked journal lock was accepted or modified.");
        RemoveExact(lockPath);
        RemoveExact(lockVictim);
    }

    void TestDurabilityBarrierAndWritingCandidate()
    {
        const auto trusted = TrustedPath("flush-barrier");
        std::filesystem::path finalPath;
        {
            auto journal = MakeJournal(trusted);
            finalPath = journal->FilePath();
            LamaPon::Detail::SetCloudSaveJournalTestFailPoint(
                LamaPon::Detail::CloudSaveJournalTestFailPoint::
                    BeforeNextFlush);
            Require(
                Throws([&]
                {
                    journal->QueuePut(
                        LamaPon::CloudSaveResource::Preferences(),
                        EmptyContent(),
                        MutationOne);
                })
                    && journal->Generation() == 0u
                    && !journal->Pending(
                        LamaPon::CloudSaveResource::Preferences())
                    && !std::filesystem::exists(finalPath)
                    && std::filesystem::exists(
                        Suffix(finalPath, L".writing")),
                "A mutation without a successful flush became dispatchable.");
        }
        {
            auto reopened = MakeJournal(trusted);
            Require(
                reopened->Generation() == 0u
                    && reopened->Resources().empty(),
                "A selector adopted the non-candidate .writing file.");
            LamaPon::Detail::SetCloudSaveJournalTestFailPoint(
                LamaPon::Detail::CloudSaveJournalTestFailPoint::
                    AfterNextFlush);
            reopened->QueuePut(
                LamaPon::CloudSaveResource::Preferences(),
                EmptyContent(),
                MutationOne);
            Require(
                reopened->Generation() == 1u
                    && reopened->Pending(
                        LamaPon::CloudSaveResource::Preferences()),
                "A flushed .next candidate was not recovered as committed.");
        }
    }

    void TestResourceLimits()
    {
        const auto trusted = TrustedPath("limits");
        auto journal = MakeJournal(trusted);
        journal->RecordBaseline(Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"preferences\""));
        for (std::size_t index = 0u;
             index < LamaPon::CloudSaveMaxSlots;
             ++index)
        {
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::SaveSlot(
                    "slot-" + std::to_string(index)),
                "\"v-" + std::to_string(index) + "\""));
        }
        const auto generation = journal->Generation();
        Require(
            journal->Resources().size()
                    == LamaPon::CloudSaveMaxSlots + 1u
                && Throws([&]
                {
                    journal->RecordBaseline(Tombstone(
                        LamaPon::CloudSaveResource::SaveSlot("slot-overflow"),
                        "\"overflow\""));
                })
                && journal->Generation() == generation,
            "Journal save-slot limits were not enforced atomically.");
        LamaPon::CloudSaveResource invalid;
        invalid.kind = static_cast<LamaPon::CloudSaveResourceKind>(255u);
        Require(
            Throws([&]
            {
                journal->RecordBaseline(Tombstone(invalid, "\"invalid\""));
            }),
            "An invalid resource kind was accepted.");
    }

    void TestRecoveryTopologyAndSplitBrain()
    {
        const auto trusted = TrustedPath("recovery");
        std::filesystem::path finalPath;
        std::string generationOne;
        std::string generationTwo;
        {
            auto journal = MakeJournal(trusted);
            finalPath = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"one\""));
            generationOne = ReadText(finalPath);
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"two\""));
            generationTwo = ReadText(finalPath);
        }
        Require(
            ReadText(Suffix(finalPath, L".bak")) == generationOne,
            "Normal publish did not retain the parent generation.");
        WriteText(Suffix(finalPath, L".next"), generationTwo);
        WriteText(finalPath, "{truncated");
        {
            auto recovered = MakeJournal(trusted);
            Require(
                recovered->Generation() == 2u
                    && recovered->Baseline(
                        LamaPon::CloudSaveResource::Preferences())->etag
                        == "\"two\""
                    && ReadText(Suffix(finalPath, L".bak")) == generationOne,
                "A flushed child was not recovered without destroying its valid parent.");
        }
        // newer final(g2)に対し、同一の古いnext/backup(g1)が残っても
        // 冗長copyを理由にrollbackしてはいけません。
        WriteText(Suffix(finalPath, L".next"), generationOne);
        Require(
            Throws([&] { (void)MakeJournal(trusted); })
                && ReadText(finalPath) == generationTwo,
            "A stale identical next/backup pair rolled back a newer final.");
        WriteText(finalPath, generationOne);
        WriteText(Suffix(finalPath, L".next"), generationOne);
        WriteText(Suffix(finalPath, L".bak"), generationTwo);
        Require(
            Throws([&] { (void)MakeJournal(trusted); })
                && ReadText(Suffix(finalPath, L".bak")) == generationTwo,
            "An identical old final/next pair hid a newer backup.");

        const auto backupTrusted = TrustedPath("backup-recovery");
        std::filesystem::path backupFinal;
        std::string backupDocument;
        {
            auto journal = MakeJournal(backupTrusted);
            backupFinal = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"backup-one\""));
            backupDocument = ReadText(backupFinal);
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"discarded-child\""));
        }
        WriteText(backupFinal, "{truncated");
        WriteText(Suffix(backupFinal, L".bak"), backupDocument);
        // backupをfinalへ戻す途中で、同じ文書をnextへflushした直後の窓です。
        WriteText(Suffix(backupFinal, L".next"), backupDocument);
        {
            auto recovered = MakeJournal(backupTrusted);
            Require(
                recovered->Generation() == 1u
                    && recovered->Baseline(
                        LamaPon::CloudSaveResource::Preferences())->etag
                        == "\"backup-one\"",
                "Equivalent backup/next recovery copies were treated as split brain.");
        }

        const auto splitTrusted = TrustedPath("split-brain");
        std::filesystem::path splitFinal;
        std::string branchA;
        std::string branchAChild;
        std::string branchB;
        {
            auto journal = MakeJournal(splitTrusted);
            splitFinal = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"branch-a\""));
            branchA = ReadText(splitFinal);
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"branch-a-child\""));
            branchAChild = ReadText(splitFinal);
        }
        RemoveExact(splitFinal);
        RemoveExact(Suffix(splitFinal, L".bak"));
        RemoveExact(Suffix(splitFinal, L".next"));
        {
            auto journal = MakeJournal(splitTrusted);
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"branch-b\""));
            branchB = ReadText(splitFinal);
        }
        WriteText(splitFinal, branchA);
        WriteText(Suffix(splitFinal, L".next"), branchB);
        Require(
            Throws([&] { (void)MakeJournal(splitTrusted); }),
            "Same-generation split brain was accepted.");
        WriteText(splitFinal, branchAChild);
        WriteText(Suffix(splitFinal, L".next"), branchAChild);
        WriteText(Suffix(splitFinal, L".bak"), branchB);
        Require(
            Throws([&] { (void)MakeJournal(splitTrusted); }),
            "Identical final/next bypassed an unrelated parent candidate.");
        WriteText(splitFinal, branchB);
        WriteText(Suffix(splitFinal, L".next"), branchAChild);
        WriteText(Suffix(splitFinal, L".bak"), branchAChild);
        Require(
            Throws([&] { (void)MakeJournal(splitTrusted); }),
            "Identical next/backup bypassed an unrelated parent candidate.");
    }

    void TestBindingSchemaAndContentFailClosed()
    {
        const auto trusted = TrustedPath("binding");
        std::filesystem::path filePath;
        {
            auto journal = MakeJournal(trusted);
            filePath = journal->FilePath();
            journal->QueuePut(
                LamaPon::CloudSaveResource::Preferences(),
                EmptyContent(),
                MutationOne);
            const auto generation = journal->Generation();
            const std::vector<std::uint8_t> duplicate{
                '{', '"', 'x', '"', ':', '1', ',',
                '"', 'x', '"', ':', '2', '}'
            };
            Require(
                Throws([&]
                {
                    journal->QueuePut(
                        LamaPon::CloudSaveResource::SaveSlot("dup"),
                        duplicate,
                        MutationTwo);
                })
                    && journal->Generation() == generation,
                "Duplicate keys in embedded JSON were accepted.");
        }
        Require(
            Throws([&]
            {
                (void)MakeJournal(
                    trusted,
                    "https://online.example.test/tenant-b/");
            }),
            "A different backend base path reused a bound journal.");

        auto future = Json::parse(ReadText(filePath));
        future["version"] = 3;
        WriteText(Suffix(filePath, L".next"), future.dump());
        Require(
            Throws([&] { (void)MakeJournal(trusted); }),
            "A future journal version was ignored in favor of an old candidate.");

        const auto schemaTrusted = TrustedPath("unknown-schema");
        std::filesystem::path schemaPath;
        {
            auto journal = MakeJournal(schemaTrusted);
            schemaPath = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"schema\""));
        }
        auto unknownSchema = Json::parse(ReadText(schemaPath));
        unknownSchema["futureField"] = true;
        WriteText(Suffix(schemaPath, L".next"), unknownSchema.dump());
        Require(
            Throws([&] { (void)MakeJournal(schemaTrusted); }),
            "An unknown journal schema was ignored in favor of an old candidate.");

        const auto corruptTrusted = TrustedPath("corrupt");
        std::filesystem::path corruptPath;
        {
            auto journal = MakeJournal(corruptTrusted);
            corruptPath = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"valid\""));
        }
        auto corrupt = ReadText(corruptPath);
        const auto position = corrupt.find("\"format\"");
        Require(position != std::string::npos, "Journal fixture had no format field.");
        corrupt.insert(position, "\"format\":\"duplicate\",");
        WriteText(corruptPath, corrupt);
        Require(
            Throws([&] { (void)MakeJournal(corruptTrusted); }),
            "Duplicate journal keys were accepted as an empty state.");
    }

    void TestTrustedPathAndAccountNonCreation()
    {
        const auto trusted = TrustedPath("paths");
        auto profile = Profile(trusted);
        const auto arbitrary = TestRoot()
            / L"arbitrary"
            / L"OnlineProfiles"
            / std::filesystem::path(std::string(AccountKey));
        auto forged = profile;
        forged.rootDirectory = arbitrary;
        forged.playerPrefsFile = arbitrary / L"PlayerPrefs.json";
        forged.saveDataDirectory = arbitrary / L"Saves";
        Require(
            Throws([&]
            {
                Journal journal(
                    trusted,
                    forged,
                    "game",
                    "production",
                    "https://online.example.test");
            })
                && !std::filesystem::exists(arbitrary),
            "A forged account root escaped the trusted anchor.");

        auto guest = profile;
        guest.isGuest = true;
        Require(
            Throws([&]
            {
                Journal journal(
                    trusted,
                    guest,
                    "game",
                    "production",
                    "https://online.example.test");
            }),
            "A guest profile was accepted by the account journal.");
        Require(
            Throws([&]
            {
                Journal journal(
                    L"relative-user-data",
                    profile,
                    "game",
                    "production",
                    "https://online.example.test");
            }),
            "A relative trusted anchor was accepted.");
        Require(
            Throws([&]
            {
                Journal journal(
                    L"\\\\server\\share\\Guest",
                    profile,
                    "game",
                    "production",
                    "https://online.example.test");
            }),
            "A network trusted anchor was accepted.");

        const auto caseTrusted = TrustedPath("case-spelling");
        auto caseForged = Profile(caseTrusted);
        caseForged.playerPrefsFile =
            caseForged.rootDirectory / L"playerprefs.json";
        Require(
            Throws([&]
            {
                Journal journal(
                    caseTrusted,
                    caseForged,
                    "game",
                    "production",
                    "https://online.example.test");
            }),
            "A case-only forged account profile path was accepted.");

        auto valid = MakeJournal(trusted);
        valid->QueuePut(
            LamaPon::CloudSaveResource::Preferences(),
            EmptyContent(),
            MutationOne);
        Require(
            !std::filesystem::exists(profile.rootDirectory)
                && !std::filesystem::exists(
                    trusted.parent_path() / L"OnlineProfiles"),
            "Journal activity changed guest-import account existence.");
    }

    void TestLocalDeleteIntentAndVersionOneMigration()
    {
        const auto trusted = TrustedPath("version-migration");
        std::filesystem::path filePath;
        {
            auto journal = MakeJournal(trusted);
            filePath = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"v1-baseline\""));
        }

        auto versionOne = Json::parse(ReadText(filePath));
        versionOne["version"] = 1u;
        for (auto& entry : versionOne.at("entries"))
        {
            entry.erase("localDeleteIntent");
        }
        versionOne.erase("checksum");
        versionOne["checksum"] = Sha256LowerHex(versionOne.dump());
        WriteText(filePath, versionOne.dump());
        RemoveExact(Suffix(filePath, L".next"));
        RemoveExact(Suffix(filePath, L".bak"));

        {
            auto migrated = MakeJournal(trusted);
            Require(
                migrated->Generation() == 1u
                    && migrated->Baseline(
                        LamaPon::CloudSaveResource::Preferences())
                        ->etag == "\"v1-baseline\"",
                "A valid version-1 journal could not be loaded.");
            migrated->RecordLocalDeleteIntent(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                migrated->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences()),
                "A local delete intent was not durable in memory.");
        }
        Require(
            Json::parse(ReadText(filePath)).at("version") == 2u,
            "A version-1 journal was not durably migrated on mutation.");
        {
            auto reopened = MakeJournal(trusted);
            Require(
                reopened->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences()),
                "A local delete intent did not survive restart.");
            reopened->ClearLocalDeleteIntent(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                !reopened->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences())
                    && reopened->Baseline(
                        LamaPon::CloudSaveResource::Preferences()),
                "Clearing a local delete intent discarded its baseline.");
        }
    }
}

int main()
{
    try
    {
        ResetTestRoot();
        TestWriteAheadRoundTripAndConflict();
        TestDeleteUseRemoteAndGlobalMutationIds();
        TestGenerationCasBlocksStaleInstance();
        TestOperatingSystemLockFailsClosed();
        TestHardLinksAreRejectedBeforeMutation();
        TestDurabilityBarrierAndWritingCandidate();
        TestResourceLimits();
        TestRecoveryTopologyAndSplitBrain();
        TestBindingSchemaAndContentFailClosed();
        TestTrustedPathAndAccountNonCreation();
        TestLocalDeleteIntentAndVersionOneMigration();
        std::error_code cleanupError;
        std::filesystem::remove_all(TestRoot(), cleanupError);
        Require(!cleanupError, "Journal test final cleanup failed.");
        std::cout << "Cloud save journal durability tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
