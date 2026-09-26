#include "LamaPon/Online/NetworkEndpoint.h"
#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Online/NetworkCrypto.h"
#include "LamaPon/Online/NetworkPortMapping.h"
#include "LamaPon/Online/NetworkRoomBrowser.h"
#include "LamaPon/Online/NetworkSettingsJson.h"
#include "../samples/Networking/P2PTurnBasedController.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace LamaPon;
    using namespace LamaPon::Detail;
    void Require(const bool value, const char* text) { if (!value) throw std::runtime_error(text); }
    template<class Action> void Rejected(Action action, const char* text)
    {
        bool rejected{}; try { action(); } catch (const std::exception&) { rejected = true; }
        Require(rejected, text);
    }
    template<class Condition> void Until(NetworkSession& host, NetworkSession& client, Condition condition,
        NetworkSession* other = nullptr)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!condition())
        {
            host.Update(0.005f); client.Update(0.005f); if (other) other->Update(0.005f);
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Direct test timeout: " + host.LastError() + " / " + client.LastError());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void CryptoVectors()
    {
        const std::array<unsigned char, 20> hmacKey{ 11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11 };
        constexpr std::string_view message = "Hi There";
        Require(Hex(NetworkHmac(hmacKey, std::span(reinterpret_cast<const unsigned char*>(message.data()), message.size())))
            == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "RFC 4231 HMAC vector");
        std::array<unsigned char, 22> secret{}; secret.fill(11);
        std::array<unsigned char, 13> salt{}; for (unsigned char i = 0; i < salt.size(); ++i) salt[i] = i;
        std::string info; for (int i = 0xf0; i <= 0xf9; ++i) info += static_cast<char>(i);
        Require(Hex(NetworkHkdf(salt, secret, info)) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf", "RFC 5869 HKDF vector");
        NetworkKey key{}; std::array<unsigned char, 12> nonce{}; std::array<unsigned char, 16> tag{};
        const auto cipher = NetworkAesGcm(false, key, nonce, {}, std::string(16, '\0'), tag);
        Require(Hex(std::span(reinterpret_cast<const unsigned char*>(cipher.data()), cipher.size()))
            == "cea7403d4d606b6e074ec5d3baf39d18" && Hex(tag) == "d0d1c8a799996bf0265b98b5d48ab919", "AES-256-GCM vector");
        Require(NetworkAesGcm(true, key, nonce, {}, cipher, tag) == std::string(16, '\0'), "AES vector decrypt");
        NetworkCipher send, receive; key = RandomNetworkKey(); send.Initialize(key); receive.Initialize(key);
        const auto packet = send.Seal("private game message");
        Require(packet.find("private game message") == std::string::npos, "Encrypted body");
        auto changed = packet; changed.back() ^= 1;
        Rejected([&] { static_cast<void>(receive.Open(changed)); }, "Tampered tag rejected");
        Require(receive.Open(packet) == "private game message", "Valid record after invalid authentication");
        Rejected([&] { static_cast<void>(receive.Open(packet)); }, "Replay rejected");
        Require(receive.Open(send.Seal("second")) == "second", "Monotonic record sequence");
        NetworkKeyExchange first, second;
        const auto firstPublic = first.PublicKey(), secondPublic = second.PublicKey();
        Require(first.Agree(secondPublic) == second.Agree(firstPublic), "Ephemeral ECDH agreement");
        auto invalid = secondPublic; invalid[0] ^= 1;
        Rejected([&] { static_cast<void>(first.Agree(invalid)); }, "Incorrect ECDH curve rejected");
    }
    void EndpointAndSettings()
    {
        NetworkEndpoint endpoint;
        Require(NetworkEndpoint::Parse("[::1]:27840", 0, endpoint) && endpoint.Family() == AF_INET6
            && endpoint.Text() == "[::1]:27840" && endpoint.Unicast(), "Bracketed IPv6 endpoint");
        Require(NetworkEndpoint::Parse("::", 0, endpoint) && endpoint.Wildcard(), "IPv6 wildcard ephemeral port");
        Require(NetworkEndpoint::Parse("[fe80::1%7]:1234", 0, endpoint) && endpoint.Host() == "fe80::1%7", "Numeric IPv6 scope");
        Require(!NetworkEndpoint::Parse("[::1]:65536", 0, endpoint)
            && !NetworkEndpoint::Parse("hostname.invalid", 0, endpoint)
            && !NetworkEndpoint::Parse("127.0.0.1:-1", 0, endpoint), "Invalid endpoint rejected");
        Require(IsPublicNetworkIpv4("8.8.8.8") && IsPublicNetworkIpv4("203.1.2.3") && IsPublicNetworkIpv4("192.1.2.3"), "Public ranges");
        for (const auto* address : { "127.0.0.1", "192.168.1.2", "10.0.0.1", "172.16.1.2", "100.64.1.2", "198.18.0.1", "203.0.113.1", "0.0.0.0", "224.0.0.1" })
            Require(!IsPublicNetworkIpv4(address), "Non-public mapped address rejected");
        NetworkConfiguration config; config.backend = NetworkBackend::Direct; config.syncMode = NetworkSyncMode::OnChange;
        config.advertiseLan = true; config.automaticPortMapping = true; config.roomName = "ターン制の部屋"; config.discoveryPort = 27849;
        const auto json = NetworkSettingsToJson(config); const auto loaded = NetworkSettingsFromJson(json);
        Require(loaded.backend == config.backend && loaded.syncMode == config.syncMode && loaded.advertiseLan
            && loaded.automaticPortMapping && loaded.roomName == config.roomName && loaded.discoveryPort == 27849, "New settings round trip");
        Require(json.dump().find("LPD1") == std::string::npos && !json.contains("accessKey"), "No ephemeral credentials in settings");
        const auto old = NetworkSettingsFromJson(nlohmann::json::object());
        Require(old.backend == NetworkBackend::Lan && !old.automaticPortMapping && !old.advertiseLan, "Old project remains opt-in");
        auto invalid = json; invalid["syncMode"] = "Unknown";
        Rejected([&] { static_cast<void>(NetworkSettingsFromJson(invalid)); }, "Invalid sync profile rejected");
    }
    class FakeGateway final : public INetworkGateway
    {
    public:
        std::optional<NetworkPortEntry> entry;
        int adds{}, removes{};
        bool permanent{}, fail{};
        std::optional<NetworkPortEntry> Inspect(std::uint16_t) override
        { if (fail) throw std::runtime_error("Mock timeout"); return entry; }
        bool Add(std::uint16_t, const NetworkPortEntry& value) override
        { ++adds; entry = value; if (permanent) entry->leaseSeconds = 0; return true; }
        void Remove(std::uint16_t) override { ++removes; entry.reset(); }
        std::string ExternalAddress() override { return "8.8.8.8"; }
    };
    void MappingOwnership()
    {
        FakeGateway gateway;
        {
            NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own");
            Require(lease.Acquire() && gateway.entry->leaseSeconds == 120 && lease.Renew(), "Acquire and renew bounded lease");
        }
        Require(gateway.removes == 1 && !gateway.entry, "Release own mapping");
        gateway.entry = NetworkPortEntry{ "192.168.1.30", 27840, "other application", 120 };
        const int before = gateway.adds;
        { NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own"); Require(!lease.Acquire(), "Existing mapping protected"); }
        Require(gateway.adds == before && gateway.removes == 1 && gateway.entry, "Never overwrite or delete existing mapping");
        gateway.entry.reset();
        {
            NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own"); Require(lease.Acquire(), "Ownership test acquire");
            gateway.entry->description = "new owner";
            Require(!lease.Renew(), "Renew does not overwrite replacement");
        }
        Require(gateway.removes == 1 && gateway.entry->description == "new owner", "Replacement mapping retained");
        gateway.entry.reset(); gateway.permanent = true;
        { NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own"); Require(!lease.Acquire(), "Permanent-only mapping rejected"); }
        Require(gateway.removes == 2 && !gateway.entry, "Rollback unexpected permanent mapping");
        gateway.fail = true;
        NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own");
        Rejected([&] { lease.Acquire(); }, "Unknown inspection is not treated as empty");
    }
    void DirectConnection(const char* address)
    {
        NetworkConfiguration config; config.backend = NetworkBackend::Direct; config.port = 0; config.syncMode = NetworkSyncMode::OnChange;
        NetworkSession host, client, late, fourth;
        Require(host.Configure(config) && client.Configure(config) && late.Configure(config) && fourth.Configure(config) && host.Host("Host", address), "Direct host start");
        host.Update(0); Require(host.IsHost() && host.AccessKey().size() == 64 && host.ConnectionCode().starts_with("LPD1|"), "Direct ready code");
        Require(host.SetSessionState("initial turn state"), "Host global state");
        Require(client.JoinDirect(host.LocalAddress(), host.AccessKey(), "Client"), "Separate endpoint and key");
        Until(host, client, [&] { return client.State() == NetworkState::Connected; });
        Require(client.SessionState() == "initial turn state" && client.Members().size() == 2, "Global state before ready");
        Require(!client.SetSessionState("cheat") && !client.BroadcastEvent("cheat", ""), "Host-only state and events");
        Require(client.SendCommand("choose", "card-3"), "Authenticated command");
        bool received{};
        Until(host, client, [&]
        {
            NetworkEvent event;
            while (host.PollEvent(event)) if (event.kind == NetworkEventKind::Command)
                received = event.peer == client.LocalPeer() && event.name == "choose" && event.data == "card-3";
            return received;
        });
        NetworkObjectState object; object.sceneKey = "board";
        const auto id = host.Spawn(object); Require(id != 0, "Direct object create");
        Until(host, client, [&] { return client.FindObject(id) != nullptr; });
        const auto bytes = host.Statistics().sentBytes;
        for (int i = 0; i < 25; ++i) { Require(host.SetObject(*host.FindObject(id)), "Unchanged object"); host.Update(0.01f); client.Update(0.01f); }
        Require(host.Statistics().sentBytes == bytes, "On-change avoids unchanged snapshots");
        object = *host.FindObject(id); object.data = "changed"; Require(host.SetObject(object), "Dirty object");
        Until(host, client, [&] { return client.FindObject(id)->data == "changed"; });
        for (int index = 1; index < 128; ++index)
        {
            NetworkObjectState extra; extra.sceneKey = "bulk." + std::to_string(index);
            Require(host.Spawn(extra) != 0, "Secure full baseline spawn");
        }
        Until(host, client, [&] { return client.Objects().size() == 128; });
        Require(host.SetSessionState("late join turn"), "Latest global state");
        Require(late.Join(host.ConnectionCode(), "Late"), "Code is optional connection method");
        Until(host, client, [&] { return late.State() == NetworkState::Connected; }, &late);
        Require(late.SessionState() == "late join turn" && late.FindObject(id)->data == "changed", "Secure late join baseline");
        Require(late.Objects().size() == 128, "Encrypted 128-object baseline");
        Require(fourth.Join(host.RoomAddress(), "Fourth"), "Fourth encrypted player");
        Until(host, client, [&] { return fourth.State() == NetworkState::Connected; }, &fourth);
        NetworkSession full; Require(full.Configure(config) && full.Join(host.RoomAddress()), "Full encrypted room attempt");
        Until(host, client, [&] { return full.State() == NetworkState::Error; }, &full);
        Require(host.Members().size() == 4, "Full room does not expand membership");
        NetworkSession wrong; Require(wrong.Configure(config), "Bad-key configuration");
        auto badKey = host.AccessKey(); badKey[0] = badKey[0] == '0' ? '1' : '0';
        Require(wrong.JoinDirect(host.LocalAddress(), badKey), "Bad key begins transport");
        Until(host, client, [&] { return wrong.State() == NetworkState::Error; }, &wrong);
        Require(host.IsHost() && host.Members().size() == 4 && wrong.LastError().find(badKey) == std::string::npos, "Bad key rejected without host failure or secret log");
        const auto oldKey = host.AccessKey();
        fourth.Stop(); late.Stop(); client.Stop(); host.Stop();
        Require(host.AccessKey().empty() && host.SessionState().empty() && host.RoomAddress().empty(), "Stop clears runtime state");
        Require(host.Host("Again", address), "Direct restart"); host.Update(0);
        Require(host.AccessKey() != oldKey, "Fresh runtime key");
        NetworkSession expired; Require(expired.Configure(config) && expired.JoinDirect(host.LocalAddress(), oldKey), "Expired key attempt");
        Until(host, client, [&] { return expired.State() == NetworkState::Error; }, &expired);
        Require(host.Members().size() == 1, "Stopped room credentials are invalid after restart");
        if (std::string_view(address) == "127.0.0.1")
        {
            NetworkConfiguration legacy; legacy.port = 0;
            NetworkSession plaintext; Require(plaintext.Configure(legacy) && plaintext.Join(host.LocalAddress()), "Plaintext downgrade attempt");
            Until(host, client, [&] { return plaintext.State() == NetworkState::Error; }, &plaintext);
            Require(host.IsHost() && host.Members().size() == 1, "Direct never accepts plaintext fallback");
        }
    }
    void TurnBasedGame()
    {
        NetworkConfiguration config; config.backend = NetworkBackend::Direct; config.port = 0; config.syncMode = NetworkSyncMode::OnChange;
        NetworkSession host, client, spectator;
        Require(host.Configure(config) && client.Configure(config) && spectator.Configure(config), "Turn config");
        Require(host.Host(), "Turn host"); host.Update(0); Require(client.Join(host.RoomAddress()), "Turn client");
        Until(host, client, [&] { return client.State() == NetworkState::Connected; });
        Samples::P2PTurnBasedController first, second, watching;
        const auto step = [&]
        {
            host.Update(0.01f); client.Update(0.01f); spectator.Update(0.01f);
            first.Update(host); second.Update(client); watching.Update(spectator);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        for (int i = 0; i < 20; ++i) step();
        Require(!second.RequestMove(client, 0) && first.RequestMove(host, 0), "Only active turn may request");
        for (int i = 0; i < 20; ++i) step();
        Require(second.Board()[0] == 'X' && second.Turn() == client.LocalPeer(), "Turn state replicated");
        Require(client.SendCommand("board.move", "0:4"), "Stale turn command");
        for (int i = 0; i < 20; ++i) step();
        Require(first.Board()[4] == '.', "Stale revision rejected by game");
        Require(second.RequestMove(client, 4), "Second turn");
        for (int i = 0; i < 20; ++i) step();
        Require(spectator.Join(host.RoomAddress()), "Spectator late join");
        for (int i = 0; i < 30; ++i) step();
        Require(watching.Board()[0] == 'X' && watching.Board()[4] == 'O' && !watching.RequestMove(spectator, 8), "Late spectator sees board and cannot move");
        Require(first.RequestMove(host, 1), "Third turn"); for (int i = 0; i < 20; ++i) step();
        Require(second.RequestMove(client, 5), "Fourth turn"); for (int i = 0; i < 20; ++i) step();
        Require(first.RequestMove(host, 2), "Winning turn"); for (int i = 0; i < 20; ++i) step();
        Require(first.Winner() == 1 && second.Winner() == 1 && watching.Winner() == 1 && host.Objects().empty(), "Complete turn game with no scene objects");
    }
    void RoomDiscovery()
    {
        NetworkConfiguration config; config.backend = NetworkBackend::Direct; config.port = 0;
        config.advertiseLan = true; config.roomName = "Public LAN room"; config.discoveryPort = 27849;
        NetworkSession host, client; NetworkRoomBrowser browser;
        Require(host.Configure(config) && client.Configure(config) && host.Host("Host", "0.0.0.0"), "Discoverable host"); host.Update(0);
        Require(browser.Start(config, NetworkDiscoveryScope::SameComputer), "Same-PC discovery start");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (browser.Rooms().empty())
        {
            browser.Update(0.005f); host.Update(0.005f);
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Discovery timeout: " + browser.LastError());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto room = browser.Rooms().front();
        Require(room.name == config.roomName && room.players == 1 && room.capacity == 4 && room.connection.starts_with("LPD1|127."), "Room metadata and source-derived connection");
        auto foreign = room; foreign.gameId = "different.game";
        Require(!client.JoinRoom(foreign), "Directory game mismatch rejected");
        Require(client.JoinRoom(room), "Join by room selection without copying invitation");
        Until(host, client, [&] { return client.State() == NetworkState::Connected; });
        browser.Refresh();
        for (int i = 0; i < 30; ++i) { browser.Update(0.005f); host.Update(0.005f); client.Update(0.005f); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        Require(browser.Rooms().size() == 1 && browser.Rooms().front().players == 2, "Discovery deduplicates and refreshes capacity");
        browser.Stop(); Require(browser.Rooms().empty() && !browser.IsSearching(), "Browser stop");
    }
}
int main()
{
    try
    {
        CryptoVectors(); EndpointAndSettings(); MappingOwnership(); DirectConnection("127.0.0.1"); DirectConnection("::1");
        TurnBasedGame(); RoomDiscovery();
        std::cout << "Direct authenticated IPv4/IPv6, crypto vectors, tamper/replay, turn-based state, LAN discovery and lease ownership passed.\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
