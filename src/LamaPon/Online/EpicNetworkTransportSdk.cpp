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
        // SDKはプロセス単位で参照管理し、すべての操作をゲームスレッドで行います。
        // この通信実装のSDK利用者数
        std::uint32_t sdkUsers{};
        // この実装がSDKを初期化したか
        bool ownsSdk{};

        // 初期化済みSDKを借用または初期化し、プロセス内の参照数を増やす。
        bool AcquireSdk()
        {
            if (sdkUsers != 0) { ++sdkUsers; return true; }
            // EOS APIの呼び出し設定
            EOS_InitializeOptions options{};
            options.ApiVersion = EOS_INITIALIZE_API_LATEST;
            options.ProductName = "LamaPon";
            options.ProductVersion = "1";
            // 初期化結果または通信イベント
            const auto result = EOS_Initialize(&options);
            if (result != EOS_EResult::EOS_Success && result != EOS_EResult::EOS_AlreadyConfigured) return false;
            ownsSdk = result == EOS_EResult::EOS_Success;
            sdkUsers = 1;
            return true;
        }
        // 参照数を減らし、自身が初期化したSDKだけを最後に終了する。
        void ReleaseSdk()
        {
            if (sdkUsers == 0) return;
            if (--sdkUsers == 0 && ownsSdk) { EOS_Shutdown(); ownsSdk = false; }
        }
        // EOSユーザーIDを文字列へ変換する(user: 変換するユーザーID)。
        std::string UserKey(EOS_ProductUserId user)
        {
            // EOSユーザーIDの出力領域
            std::array<char, EOS_PRODUCTUSERID_MAX_LENGTH + 1> text{};
            // 文字領域サイズまたは受信長
            int32_t length = static_cast<int32_t>(text.size());
            if (EOS_ProductUserId_ToString(user, text.data(), &length) != EOS_EResult::EOS_Success) return {};
            return text.data();
        }
        // LPと12バイトの乱数で部屋固有のソケット名を作る。
        std::string NewSocketName()
        {
            // ソケット名に使う12バイト乱数
            std::array<unsigned char, 12> random{};
            if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
            // 小文字16進数の文字表
            constexpr char hex[] = "0123456789abcdef";
            // 部屋ソケットの識別文字列
            std::string name = "LP";
            // ソケット名に変換するバイト
            for (const auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
            return name;
        }

        class EpicTransport final : public INetworkTransport
        {
        public:
            // 非同期通知とPlatformを停止してからSDKとDLLを解放する。
            ~EpicTransport() override { Stop(); }

            // 部屋IDと資格情報を検証して端末ログインを始める(configuration: EOS接続設定, host: ホスト側か, address: 接続先のEOS部屋ID, name: 参加者の表示名)。
            bool Start(const NetworkConfiguration& configuration, const bool host,
                const std::string& address, const std::string& name) override
            {
                Stop(); m_error.clear(); m_host = host; m_name = name;
                m_socket = {}; m_socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
                // 接続先ホストのEOSユーザーID
                std::string remoteHostKey;
                if (host)
                {
                    // LPと24桁の16進ソケット名
                    const auto socketName = NewSocketName();
                    if (socketName.empty()) { m_error = "部屋IDを生成できません。"; return false; }
                    strcpy_s(m_socket.SocketName, socketName.c_str());
                }
                else
                {
                    // ユーザーIDとソケット名の区切り
                    const auto split = address.find(':');
                    if (split == std::string::npos || split == 0 || address.size() - split - 1 != 26)
                    { m_error = "ホストのEOS部屋IDをそのまま入力してください。"; return false; }
                    remoteHostKey = address.substr(0, split);
                    // LPと24桁の16進ソケット名
                    const auto socketName = address.substr(split + 1);
                    // 小文字16進数を検証する(c: ソケット名の1文字)。
                    if (socketName.substr(0, 2) != "LP"
                        || !std::ranges::all_of(socketName.substr(2), [](char c)
                            { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                    { m_error = "EOS部屋IDの形式が正しくありません。"; return false; }
                    strcpy_s(m_socket.SocketName, socketName.c_str());
                    m_room = address;
                }
                // 環境変数から複写した秘密
                char* credential{};
                // 秘密の終端を含むバイト数
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
                    // SDKのID変換だけでは文字列形式を検証できないため、ユーザーIDは32桁の16進数に限定します。
                    // 16進数のユーザーIDを検証する(c: ユーザーIDの1文字)。
                    if (remoteHostKey.size() != EOS_PRODUCTUSERID_MAX_LENGTH
                        || !std::ranges::all_of(remoteHostKey, [](char c)
                            { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                                || (c >= 'A' && c <= 'F'); })
                        || !EOS_ProductUserId_IsValid(m_remoteHost))
                    { m_error = "EOS部屋IDのユーザー形式が正しくありません。"; Stop(); return false; }
                }
                // EOS APIの呼び出し設定
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
                // 端末IDの作成設定
                EOS_Connect_CreateDeviceIdOptions device{};
                device.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
                device.DeviceModel = "LamaPon Windows";
                EOS_Connect_CreateDeviceId(m_connect, &device, this, DeviceCreated);
                return true;
            }

            // 通知を解除してPlatformを解放し、資格情報を消去する。
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
                        // 全接続を閉じるEOS設定
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
                // Platformを解放してからthisを破棄し、非同期コールバックのClientDataが破棄済みのTransportを指さないようにします。
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

            // SDKを進め、最大64パケットの受信と通知を取り出す。
            std::vector<TransportEvent> Poll(float) override
            {
                if (!m_platform) return {};
                EOS_Platform_Tick(m_platform);
                // 初期化結果または通信イベント
                auto result = std::move(m_pending); m_pending.clear();
                if (!m_local) return result;
                // EOS APIの呼び出し設定
                EOS_P2P_ReceivePacketOptions options{};
                options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
                options.LocalUserId = m_local;
                options.MaxDataSizeBytes = EOS_P2P_MAX_PACKET_SIZE;
                // 最大64件の受信処理番号
                for (int count = 0; count < 64; ++count)
                {
                    // EOSの受信領域
                    std::array<unsigned char, EOS_P2P_MAX_PACKET_SIZE> buffer{};
                    // 受信元のEOSユーザーID
                    EOS_ProductUserId sender{};
                    // 受信先のEOSソケットID
                    EOS_P2P_SocketId socket{}; socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
                    // 受信したチャンネル番号
                    uint8_t channel{};
                    // 文字領域サイズまたは受信長
                    uint32_t length{};
                    // EOS受信APIの終了コード
                    const auto received = EOS_P2P_ReceivePacket(m_p2p, &options, &sender,
                        &socket, &channel, buffer.data(), &length);
                    if (received == EOS_EResult::EOS_NotFound) break;
                    if (received != EOS_EResult::EOS_Success) { Fail("EOSの受信処理に失敗しました。"); break; }
                    if (channel != 0 || std::strcmp(socket.SocketName, m_socket.SocketName) != 0) continue;
                    // 照合するEOSユーザーID文字列
                    const auto key = UserKey(sender);
                    // 送信元を照合する(value: 接続IDとEOSユーザーID)。
                    // TCP互換の内部接続IDまたは位置
                    const auto peer = std::ranges::find_if(m_peers, [&key](const auto& value) { return UserKey(value.second) == key; });
                    if (peer == m_peers.end()) continue;
                    if (length == 0 || length > NetworkPacketMaxBytes) { Disconnect(peer->first); continue; }
                    result.push_back({ TransportEventKind::Message, peer->first,
                        std::string(reinterpret_cast<const char*>(buffer.data()), length) });
                }
                result.insert(result.end(), m_pending.begin(), m_pending.end()); m_pending.clear();
                return result;
            }

            // チャンネル0で順序保証付き送信を要求する(peer: 宛先の内部接続ID, packet: 送信内容)。
            bool Send(const TransportPeer peer, const std::string& packet) override
            {
                // 登録された相手の検索結果
                const auto found = m_peers.find(peer);
                if (!m_local || found == m_peers.end() || packet.empty()
                    || packet.size() > NetworkPacketMaxBytes) return false;
                // 信頼性と順序を指定する送信設定
                EOS_P2P_SendPacketOptions send{};
                send.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
                send.LocalUserId = m_local; send.RemoteUserId = found->second;
                send.SocketId = &m_socket; send.Channel = 0;
                send.DataLengthBytes = static_cast<uint32_t>(packet.size()); send.Data = packet.data();
                send.bAllowDelayedDelivery = EOS_TRUE;
                send.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
                send.bDisableAutoAcceptConnection = EOS_TRUE;
                // 送信キュー満杯はSessionが切断として扱い、アプリ側の再送キューは追加しません。
                return EOS_P2P_SendPacket(m_p2p, &send) == EOS_EResult::EOS_Success;
            }

            // EOSの接続を閉じて切断通知を残す(peer: 切断する内部接続ID)。
            void Disconnect(const TransportPeer peer) override
            {
                // 登録された相手の検索結果
                const auto found = m_peers.find(peer);
                if (found == m_peers.end()) return;
                // EOS APIの呼び出し設定
                EOS_P2P_CloseConnectionOptions options{};
                options.ApiVersion = EOS_P2P_CLOSECONNECTION_API_LATEST;
                options.LocalUserId = m_local; options.RemoteUserId = found->second; options.SocketId = &m_socket;
                EOS_P2P_CloseConnection(m_p2p, &options);
                m_peers.erase(found);
                Queue({ TransportEventKind::Disconnected, peer, {} });
            }
            // 共有するEOS部屋IDを返す。
            std::string Address() const override { return m_room; }
            // 直近のEOSエラーを返す。
            std::string Error() const override { return m_error; }

        private:
            // クライアント秘密を上書きして保持文字列を消す。
            void ClearSecret() noexcept
            {
                if (!m_secret.empty()) SecureZeroMemory(m_secret.data(), m_secret.size());
                m_secret.clear();
            }
            // 最大64件の通知を保留し、超過時はエラー通知へ置き換える(event: 保留する通知)。
            void Queue(TransportEvent event)
            {
                if (m_pending.size() < 64) m_pending.push_back(std::move(event));
                else Fail("EOS通知キューの上限を超えました。");
            }
            // 最初の失敗を保持し、保留通知をエラーへ置き換える(message: 失敗理由)。
            void Fail(std::string message)
            {
                if (!m_error.empty()) return;
                m_error = std::move(message); m_pending.clear();
                m_pending.push_back({ TransportEventKind::Error, 0, m_error });
            }
            // 表示名を所有したまま端末IDログインを要求する。
            void Login()
            {
                if (m_loggingIn) return;
                m_loggingIn = true;
                // EOSの非同期APIへ渡す文字列はTransportが所有します。
                // 端末IDによるログイン方式
                EOS_Connect_Credentials credentials{};
                credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
                credentials.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
                // ログインに渡す表示名の設定
                EOS_Connect_UserLoginInfo user{};
                user.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST; user.DisplayName = m_name.c_str();
                // EOS APIの呼び出し設定
                EOS_Connect_LoginOptions options{};
                options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
                options.Credentials = &credentials; options.UserLoginInfo = &user;
                EOS_Connect_Login(m_connect, &options, this, LoggedIn);
            }
            // ユーザーを固定して中継・キュー・通知を登録する(local: ログインしたEOSユーザーID)。
            void Ready(EOS_ProductUserId local)
            {
                m_loggingIn = false;
                if (m_local) { if (m_local != local) Fail("EOSユーザーが接続中に変更されました。"); return; }
                m_local = local;
                // 中継サーバーの許可設定
                EOS_P2P_SetRelayControlOptions relay{};
                relay.ApiVersion = EOS_P2P_SETRELAYCONTROL_API_LATEST;
                relay.RelayControl = EOS_ERelayControl::EOS_RC_AllowRelays;
                if (EOS_P2P_SetRelayControl(m_p2p, &relay) != EOS_EResult::EOS_Success)
                { Fail("EOS中継を設定できません。"); return; }
                // 送受信キューの上限設定
                EOS_P2P_SetPacketQueueSizeOptions queue{};
                queue.ApiVersion = EOS_P2P_SETPACKETQUEUESIZE_API_LATEST;
                queue.IncomingPacketQueueMaxSizeBytes = 256 * 1024;
                queue.OutgoingPacketQueueMaxSizeBytes = 256 * 1024;
                if (EOS_P2P_SetPacketQueueSize(m_p2p, &queue) != EOS_EResult::EOS_Success)
                { Fail("EOS通信キューを設定できません。"); return; }
                // 接続要求の通知登録設定
                EOS_P2P_AddNotifyPeerConnectionRequestOptions request{};
                request.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
                request.LocalUserId = local; request.SocketId = &m_socket;
                m_requestNotify = EOS_P2P_AddNotifyPeerConnectionRequest(m_p2p, &request, this, Requested);
                // 切断の通知登録設定
                EOS_P2P_AddNotifyPeerConnectionClosedOptions closed{};
                closed.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
                closed.LocalUserId = local; closed.SocketId = &m_socket;
                m_closedNotify = EOS_P2P_AddNotifyPeerConnectionClosed(m_p2p, &closed, this, Closed);
                // 認証期限の通知登録設定
                EOS_Connect_AddNotifyAuthExpirationOptions expiration{};
                expiration.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
                m_expirationNotify = EOS_Connect_AddNotifyAuthExpiration(m_connect, &expiration, this, Expiring);
                // ログイン状態の通知登録設定
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
                    // ホストへの接続許可設定
                    EOS_P2P_AcceptConnectionOptions accept{};
                    accept.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
                    accept.LocalUserId = local; accept.RemoteUserId = m_remoteHost; accept.SocketId = &m_socket;
                    if (EOS_P2P_AcceptConnection(m_p2p, &accept) != EOS_EResult::EOS_Success)
                    { Fail("EOSホストへの接続を開始できません。"); return; }
                    m_peers.emplace(1, m_remoteHost);
                    Queue({ TransportEventKind::Connected, 1, {} });
                }
            }

            // 端末IDの作成成功または既存登録からログインへ進む(info: 端末ID作成の完了通知)。
            static void EOS_CALL DeviceCreated(const EOS_Connect_CreateDeviceIdCallbackInfo* info)
            {
                // コールバック元の通信状態
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                if (info->ResultCode == EOS_EResult::EOS_Success || info->ResultCode == EOS_EResult::EOS_DuplicateNotAllowed) self.Login();
                else self.Fail("EOS端末アカウントを準備できません。ConnectのClient Policyを確認してください。");
            }
            // ログインを確定し、新規ユーザーなら作成を要求する(info: ログインの完了通知)。
            static void EOS_CALL LoggedIn(const EOS_Connect_LoginCallbackInfo* info)
            {
                // コールバック元の通信状態
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                if (info->ResultCode == EOS_EResult::EOS_Success) self.Ready(info->LocalUserId);
                else if (info->ResultCode == EOS_EResult::EOS_InvalidUser && info->ContinuanceToken)
                {
                    // EOS APIの呼び出し設定
                    EOS_Connect_CreateUserOptions options{};
                    options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
                    options.ContinuanceToken = info->ContinuanceToken;
                    EOS_Connect_CreateUser(self.m_connect, &options, &self, UserCreated);
                }
                else { self.m_loggingIn = false; self.Fail("EOSログインに失敗しました。製品設定・通信環境・Connect権限を確認してください。"); }
            }
            // 新規ユーザー作成後に通信準備を進める(info: ユーザー作成の完了通知)。
            static void EOS_CALL UserCreated(const EOS_Connect_CreateUserCallbackInfo* info)
            {
                // コールバック元の通信状態
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                if (info->ResultCode == EOS_EResult::EOS_Success) self.Ready(info->LocalUserId);
                else { self.m_loggingIn = false; self.Fail("EOSのユーザー作成に失敗しました。"); }
            }
            // 部屋と相手を照合し、未登録なら最大8接続まで許可する(info: P2P接続要求の通知)。
            static void EOS_CALL Requested(const EOS_P2P_OnIncomingConnectionRequestInfo* info)
            {
                // コールバック元の通信状態
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                if (!info->SocketId || std::strcmp(info->SocketId->SocketName, self.m_socket.SocketName) != 0) return;
                if (!self.m_host && info->RemoteUserId != self.m_remoteHost) return;
                // 照合するEOSユーザーID文字列
                const auto key = UserKey(info->RemoteUserId);
                if (key.empty()) return;
                // 登録済みかを調べる(value: 接続IDとEOSユーザーID)。
                if (std::ranges::any_of(self.m_peers, [&key](const auto& value) { return UserKey(value.second) == key; })) return;
                if (self.m_peers.size() >= 8) return;
                // EOS APIの呼び出し設定
                EOS_P2P_AcceptConnectionOptions options{};
                options.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
                options.LocalUserId = self.m_local; options.RemoteUserId = info->RemoteUserId;
                options.SocketId = &self.m_socket;
                if (EOS_P2P_AcceptConnection(self.m_p2p, &options) != EOS_EResult::EOS_Success) return;
                // TCP互換の内部接続IDまたは位置
                const auto peer = self.m_nextPeer++;
                self.m_peers.emplace(peer, info->RemoteUserId);
                self.Queue({ TransportEventKind::Connected, peer, {} });
            }
            // 登録相手の切断を通知へ変換する(info: P2P切断の通知)。
            static void EOS_CALL Closed(const EOS_P2P_OnRemoteConnectionClosedInfo* info)
            {
                // コールバック元の通信状態
                auto& self = *static_cast<EpicTransport*>(info->ClientData);
                // 照合するEOSユーザーID文字列
                const auto key = UserKey(info->RemoteUserId);
                // 切断相手を照合する(value: 接続IDとEOSユーザーID)。
                // 登録された相手の検索結果
                const auto found = std::ranges::find_if(self.m_peers, [&key](const auto& value) { return UserKey(value.second) == key; });
                if (found == self.m_peers.end()) return;
                // 切断した相手の内部接続ID
                const auto id = found->first; self.m_peers.erase(found);
                self.Queue({ TransportEventKind::Disconnected, id, {} });
            }
            // 認証期限の通知から再ログインを要求する(info: 認証期限の通知)。
            static void EOS_CALL Expiring(const EOS_Connect_AuthExpirationCallbackInfo* info)
            { static_cast<EpicTransport*>(info->ClientData)->Login(); }
            // ログインが終了した場合だけエラーを通知する(info: ログイン状態の通知)。
            static void EOS_CALL StatusChanged(const EOS_Connect_LoginStatusChangedCallbackInfo* info)
            {
                if (info->CurrentStatus == EOS_ELoginStatus::EOS_LS_NotLoggedIn)
                    static_cast<EpicTransport*>(info->ClientData)->Fail("EOSのログインセッションが終了しました。");
            }

            // ホスト側として動作するか
            bool m_host{};
            // SDK参照数を取得済みか
            bool m_acquired{};
            // ログインの完了待ちか
            bool m_loggingIn{};
            // 非同期ログイン用の表示名
            std::string m_name;
            // 環境変数由来のクライアント秘密
            std::string m_secret;
            // EOSユーザーIDとソケット名
            std::string m_room;
            // 直近のEOS通信エラー
            std::string m_error;
            // EOS Platformの所有先
            EOS_HPlatform m_platform{};
            // EOS DLLの参照ハンドル
            HMODULE m_sdkLibrary{};
            // Platformが持つConnect API
            EOS_HConnect m_connect{};
            // Platformが持つP2P API
            EOS_HP2P m_p2p{};
            // 自身のEOSユーザーID
            EOS_ProductUserId m_local{};
            // 参加先ホストのEOSユーザーID
            EOS_ProductUserId m_remoteHost{};
            // 部屋に固有のEOSソケットID
            EOS_P2P_SocketId m_socket{};
            // 接続要求の通知登録ID
            EOS_NotificationId m_requestNotify{ EOS_INVALID_NOTIFICATIONID };
            // 切断の通知登録ID
            EOS_NotificationId m_closedNotify{ EOS_INVALID_NOTIFICATIONID };
            // 認証期限の通知登録ID
            EOS_NotificationId m_expirationNotify{ EOS_INVALID_NOTIFICATIONID };
            // ログイン状態の通知登録ID
            EOS_NotificationId m_statusNotify{ EOS_INVALID_NOTIFICATIONID };
            // 次に割り当てる内部接続ID
            TransportPeer m_nextPeer{ 1 };
            // 内部接続IDとEOSユーザーID
            std::map<TransportPeer, EOS_ProductUserId> m_peers;
            // 次のPollで返す通知イベント
            std::vector<TransportEvent> m_pending;
        };
    }

    // EOS SDKによる通信実装を所有権付きで作る。
    std::unique_ptr<INetworkTransport> CreateEpicSdkTransport()
    {
        return std::make_unique<EpicTransport>();
    }
}
