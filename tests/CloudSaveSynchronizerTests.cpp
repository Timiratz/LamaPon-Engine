#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Online/CloudSaveClient.h"
#include "LamaPon/Online/CloudSaveJournal.h"
#include "LamaPon/Online/CloudSaveSynchronizer.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using Documents = LamaPon::Detail::LocalPersistenceDocuments;
    using Journal = LamaPon::Detail::CloudSaveJournal;
    using Synchronizer = LamaPon::Detail::CloudSaveSynchronizer;

    constexpr std::string_view AccountKey =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    constexpr std::string_view Token = "backend-access-token";
    constexpr std::array<std::string_view, 8> MutationIds{
        "123e4567-e89b-42d3-a456-426614174000",
        "123e4567-e89b-42d3-b456-426614174001",
        "123e4567-e89b-42d3-8456-426614174002",
        "123e4567-e89b-42d3-9456-426614174003",
        "123e4567-e89b-42d3-a456-426614174004",
        "123e4567-e89b-42d3-b456-426614174005",
        "123e4567-e89b-42d3-8456-426614174006",
        "123e4567-e89b-42d3-9456-426614174007"
    };

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::filesystem::path TestRoot()
    {
        const auto root = std::filesystem::absolute(
            std::filesystem::current_path()
            / L"test-output"
            / L"cloud-save-synchronizer").lexically_normal();
        Require(
            root.parent_path().filename() == L"test-output",
            "Synchronizer test root escaped test-output.");
        return root;
    }

    void ResetTestRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(TestRoot(), error);
        Require(!error, "Synchronizer test cleanup failed.");
        std::filesystem::create_directories(TestRoot(), error);
        Require(!error, "Synchronizer test root creation failed.");
    }

    std::filesystem::path TrustedPath(const std::string_view name)
    {
        return TestRoot()
            / std::filesystem::path(name)
            / L"GuestDisplayName";
    }

    LamaPon::PersistenceProfilePaths Profile(
        const std::filesystem::path& trusted)
    {
        const auto root = trusted.parent_path()
            / L"OnlineProfiles"
            / std::filesystem::path(AccountKey);
        return {
            root,
            root / L"PlayerPrefs.json",
            root / L"Saves",
            std::string(AccountKey),
            false
        };
    }

    std::vector<std::uint8_t> Bytes(const std::string_view text)
    {
        return {
            reinterpret_cast<const std::uint8_t*>(text.data()),
            reinterpret_cast<const std::uint8_t*>(text.data() + text.size())
        };
    }

    constexpr char Base64UrlAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string Base64Url(
        const std::uint8_t* const bytes,
        const std::size_t size)
    {
        std::string result;
        result.reserve((size * 4u + 2u) / 3u);
        std::size_t index{};
        while (index + 3u <= size)
        {
            const auto value =
                (static_cast<std::uint32_t>(bytes[index]) << 16u)
                | (static_cast<std::uint32_t>(bytes[index + 1u]) << 8u)
                | bytes[index + 2u];
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 6u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[value & 0x3fu]);
            index += 3u;
        }
        if (size - index == 1u)
        {
            const auto value =
                static_cast<std::uint32_t>(bytes[index]) << 16u;
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
        }
        else if (size - index == 2u)
        {
            const auto value =
                (static_cast<std::uint32_t>(bytes[index]) << 16u)
                | (static_cast<std::uint32_t>(bytes[index + 1u]) << 8u);
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 6u) & 0x3fu]);
        }
        return result;
    }

    std::string Hash(const std::vector<std::uint8_t>& bytes)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        Require(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0u) >= 0,
            "SHA-256 provider failed.");
        std::array<std::uint8_t, 32> digest{};
        const auto status = BCryptHash(
            algorithm,
            nullptr,
            0u,
            const_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()),
            digest.data(),
            static_cast<ULONG>(digest.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0u);
        Require(status >= 0, "SHA-256 failed.");
        return Base64Url(digest.data(), digest.size());
    }

    Json ResourceJson(const LamaPon::CloudSaveResource& resource)
    {
        if (resource.kind == LamaPon::CloudSaveResourceKind::Preferences)
        {
            return { { "kind", "preferences" } };
        }
        return {
            { "kind", "save_slot" },
            { "slot", resource.slot }
        };
    }

    Json SnapshotJson(
        const LamaPon::CloudSaveSnapshot& snapshot,
        const bool includeProtocolVersion = true)
    {
        Json result{
            { "resource", ResourceJson(snapshot.resource) },
            { "etag", snapshot.etag },
            { "deleted", snapshot.deleted },
            { "byteLength", snapshot.content.size() }
        };
        if (!snapshot.deleted)
        {
            result["sha256"] = snapshot.sha256;
            result["content"] = Base64Url(
                snapshot.content.data(),
                snapshot.content.size());
        }
        if (includeProtocolVersion)
        {
            result["protocolVersion"] = 1;
        }
        return result;
    }

    Json ManifestItemJson(const LamaPon::CloudSaveSnapshot& snapshot)
    {
        Json result{
            { "resource", ResourceJson(snapshot.resource) },
            { "etag", snapshot.etag },
            { "deleted", snapshot.deleted },
            { "byteLength", snapshot.content.size() }
        };
        if (!snapshot.deleted)
        {
            result["sha256"] = snapshot.sha256;
        }
        return result;
    }

    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const Json& json,
        const std::string_view etag = {})
    {
        LamaPon::HttpResponse response;
        response.statusCode = status;
        const auto text = json.dump();
        response.body.assign(text.begin(), text.end());
        response.headers.emplace_back(
            L"Content-Type",
            L"application/json; charset=utf-8");
        response.headers.emplace_back(
            L"Content-Length",
            std::to_wstring(response.body.size()));
        if (!etag.empty())
        {
            response.headers.emplace_back(
                L"ETag",
                std::wstring(etag.begin(), etag.end()));
        }
        return response;
    }

    LamaPon::HttpResponse ManifestResponse(
        const std::vector<LamaPon::CloudSaveSnapshot>& snapshots)
    {
        Json items = Json::array();
        for (const auto& snapshot : snapshots)
        {
            items.push_back(ManifestItemJson(snapshot));
        }
        return JsonResponse(
            200u,
            { { "protocolVersion", 1 }, { "items", std::move(items) } });
    }

    LamaPon::HttpResponse SnapshotResponse(
        const std::uint32_t status,
        const LamaPon::CloudSaveSnapshot& snapshot)
    {
        return JsonResponse(
            status,
            SnapshotJson(snapshot),
            snapshot.etag);
    }

    LamaPon::HttpResponse ConflictResponse(
        const LamaPon::CloudSaveSnapshot& snapshot)
    {
        return JsonResponse(
            412u,
            {
                { "protocolVersion", 1 },
                { "error", { { "code", "revision_conflict" } } },
                { "current", SnapshotJson(snapshot, false) }
            },
            snapshot.etag);
    }

    using Handler =
        std::function<LamaPon::HttpResponse(const LamaPon::HttpRequest&)>;

    struct ScriptedBackend final
    {
        void Push(Handler handler)
        {
            std::scoped_lock lock(mutex);
            handlers.push_back(std::move(handler));
        }

        LamaPon::HttpResponse Send(const LamaPon::HttpRequest& request)
        {
            Handler handler;
            {
                std::scoped_lock lock(mutex);
                requests.push_back(request);
                if (handlers.empty())
                {
                    LamaPon::HttpResponse response;
                    response.transportError = "unexpected fake request";
                    return response;
                }
                handler = std::move(handlers.front());
                handlers.pop_front();
            }
            return handler(request);
        }

        [[nodiscard]] std::vector<LamaPon::HttpRequest> Requests() const
        {
            std::scoped_lock lock(mutex);
            return requests;
        }

        mutable std::mutex mutex;
        std::deque<Handler> handlers;
        std::vector<LamaPon::HttpRequest> requests;
    };

    std::shared_ptr<LamaPon::Detail::CloudSaveClient> MakeClient(
        ScriptedBackend& backend)
    {
        return std::make_shared<LamaPon::Detail::CloudSaveClient>(
            "https://online.example.test/tenant/",
            "sync-game",
            "staging",
            false,
            [&backend](const LamaPon::HttpRequest& request)
            {
                return backend.Send(request);
            });
    }

    std::string RequestText(const LamaPon::HttpRequest& request)
    {
        return { request.body.begin(), request.body.end() };
    }

    std::optional<std::wstring> RequestHeader(
        const LamaPon::HttpRequest& request,
        const std::wstring_view name)
    {
        for (const auto& [headerName, value] : request.headers)
        {
            if (_wcsicmp(headerName.c_str(), std::wstring(name).c_str()) == 0)
            {
                return value;
            }
        }
        return std::nullopt;
    }

    LamaPon::CloudSaveSnapshot SnapshotFromPut(
        const LamaPon::HttpRequest& request,
        std::string etag)
    {
        const auto body = Json::parse(RequestText(request));
        LamaPon::CloudSaveResource resource =
            body["resource"]["kind"] == "preferences"
            ? LamaPon::CloudSaveResource::Preferences()
            : LamaPon::CloudSaveResource::SaveSlot(
                body["resource"]["slot"].get<std::string>());

        // Test server only decodes the canonical base64url emitted by client.
        const auto encoded = body["content"].get<std::string>();
        auto value = [](const unsigned char character) -> int
        {
            if (character >= 'A' && character <= 'Z') return character - 'A';
            if (character >= 'a' && character <= 'z') return character - 'a' + 26;
            if (character >= '0' && character <= '9') return character - '0' + 52;
            if (character == '-') return 62;
            if (character == '_') return 63;
            return -1;
        };
        std::vector<std::uint8_t> content;
        std::uint32_t accumulator{};
        int bits{};
        for (const unsigned char character : encoded)
        {
            accumulator = (accumulator << 6u)
                | static_cast<std::uint32_t>(value(character));
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                content.push_back(static_cast<std::uint8_t>(
                    accumulator >> bits));
                accumulator &= bits == 0
                    ? 0u
                    : (1u << bits) - 1u;
            }
        }
        return {
            std::move(resource),
            std::move(etag),
            false,
            std::move(content),
            body["sha256"].get<std::string>()
        };
    }

    LamaPon::CloudSaveSnapshot LiveSnapshot(
        LamaPon::CloudSaveResource resource,
        std::string etag,
        std::vector<std::uint8_t> content)
    {
        const auto hash = Hash(content);
        return {
            std::move(resource),
            std::move(etag),
            false,
            std::move(content),
            hash
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

    template<class Predicate>
    void PumpUntil(
        Synchronizer& synchronizer,
        Predicate predicate,
        std::uint64_t& now)
    {
        for (std::size_t attempt = 0; attempt < 5000u; ++attempt)
        {
            synchronizer.Tick(now);
            now += 10u;
            if (predicate())
            {
                return;
            }
            Sleep(1u);
        }
        throw std::runtime_error("Synchronizer test timed out.");
    }

    LamaPon::Detail::CloudSaveMutationIdGenerator IdGenerator(
        std::shared_ptr<std::size_t> index)
    {
        return [index]
        {
            Require(*index < MutationIds.size(), "Mutation IDs exhausted.");
            return std::string(MutationIds[(*index)++]);
        };
    }

    void DrainDetached(Synchronizer& synchronizer, std::uint64_t& now)
    {
        synchronizer.Detach();
        PumpUntil(
            synchronizer,
            [&synchronizer]
            {
                return !synchronizer.HasInFlightRequest();
            },
            now);
    }

    void TestPendingOverlayChainsAfterAck()
    {
        const auto trusted = TrustedPath("overlay");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        preferences.SetInteger("progress", 1);
        preferences.Save();
        const auto p1 = Documents::ReadPlayerPrefs(preferences).bytes;

        ScriptedBackend backend;
        backend.Push([](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({});
        });

        struct Gate final
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool entered{};
            bool release{};
        };
        auto gate = std::make_shared<Gate>();
        backend.Push([gate](const LamaPon::HttpRequest& request)
        {
            {
                std::unique_lock lock(gate->mutex);
                gate->entered = true;
                gate->condition.notify_all();
                gate->condition.wait(lock, [&] { return gate->release; });
            }
            const auto snapshot = SnapshotFromPut(request, "\"p1\"");
            return SnapshotResponse(201u, snapshot);
        });
        auto p2Remote = std::make_shared<
            std::optional<LamaPon::CloudSaveSnapshot>>();
        auto p2RemoteMutex = std::make_shared<std::mutex>();
        backend.Push([p2Remote, p2RemoteMutex](
            const LamaPon::HttpRequest& request)
        {
            const auto snapshot = SnapshotFromPut(request, "\"p2\"");
            {
                std::scoped_lock lock(*p2RemoteMutex);
                *p2Remote = snapshot;
            }
            return SnapshotResponse(200u, snapshot);
        });
        backend.Push([p2Remote, p2RemoteMutex](const LamaPon::HttpRequest&)
        {
            std::scoped_lock lock(*p2RemoteMutex);
            Require(p2Remote->has_value(), "P2 response was not recorded.");
            return ManifestResponse({ **p2Remote });
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 7u, std::string(AccountKey), std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);

        preferences.SetInteger("progress", 2);
        preferences.Save();
        const auto p2 = Documents::ReadPlayerPrefs(preferences).bytes;
        Require(p1 != p2, "P2 fixture did not change local bytes.");
        {
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();

        PumpUntil(
            synchronizer,
            [&]
            {
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences());
                return baseline
                    && baseline->etag == "\"p2\""
                    && baseline->content == p2
                    && !journal.Pending(
                        LamaPon::CloudSaveResource::Preferences())
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        const auto requests = backend.Requests();
        Require(requests.size() == 4u, "P1/P2 made an unexpected wire request.");
        Require(
            requests[0].method == L"GET"
                && requests[1].method == L"PUT"
                && requests[2].method == L"PUT"
                && requests[3].method == L"GET",
            "P1/P2 request order was not manifest, P1, P2.");
        Require(
            RequestHeader(requests[1], L"If-None-Match") == L"*"
                && RequestHeader(requests[2], L"If-Match") == L"\"p1\""
                && RequestHeader(requests[1], L"Idempotency-Key")
                    != RequestHeader(requests[2], L"Idempotency-Key"),
            "P2 did not use a fresh UUID and the P1 ETag.");
        for (const auto& request : requests)
        {
            Require(
                RequestText(request).find(AccountKey) == std::string::npos
                    && request.url.find(std::wstring(
                        AccountKey.begin(),
                        AccountKey.end())) == std::wstring::npos,
                "Account storage key escaped into the cloud request.");
        }
        DrainDetached(synchronizer, now);
    }

    void TestPendingRetryKeepsMutationIdentity()
    {
        const auto trusted = TrustedPath("pending-retry");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        preferences.SetInteger("retry", 1);
        preferences.Save();

        ScriptedBackend backend;
        backend.Push([](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({});
        });
        backend.Push([](const LamaPon::HttpRequest&)
        {
            LamaPon::HttpResponse response;
            response.statusCode = 503u;
            return response;
        });
        auto succeeded = std::make_shared<
            std::optional<LamaPon::CloudSaveSnapshot>>();
        backend.Push([succeeded](const LamaPon::HttpRequest& request)
        {
            *succeeded = SnapshotFromPut(request, "\"retry-ok\"");
            return SnapshotResponse(201u, **succeeded);
        });
        backend.Push([succeeded](const LamaPon::HttpRequest&)
        {
            Require(succeeded->has_value(), "Retry snapshot missing.");
            return ManifestResponse({ **succeeded });
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 9u, std::string(AccountKey), std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                    == LamaPon::Detail::CloudSaveSynchronizerState::BackingOff;
            },
            now);
        const auto retryAt = synchronizer.Status().retryAtMilliseconds;
        synchronizer.Tick(retryAt - 1u);
        Require(
            backend.Requests().size() == 2u,
            "Pending mutation retried before backoff expired.");
        now = retryAt;
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        const auto requests = backend.Requests();
        Require(
            requests.size() == 4u
                && RequestHeader(requests[1], L"Idempotency-Key")
                    == RequestHeader(requests[2], L"Idempotency-Key")
                && requests[1].body == requests[2].body,
            "Pending retry changed mutation ID or content.");
        DrainDetached(synchronizer, now);
    }

    void TestRemoteDeleteThenLocalRecreationBecomesPut()
    {
        const auto trusted = TrustedPath("delete-recreate");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        preferences.SetInteger("progress", 10);
        preferences.Save();
        const auto oldDocument = Documents::ReadPlayerPrefs(preferences).bytes;
        journal.RecordBaseline(LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"old\"",
            oldDocument));
        const auto deleted = Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"deleted\"");

        ScriptedBackend backend;
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });
        auto client = MakeClient(backend);
        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            client,
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 11u, std::string(AccountKey), std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences());
                return baseline && baseline->deleted
                    && Documents::ReadPlayerPrefs(preferences).state
                        == LamaPon::Detail::LocalPersistenceDocumentState::Missing
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        preferences.SetInteger("progress", 20);
        preferences.Save();
        const auto recreated = Documents::ReadPlayerPrefs(preferences).bytes;

        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });
        auto remoteRecreated = std::make_shared<
            std::optional<LamaPon::CloudSaveSnapshot>>();
        backend.Push([remoteRecreated](const LamaPon::HttpRequest& request)
        {
            *remoteRecreated = SnapshotFromPut(request, "\"recreated\"");
            return SnapshotResponse(200u, **remoteRecreated);
        });
        backend.Push([remoteRecreated](const LamaPon::HttpRequest&)
        {
            Require(remoteRecreated->has_value(), "Recreated snapshot missing.");
            return ManifestResponse({ **remoteRecreated });
        });
        synchronizer.RequestReconcile();

        PumpUntil(
            synchronizer,
            [&]
            {
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences());
                return baseline
                    && baseline->etag == "\"recreated\""
                    && baseline->content == recreated
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        const auto requests = backend.Requests();
        Require(requests.size() == 4u, "Delete/recreate request count changed.");
        Require(
            requests[2].method == L"PUT"
                && RequestHeader(requests[2], L"If-Match")
                    == L"\"deleted\"",
            "Local recreation after tombstone was not a CAS Put.");
        DrainDetached(synchronizer, now);
    }

    void TestRemoteSlotCaseUsesExistingLocalSpelling()
    {
        const auto trusted = TrustedPath("slot-case-identity");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        saves.SaveJson("case-slot", R"({"value":1})");
        const auto local = Documents::ReadSaveData(saves, "case-slot");
        Require(
            local.state
                == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
            "Case fixture local slot was not readable.");
        journal.RecordBaseline(LiveSnapshot(
            LamaPon::CloudSaveResource::SaveSlot("case-slot"),
            "\"case-base\"",
            local.bytes));

        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::SaveSlot("Case-Slot"),
            "\"case-remote\"",
            Bytes(R"({"format":"LamaPonSaveData","version":1,"slot":"Case-Slot","data":{"value":2}})"));
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest& request)
        {
            Require(
                request.method == L"POST",
                "Case-different remote slot was not read.");
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            12u,
            std::string(AccountKey),
            std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::SaveSlot("case-slot"));
                return baseline
                    && baseline->etag == "\"case-remote\""
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        const auto payload = saves.LoadJson("case-slot");
        const auto slots = saves.ListSlots();
        Require(
            payload
                && Json::parse(*payload).at("value") == 2
                && slots.size() == 1u
                && slots.front() == "case-slot"
                && std::filesystem::is_regular_file(
                    saves.SlotPath("case-slot")),
            "Remote case change created an alias or broke public LoadJson.");
        DrainDetached(synchronizer, now);
    }

    void TestCasConflictRetryAndUseRemote()
    {
        const auto trusted = TrustedPath("conflict");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        preferences.SetInteger("progress", 1);
        preferences.Save();
        const auto baseDocument = Documents::ReadPlayerPrefs(preferences).bytes;
        journal.RecordBaseline(LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"base\"",
            baseDocument));
        preferences.SetInteger("progress", 2);
        preferences.Save();

        const auto remoteDocument = Bytes(
            R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":99}}})");
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote\"",
            remoteDocument);

        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ConflictResponse(remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 13u, std::string(AccountKey), std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && journal.Conflict(
                        LamaPon::CloudSaveResource::Preferences()).has_value()
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        const auto firstPending = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        Require(firstPending.has_value(), "Conflict lost its pending mutation.");
        synchronizer.ResolveConflict(
            LamaPon::CloudSaveResource::Preferences(),
            firstPending->mutationId,
            LamaPon::Detail::CloudSaveConflictResolution::RetryLocal);
        const auto retryPending = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        Require(
            retryPending && retryPending->mutationId != firstPending->mutationId,
            "RetryLocal reused the stale mutation ID.");

        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ConflictResponse(remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        PumpUntil(
            synchronizer,
            [&]
            {
                const auto pending = journal.Pending(
                    LamaPon::CloudSaveResource::Preferences());
                return pending && journal.Conflict(
                        LamaPon::CloudSaveResource::Preferences())
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        const auto finalPending = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        synchronizer.ResolveConflict(
            LamaPon::CloudSaveResource::Preferences(),
            finalPending->mutationId,
            LamaPon::Detail::CloudSaveConflictResolution::UseRemote);
        Require(
            Documents::ReadPlayerPrefs(preferences).bytes == remoteDocument
                && !journal.Pending(
                    LamaPon::CloudSaveResource::Preferences())
                && !journal.Conflict(
                    LamaPon::CloudSaveResource::Preferences())
                && journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences())->etag
                    == "\"remote\"",
            "UseRemote did not apply local document before resolving journal.");
        DrainDetached(synchronizer, now);
    }

    void TestInitialDualContentRequiresConflict()
    {
        const auto trusted = TrustedPath("initial-conflict");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        preferences.SetInteger("local", 1);
        preferences.Save();
        const auto local = Documents::ReadPlayerPrefs(preferences).bytes;
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote-first\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":2}}})"));

        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 17u, std::string(AccountKey), std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        const auto pending = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        Require(
            pending && pending->content == local
                && journal.Conflict(
                    LamaPon::CloudSaveResource::Preferences())->content
                    == remote.content,
            "Initial local/remote content was not retained as a conflict.");
        const auto requests = backend.Requests();
        Require(
            requests.size() == 3u
                && requests[0].method == L"GET"
                && requests[1].method == L"POST"
                && requests[2].method == L"GET",
            "Initial conflict uploaded or skipped full remote validation.");
        DrainDetached(synchronizer, now);
    }

    void TestLocalCommitInvalidatesActiveRead()
    {
        const auto trusted = TrustedPath("read-invalidation");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote-read\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":8}}})"));
        struct Gate final
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool entered{};
            bool release{};
        };
        auto gate = std::make_shared<Gate>();
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([gate, remote](const LamaPon::HttpRequest&)
        {
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 18u, std::string(AccountKey), std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);

        preferences.SetInteger("created-during-read", 44);
        preferences.Save();
        const auto local = Documents::ReadPlayerPrefs(preferences).bytes;
        synchronizer.RequestReconcile();
        {
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        Require(
            Documents::ReadPlayerPrefs(preferences).bytes == local
                && journal.Pending(
                    LamaPon::CloudSaveResource::Preferences())->content == local
                && journal.Conflict(
                    LamaPon::CloudSaveResource::Preferences())->content
                    == remote.content,
            "An active Read overwrote a newer local commit.");
        const auto requests = backend.Requests();
        Require(
            requests.size() == 5u
                && requests[0].method == L"GET"
                && requests[1].method == L"POST"
                && requests[2].method == L"GET"
                && requests[3].method == L"POST"
                && requests[4].method == L"GET",
            "Invalidated Read was not discarded and reconciled afresh.");
        DrainDetached(synchronizer, now);
    }

    void TestDeleteDuringInitialReadIsNotInitialMissing()
    {
        const auto trusted = TrustedPath("delete-during-read");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        preferences.SetInteger("delete-me", 1);
        preferences.Save();
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote-before-delete\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":7}}})"));
        const auto deleted = Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote-deleted\"");
        struct Gate final
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool entered{};
            bool release{};
        };
        auto gate = std::make_shared<Gate>();
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([gate, remote](const LamaPon::HttpRequest&)
        {
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
            return SnapshotResponse(200u, remote);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, deleted);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 20u, std::string(AccountKey), std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);
        Documents::DeletePlayerPrefs(preferences);
        synchronizer.RequestReconcile();
        const auto durableDelete = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        Require(
            durableDelete
                && durableDelete->kind
                    == LamaPon::Detail::CloudSavePendingKind::Delete
                && durableDelete->baseEtag == "\"remote-before-delete\"",
            "Delete during Read was not write-ahead journaled immediately.");
        {
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();
        PumpUntil(
            synchronizer,
            [&]
            {
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences());
                return baseline && baseline->deleted
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        const auto requests = backend.Requests();
        Require(
            requests.size() == 4u
                && requests[0].method == L"GET"
                && requests[1].method == L"POST"
                && requests[2].method == L"DELETE"
                && requests[3].method == L"GET"
                && RequestHeader(requests[2], L"If-Match")
                    == L"\"remote-before-delete\"",
            "Delete during initial Read was mistaken for initial Missing.");
        DrainDetached(synchronizer, now);
    }

    void TestCorruptLocalStopsBeforeNetwork()
    {
        const auto trusted = TrustedPath("corrupt-local");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        preferences.SetInteger("valid", 1);
        preferences.Save();
        {
            std::ofstream output(
                profile.playerPrefsFile,
                std::ios::binary | std::ios::trunc);
            Require(output.good(), "Corrupt fixture open failed.");
            output << "{not-json";
        }

        ScriptedBackend backend;
        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        bool rejected{};
        try
        {
            synchronizer.Attach(
                journal,
                19u,
                std::string(AccountKey),
                std::string(Token));
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        Require(
            rejected
                && !synchronizer.IsAttached()
                && backend.Requests().empty()
                && journal.Generation() == 0u,
            "Corrupt local persistence passed prepared attachment.");
        std::uint64_t now{};
        Documents::DeletePlayerPrefs(preferences);
        backend.Push([](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({});
        });
        synchronizer.Attach(
            journal,
            20u,
            std::string(AccountKey),
            std::string(Token));
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        Require(
            backend.Requests().size() == 1u,
            "Explicit retry did not recover a repaired local document.");
        DrainDetached(synchronizer, now);
    }

    void TestUnauthorizedRefreshAndRateLimitBackoff()
    {
        {
            const auto trusted = TrustedPath("unauthorized");
            const auto profile = Profile(trusted);
            LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
            LamaPon::SaveDataStore saves(profile.saveDataDirectory);
            Journal journal(
                trusted,
                profile,
                "sync-game",
                "staging",
                "https://online.example.test/tenant/");
            ScriptedBackend backend;
            backend.Push([](const LamaPon::HttpRequest&)
            {
                LamaPon::HttpResponse response;
                response.statusCode = 401u;
                return response;
            });
            backend.Push([](const LamaPon::HttpRequest&)
            {
                return ManifestResponse({});
            });
            auto idIndex = std::make_shared<std::size_t>(0u);
            Synchronizer synchronizer(
                preferences,
                saves,
                MakeClient(backend),
                IdGenerator(idIndex));
            synchronizer.Attach(
                journal,
                23u,
                std::string(AccountKey),
                std::string(Token));
            std::uint64_t now{};
            PumpUntil(
                synchronizer,
                [&]
                {
                    return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Unauthorized;
                },
                now);
            synchronizer.Tick(now + 100000u);
            Require(
                backend.Requests().size() == 1u,
                "Unauthorized response was retried automatically.");
            synchronizer.UpdateAccessToken("refreshed-backend-token");
            PumpUntil(
                synchronizer,
                [&]
                {
                    return synchronizer.Status().state
                            == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                        && !synchronizer.HasInFlightRequest();
                },
                now);
            Require(
                backend.Requests().size() == 2u,
                "Token refresh did not resume synchronization exactly once.");
            DrainDetached(synchronizer, now);
        }

        {
            const auto trusted = TrustedPath("rate-limit");
            const auto profile = Profile(trusted);
            LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
            LamaPon::SaveDataStore saves(profile.saveDataDirectory);
            Journal journal(
                trusted,
                profile,
                "sync-game",
                "staging",
                "https://online.example.test/tenant/");
            ScriptedBackend backend;
            backend.Push([](const LamaPon::HttpRequest&)
            {
                LamaPon::HttpResponse response;
                response.statusCode = 429u;
                response.headers.emplace_back(L"Retry-After", L"2");
                return response;
            });
            backend.Push([](const LamaPon::HttpRequest&)
            {
                return ManifestResponse({});
            });
            auto idIndex = std::make_shared<std::size_t>(0u);
            Synchronizer synchronizer(
                preferences,
                saves,
                MakeClient(backend),
                IdGenerator(idIndex));
            synchronizer.Attach(
                journal,
                29u,
                std::string(AccountKey),
                std::string(Token));
            std::uint64_t now{};
            PumpUntil(
                synchronizer,
                [&]
                {
                    return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::BackingOff;
                },
                now);
            const auto retryAt = synchronizer.Status().retryAtMilliseconds;
            synchronizer.Tick(retryAt - 1u);
            Require(
                backend.Requests().size() == 1u,
                "Rate-limited request ignored its retry deadline.");
            now = retryAt;
            PumpUntil(
                synchronizer,
                [&]
                {
                    return synchronizer.Status().state
                            == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                        && !synchronizer.HasInFlightRequest();
                },
                now);
            Require(
                backend.Requests().size() == 2u,
                "Rate-limited request did not resume once.");
            DrainDetached(synchronizer, now);
        }
    }

    void TestJournalBusyDuringActiveReadRetriesWithoutHalting()
    {
        const auto trusted = TrustedPath("journal-busy-read");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        preferences.SetInteger("local", 1);
        preferences.Save();
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"busy-remote\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":2}}})"));
        const auto deleted = Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"busy-deleted\"");
        struct Gate final
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool entered{};
            bool release{};
        };
        auto gate = std::make_shared<Gate>();
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([gate, remote](const LamaPon::HttpRequest&)
        {
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
            return SnapshotResponse(200u, remote);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, deleted);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            31u,
            std::string(AccountKey),
            std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);

        Documents::DeletePlayerPrefs(preferences);
        const auto lockPath =
            journal.FilePath().parent_path() / L"CloudSaveJournal.lock";
        const auto held = CreateFileW(
            lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(held != INVALID_HANDLE_VALUE, "Journal busy fixture failed.");
        std::cout << "[cloud-sync] busy acquired" << std::endl;
        synchronizer.RequestReconcile();
        std::cout << "[cloud-sync] busy request returned" << std::endl;
        const bool enteredBackoff = synchronizer.Status().state
            == LamaPon::Detail::CloudSaveSynchronizerState::BackingOff;
        CloseHandle(held);
        std::cout << "[cloud-sync] busy released" << std::endl;
        {
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();
        std::cout << "[cloud-sync] read released" << std::endl;
        Require(
            enteredBackoff,
            "A transient journal lock did not enter bounded backoff.");
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        std::cout << "[cloud-sync] busy idle" << std::endl;
        const auto requests = backend.Requests();
        Require(
            requests.size() == 4u
                && requests[0].method == L"GET"
                && requests[1].method == L"POST"
                && requests[2].method == L"DELETE"
                && requests[3].method == L"GET"
                && !journal.HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences())
                && synchronizer.Status().state
                    != LamaPon::Detail::CloudSaveSynchronizerState::Halted,
            "Active Read journal contention lost the delete or halted sync.");
        DrainDetached(synchronizer, now);
    }

    void TestQuotaSwapShrinksBeforeGrowth()
    {
        const auto trusted = TrustedPath("quota-swap-order");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        auto saveAndSnapshot = [&saves, &journal](
            const std::string& slot,
            const std::size_t blobBytes,
            const std::string& etag)
        {
            const auto payload = std::string("{\"blob\":\"")
                + std::string(blobBytes, 'x') + "\"}";
            saves.SaveJson(slot, payload);
            const auto document = Documents::ReadSaveData(saves, slot);
            Require(
                document.state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
                "Quota fixture SaveData was not readable.");
            auto snapshot = LiveSnapshot(
                LamaPon::CloudSaveResource::SaveSlot(slot),
                etag,
                document.bytes);
            journal.RecordBaseline(snapshot);
            return snapshot;
        };

        std::vector<LamaPon::CloudSaveSnapshot> remote;
        std::uint64_t remoteTotal{};
        for (std::size_t index = 0u; index < 15u; ++index)
        {
            const auto slot = "filler-" + std::to_string(index);
            const auto resource =
                LamaPon::CloudSaveResource::SaveSlot(slot);
            auto snapshot = LiveSnapshot(
                resource,
                "\"filler-" + std::to_string(index) + "\"",
                Bytes(Json{
                    { "format", "LamaPonSaveData" },
                    { "version", 1 },
                    { "slot", slot },
                    {
                        "data",
                        { { "blob", std::string(1'019'000u, 'x') } }
                    }
                }.dump()));
            // fillerはremote側のquotaを占有するだけです。contentを含む
            // baselineを15回再serializeせず、local Missingと一致する小さな
            // tombstoneを記録してremote->local growthとして分類します。
            journal.RecordBaseline(Tombstone(
                resource,
                "\"filler-base-" + std::to_string(index) + "\""));
            remoteTotal += snapshot.content.size();
            remote.push_back(std::move(snapshot));
        }
        auto growBaseline = saveAndSnapshot(
            "a-grow", 1u, "\"grow-base\"");
        auto shrinkBaseline = saveAndSnapshot(
            "z-shrink", 800'000u, "\"shrink-base\"");
        remoteTotal += growBaseline.content.size()
            + shrinkBaseline.content.size();
        remote.push_back(growBaseline);
        remote.push_back(shrinkBaseline);

        saves.SaveJson(
            "a-grow",
            std::string("{\"blob\":\"")
                + std::string(800'000u, 'g') + "\"}");
        const auto grownLocal = Documents::ReadSaveData(saves, "a-grow");
        Require(saves.DeleteSlot("z-shrink"), "Quota shrink delete failed.");
        std::uint64_t localTotal{};
        const auto listing = Documents::ListSaveData(saves);
        for (const auto& slot : listing.slots)
        {
            localTotal += Documents::ReadSaveData(saves, slot).bytes.size();
        }
        const auto growth = grownLocal.bytes.size() - growBaseline.content.size();
        Require(
            remoteTotal <= LamaPon::CloudSaveAccountMaxBytes
                && localTotal <= LamaPon::CloudSaveAccountMaxBytes
                && remoteTotal + growth > LamaPon::CloudSaveAccountMaxBytes,
            "Quota swap fixture did not exercise transient overflow.");

        const auto remoteAfterShrink = [&]
        {
            auto value = remote;
            for (auto& item : value)
            {
                if (item.resource.slot == "z-shrink")
                {
                    item = Tombstone(item.resource, "\"shrink-done\"");
                }
            }
            return value;
        }();
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse(remote);
        });
        backend.Push([](const LamaPon::HttpRequest& request)
        {
            const auto body = Json::parse(RequestText(request));
            Require(
                request.method == L"DELETE"
                    && body.at("resource").at("slot") == "z-shrink",
                "Quota swap did not send the remote shrink first.");
            return SnapshotResponse(
                200u,
                Tombstone(
                    LamaPon::CloudSaveResource::SaveSlot("z-shrink"),
                    "\"shrink-done\""));
        });
        backend.Push([remoteAfterShrink](const LamaPon::HttpRequest&)
        {
            return ManifestResponse(remoteAfterShrink);
        });
        backend.Push([](const LamaPon::HttpRequest& request)
        {
            const auto body = Json::parse(RequestText(request));
            Require(
                request.method == L"PUT"
                    && body.at("resource").at("slot") == "a-grow",
                "Quota swap did not defer remote growth.");
            return SnapshotResponse(
                200u,
                SnapshotFromPut(request, "\"grow-done\""));
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            32u,
            std::string(AccountKey),
            std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return backend.Requests().size() >= 4u;
            },
            now);
        const auto requests = backend.Requests();
        Require(
            requests.size() >= 4u
                && requests[1].method == L"DELETE"
                && requests[3].method == L"PUT",
            "Quota-safe shrink/growth wire ordering regressed.");
        DrainDetached(synchronizer, now);
    }

    void TestDeleteIntentDuringManifestSurvivesRestart()
    {
        const auto trusted = TrustedPath("delete-restart");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        preferences.SetInteger("delete-before-etag", 1);
        preferences.Save();
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"manifest-live\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":5}}})"));
        const auto deleted = Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"manifest-deleted\"");
        struct Gate final
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool entered{};
            bool release{};
        };
        auto gate = std::make_shared<Gate>();
        std::uint64_t now{};
        {
            Journal journal(
                trusted,
                profile,
                "sync-game",
                "staging",
                "https://online.example.test/tenant/");
            ScriptedBackend firstBackend;
            firstBackend.Push([gate, remote](const LamaPon::HttpRequest&)
            {
                std::unique_lock lock(gate->mutex);
                gate->entered = true;
                gate->condition.notify_all();
                gate->condition.wait(lock, [&] { return gate->release; });
                return ManifestResponse({ remote });
            });
            auto idIndex = std::make_shared<std::size_t>(0u);
            Synchronizer first(
                preferences,
                saves,
                MakeClient(firstBackend),
                IdGenerator(idIndex));
            first.Attach(
                journal,
                33u,
                std::string(AccountKey),
                std::string(Token));
            PumpUntil(
                first,
                [&]
                {
                    std::scoped_lock lock(gate->mutex);
                    return gate->entered;
                },
                now);
            Documents::DeletePlayerPrefs(preferences);
            first.RequestReconcile();
            Require(
                journal.HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences()),
                "Active Manifest did not durably record the local delete intent.");
            first.Detach();
            {
                std::scoped_lock lock(gate->mutex);
                gate->release = true;
            }
            gate->condition.notify_all();
            PumpUntil(first, [&] { return !first.HasInFlightRequest(); }, now);
        }

        Journal reopened(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        Require(
            reopened.HasLocalDeleteIntent(
                LamaPon::CloudSaveResource::Preferences()),
            "Local delete intent was lost across restart.");
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, deleted);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });
        auto idIndex = std::make_shared<std::size_t>(1u);
        Synchronizer second(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        second.Attach(
            reopened,
            34u,
            std::string(AccountKey),
            std::string(Token));
        PumpUntil(
            second,
            [&]
            {
                return second.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !second.HasInFlightRequest();
            },
            now);
        const auto requests = backend.Requests();
        Require(
            requests.size() == 3u
                && requests[0].method == L"GET"
                && requests[1].method == L"DELETE"
                && requests[2].method == L"GET"
                && !reopened.HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences()),
            "Restart treated a durable delete intent as initial Missing.");
        DrainDetached(second, now);
    }

    void TestPreDeleteIntentUsesObservedBaselineEtag()
    {
        const auto trusted = TrustedPath("predelete-etag");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        const auto resource =
            LamaPon::CloudSaveResource::SaveSlot("etag-slot");
        saves.SaveJson("etag-slot", R"({"value":1})");
        const auto local = Documents::ReadSaveData(saves, "etag-slot");
        const auto baseline = LiveSnapshot(
            resource,
            "\"etag-e1\"",
            local.bytes);
        journal.RecordBaseline(baseline);
        const auto remote = LiveSnapshot(
            resource,
            "\"etag-e2\"",
            Bytes(R"({"format":"LamaPonSaveData","version":1,"slot":"etag-slot","data":{"value":2}})"));

        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest& request)
        {
            (void)request;
            return ConflictResponse(remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });

        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            35u,
            std::string(AccountKey),
            std::string(Token));
        synchronizer.PrepareLocalDelete(resource);
        Require(
            saves.DeleteSlot("etag-slot")
                && journal.HasLocalDeleteIntent(resource),
            "Pre-delete WAL was not durable before the local delete returned.");

        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                if (synchronizer.Status().state
                    == LamaPon::Detail::CloudSaveSynchronizerState::Halted)
                {
                    throw std::runtime_error(
                        "Pre-delete synchronization halted, reason="
                        + std::to_string(static_cast<int>(
                            synchronizer.Status().stopReason))
                        + ", requests="
                        + std::to_string(backend.Requests().size()));
                }
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && journal.Conflict(resource).has_value()
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        const auto requests = backend.Requests();
        Require(
            requests.size() == 3u
                && requests[0].method == L"GET"
                && requests[1].method == L"DELETE"
                && requests[2].method == L"GET"
                && RequestHeader(requests[1], L"If-Match")
                    == L"\"etag-e1\""
                && journal.Conflict(resource)->etag == "\"etag-e2\"",
            "Stale pre-delete CAS did not preserve the remote conflict.");
        DrainDetached(synchronizer, now);
    }

    void TestRemoteTombstoneCrashAdvancesBaselineWithoutDelete()
    {
        const auto trusted = TrustedPath("tombstone-crash");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        const auto resource =
            LamaPon::CloudSaveResource::SaveSlot("crash-slot");
        saves.SaveJson("crash-slot", R"({"value":1})");
        const auto original = Documents::ReadSaveData(saves, "crash-slot");
        journal.RecordBaseline(LiveSnapshot(
            resource,
            "\"crash-e1\"",
            original.bytes));
        // remote tombstoneのlocal適用だけがcommitし、journal baseline publish
        // 前にprocessが落ちた状態を再現します。
        Documents::DeleteSaveData(saves, "crash-slot");
        const auto remote = Tombstone(resource, "\"crash-e2\"");

        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            36u,
            std::string(AccountKey),
            std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                const auto updated = journal.Baseline(resource);
                return updated
                    && updated->etag == "\"crash-e2\""
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        const auto requests = backend.Requests();
        Require(
            requests.size() == 2u
                && requests[0].method == L"GET"
                && requests[1].method == L"GET"
                && !journal.Pending(resource)
                && !journal.HasLocalDeleteIntent(resource),
            "Recovered remote tombstone sent a stale DELETE instead of advancing baseline.");
        DrainDetached(synchronizer, now);
    }

    void TestPendingMutationsDispatchShrinkBeforeGrowth()
    {
        // CTestのbuild-directoryから実行しても、Win32の一時file suffixを
        // 含むpathがlegacy MAX_PATHへ近づき過ぎない短いfixture名にします。
        const auto trusted = TrustedPath("pending-priority");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        const auto grow =
            LamaPon::CloudSaveResource::SaveSlot("a-grow");
        const auto shrink =
            LamaPon::CloudSaveResource::SaveSlot("z-shrink");

        saves.SaveJson("a-grow", R"({"blob":"x"})");
        const auto growBase =
            Documents::ReadSaveData(saves, "a-grow");
        journal.RecordBaseline(LiveSnapshot(
            grow,
            "\"grow-old\"",
            growBase.bytes));
        saves.SaveJson(
            "a-grow",
            std::string("{\"blob\":\"")
                + std::string(4096u, 'g') + "\"}");
        const auto growNext =
            Documents::ReadSaveData(saves, "a-grow");
        journal.QueuePut(
            grow,
            growNext.bytes,
            MutationIds[0],
            "\"grow-old\"");

        saves.SaveJson("z-shrink", R"({"value":1})");
        const auto shrinkBase =
            Documents::ReadSaveData(saves, "z-shrink");
        journal.RecordBaseline(LiveSnapshot(
            shrink,
            "\"shrink-old\"",
            shrinkBase.bytes));
        Documents::DeleteSaveData(saves, "z-shrink");
        journal.QueueDelete(
            shrink,
            MutationIds[1],
            "\"shrink-old\"");

        ScriptedBackend backend;
        backend.Push([shrink](const LamaPon::HttpRequest& request)
        {
            Require(
                request.method == L"DELETE"
                    && RequestText(request).find("z-shrink")
                        != std::string::npos,
                "Restarted pending growth ran before a pending shrink.");
            return SnapshotResponse(
                200u,
                Tombstone(shrink, "\"shrink-new\""));
        });
        backend.Push([](const LamaPon::HttpRequest& request)
        {
            Require(
                request.method == L"PUT"
                    && RequestText(request).find("a-grow")
                        != std::string::npos,
                "Pending growth did not follow the completed shrink.");
            return SnapshotResponse(
                200u,
                SnapshotFromPut(request, "\"grow-new\""));
        });

        auto idIndex = std::make_shared<std::size_t>(2u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            38u,
            std::string(AccountKey),
            std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return backend.Requests().size() >= 2u;
            },
            now);
        const auto requests = backend.Requests();
        Require(
            requests.size() == 2u
                && requests[0].method == L"DELETE"
                && requests[1].method == L"PUT",
            "Pending dispatch ordering was not shrink-before-growth.");
        DrainDetached(synchronizer, now);
    }

    void TestDetachDiscardsStaleWorkerWithoutBlocking()
    {
        const auto trusted = TrustedPath("detach-race");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        struct Gate final
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool entered{};
            bool release{};
        };
        auto gate = std::make_shared<Gate>();
        ScriptedBackend backend;
        backend.Push([gate](const LamaPon::HttpRequest&)
        {
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
            return ManifestResponse({});
        });
        auto idIndex = std::make_shared<std::size_t>(0u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 31u, std::string(AccountKey), std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);

        const auto started = std::chrono::steady_clock::now();
        synchronizer.Detach();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        Require(
            elapsed < std::chrono::milliseconds(100),
            "Detach waited for the synchronous cloud request.");
        {
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();
        PumpUntil(
            synchronizer,
            [&]
            {
                return !synchronizer.HasInFlightRequest();
            },
            now);
        Require(
            synchronizer.Status().state
                    == LamaPon::Detail::CloudSaveSynchronizerState::Detached
                && journal.Generation() == 0u
                && !journal.Pending(
                    LamaPon::CloudSaveResource::Preferences()),
            "A stale worker result mutated detached account state.");
    }

    void TestDestructorDoesNotJoinWorker()
    {
        const auto trusted = TrustedPath("destructor-race");
        const auto profile = Profile(trusted);
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        struct Gate final
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool entered{};
            bool release{};
            bool completed{};
        };
        auto gate = std::make_shared<Gate>();
        ScriptedBackend backend;
        backend.Push([gate](const LamaPon::HttpRequest&)
        {
            {
                std::unique_lock lock(gate->mutex);
                gate->entered = true;
                gate->condition.notify_all();
                gate->condition.wait(lock, [&] { return gate->release; });
                gate->completed = true;
            }
            gate->condition.notify_all();
            return ManifestResponse({});
        });
        auto idIndex = std::make_shared<std::size_t>(0u);
        auto synchronizer = std::make_unique<Synchronizer>(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer->Attach(
            journal,
            37u,
            std::string(AccountKey),
            std::string(Token));
        std::uint64_t now{};
        PumpUntil(
            *synchronizer,
            [&]
            {
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);
        std::thread releaser([gate]
        {
            Sleep(250u);
            {
                std::scoped_lock lock(gate->mutex);
                gate->release = true;
            }
            gate->condition.notify_all();
        });
        const auto started = std::chrono::steady_clock::now();
        synchronizer.reset();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        releaser.join();
        Require(
            elapsed < std::chrono::milliseconds(100),
            "Synchronizer destructor joined a wire worker.");
        {
            std::unique_lock lock(gate->mutex);
            Require(
                gate->condition.wait_for(
                    lock,
                    std::chrono::seconds(5),
                    [&] { return gate->completed; }),
                "Detached worker did not finish after destruction.");
        }
        // completedはHTTP sender復帰直前なのでmailbox publishまで短く待ちます。
        Sleep(10u);
        Require(
            journal.Generation() == 0u,
            "Destroyed synchronizer's worker touched the journal.");
    }

    void TestConflictDescriptorCacheIsInvalidatedAcrossAttachments()
    {
        const auto trustedA = TrustedPath("descriptor-cache-a");
        const auto trustedB = TrustedPath("descriptor-cache-b");
        const auto profileA = Profile(trustedA);
        const auto profileB = Profile(trustedB);
        LamaPon::PlayerPrefs preferences(profileA.playerPrefsFile);
        preferences.SetInteger("value", 1);
        preferences.Save();
        LamaPon::SaveDataStore saves(profileA.saveDataDirectory);
        Journal journalA(
            trustedA,
            profileA,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        Journal journalB(
            trustedB,
            profileB,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        const auto preferencesBytes =
            Documents::ReadPlayerPrefs(preferences).bytes;
        const auto slotBytes = Bytes(
            R"({"format":"LamaPonSaveData","version":1,"slot":"other-slot","data":{}})");
        const auto preferencesResource =
            LamaPon::CloudSaveResource::Preferences();
        const auto slotResource =
            LamaPon::CloudSaveResource::SaveSlot("other-slot");
        journalA.QueuePut(
            preferencesResource,
            preferencesBytes,
            MutationIds[0]);
        journalA.RecordConflict(
            preferencesResource,
            MutationIds[0],
            Tombstone(preferencesResource, "\"remote-a\""));
        journalB.QueuePut(slotResource, slotBytes, MutationIds[1]);
        journalB.RecordConflict(
            slotResource,
            MutationIds[1],
            Tombstone(slotResource, "\"remote-b\""));
        Require(
            journalA.Generation() == journalB.Generation(),
            "Descriptor cache journals did not share a generation fixture.");

        ScriptedBackend backend;
        auto idIndex = std::make_shared<std::size_t>(2u);
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journalA,
            77u,
            std::string(AccountKey),
            std::string(Token));
        const auto first = synchronizer.Conflicts();
        synchronizer.Detach();
        preferences.Rebind(profileB.playerPrefsFile);
        saves.Rebind(profileB.saveDataDirectory);
        synchronizer.Attach(
            journalB,
            77u,
            std::string(AccountKey),
            std::string(Token));
        const auto second = synchronizer.Conflicts();
        Require(
            first.size() == 1u
                && first.front().resource.kind
                    == LamaPon::CloudSaveResourceKind::Preferences
                && second.size() == 1u
                && second.front().resource.kind
                    == LamaPon::CloudSaveResourceKind::SaveSlot
                && second.front().resource.slot == "other-slot",
            "Conflict descriptor cache leaked across attachments.");
        synchronizer.Detach();
    }
}

int main()
{
    try
    {
        ResetTestRoot();
        const auto run = [](const char* name, const auto test)
        {
            std::cout << "[cloud-sync] " << name << std::endl;
            try
            {
                test();
            }
            catch (const std::exception& exception)
            {
                throw std::runtime_error(
                    std::string(name) + ": " + exception.what());
            }
        };
        run("pending overlay", TestPendingOverlayChainsAfterAck);
        run("pending retry", TestPendingRetryKeepsMutationIdentity);
        run("delete recreation", TestRemoteDeleteThenLocalRecreationBecomesPut);
        run("slot case identity", TestRemoteSlotCaseUsesExistingLocalSpelling);
        run("conflict", TestCasConflictRetryAndUseRemote);
        run("initial conflict", TestInitialDualContentRequiresConflict);
        run("read invalidation", TestLocalCommitInvalidatesActiveRead);
        run("initial delete", TestDeleteDuringInitialReadIsNotInitialMissing);
        run("corrupt local", TestCorruptLocalStopsBeforeNetwork);
        run("wire retry", TestUnauthorizedRefreshAndRateLimitBackoff);
        run("journal busy read", TestJournalBusyDuringActiveReadRetriesWithoutHalting);
        run("quota swap", TestQuotaSwapShrinksBeforeGrowth);
        run("manifest delete restart", TestDeleteIntentDuringManifestSurvivesRestart);
        run("pre-delete etag", TestPreDeleteIntentUsesObservedBaselineEtag);
        run("tombstone crash", TestRemoteTombstoneCrashAdvancesBaselineWithoutDelete);
        run("pending shrink priority", TestPendingMutationsDispatchShrinkBeforeGrowth);
        run("detach", TestDetachDiscardsStaleWorkerWithoutBlocking);
        run("destructor", TestDestructorDoesNotJoinWorker);
        run("descriptor cache", TestConflictDescriptorCacheIsInvalidatedAcrossAttachments);
        ResetTestRoot();
        std::cout << "Cloud save synchronizer tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Cloud save synchronizer tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
