#include "LamaPon/Online/CloudSaveClient.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;

    constexpr std::string_view EmptyObjectContent = "e30";
    constexpr std::string_view EmptyObjectHash =
        "RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o";
    constexpr std::string_view LevelContent = "eyJsZXZlbCI6N30";
    constexpr std::string_view LevelHash =
        "fUV07UsUNLX3A0GWwak_tdwTcH_rWlJ1CqBLNiwLRwY";
    constexpr std::string_view MutationId =
        "123e4567-e89b-42d3-a456-426614174000";
    constexpr std::string_view DeleteMutationId =
        "123e4567-e89b-42d3-b456-426614174001";
    constexpr std::string_view ConflictMutationId =
        "123e4567-e89b-42d3-8456-426614174002";

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    Json PreferencesResource()
    {
        return Json{ { "kind", "preferences" } };
    }

    Json SlotResource(std::string slot = "slot-1")
    {
        return Json{
            { "kind", "save_slot" },
            { "slot", std::move(slot) }
        };
    }

    Json LiveSnapshot(
        Json resource,
        std::string etag,
        const std::string_view content,
        const std::string_view hash,
        const std::size_t byteLength,
        const bool protocolVersion = true)
    {
        Json result{
            { "resource", std::move(resource) },
            { "etag", std::move(etag) },
            { "deleted", false },
            { "byteLength", byteLength },
            { "sha256", hash },
            { "content", content }
        };
        if (protocolVersion)
        {
            result["protocolVersion"] = 1;
        }
        return result;
    }

    Json Tombstone(
        Json resource,
        std::string etag,
        const bool protocolVersion = true)
    {
        Json result{
            { "resource", std::move(resource) },
            { "etag", std::move(etag) },
            { "deleted", true },
            { "byteLength", 0 }
        };
        if (protocolVersion)
        {
            result["protocolVersion"] = 1;
        }
        return result;
    }

    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const Json& json,
        std::string_view etag = {})
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

    bool HeaderNameEquals(
        const std::wstring_view left,
        const std::wstring_view right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            const auto fold = [](const wchar_t value)
            {
                return value >= L'A' && value <= L'Z'
                    ? value - L'A' + L'a'
                    : value;
            };
            if (fold(left[index]) != fold(right[index]))
            {
                return false;
            }
        }
        return true;
    }

    std::size_t HeaderCount(
        const LamaPon::HttpRequest& request,
        const std::wstring_view name,
        const std::wstring_view value = {})
    {
        std::size_t count{};
        for (const auto& [headerName, headerValue] : request.headers)
        {
            if (HeaderNameEquals(headerName, name)
                && (value.empty() || headerValue == value))
            {
                ++count;
            }
        }
        return count;
    }

    struct ScriptedBackend final
    {
        std::deque<LamaPon::HttpResponse> responses;
        std::vector<LamaPon::HttpRequest> requests;

        LamaPon::HttpResponse Send(const LamaPon::HttpRequest& request)
        {
            requests.push_back(request);
            if (responses.empty())
            {
                LamaPon::HttpResponse response;
                response.transportError = "unexpected request secret";
                return response;
            }
            auto response = std::move(responses.front());
            responses.pop_front();
            return response;
        }
    };

    LamaPon::Detail::CloudSaveClient MakeClient(
        ScriptedBackend& backend)
    {
        return LamaPon::Detail::CloudSaveClient(
            "https://online.example.test/",
            "game-42",
            "staging",
            false,
            [&backend](const LamaPon::HttpRequest& request)
            {
                return backend.Send(request);
            });
    }

    void TestSuccessfulProtocol()
    {
        ScriptedBackend backend;
        backend.responses.push_back(JsonResponse(
            200,
            {
                { "protocolVersion", 1 },
                {
                    "items",
                    Json::array({
                        {
                            { "resource", PreferencesResource() },
                            { "etag", "\"prefs-1\"" },
                            { "deleted", false },
                            { "byteLength", 2 },
                            { "sha256", EmptyObjectHash }
                        },
                        {
                            { "resource", SlotResource() },
                            { "etag", "\"slot-0\"" },
                            { "deleted", true },
                            { "byteLength", 0 }
                        }
                    })
                }
            }));
        backend.responses.push_back(JsonResponse(
            200,
            LiveSnapshot(
                PreferencesResource(),
                "\"prefs-1\"",
                EmptyObjectContent,
                EmptyObjectHash,
                2),
            "\"prefs-1\""));
        backend.responses.push_back(JsonResponse(
            201,
            LiveSnapshot(
                SlotResource(),
                "\"slot-1\"",
                LevelContent,
                LevelHash,
                11),
            "\"slot-1\""));
        backend.responses.push_back(JsonResponse(
            200,
            Tombstone(SlotResource(), "\"slot-2\""),
            "\"slot-2\""));
        backend.responses.push_back(JsonResponse(
            412,
            {
                { "protocolVersion", 1 },
                { "error", { { "code", "revision_conflict" } } },
                {
                    "current",
                    LiveSnapshot(
                        SlotResource(),
                        "\"slot-remote\"",
                        EmptyObjectContent,
                        EmptyObjectHash,
                        2,
                        false)
                }
            },
            "\"slot-remote\""));

        auto client = MakeClient(backend);
        const auto manifest = client.FetchManifest("backend-access-token");
        Require(
            manifest.outcome.Succeeded()
                && manifest.items.size() == 2
                && manifest.items[0].sha256 == EmptyObjectHash
                && manifest.items[1].deleted,
            "A valid cloud manifest was not parsed.");

        const auto read = client.Read(
            "backend-access-token",
            LamaPon::CloudSaveResource::Preferences());
        Require(
            read.outcome.Succeeded()
                && read.snapshot
                && std::string(
                    read.snapshot->content.begin(),
                    read.snapshot->content.end()) == "{}",
            "A valid cloud snapshot was not parsed.");

        const std::vector<std::uint8_t> level{
            '{', '"', 'l', 'e', 'v', 'e', 'l', '"', ':', '7', '}'
        };
        const auto put = client.Put(
            "backend-access-token",
            LamaPon::CloudSaveResource::SaveSlot("slot-1"),
            level,
            MutationId);
        Require(
            put.outcome.Succeeded()
                && put.snapshot
                && put.snapshot->etag == "\"slot-1\"",
            "A valid cloud create response was not parsed.");

        const auto deleted = client.Delete(
            "backend-access-token",
            LamaPon::CloudSaveResource::SaveSlot("slot-1"),
            DeleteMutationId,
            "\"slot-1\"");
        Require(
            deleted.outcome.Succeeded()
                && deleted.snapshot
                && deleted.snapshot->deleted,
            "A valid cloud delete response was not parsed.");

        const auto conflict = client.Put(
            "backend-access-token",
            LamaPon::CloudSaveResource::SaveSlot("slot-1"),
            level,
            ConflictMutationId,
            "\"stale\"");
        Require(
            conflict.outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::Conflict
                && conflict.conflictSnapshot
                && conflict.conflictSnapshot->etag == "\"slot-remote\"",
            "A valid CAS conflict was not retained.");

        Require(
            backend.responses.empty() && backend.requests.size() == 5,
            "The cloud client retried or omitted a request.");
        const std::array<std::wstring_view, 5> methods{
            L"GET", L"POST", L"PUT", L"DELETE", L"PUT"
        };
        const std::array<std::wstring_view, 5> paths{
            L"/v1/cloud-saves/manifest",
            L"/v1/cloud-saves/read",
            L"/v1/cloud-saves/item",
            L"/v1/cloud-saves/item",
            L"/v1/cloud-saves/item"
        };
        for (std::size_t index = 0; index < backend.requests.size(); ++index)
        {
            const auto& request = backend.requests[index];
            Require(
                request.method == methods[index]
                    && request.url
                        == L"https://online.example.test"
                            + std::wstring(paths[index])
                    && !request.followRedirects
                    && HeaderCount(
                        request,
                        L"Authorization",
                        L"Bearer backend-access-token") == 1
                    && HeaderCount(
                        request,
                        L"X-LamaPon-Game-Id",
                        L"game-42") == 1
                    && HeaderCount(
                        request,
                        L"X-LamaPon-Environment-Id",
                        L"staging") == 1
                    && HeaderCount(
                        request,
                        L"Cache-Control",
                        L"no-store") == 1,
                "A cloud request violated its endpoint or secret policy.");
            const std::string requestText(
                request.body.begin(),
                request.body.end());
            Require(
                requestText.find("backend-access-token")
                        == std::string::npos
                    && requestText.find("playerId") == std::string::npos
                    && requestText.find("discord") == std::string::npos,
                "A cloud request body contained an owner or token.");
        }
        Require(
            backend.requests[0].body.empty()
                && HeaderCount(backend.requests[0], L"Content-Type") == 0
                && HeaderCount(
                    backend.requests[2],
                    L"If-None-Match",
                    L"*") == 1
                && HeaderCount(backend.requests[2], L"If-Match") == 0
                && HeaderCount(
                    backend.requests[3],
                    L"If-Match",
                    L"\"slot-1\"") == 1
                && HeaderCount(
                    backend.requests[4],
                    L"If-Match",
                    L"\"stale\"") == 1
                && HeaderCount(
                    backend.requests[2],
                    L"Idempotency-Key",
                    std::wstring(MutationId.begin(), MutationId.end())) == 1
                && HeaderCount(
                    backend.requests[3],
                    L"Idempotency-Key",
                    std::wstring(
                        DeleteMutationId.begin(),
                        DeleteMutationId.end())) == 1
                && HeaderCount(
                    backend.requests[4],
                    L"Idempotency-Key",
                    std::wstring(
                        ConflictMutationId.begin(),
                        ConflictMutationId.end())) == 1,
            "Cloud CAS or idempotency headers were incorrect.");
        const auto putBody = Json::parse(
            std::string(
                backend.requests[2].body.begin(),
                backend.requests[2].body.end()));
        Require(
            putBody.at("content").get<std::string>()
                    == std::string(LevelContent)
                && putBody.at("sha256").get<std::string>()
                    == std::string(LevelHash)
                && putBody.at("byteLength") == 11
                && putBody.at("resource").at("slot") == "slot-1",
            "Cloud create payload was not canonical.");
    }

    void TestInvalidInputsDoNotSend()
    {
        ScriptedBackend backend;
        auto client = MakeClient(backend);
        const std::vector<std::uint8_t> valid{ '{', '}' };
        const std::vector<std::uint8_t> invalid{ '{' };
        std::string tooDeepJson(65, '[');
        tooDeepJson += "null";
        tooDeepJson.append(65, ']');
        const std::vector<std::uint8_t> tooDeep(
            tooDeepJson.begin(),
            tooDeepJson.end());

        Require(
            client.FetchManifest("bad token").outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Read(
                    "token",
                    LamaPon::CloudSaveResource::SaveSlot("CON"))
                    .outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Put(
                    "token",
                    LamaPon::CloudSaveResource::Preferences(),
                    invalid,
                    MutationId).outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Put(
                    "token",
                    LamaPon::CloudSaveResource::Preferences(),
                    valid,
                    "not-a-uuid").outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Put(
                    "token",
                    LamaPon::CloudSaveResource::Preferences(),
                    tooDeep,
                    MutationId).outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Delete(
                    "token",
                    LamaPon::CloudSaveResource::Preferences(),
                    MutationId,
                    "W/\"weak\"").outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && backend.requests.empty(),
            "Invalid cloud input reached the HTTP sender.");

        for (const std::string_view reserved : {
                "CON", "con.txt", "NUL", "AUX.save", "PRN",
                "CLOCK$", "CONIN$", "CONOUT$", "COM1", "LPT9.log",
                "COM¹", "LPT².txt", "COM³.save", "CON .txt" })
        {
            Require(
                client.Read(
                    "token",
                    LamaPon::CloudSaveResource::SaveSlot(
                        std::string(reserved))).outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest,
                "A Windows reserved save slot was accepted.");
        }
        Require(
            backend.requests.empty(),
            "A Windows reserved save slot reached the HTTP sender.");

        bool invalidNamespaceRejected{};
        try
        {
            LamaPon::Detail::CloudSaveClient invalidClient(
                "https://online.example.test",
                "bad/game",
                "production");
        }
        catch (const std::invalid_argument&)
        {
            invalidNamespaceRejected = true;
        }
        Require(
            invalidNamespaceRejected,
            "An unsafe cloud namespace was accepted.");
    }

    LamaPon::Detail::CloudSaveWireStatus ReadWithResponse(
        LamaPon::HttpResponse response,
        std::size_t& requestCount,
        std::string& publicMessage)
    {
        LamaPon::Detail::CloudSaveClient client(
            "https://online.example.test",
            "game",
            "production",
            false,
            [&response, &requestCount](const LamaPon::HttpRequest&)
            {
                ++requestCount;
                return response;
            });
        const auto result = client.Read(
            "backend-token",
            LamaPon::CloudSaveResource::Preferences());
        publicMessage = result.outcome.errorMessage
            + result.outcome.serviceCode;
        return result.outcome.status;
    }

    void TestInvalidResponsesAndStatusMapping()
    {
        {
            auto response = JsonResponse(
                200,
                LiveSnapshot(
                    PreferencesResource(),
                    "\"v1\"",
                    EmptyObjectContent,
                    EmptyObjectHash,
                    2),
                "\"v1\"");
            response.headers.emplace_back(L"etag", L"\"v1\"");
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse
                    && count == 1,
                "Duplicate response ETags were accepted.");
        }
        {
            auto response = JsonResponse(
                200,
                LiveSnapshot(
                    PreferencesResource(),
                    "\"v1\"",
                    "e31",
                    EmptyObjectHash,
                    2),
                "\"v1\"");
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
                "Non-canonical base64url was accepted.");
        }
        {
            auto response = JsonResponse(
                200,
                LiveSnapshot(
                    PreferencesResource(),
                    "\"v1\"",
                    EmptyObjectContent,
                    EmptyObjectHash,
                    2),
                "\"v1\"");
            response.headers[0].second = L"text/json";
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
                "An invalid JSON Content-Type was accepted.");
        }
        {
            auto response = JsonResponse(
                200,
                LiveSnapshot(
                    PreferencesResource(),
                    "\"v1\"",
                    EmptyObjectContent,
                    EmptyObjectHash,
                    2),
                "\"v1\"");
            response.headers[1].second = L"1";
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
                "A mismatched Content-Length was accepted.");
        }
        {
            LamaPon::HttpResponse response;
            response.statusCode = 401;
            response.body.assign(2000000, 'x');
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
                "A fake sender bypassed the response size cap.");
        }
        {
            LamaPon::HttpResponse response;
            response.statusCode = 429;
            response.headers.emplace_back(L"Retry-After", L"999");
            LamaPon::Detail::CloudSaveClient client(
                "https://online.example.test",
                "game",
                "production",
                false,
                [response](const LamaPon::HttpRequest&)
                {
                    return response;
                });
            const auto result = client.FetchManifest("backend-token");
            Require(
                result.outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::RateLimited
                    && result.outcome.retryAfterSeconds == 300,
                "Rate limit retry metadata was not bounded.");
        }
        {
            LamaPon::HttpResponse response;
            response.statusCode = 401;
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::Unauthorized
                    && count == 1,
                "An authorization failure was misclassified.");
        }
        {
            auto response = JsonResponse(
                401,
                { { "error", { { "code", "backend-token" } } } });
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                        == LamaPon::Detail::CloudSaveWireStatus::Unauthorized
                    && message.find("backend-token") == std::string::npos
                    && message.find("unauthorized") != std::string::npos,
                "A server-controlled error code escaped through the outcome.");
        }
        {
            ScriptedBackend backend;
            backend.responses.push_back(JsonResponse(
                412,
                {
                    { "protocolVersion", 1 },
                    { "error", { { "code", "revision_conflict" } } },
                    {
                        "current",
                        LiveSnapshot(
                            PreferencesResource(),
                            "\"remote\"",
                            EmptyObjectContent,
                            EmptyObjectHash,
                            2,
                            false)
                    }
                }));
            auto client = MakeClient(backend);
            const std::vector<std::uint8_t> content{ '{', '}' };
            const auto result = client.Put(
                "backend-token",
                LamaPon::CloudSaveResource::Preferences(),
                content,
                MutationId,
                "\"stale\"");
            Require(
                result.outcome.status
                        == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse
                    && backend.requests.size() == 1,
                "A conflict without a strong response ETag was accepted.");
        }
        {
            LamaPon::HttpResponse response;
            response.statusCode = 404;
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::NotFound
                    && count == 1,
                "A missing cloud item was misclassified.");
        }
        {
            LamaPon::HttpResponse response;
            response.statusCode = 503;
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::RetryableServiceError
                    && count == 1,
                "A retryable service status was misclassified or retried.");
        }
        {
            LamaPon::HttpResponse response;
            response.transportError =
                "transport leaked backend-token and player-secret";
            std::size_t count{};
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::TransportError
                    && message.find("backend-token") == std::string::npos
                    && message.find("player-secret") == std::string::npos,
                "A transport failure leaked a secret.");
        }
        {
            std::size_t count{};
            LamaPon::Detail::CloudSaveClient client(
                "https://online.example.test",
                "game",
                "production",
                false,
                [&count](const LamaPon::HttpRequest&) -> LamaPon::HttpResponse
                {
                    ++count;
                    throw std::runtime_error("backend-token secret");
                });
            const auto result = client.FetchManifest("backend-token");
            Require(
                result.outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::TransportError
                    && result.outcome.errorMessage.find("backend-token")
                        == std::string::npos
                    && count == 1,
                "A sender exception escaped or was retried.");
        }
    }

    void TestManifestConflictsFailClosed()
    {
        ScriptedBackend backend;
        backend.responses.push_back(JsonResponse(
            200,
            {
                { "protocolVersion", 1 },
                {
                    "items",
                    Json::array({
                        {
                            { "resource", SlotResource("Save") },
                            { "etag", "\"one\"" },
                            { "deleted", false },
                            { "byteLength", 2 },
                            { "sha256", EmptyObjectHash }
                        },
                        {
                            { "resource", SlotResource("save") },
                            { "etag", "\"two\"" },
                            { "deleted", false },
                            { "byteLength", 2 },
                            { "sha256", EmptyObjectHash }
                        }
                    })
                }
            }));
        auto client = MakeClient(backend);
        const auto result = client.FetchManifest("backend-token");
        Require(
            result.outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse
                && result.items.empty(),
            "Case-colliding remote slots were accepted.");

        ScriptedBackend duplicateKeys;
        LamaPon::HttpResponse duplicateResponse;
        duplicateResponse.statusCode = 200;
        const std::string duplicateBody =
            R"({"protocolVersion":1,"protocolVersion":1,"items":[]})";
        duplicateResponse.body.assign(
            duplicateBody.begin(),
            duplicateBody.end());
        duplicateResponse.headers.emplace_back(
            L"Content-Type",
            L"application/json");
        duplicateResponse.headers.emplace_back(
            L"Content-Length",
            std::to_wstring(duplicateResponse.body.size()));
        duplicateKeys.responses.push_back(std::move(duplicateResponse));
        auto duplicateClient = MakeClient(duplicateKeys);
        Require(
            duplicateClient.FetchManifest("backend-token").outcome.status
                == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
            "Duplicate JSON keys were accepted.");

        ScriptedBackend tooMany;
        Json items = Json::array();
        for (int index = 0; index < 33; ++index)
        {
            items.push_back({
                { "resource", SlotResource("slot-" + std::to_string(index)) },
                { "etag", "\"revision-" + std::to_string(index) + "\"" },
                { "deleted", true },
                { "byteLength", 0 }
            });
        }
        tooMany.responses.push_back(JsonResponse(
            200,
            {
                { "protocolVersion", 1 },
                { "items", std::move(items) }
            }));
        auto tooManyClient = MakeClient(tooMany);
        Require(
            tooManyClient.FetchManifest("backend-token").outcome.status
                == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
            "A manifest with 33 save slots was accepted.");

        ScriptedBackend overQuota;
        Json quotaItems = Json::array();
        for (int index = 0; index < 17; ++index)
        {
            quotaItems.push_back({
                { "resource", SlotResource("quota-" + std::to_string(index)) },
                { "etag", "\"quota-" + std::to_string(index) + "\"" },
                { "deleted", false },
                { "byteLength", LamaPon::CloudSaveSlotMaxBytes },
                { "sha256", EmptyObjectHash }
            });
        }
        overQuota.responses.push_back(JsonResponse(
            200,
            {
                { "protocolVersion", 1 },
                { "items", std::move(quotaItems) }
            }));
        auto overQuotaClient = MakeClient(overQuota);
        Require(
            overQuotaClient.FetchManifest("backend-token").outcome.status
                == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
            "A manifest above the account byte quota was accepted.");
    }
}

int main()
{
    try
    {
        TestSuccessfulProtocol();
        TestInvalidInputsDoNotSend();
        TestInvalidResponsesAndStatusMapping();
        TestManifestConflictsFailClosed();
        std::cout << "Cloud save wire protocol tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
