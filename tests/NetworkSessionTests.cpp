#include <WinSock2.h>
#include <WS2tcpip.h>
#include "LamaPon/Online/NetworkSession.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    void Require(const bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    template<class Predicate>
    void Until(Predicate predicate, LamaPon::NetworkSession& host,
        LamaPon::NetworkSession& client, LamaPon::NetworkSession* third = nullptr)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate())
        {
            host.Update(0.005f); client.Update(0.005f);
            if (third) third->Update(0.005f);
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Timed out: " + host.LastError() + " / " + client.LastError());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void ConnectionAndReplication()
    {
        using namespace LamaPon;
        NetworkConfiguration config;
        config.port = 0;
        NetworkSession host, client, third, fourth, excess;
        Require(host.Configure(config), "Host configuration");
        Require(host.Host("Host"), "Host start");
        host.Update(0);
        Require(host.IsHost() && host.LocalPeer() == 1, "Host state");
        NetworkObjectState fixed;
        fixed.sceneKey = "world.box";
        fixed.transform.position[0] = 5;
        const auto fixedId = host.Spawn(fixed);
        Require(fixedId != 0, "Host spawn");
        Require(client.Configure(config) && client.Join(host.RoomAddress(), "Second"), "Client join");
        Until([&] { return client.State() == NetworkState::Connected; }, host, client);
        Require(client.Members().size() == 2 && client.FindObject(fixedId)
            && client.FindObject(fixedId)->transform.position[0] == 5, "Late join baseline");
        Require(client.Spawn(fixed) == 0 && !client.Despawn(fixedId), "Client cannot mutate world");
        NetworkObjectState player;
        player.prefabKey = "player";
        player.owner = client.LocalPeer();
        const auto playerId = host.Spawn(player);
        Until([&] { return client.FindObject(playerId) != nullptr; }, host, client);
        Require(!client.SendInput(fixedId, "move", "1"), "Ownership rejection");
        Require(client.SendInput(playerId, "move", "1"), "Owned input");
        bool input{};
        Until([&]
        {
            NetworkEvent event;
            while (host.PollEvent(event))
                if (event.kind == NetworkEventKind::Input)
                {
                    Require(event.peer == client.LocalPeer() && event.object == playerId
                        && event.name == "move" && event.data == "1", "Input attribution");
                    input = true;
                }
            return input;
        }, host, client);
        auto changed = *host.FindObject(fixedId);
        changed.transform.position[0] = 42;
        Require(host.SetObject(changed), "State change");
        Until([&] { return client.FindObject(fixedId)->transform.position[0] == 42; }, host, client);
        Require(host.BroadcastEvent("score", "12"), "Broadcast");
        bool broadcast{};
        Until([&]
        {
            NetworkEvent event;
            while (client.PollEvent(event))
                if (event.kind == NetworkEventKind::GameEvent) broadcast = event.name == "score" && event.data == "12";
            return broadcast;
        }, host, client);
        Require(third.Configure(config) && third.Join(host.RoomAddress(), "Third"), "Third join");
        Until([&] { return third.State() == NetworkState::Connected; }, host, client, &third);
        Require(third.FindObject(fixedId)->transform.position[0] == 42, "Current baseline");
        Require(fourth.Configure(config) && fourth.Join(host.RoomAddress(), "Fourth"), "Fourth join");
        Until([&] { return fourth.State() == NetworkState::Connected; }, host, fourth);
        Require(excess.Configure(config) && excess.Join(host.RoomAddress(), "Fifth"), "Excess connection");
        Until([&] { return excess.State() == NetworkState::Error; }, host, excess);
        Require(host.Members().size() == 4, "Player limit");
        client.Stop();
        Until([&] { return host.Members().size() == 3 && third.FindObject(playerId) == nullptr; }, host, third);
        Require(host.FindObject(playerId) == nullptr, "Owned objects removed on departure");
        Require(host.Despawn(fixedId), "Despawn");
        Until([&] { return third.FindObject(fixedId) == nullptr; }, host, third);
        host.Stop();
        Until([&] { return third.State() == NetworkState::Error; }, host, third);
        Require(third.Objects().empty() && third.LocalPeer() == 0, "Host exit cleanup");
        Require(host.Host("Restart"), "Host restart");
        host.Update(0);
        Require(host.Members().size() == 1 && host.Objects().empty(), "Clean restart");
    }
    void IncompatibleGames()
    {
        using namespace LamaPon;
        for (int difference = 0; difference < 3; ++difference)
        {
            NetworkSession host, client;
            NetworkConfiguration config;
            config.port = 0;
            Require(host.Configure(config) && host.Host(), "Mismatch host");
            host.Update(0);
            if (difference == 0) config.gameId = "another.game";
            if (difference == 1) config.gameVersion = "2";
            if (difference == 2) config.sceneId = "other";
            Require(client.Configure(config) && client.Join(host.RoomAddress()), "Mismatch client");
            Until([&] { return client.State() == NetworkState::Error; }, host, client);
            Require(host.Members().size() == 1, "Incompatible peer admitted");
        }
    }
    void Bounds()
    {
        using namespace LamaPon;
        NetworkSession session;
        NetworkConfiguration invalid;
        invalid.maxPlayers = 5;
        Require(!session.Configure(invalid), "Configuration limits");
        invalid.maxPlayers = 4; invalid.port = 0;
        Require(session.Configure(invalid) && session.Host(), "Bounds host");
        session.Update(0);
        NetworkObjectState object;
        object.sceneKey = "unique";
        Require(session.Spawn(object) != 0 && session.Spawn(object) == 0, "Duplicate scene key");
        object.sceneKey.clear(); object.prefabKey = "player";
        object.data.assign(257, 'a');
        Require(session.Spawn(object) == 0, "Payload bound");
        object.data.clear(); object.owner = 99;
        Require(session.Spawn(object) == 0, "Unknown owner");
        object.owner = 1;
        for (int index = 1; index < 128; ++index) Require(session.Spawn(object) != 0, "Object capacity");
        Require(session.Spawn(object) == 0, "Object limit");
        NetworkSession late;
        Require(late.Configure(invalid) && late.Join(session.RoomAddress()), "Maximum baseline join");
        Until([&] { return late.State() == NetworkState::Connected; }, session, late);
        Require(late.Objects().size() == 128, "Complete maximum-size baseline before connected");
        late.Stop();
        NetworkEvent event;
        while (session.PollEvent(event)) {}
        for (int index = 0; index < 512; ++index) Require(session.BroadcastEvent("event", "data"), "Event capacity");
        Require(!session.BroadcastEvent("event", "overflow"), "Event queue bound");
        Require(!session.Configure(invalid), "Cannot reconfigure active session");
        session.Stop();
        SetActiveNetworkSession(&session);
        Require(ActiveNetworkSession() == &session, "Active API");
        SetActiveNetworkSession(nullptr);
    }

    struct RawPeer final
    {
        SOCKET socket{ INVALID_SOCKET };
        explicit RawPeer(const std::string& address)
        {
            socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            Require(socket != INVALID_SOCKET, "Raw socket");
            sockaddr_in endpoint{};
            endpoint.sin_family = AF_INET;
            endpoint.sin_port = htons(static_cast<unsigned short>(std::stoi(address.substr(address.find(':') + 1))));
            InetPtonA(AF_INET, "127.0.0.1", &endpoint.sin_addr);
            Require(connect(socket, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0, "Raw connect");
        }
        ~RawPeer() { if (socket != INVALID_SOCKET) closesocket(socket); }
        void Wire(const std::string& bytes) const
        {
            std::size_t offset{};
            while (offset < bytes.size())
            {
                const int sent = send(socket, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
                Require(sent > 0, "Raw send"); offset += static_cast<std::size_t>(sent);
            }
        }
        static std::string Frame(const std::string& message)
        {
            std::string bytes(4, '\0');
            const auto length = static_cast<std::uint32_t>(message.size());
            for (std::uint32_t index = 0; index < 4; ++index)
                bytes[index] = static_cast<char>((length >> (24 - index * 8)) & 255);
            return bytes + message;
        }
    };

    void MalformedTraffic()
    {
        using namespace LamaPon;
        constexpr auto hello = R"({"op":"hello","protocol":1,"game":"lamapon.game","version":"1","scene":"main","name":"Raw"})";
        const std::vector<std::string> attacks{
            "{broken", R"({"op":"unknown"})", R"({"op":"input","id":1,"name":"move","data":"1"})",
            R"({"op":"input","id":-1,"name":"move","data":"1"})",
            R"({"op":"input","id":1.0,"name":"move","data":"1"})",
            R"({"op":"input","id":4294967296,"name":"move","data":"1"})",
            R"({"op":"object","id":1})", R"({"op":"ping","id":null})", R"({"op":"pong","id":-1})",
            R"({"op":"ping","id":1,"extra":[[[[[[[[[[0]]]]]]]]]]})"
        };
        for (const auto& attack : attacks)
        {
            NetworkConfiguration config; config.port = 0;
            NetworkSession host, healthy;
            Require(host.Configure(config) && host.Host(), "Malformed host"); host.Update(0);
            NetworkObjectState fixed; fixed.sceneKey = "world.box";
            const auto id = host.Spawn(fixed);
            Require(healthy.Configure(config) && healthy.Join(host.RoomAddress()), "Healthy peer");
            Until([&] { return healthy.State() == NetworkState::Connected; }, host, healthy);
            RawPeer raw(host.RoomAddress());
            // 同一受信batchで認証と攻撃が届いても、切断後のデータを採用しません。
            raw.Wire(RawPeer::Frame(hello) + RawPeer::Frame(attack));
            Until([&] { return host.Statistics().rejectedMessages != 0; }, host, healthy);
            Require(host.IsHost() && host.Members().size() == 2 && host.FindObject(id)
                && host.FindObject(id)->transform.position[0] == 0, "Malformed peer isolated");
            Require(host.BroadcastEvent("still.alive", "ok"), "Host survives malformed traffic");
        }
        for (const std::uint32_t length : { 0u, 1101u, 0xffffffffu })
        {
            NetworkConfiguration config; config.port = 0;
            NetworkSession host, unused;
            Require(host.Configure(config) && host.Host(), "Invalid framing host"); host.Update(0);
            RawPeer raw(host.RoomAddress());
            std::string bytes(4, '\0');
            for (std::uint32_t index = 0; index < 4; ++index)
                bytes[index] = static_cast<char>((length >> (24 - index * 8)) & 255);
            raw.Wire(bytes);
            for (int index = 0; index < 20; ++index)
            { host.Update(0.01f); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            Require(host.IsHost() && host.Members().size() == 1, "Invalid frame not admitted");
            char incoming{};
            u_long nonblocking = 1; ioctlsocket(raw.socket, FIONBIO, &nonblocking);
            const int received = recv(raw.socket, &incoming, 1, 0);
            Require(received == 0 || (received < 0 && WSAGetLastError() == WSAECONNRESET), "Invalid frame socket closed");
        }
        {
            NetworkConfiguration config; config.port = 0;
            NetworkSession host, unused;
            Require(host.Configure(config) && host.Host(), "Fragment host"); host.Update(0);
            RawPeer raw(host.RoomAddress());
            const auto bytes = RawPeer::Frame(hello);
            raw.Wire(bytes.substr(0, 2)); host.Update(0);
            raw.Wire(bytes.substr(2, 5)); host.Update(0);
            Require(host.Members().size() == 1, "Partial frame buffered");
            raw.Wire(bytes.substr(7));
            Until([&] { return host.Members().size() == 2; }, host, unused);
            std::string flood;
            for (int index = 0; index < 300; ++index) flood += RawPeer::Frame(R"({"op":"ping","id":1})");
            raw.Wire(flood);
            Until([&] { return host.Members().size() == 1; }, host, unused);
            Require(host.Statistics().rejectedMessages != 0, "Input rate limit");
        }
    }

    std::string VerificationSetting(const char* name)
    {
        char* value{};
        std::size_t size{};
        if (_dupenv_s(&value, &size, name) != 0 || !value || size <= 1)
        {
            if (value) std::free(value);
            throw std::runtime_error(std::string("Missing verification environment: ") + name);
        }
        std::string setting(value);
        SecureZeroMemory(value, size);
        std::free(value);
        return setting;
    }

    void EpicHostSmoke()
    {
        using namespace LamaPon;
        Require(HasEpicNetworkBackend(), "Configure an EOS SDK build before this manual test.");
        NetworkConfiguration config;
        config.backend = NetworkBackend::EpicOnlineServices;
        config.gameId = "LamaPon.EOS.Verification";
        config.gameVersion = "1";
        config.timeoutSeconds = 60;
        config.eosProductId = VerificationSetting("LAMAPON_EOS_PRODUCT_ID");
        config.eosSandboxId = VerificationSetting("LAMAPON_EOS_SANDBOX_ID");
        config.eosDeploymentId = VerificationSetting("LAMAPON_EOS_DEPLOYMENT_ID");
        config.eosClientId = VerificationSetting("LAMAPON_EOS_CLIENT_ID");
        NetworkSession host;
        Require(host.Configure(config), "EOS host verification configuration");
        std::string previousRoom;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!host.Host("Verification"))
                throw std::runtime_error("EOS host start: " + host.LastError());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
            auto previousTick = std::chrono::steady_clock::now();
            while (!host.IsHost())
            {
                const auto now = std::chrono::steady_clock::now();
                host.Update(std::chrono::duration<float>(now - previousTick).count());
                previousTick = now;
                if (host.State() == NetworkState::Error)
                    throw std::runtime_error("EOS host login: " + host.LastError());
                if (now > deadline) throw std::runtime_error("EOS host login timed out after 45 seconds.");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const auto room = host.RoomAddress();
            Require(room.size() == 59 && room.substr(32, 3) == ":LP"
                && host.Members().size() == 1 && host.LocalPeer() == 1,
                "EOS host ready state and room format");
            if (!previousRoom.empty()) Require(room != previousRoom, "EOS restart uses a new room");
            previousRoom = room;
            // 部屋ID・資格情報をログに載せず、実際のReady到達だけを記録します。
            std::cout << "EOS online login and host ready passed (" << attempt + 1 << "/2).\n";
            host.Stop();
            Require(host.State() == NetworkState::Stopped && host.RoomAddress().empty()
                && host.Members().empty(), "EOS host stop releases session");
        }
        std::cout << "EOS online host creation, stop and restart passed. Peer connection is not tested.\n";
    }

    void EpicSdkLifecycle()
    {
        using namespace LamaPon;
        if (!HasEpicNetworkBackend()) return;
        // 本物のSDKでDLL読み込み・初期化・ID検証・終了を通します。
        // 製品の資格情報もネット接続も使わない試験です。
        constexpr auto variable = "LAMAPON_EOS_TEST_DUMMY_SECRET";
        struct EnvironmentGuard
        {
            const char* name;
            std::string previous;
            bool existed{};
            explicit EnvironmentGuard(const char* key) : name(key)
            {
                char* value{};
                std::size_t size{};
                if (_dupenv_s(&value, &size, name) == 0 && value)
                {
                    previous = value;
                    existed = true;
                    SecureZeroMemory(value, size);
                    std::free(value);
                }
            }
            ~EnvironmentGuard()
            {
                _putenv_s(name, existed ? previous.c_str() : "");
                if (!previous.empty()) SecureZeroMemory(previous.data(), previous.size());
            }
        } restore(variable);
        NetworkConfiguration config;
        config.backend = NetworkBackend::EpicOnlineServices;
        config.eosProductId = config.eosSandboxId = config.eosDeploymentId = config.eosClientId = "test";
        config.eosClientSecretEnvironment = variable;
        NetworkSession session;
        Require(session.Configure(config), "EOS SDK configuration");
        Require(_putenv_s(variable, "") == 0, "Clear test environment");
        Require(!session.Host() && session.State() == NetworkState::Error
            && session.LastError().find("環境変数") != std::string::npos, "EOS missing credential");
        Require(_putenv_s(variable, "offline-test-placeholder") == 0, "Set dummy credential");
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const auto started = session.Join("not-a-product-user:LP0123456789abcdef01234567");
            if (started || session.State() != NetworkState::Error
                || session.LastError().find("ユーザー形式") == std::string::npos)
                throw std::runtime_error("EOS SDK lifecycle attempt " + std::to_string(attempt)
                    + ": " + session.LastError());
            session.Stop();
            Require(session.State() == NetworkState::Stopped, "EOS restart after SDK validation failure");
        }
        std::cout << "EOS SDK real DLL initialization and shutdown (16 restarts) passed.\n";
    }

    void TimeoutAndBackendAvailability()
    {
        using namespace LamaPon;
        NetworkConfiguration config; config.port = 0; config.timeoutSeconds = 5;
        NetworkSession host, client;
        Require(host.Configure(config) && host.Host(), "Timeout host"); host.Update(0);
        Require(client.Configure(config) && client.Join(host.RoomAddress()), "Timeout join");
        Until([&] { return client.State() == NetworkState::Connected; }, host, client);
        NetworkObjectState player; player.prefabKey = "player"; player.owner = client.LocalPeer();
        const auto id = host.Spawn(player);
        Until([&] { return client.FindObject(id) != nullptr; }, host, client);
        host.Update(0); host.Update(5.1f);
        Require(host.Members().size() == 1 && host.FindObject(id) == nullptr, "Silent peer expires with owned objects");
        Until([&] { return client.State() == NetworkState::Error; }, host, client);
        host.Stop();
        Require(!host.Host("bad\nname"), "Control characters rejected");
        Require(!host.Join("example.com"), "DNS not accepted as LAN address");
        if (!HasEpicNetworkBackend())
        {
            config.backend = NetworkBackend::EpicOnlineServices;
            config.eosProductId = config.eosSandboxId = config.eosDeploymentId = config.eosClientId = "test";
            Require(host.Configure(config) && !host.Host() && host.State() == NetworkState::Error
                && !host.LastError().empty(), "Missing SDK reports actionable error");
        }
    }
}

int main(const int argc, char* argv[])
{
    try
    {
        // 通常のCTestは資格情報や外部サービスを使いません。
        // この明示的なオプションだけが実際のEOS認証と部屋作成を行います。
        if (argc == 2 && std::string_view(argv[1]) == "--eos-host-smoke")
        {
            EpicHostSmoke();
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Usage: LamaPonNetworkSessionTests [--eos-host-smoke]");
        ConnectionAndReplication(); IncompatibleGames(); Bounds(); MalformedTraffic(); TimeoutAndBackendAvailability(); EpicSdkLifecycle();
        std::cout << "P2P connection, baseline, ownership, replication, events, limits, departure and restart passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
