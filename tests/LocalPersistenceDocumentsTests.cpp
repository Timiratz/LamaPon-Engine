#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Online/CloudSave.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using Documents = LamaPon::Detail::LocalPersistenceDocuments;
    using State = LamaPon::Detail::LocalPersistenceDocumentState;

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
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

    std::filesystem::path TestRoot()
    {
        const auto root = std::filesystem::absolute(
            std::filesystem::current_path()
            / L"test-output"
            / L"local-persistence-documents").lexically_normal();
        Require(
            root.parent_path().filename() == L"test-output",
            "Local persistence test root escaped test-output.");
        return root;
    }

    void ResetRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(TestRoot(), error);
        Require(!error, "Local persistence cleanup failed.");
        std::filesystem::create_directories(TestRoot(), error);
        Require(!error, "Local persistence test root creation failed.");
    }

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Require(input.good(), "Local persistence fixture could not be read.");
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
        Require(output.good(), "Local persistence fixture could not be opened.");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        Require(output.good(), "Local persistence fixture could not be written.");
    }

    std::vector<std::uint8_t> Bytes(const std::string_view text)
    {
        return {
            reinterpret_cast<const std::uint8_t*>(text.data()),
            reinterpret_cast<const std::uint8_t*>(text.data() + text.size())
        };
    }

    std::string AsText(const std::vector<std::uint8_t>& bytes)
    {
        return {
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()
        };
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
        Require(!error, "Local persistence fixture cleanup failed.");
    }

    constexpr std::string_view RemotePreferences =
        R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":9}}})";
    constexpr std::string_view RemotePreferencesTwo =
        R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":10}}})";
    constexpr std::string_view RemoteSave =
        R"({"format":"LamaPonSaveData","version":1,"slot":"remote","data":{"level":8}})";

    struct ObserverProbe final
    {
        std::size_t calls{};
        std::uint64_t epoch{};
        const void* expectedSource{};
        bool observedCommittedState{};
        bool returnSuccess{ true };
    };

    bool ObserveCommit(
        void* const context,
        const std::uint64_t epoch,
        const LamaPon::Detail::LocalPersistenceCommitEvent& event) noexcept
    {
        auto& probe = *static_cast<ObserverProbe*>(context);
        ++probe.calls;
        probe.epoch = epoch;
        if (event.source != probe.expectedSource || event.filePath == nullptr)
        {
            return false;
        }
        try
        {
            if (event.kind
                == LamaPon::Detail::LocalPersistenceResourceKind::PlayerPrefs)
            {
                const auto& prefs = *static_cast<const LamaPon::PlayerPrefs*>(
                    event.source);
                probe.observedCommittedState =
                    Documents::ReadPlayerPrefs(prefs).state == State::Loaded;
            }
            else
            {
                const auto& saves = *static_cast<const LamaPon::SaveDataStore*>(
                    event.source);
                const auto state = Documents::ReadSaveData(
                    saves,
                    event.slot).state;
                probe.observedCommittedState = event.deleted
                    ? state == State::Missing
                    : state == State::Loaded;
            }
        }
        catch (...)
        {
            return false;
        }
        return probe.returnSuccess && probe.observedCommittedState;
    }

    void TestReadStatesAndStrictSchema()
    {
        const auto directory = TestRoot() / L"states";
        LamaPon::PlayerPrefs prefs(directory / L"PlayerPrefs.json");
        LamaPon::SaveDataStore saves(directory / L"Saves");
        Require(
            Documents::ReadPlayerPrefs(prefs).state == State::Missing
                && Documents::ReadSaveData(saves, "slot").state
                    == State::Missing
                && Documents::ListSaveData(saves).state == State::Missing,
            "Missing local persistence was not distinguished.");

        prefs.SetInteger("level", 3);
        prefs.Save();
        saves.SaveJson("slot", R"({"level":3})");
        const auto prefsRead = Documents::ReadPlayerPrefs(prefs);
        const auto saveRead = Documents::ReadSaveData(saves, "slot");
        const auto listing = Documents::ListSaveData(saves);
        Require(
            prefsRead.state == State::Loaded
                && saveRead.state == State::Loaded
                && AsText(prefsRead.bytes).find("LamaPonPlayerPrefs")
                    != std::string::npos
                && AsText(saveRead.bytes).find("LamaPonSaveData")
                    != std::string::npos
                && listing.state == State::Loaded
                && listing.slots == std::vector<std::string>{ "slot" },
            "Valid full local documents were not returned.");

        const auto unavailablePath = directory / L"not-a-directory";
        WriteText(unavailablePath, "file");
        LamaPon::SaveDataStore unavailable(unavailablePath);
        Require(
            Documents::ListSaveData(unavailable).state == State::Unavailable,
            "A non-directory SaveData root was treated as missing.");

        LamaPon::SaveDataStore tooMany(directory / L"TooManySaves");
        for (std::size_t index = 0u;
             index <= LamaPon::CloudSaveMaxSlots;
             ++index)
        {
            tooMany.SaveJson("slot-" + std::to_string(index), "{}");
        }
        Require(
            Documents::ListSaveData(tooMany).state == State::Corrupt,
            "A 33rd cloud save slot was accepted by strict enumeration.");

        const auto file = CreateFileW(
            prefs.FilePath().c_str(),
            GENERIC_READ,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(file != INVALID_HANDLE_VALUE, "Sharing fixture could not open.");
        const auto unavailableRead = Documents::ReadPlayerPrefs(prefs).state;
        CloseHandle(file);
        Require(
            unavailableRead == State::Unavailable,
            "Sharing denial was treated as missing or corrupt.");

        WriteText(prefs.FilePath(), "{truncated");
        Require(
            Documents::ReadPlayerPrefs(prefs).state == State::Corrupt,
            "Malformed PlayerPrefs was not classified as corrupt.");
    }

    void TestDurableBarriersAndConcurrentWriterContract()
    {
        const auto path = TestRoot() / L"durability" / L"PlayerPrefs.json";
        LamaPon::PlayerPrefs first(path);
        first.SetString("writer", "baseline");
        first.Save();
        const auto baseline = ReadText(path);
        Require(
            LamaPon::Detail::IsLocalPersistenceLockExclusiveForTesting(path),
            "Two production local persistence locks were acquired.");

        first.SetString("writer", "P1");
        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::BeforeFlush);
        Require(
            Throws([&] { first.Save(); })
                && first.IsDirty()
                && ReadText(path) == baseline
                && std::filesystem::exists(Suffix(path, L".writing")),
            "A pre-flush failure changed the published PlayerPrefs.");

        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::
                AfterFlushBeforePublish);
        Require(
            Throws([&] { first.Save(); })
                && first.IsDirty()
                && ReadText(path) == baseline,
            "A pre-publish failure changed the published PlayerPrefs.");

        LamaPon::PlayerPrefs second(path);
        second.Load();
        second.SetString("writer", "P2");
        second.Save();
        const auto durable = Documents::ReadPlayerPrefs(second);
        Require(
            durable.state == State::Loaded
                && second.GetString("writer") == "P2"
                && AsText(durable.bytes).find("P2") != std::string::npos,
            "A successful later writer did not return with durable P2 bytes.");

        LamaPon::SaveDataStore saves(TestRoot() / L"save-barrier" / L"Saves");
        saves.SaveJson("slot", R"({"writer":"old"})");
        const auto savePath = saves.SlotPath("slot");
        const auto oldSave = ReadText(savePath);
        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::
                AfterFlushBeforePublish);
        Require(
            Throws([&]
            {
                saves.SaveJson("slot", R"({"writer":"new"})");
            })
                && ReadText(savePath) == oldSave,
            "A failed SaveData publish changed the existing document.");
        saves.SaveJson("slot", R"({"writer":"new"})");
        Require(
            Documents::ReadSaveData(saves, "slot").state == State::Loaded
                && saves.LoadJson("slot")
                    == std::optional<std::string>(R"({"writer":"new"})"),
            "SaveData did not return after its full document became durable.");
    }

    void TestRemoteApplyStrongGuaranteeAndIdentity()
    {
        const auto directory = TestRoot() / L"remote";
        LamaPon::PlayerPrefs prefs(directory / L"PlayerPrefs.json");
        auto* const address = &prefs;
        prefs.SetInteger("local", 4);
        prefs.Save();
        Documents::ApplyPlayerPrefs(prefs, Bytes(RemotePreferences));
        Require(
            &prefs == address
                && prefs.GetInteger("remote") == 9
                && !prefs.IsDirty()
                && ReadText(prefs.FilePath()) == RemotePreferences,
            "Remote PlayerPrefs apply changed identity or reserialized bytes.");

        constexpr std::string_view MaximumIntegerPreferences =
            R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":9223372036854775807}}})";
        Documents::ApplyPlayerPrefs(prefs, Bytes(MaximumIntegerPreferences));
        Require(
            prefs.GetInteger("remote")
                == (std::numeric_limits<std::int64_t>::max)(),
            "INT64_MAX PlayerPrefs did not round-trip exactly.");
        Documents::ApplyPlayerPrefs(prefs, Bytes(RemotePreferences));

        for (const auto invalid : {
                std::string(
                    R"({"format":"LamaPonPlayerPrefs","format":"duplicate","version":1,"values":{}})"),
                std::string(
                    R"({"format":"LamaPonPlayerPrefs","version":2,"values":{}})"),
                std::string(
                    R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"bad":{"type":"integer","value":"x"}}})"),
                std::string(
                    R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":9223372036854775808}}})") })
        {
            const auto before = ReadText(prefs.FilePath());
            Require(
                Throws([&]
                {
                    Documents::ApplyPlayerPrefs(prefs, Bytes(invalid));
                })
                    && ReadText(prefs.FilePath()) == before
                    && prefs.GetInteger("remote") == 9,
                "Invalid remote PlayerPrefs mutated disk or memory.");
        }
        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::BeforeFlush);
        Require(
            Throws([&]
            {
                Documents::ApplyPlayerPrefs(
                    prefs,
                    Bytes(RemotePreferencesTwo));
            })
                && prefs.GetInteger("remote") == 9
                && ReadText(prefs.FilePath()) == RemotePreferences,
            "Failed remote PlayerPrefs publish mutated disk or memory.");

        LamaPon::SaveDataStore saves(directory / L"Saves");
        saves.SaveJson("remote", R"({"level":1})");
        Documents::ApplySaveData(saves, "remote", Bytes(RemoteSave));
        Require(
            ReadText(saves.SlotPath("remote")) == RemoteSave
                && saves.LoadJson("remote")
                    == std::optional<std::string>(R"({"level":8})"),
            "Remote SaveData was wrapped twice or changed.");
        for (const auto invalid : {
                std::string(
                    R"({"format":"LamaPonSaveData","version":1,"slot":"wrong","data":{}})"),
                std::string(
                    R"({"format":"LamaPonSaveData","version":1,"slot":"remote","slot":"remote","data":{}})"),
                std::string(
                    R"({"format":"LamaPonSaveData","version":2,"slot":"remote","data":{}})" ) })
        {
            const auto before = ReadText(saves.SlotPath("remote"));
            Require(
                Throws([&]
                {
                    Documents::ApplySaveData(saves, "remote", Bytes(invalid));
                })
                    && ReadText(saves.SlotPath("remote")) == before,
                "Invalid remote SaveData mutated the existing slot.");
        }

        Documents::DeleteSaveData(saves, "remote");
        Require(
            Documents::ReadSaveData(saves, "remote").state == State::Missing,
            "Remote SaveData delete did not publish missing state.");
        WriteText(Suffix(saves.SlotPath("remote"), L".deleting"), "stale");
        Require(
            Documents::ReadSaveData(saves, "remote").state == State::Missing
                && Documents::ListSaveData(saves).slots.empty(),
            "A stale .deleting file was adopted as local data.");

        Documents::DeletePlayerPrefs(prefs);
        Require(
            &prefs == address
                && prefs.Keys().empty()
                && !prefs.IsDirty()
                && Documents::ReadPlayerPrefs(prefs).state == State::Missing,
            "Remote PlayerPrefs delete did not atomically clear disk and memory.");
    }

    void TestObserverOrderingAndOwnership()
    {
        const auto directory = TestRoot() / L"observer";
        LamaPon::PlayerPrefs prefs(directory / L"PlayerPrefs.json");
        ObserverProbe probe;
        probe.expectedSource = &prefs;
        const auto firstToken =
            LamaPon::Detail::AttachLocalPersistenceCommitObserver(
                &ObserveCommit,
                &probe,
                41u);
        prefs.SetInteger("value", 1);
        prefs.Save();
        Require(
            firstToken != 0u
                && probe.calls == 1u
                && probe.epoch == 41u
                && probe.observedCommittedState,
            "PlayerPrefs observer ran before durable commit.");

        ObserverProbe replacement;
        replacement.expectedSource = &prefs;
        replacement.returnSuccess = false;
        const auto secondToken =
            LamaPon::Detail::AttachLocalPersistenceCommitObserver(
                &ObserveCommit,
                &replacement,
                42u);
        Require(
            !LamaPon::Detail::DetachLocalPersistenceCommitObserver(firstToken),
            "A stale observer owner detached its replacement.");
        prefs.SetInteger("value", 2);
        prefs.Save();
        Require(
            replacement.calls == 1u
                && LamaPon::Detail::ConsumeLocalPersistenceObserverFailure(),
            "Observer failure changed local success or was not recorded.");

        {
            LamaPon::Detail::ScopedLocalPersistenceObserverSuppression suppress;
            prefs.SetInteger("value", 3);
            prefs.Save();
        }
        Documents::ApplyPlayerPrefs(prefs, Bytes(RemotePreferences));
        Require(
            replacement.calls == 1u
                && LamaPon::Detail::DetachLocalPersistenceCommitObserver(
                    secondToken),
            "Suppressed/remote commits emitted a local observer event.");

        LamaPon::SaveDataStore saves(directory / L"Saves");
        ObserverProbe saveProbe;
        saveProbe.expectedSource = &saves;
        const auto saveToken =
            LamaPon::Detail::AttachLocalPersistenceCommitObserver(
                &ObserveCommit,
                &saveProbe,
                43u);
        saves.SaveJson("slot", "{}");
        Require(
            saveProbe.calls == 1u && saveProbe.observedCommittedState,
            "SaveData observer ran before durable commit.");
        Require(saves.DeleteSlot("slot"), "SaveData delete fixture failed.");
        Require(
            saveProbe.calls == 2u && saveProbe.observedCommittedState
                && LamaPon::Detail::DetachLocalPersistenceCommitObserver(
                    saveToken),
            "SaveData delete observer did not see committed missing state.");
    }

    void TestHardLinksFailBeforeMutation()
    {
        const auto directory = TestRoot() / L"hard-links";
        const auto path = directory / L"PlayerPrefs.json";
        LamaPon::PlayerPrefs prefs(path);
        prefs.SetString("value", "old");
        prefs.Save();

        const auto targetAlias = directory / L"target-alias";
        Require(
            CreateHardLinkW(targetAlias.c_str(), path.c_str(), nullptr) != FALSE,
            "Target hard-link fixture could not be created.");
        prefs.SetString("value", "new");
        const auto before = ReadText(path);
        Require(
            Throws([&] { prefs.Save(); })
                && ReadText(targetAlias) == before,
            "A hard-linked final document was accepted or changed.");
        RemoveExact(targetAlias);

        const auto stagePath = Suffix(path, L".writing");
        RemoveExact(stagePath);
        const auto stageVictim = TestRoot() / L"stage-victim";
        WriteText(stageVictim, "must-not-change");
        Require(
            CreateHardLinkW(
                stagePath.c_str(),
                stageVictim.c_str(),
                nullptr) != FALSE,
            "Stage hard-link fixture could not be created.");
        Require(
            Throws([&] { prefs.Save(); })
                && ReadText(stageVictim) == "must-not-change",
            "A hard-linked stage was truncated before validation.");
        RemoveExact(stagePath);
        RemoveExact(stageVictim);

        const auto lockPath = Suffix(path, L".lock");
        RemoveExact(lockPath);
        const auto lockVictim = TestRoot() / L"lock-victim";
        WriteText(lockVictim, "must-remain-data");
        Require(
            CreateHardLinkW(
                lockPath.c_str(),
                lockVictim.c_str(),
                nullptr) != FALSE,
            "Lock hard-link fixture could not be created.");
        Require(
            Throws([&] { prefs.Save(); })
                && ReadText(lockVictim) == "must-remain-data",
            "A hard-linked persistence lock was accepted or changed.");
        RemoveExact(lockPath);
        RemoveExact(lockVictim);

        LamaPon::SaveDataStore saves(directory / L"Saves");
        saves.SaveJson("slot", R"({"value":"old"})");
        const auto savePath = saves.SlotPath("slot");
        const auto saveAlias = directory / L"save-target-alias";
        Require(
            CreateHardLinkW(
                saveAlias.c_str(),
                savePath.c_str(),
                nullptr) != FALSE,
            "Delete target hard-link fixture could not be created.");
        const auto saveBefore = ReadText(savePath);
        Require(
            Throws([&] { (void)saves.DeleteSlot("slot"); })
                && ReadText(saveAlias) == saveBefore,
            "A hard-linked delete target was removed or changed.");
        RemoveExact(saveAlias);

        const auto deletingPath = Suffix(savePath, L".deleting");
        const auto deleteVictim = TestRoot() / L"delete-victim";
        WriteText(deleteVictim, "must-not-be-deleted");
        Require(
            CreateHardLinkW(
                deletingPath.c_str(),
                deleteVictim.c_str(),
                nullptr) != FALSE,
            "Delete stage hard-link fixture could not be created.");
        Require(
            Throws([&] { (void)saves.DeleteSlot("slot"); })
                && ReadText(savePath) == saveBefore
                && ReadText(deleteVictim) == "must-not-be-deleted",
            "An unsafe delete stage changed target or victim data.");
        RemoveExact(deletingPath);
        RemoveExact(deleteVictim);
    }

    void TestAncestorReparseIsRejectedWhenSupported()
    {
        const auto outside = TestRoot() / L"reparse-target";
        LamaPon::PlayerPrefs outsidePrefs(outside / L"PlayerPrefs.json");
        outsidePrefs.SetString("secret", "outside");
        outsidePrefs.Save();
        LamaPon::SaveDataStore outsideSaves(outside / L"Saves");
        outsideSaves.SaveJson("slot", "{}");
        const auto before = ReadText(outsidePrefs.FilePath());

        const auto link = TestRoot() / L"reparse-link";
        constexpr DWORD AllowUnprivilegedCreate = 0x2u;
        if (CreateSymbolicLinkW(
                link.c_str(),
                outside.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY | AllowUnprivilegedCreate) == FALSE)
        {
            // Developer Mode/権限がないWindowsでも他の回帰は実行します。
            return;
        }
        LamaPon::PlayerPrefs linkedPrefs(link / L"PlayerPrefs.json");
        linkedPrefs.SetString("secret", "overwrite");
        LamaPon::SaveDataStore linkedSaves(link / L"Saves");
        Require(
            Documents::ReadPlayerPrefs(linkedPrefs).state == State::Unavailable
                && Documents::ListSaveData(linkedSaves).state
                    == State::Unavailable
                && Throws([&] { linkedPrefs.Save(); })
                && Throws([&]
                {
                    Documents::DeletePlayerPrefs(linkedPrefs);
                })
                && ReadText(outsidePrefs.FilePath()) == before,
            "An ancestor reparse point escaped local persistence isolation.");
        RemoveExact(link);
    }
}

int main()
{
    try
    {
        ResetRoot();
        TestReadStatesAndStrictSchema();
        TestDurableBarriersAndConcurrentWriterContract();
        TestRemoteApplyStrongGuaranteeAndIdentity();
        TestObserverOrderingAndOwnership();
        TestHardLinksFailBeforeMutation();
        TestAncestorReparseIsRejectedWhenSupported();
        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::None);
        std::error_code cleanupError;
        std::filesystem::remove_all(TestRoot(), cleanupError);
        Require(!cleanupError, "Local persistence final cleanup failed.");
        std::cout << "Local persistence document tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
