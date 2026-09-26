#include "LamaPon/Online/EpicNetworkTransportSdk.h"

#include <eos_sdk.h>
#include <eos_connect.h>
#include <eos_p2p.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>

namespace LamaPon::Detail
{
    namespace
    {
        // SDKの初期化はプロセス単位です。全操作をゲームスレッドで行います。
        std::uint32_t sdkUsers{};
        bool ownsSdk{};

        bool AcquireSdk()
        {
            if (sdkUsers != 0) { ++sdkUsers; return true; }
            EOS_InitializeOptions options{};
            options.ApiVersion = EOS_INITIALIZE_API_LATEST;
            options.ProductName = "LamaPon";
            options.ProductVersion = "1";
            const auto result = EOS_Initialize(&options);
            if (result != EOS_EResult::EOS_Success && result != EOS_EResult::EOS_AlreadyConfigured) return false;
            ownsSdk = result == EOS_EResult::EOS_Success;
            sdkUsers = 1;
            return true;
        }
        void ReleaseSdk()
        {
            if (sdkUsers == 0) return;
            if (--sdkUsers == 0 && ownsSdk) { EOS_Shutdown(); ownsSdk = false; }
        }
        std::string UserKey(EOS_ProductUserId user)
        {
            std::array<char, EOS_PRODUCTUSERID_MAX_LENGTH + 1> text{};
            int32_t length = static_cast<int32_t>(text.size());
            if (EOS_ProductUserId_ToString(user, text.data(), &length) != EOS_EResult::EOS_Success) return {};
            return text.data();
        }
        std::string NewSocketName()
        {
            std::array<unsigned char, 12> random{};
            if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
            constexpr char hex[] = "0123456789abcdef";
            std::string name = "LP";
            for (const auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
            return name;
        }

        class EpicTransport final : public INetworkTransport
        {
        public:
            ~EpicTransport() override { Stop(); }

            bool Start(const NetworkConfiguration& configuration, const bool host,
                const std::string& address, const std::string& name) override
            {
                Stop(); m_error.clear(); m_host = host; m_name = name;
                m_socket = {}; m_socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
                std::string remoteHostKey;
                if (host)
                {
                    const auto socketName = NewSocketName();
                    if (socketName.empty()) { m_error = "部屋IDを生成できません。"; return false; }
                    strcpy_s(m_socket.SocketName, socketName.c_str());
                }
                else
                {
                    const auto split = address.find(':');
                    if (split == std::string::npos || split == 0 || address.size() - split - 1 != 26)
                    { m_error = "ホストのEOS部屋IDをそのまま入力してください。"; return false; }
                    remoteHostKey = address.substr(0, split);
                    const auto socketName = address.substr(split + 1);
                    if (socketName.substr(0, 2) != "LP"
                        || !std::ranges::all_of(socketName.substr(2), [](char c)
                            { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                    { m_error = "EOS部屋IDの形式が正しくありません。"; return false; }
                    strcpy_s(m_socket.SocketName, socketName.c_str());
                    m_room = address;
                }
                char* credential{};
                std::size_t size{};
                if (_dupenv_s(&credential, &size, configuration.eosClientSecretEnvironment.c_str()) != 0
                    || credential == nullptr || size <= 1)
                {
                    if (credential) std::free(credential);
                    m_error = "EOSのゲームクライアント用資格情報が環境変数へ設定されていません。";
                    return false;
                }
                m_secret = credential;
                SecureZeroMemory(credential, size); std::free(credential);
                // delay-loadの例外になる前に、配布先のDLL欠落を通常のエラーにします。
                m_sdkLibrary = LoadLibraryExW(L"EOSSDK-Win64-Shipping.dll", nullptr,
                    LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!m_sdkLibrary)
                { m_error = "EOS runtime DLLを実行ファイルの横へ配置してください。"; ClearSecret(); return false; }
                if (!AcquireSdk()) { m_error = "EOS SDKを初期化できません。"; ClearSecret(); return false; }
                m_acquired = true;
                // ID変換もSDK APIなので、DLL読み込みと初期化の後に行います。
                if (!host)
                {
                    m_remoteHost = EOS_ProductUserId_FromString(remoteHostKey.c_str());
                    // FromString/IsValidは文字列形式を検証しません。
                    // 共有する部屋IDのユーザー部分は32桁の16進IDに限定します。
                    if (remoteHostKey.size() != EOS_PRODUCTUSERID_MAX_LENGTH
                        || !std::ranges::all_of(remoteHostKey, [](char c)
                            { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                                || (c >= 'A' && c <= 'F'); })
                        || !EOS_ProductUserId_IsValid(m_remoteHost))
                    { m_error = "EOS部屋IDのユーザー形式が正しくありません。"; Stop(); return false; }
                }
                EOS_Platform_Options options{};
                options.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
                options.ProductId = configuration.eosProductId.c_str();
                options.SandboxId = configuration.eosSandboxId.c_str();
                options.DeploymentId = configuration.eosDeploymentId.c_str();
                options.ClientCredentials.ClientId = configuration.eosClientId.c_str();
                options.ClientCredentials.ClientSecret = m_secret.c_str();
                // プレイヤーがホストでも専用サーバーではありません。
                options.bIsServer = EOS_FALSE;
                options.Flags = EOS_PF_DISABLE_OVERLAY;
                options.TickBudgetInMilliseconds = 2;
                m_platform = EOS_Platform_Create(&options);
                if (!m_platform) { m_error = "EOSの製品設定を初期化できません。"; Stop(); return false; }
                m_connect = EOS_Platform_GetConnectInterface(m_platform);
                m_p2p = EOS_Platform_GetP2PInterface(m_platform);
                EOS_Connect_CreateDeviceIdOptions device{};
                device.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
                device.DeviceModel = "LamaPon Windows";
                EOS_Connect_CreateDeviceId(m_connect, &device, this, DeviceCreated);
                return true;
            }

            void Stop() noexcept override
            {
                if (m_p2p)
                {
                    if (m_requestNotify != EOS_INVALID_NOTIFICATIONID)
                        EOS_P2P_RemoveNotifyPeerConnectionRequest(m_p2p, m_requestNotify);
                    if (m_closedNotify != EOS_INVALID_NOTIFICATIONID)
                        EOS_P2P_RemoveNotifyPeerConnectionClosed(m_p2p, m_closedNotify);
                    if (m_local)
                    {
                        EOS_P2P_CloseConnectionsOptions close{};
                        close.ApiVersion = EOS_P2P_CLOSECONNECTIONS_API_LATEST;
                        close.LocalUserId = m_local; close.SocketId = &m_socket;
                        EOS_P2P_CloseConnections(m_p2p, &close);
                    }
                }
                if (m_connect && m_expirationNotify != EOS_INVALID_NOTIFICATIONID)
                    EOS_Connect_RemoveNotifyAuthExpiration(m_connect, m_expirationNotify);
                if (m_connect && m_statusNotify != EOS_INVALID_NOTIFICATIONID)
                    EOS_Connect_RemoveNotifyLoginStatusChanged(m_connect, m_statusNotify);
                // Platformを解放してからthisを破棄し、非同期コールバックの
                // ClientDataが破棄済みのTransportを指さないようにします。
                if (m_platform) EOS_Platform_Release(m_platform);
                m_platform = nullptr; m_connect = nullptr; m_p2p = nullptr;
                m_local = nullptr; m_remoteHost = nullptr;
                m_requestNotify = m_closedNotify = m_expirationNotify = m_statusNotify = EOS_INVALID_NOTIFICATIONID;
                m_peers.clear(); m_pending.clear(); m_nextPeer = 1; m_room.clear(); m_loggingIn = false;
                if (m_acquired) { ReleaseSdk(); m_acquired = false; }
                if (m_sdkLibrary) FreeLibrary(m_sdkLibrary);
                m_sdkLibrary = nullptr;
                ClearSecret();
            }

            std::vector<TransportEvent> Poll(float) override
            {
                if (!m_platform) return {};
                EOS_Platform_Tick(m_platform);
                auto result = std::move(m_pending); m_pending.clear();
                if (!m_local) return result;
                EOS_P2P_ReceivePacketOptions options{};
                options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
                options.LocalUserId = m_local;
                options.MaxDataSizeBytes = EOS_P2P_MAX_PACKET_SIZE;
                for (int count = 0; count < 64; ++count)
                {
                    std::array<unsigned char, EOS_P2P_MAX_PACKET_SIZE> buffer{};
                    EOS_ProductUserId sender{};
                    EOS_P2P_SocketId socket{}; socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
                    uint8_t channel{}; uint32_t length{};
                    const auto received = EOS_P2P_ReceivePacket(m_p2p, &options, &sender,
                        &socket, &channel, buffer.data(), &length);
                    if (received == EOS_EResult::EOS_NotFound) break;
                    if (received != EOS_EResult::EOS_Success) { Fail("EOSの受信処理に失敗しました。"); break; }
                    if (channel != 0 || std::strcmp(socket.SocketName, m_socket.SocketName) != 0) continue;
                    const auto key = UserKey(sender);
                    const auto peer = std::ranges::find_if(m_peers, [&key](const auto& value) { return UserKey(value.second) == key; });
                    if (peer == m_peers.end()) continue;
                    if (length == 0 || length > NetworkPacketMaxBytes) { Disconnect(peer->first); continue; }
                    result.push_back({ TransportEventKind::Message, peer->first,
                        std::string(reinterpret_cast<const char*>(buffer.data()), length) });
                }
                result.insert(result.end(), m_pending.begin(), m_pending.end()); m_pending.clear();
                return result;
            }

            bool Send(const TransportPeer peer, const std::string& packet) override
            {
                const auto found = m_peers.find(peer);
                if (!m_local || found == m_peers.end() || packet.empty()
                    || packet.size() > NetworkPacketMaxBytes) return false;
                EOS_P2P_SendPacketOptions send{};
                send.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
                send.LocalUserId = m_local; send.RemoteUserId = found->second;
                send.SocketId = &m_socket; send.Channel = 0;
                send.DataLengthBytes = static_cast<uint32_t>(packet.size()); send.Data = packet.data();
                send.bAllowDelayedDelivery = EOS_TRUE;
                send.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
                send.bDisableAutoAcceptConnection = EOS_TRUE;
                // EOSの送信キューが満杯ならSessionが接続を終了します。
                // 無制限のアプリ側再送キューは作りません。
                return EOS_P2P_SendPacket(m_p2p, &send) == EOS_EResult::EOS_Success;
            }

            void Disconnect(const TransportPeer peer) override
            {
                const auto found = m_peers.find(peer);
                if (found == m_peers.end()) return;
                EOS_P2P_CloseConnectionOptions options{};
                options.ApiVersion = EOS_P2P_CLOSECONNECTION_API_LATEST;
                options.LocalUserId = m_local; options.RemoteUserId = found->second; options.SocketId = &m_socket;
                EOS_P2P_CloseConnection(m_p2p, &options);
                m_peers.erase(found);
                Queue({ TransportEventKind::Disconnected, peer, {} });
            }
            std::string Address() const override { return m_room; }
            std::string Error() const override { return m_error; }

        private:
            void ClearSecret() noexcept
            {
                if (!m_secret.empty()) SecureZeroMemory(m_secret.data(), m_secret.size());
                m_secret.clear();
            }
            void Queue(TransportEvent event)
            {
                if (m_pending.size() < 64) m_pending.push_back(std::move(event));
                else Fail("EOS通知キューの上限を超えました。");
            }
            void Fail(std::string message)
            {
                if (!m_error.empty()) return;
                m_error = std::move(message); m_pending.clear();
                m_pending.push_back({ TransportEventKind::Error, 0, m_error });
            }
            void Login()
            {
                if (m_loggingIn) return;
                m_loggingIn = true;
                // EOSの非同期APIへ渡す文字列はTransportが所有します。
                EOS_Connect_Credentials credentials{};
                credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
                credentials.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
                EOS_Connect_UserLoginInfo user{};
                user.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST; user.DisplayName = m_name.c_str();
                EOS_Connect_LoginOptions options{};
                options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
                options.Credentials = &credentials; options.UserLoginInfo = &user;
                EOS_Connect_Login(m_connect, &options, this, LoggedIn);
            }
            void Ready(EOS_ProductUserId local)
            {
                m_loggingIn = false;
                if (m_local) { if (m_local != local) Fail("EOSユーザーが接続中に変更されました。"); return; }
                m_local = local;
                EOS_P2P_SetRelayControlOptions relay{};
                relay.ApiVersion = EOS_P2P_SETRELAYCONTROL_API_LATEST;
                relay.RelayControl = EOS_ERelayControl::EOS_RC_AllowRelays;
                if (EOS_P2P_SetRelayControl(m_p2p, &relay) != EOS_EResult::EOS_Success)
                { Fail("EOS中継を設定できません。"); return; }
                EOS_P2P_SetPacketQueueSizeOptions queue{};
                queue.ApiVersion = EOS_P2P_SETPACKETQUEUESIZE_API_LATEST;
                queue.IncomingPacketQueueMaxSizeBytes = 256 * 1024;
                queue.OutgoingPacketQueueMaxSizeBytes = 256 * 1024;
                if (EOS_P2P_SetPacketQueueSize(m_p2p, &queue) != EOS_EResult::EOS_Success)
                { Fail("EOS通信キューを設定できません。"); return; }
                EOS_P2P_AddNotifyPeerConnectionRequestOptions request{};
                request.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
                request.LocalUserId = local; request.SocketId = &m_socket;
                m_requestNotify = EOS_P2P_AddNotifyPeerConnectionRequest(m_p2p, &request, this, Requested);
                EOS_P2P_AddNotifyPeerConnectionClosedOptions closed{};
                closed.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
                closed.LocalUserId = local; closed.SocketId = &m_socket;
                m_closedNotify = EOS_P2P_AddNotifyPeerConnectionClosed(m_p2p, &closed, this, Closed);
                EOS_Connect_AddNotifyAuthExpirationOptions expiration{};
                expiration.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
                m_expirationNotify = EOS_Connect_AddNotifyAuthExpiration(m_connect, &expiration, this, Expiring);
                EOS_Connect_AddNotifyLoginStatusChangedOptions status{};
                status.ApiVersion = EOS_CONNECT_ADDNOTIFYLOGINSTATUSCHANGED_API_LATEST;
                m_statusNotify = EOS_Connect_AddNotifyLoginStatusChanged(m_connect, &status, this, StatusChanged);
                if (m_requestNotify == EOS_INVALID_NOTIFICATIONID || m_closedNotify == EOS_INVALID_NOTIFICATIONID
                    || m_expirationNotify == EOS_INVALID_NOTIFICATIONID || m_statusNotify == EOS_INVALID_NOTIFICATIONID)
                { Fail("EOS通知を登録できません。"); return; }
                if (m_host)
                {
                    m_room = UserKey(local) + ":" + m_socket.SocketName;
                    Queue({ TransportEventKind::Ready, 0, {} });
                }
                else
                {
                    if (local == m_remoteHost) { Fail("EOSの接続試験は異なる端末のユーザーで実行してください。"); return; }
                    EOS_P2P_AcceptConnectionOptions accept{};
                    accept.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
                    accept.LocalUserId = local; accept.RemoteUserId = m_remoteHost; accept.SocketId = &m_socket;
                    if (EOS_P2P_AcceptConnection(m_p2p, &accept) != EOS_EResult::EOS_Success)
                    { Fail("EOSホストへの接続を開始できません。"); return; }
                    m_peers.emplace(1, m_remoteHost);
                    Queue({ TransportEventKind::Connected, 1, {} });
                }
            }

            static void EOS_CALL DeviceCreated(const EOS_Connect_CreateDeviceIdCallbackInfo* info)
            {
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                if (info->ResultCode == EOS_EResult::EOS_Success || info->ResultCode == EOS_EResult::EOS_DuplicateNotAllowed) self.Login();
                else self.Fail("EOS端末アカウントを準備できません。ConnectのClient Policyを確認してください。");
            }
            static void EOS_CALL LoggedIn(const EOS_Connect_LoginCallbackInfo* info)
            {
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                if (info->ResultCode == EOS_EResult::EOS_Success) self.Ready(info->LocalUserId);
                else if (info->ResultCode == EOS_EResult::EOS_InvalidUser && info->ContinuanceToken)
                {
                    EOS_Connect_CreateUserOptions options{};
                    options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
                    options.ContinuanceToken = info->ContinuanceToken;
                    EOS_Connect_CreateUser(self.m_connect, &options, &self, UserCreated);
                }
                else { self.m_loggingIn = false; self.Fail("EOSログインに失敗しました。製品設定・通信環境・Connect権限を確認してください。"); }
            }
            static void EOS_CALL UserCreated(const EOS_Connect_CreateUserCallbackInfo* info)
            {
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                if (info->ResultCode == EOS_EResult::EOS_Success) self.Ready(info->LocalUserId);
                else { self.m_loggingIn = false; self.Fail("EOSのユーザー作成に失敗しました。"); }
            }
            static void EOS_CALL Requested(const EOS_P2P_OnIncomingConnectionRequestInfo* info)
            {
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                if (!info->SocketId || std::strcmp(info->SocketId->SocketName, self.m_socket.SocketName) != 0) return;
                if (!self.m_host && info->RemoteUserId != self.m_remoteHost) return;
                const auto key = UserKey(info->RemoteUserId);
                if (key.empty()) return;
                if (std::ranges::any_of(self.m_peers, [&key](const auto& value) { return UserKey(value.second) == key; })) return;
                if (self.m_peers.size() >= 8) return;
                EOS_P2P_AcceptConnectionOptions options{};
                options.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
                options.LocalUserId = self.m_local; options.RemoteUserId = info->RemoteUserId;
                options.SocketId = &self.m_socket;
                if (EOS_P2P_AcceptConnection(self.m_p2p, &options) != EOS_EResult::EOS_Success) return;
                const auto peer = self.m_nextPeer++;
                self.m_peers.emplace(peer, info->RemoteUserId);
                self.Queue({ TransportEventKind::Connected, peer, {} });
            }
            static void EOS_CALL Closed(const EOS_P2P_OnRemoteConnectionClosedInfo* info)
            {
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                const auto key = UserKey(info->RemoteUserId);
                const auto found = std::ranges::find_if(self.m_peers, [&key](const auto& value) { return UserKey(value.second) == key; });
                if (found == self.m_peers.end()) return;
                const auto id = found->first; self.m_peers.erase(found);
                self.Queue({ TransportEventKind::Disconnected, id, {} });
            }
            static void EOS_CALL Expiring(const EOS_Connect_AuthExpirationCallbackInfo* info)
            { static_cast<EpicTransport*>(info->ClientData)->Login(); }
            static void EOS_CALL StatusChanged(const EOS_Connect_LoginStatusChangedCallbackInfo* info)
            {
                if (info->CurrentStatus == EOS_ELoginStatus::EOS_LS_NotLoggedIn)
                    static_cast<EpicTransport*>(info->ClientData)->Fail("EOSのログインセッションが終了しました。");
            }

            bool m_host{};
            bool m_acquired{};
            bool m_loggingIn{};
            std::string m_name;
            std::string m_secret;
            std::string m_room;
            std::string m_error;
            EOS_HPlatform m_platform{};
            HMODULE m_sdkLibrary{};
            EOS_HConnect m_connect{};
            EOS_HP2P m_p2p{};
            EOS_ProductUserId m_local{};
            EOS_ProductUserId m_remoteHost{};
            EOS_P2P_SocketId m_socket{};
            EOS_NotificationId m_requestNotify{ EOS_INVALID_NOTIFICATIONID };
            EOS_NotificationId m_closedNotify{ EOS_INVALID_NOTIFICATIONID };
            EOS_NotificationId m_expirationNotify{ EOS_INVALID_NOTIFICATIONID };
            EOS_NotificationId m_statusNotify{ EOS_INVALID_NOTIFICATIONID };
            TransportPeer m_nextPeer{ 1 };
            std::map<TransportPeer, EOS_ProductUserId> m_peers;
            std::vector<TransportEvent> m_pending;
        };
    }

    std::unique_ptr<INetworkTransport> CreateEpicSdkTransport()
    {
        return std::make_unique<EpicTransport>();
    }
}
