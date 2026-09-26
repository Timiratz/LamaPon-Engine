# プレイヤーがホストになるオンライン通信（P2P）

Windows x64ゲームで、プレイヤーのPCをホストにして2〜4人で接続します。
`Direct`方式ならEpicアカウント・EOS SDK・専用ゲームサーバーは不要です。
到達可能な数値IPv4またはIPv6へ直接接続し、ホストがゲームの進行を決定します。
参加方法と同期方法は別々に選べるため、招待専用のゲームに限定されません。

| 接続方式 | 用途 | 必要な準備 |
| --- | --- | --- |
| Direct（Epic不要・暗号化） | 同じPC、LAN、到達可能なインターネット接続先 | 標準Windowsビルド。別回線では公開接続先とTCPポート転送、または到達可能なIPv6 |
| LAN（従来方式） | 信頼できる同じPC・LAN内の互換接続 | 標準ビルド。数値IPv4 / IPv6とポート。暗号化・参加認証なし |
| EOS（任意） | Epicの接続支援・中継を使うゲーム | 公式EOS SDK、Developer Portalの製品設定、SDK対応ビルド |

DirectはTCPの順序付き通信です。共有アクセスキーで接続を認証し、Windows CNGの
ECDH-P256、HKDF-SHA256、AES-256-GCMで暗号化・改ざん検出を行います。
キーは部屋を作り直すたびに変わり、プロジェクト設定へ保存しません。
キーを持つ人は部屋へ参加でき、ホストとして振る舞うこともできます。
個別アカウントの本人確認やフレンド制限はゲーム側の責務です。
独立したセキュリティ監査を受けたプロトコルではありません。

EOSは既存の任意バックエンドとして残しています。DirectにはEOS DLLも契約同意も不要です。
Discordログイン、クラウドセーブ、Rich Presenceは独立した既存機能です。

## 最初に同じPCで試す

1. 両方に同じScene・Prefab・ゲームコードを用意します。
2. エディターの「プロジェクト設定」→「プレイヤー同士の通信（P2P）」で
   「直接接続（Epic不要・暗号化）」を選びます。ゲームID・通信バージョン・シーンIDを揃えます。
3. 設定を保存し、再生を開始します。ホストの待受先は`127.0.0.1`のまま「部屋を作成」を押します。
4. ホストの「接続情報をコピー」を押し、別のゲームプロセスの「接続情報 / 接続先 / EOS部屋ID」へ貼り付け、
   プレイヤー名を入力して「部屋に参加」を押します。

接続情報は`LPD1|接続先:port|64文字のアクセスキー`です。秘密部分を含むため、
参加させたい人へ共有します。ゲームのUIでは接続先とキーを別々に扱うこともできます。
同じPCで複数のホストを起動するときは別ポート、またはポート`0`で空きポートを使います。
接続状態、参加者、往復時間、同期オブジェクト数、エラーを同じ画面で確認できます。
再生停止・Scene切り替え・ホスト終了で接続も終了します。最小化中も通信とゲーム進行は続きます。
エディターの一時停止はゲーム進行を止めます。

## 接続方法をゲームに合わせる

| 参加方法 | 利用例 | API |
| --- | --- | --- |
| 接続情報の共有 | 仲間内の協力ゲーム、非公開部屋 | `ConnectionCode(endpoint)`、`Join(code, name)` |
| 接続先とキーを別指定 | ゲーム独自の接続画面、IP指定 | `AccessKey()`、`JoinDirect(endpoint, key, name)` |
| LANの部屋一覧 | 同じネットワークの対戦・協力ゲーム | `NetworkRoomBrowser`、`JoinRoom(room, name)` |
| 公開部屋一覧・マッチング | ジャンルごとの募集・選択画面 | `INetworkRoomDirectory`へゲーム固有の提供元を実装 |

LAN検索は`advertiseLan = true`を選んだホストだけが応答します。既定はOFFです。
ホストは`0.0.0.0`で待ち受け、参加側は「LANの部屋を検索」から選択します。
同じPCだけなら「このPCの部屋を検索」も使えます。
部屋名・ゲームID・バージョン・シーン・人数で候補を絞り、接続時にもホストが設定を検証します。
DirectのLAN公開ではアクセスキーも検索応答に含めるため、LAN内の人が参加できる公開部屋になります。
検索はIPv4 UDP、既定ポート27841で、private / loopback / link-localの送信元だけを扱います。
IPv6の部屋検索やインターネット全体の検索はありません。LAN内の広告は本人確認済みの一覧ではありません。

`NetworkRoomBrowser`はゲームが所有し、`Start(configuration)`→毎フレーム`Update(dt)`→
`Rooms()`の順で使います。不要になったら`Stop()`します。
`NetworkRoom`には接続情報が含まれるため、ログや設定ファイルへ保存しません。

`INetworkRoomDirectory`は`Search` / `Publish` / `Withdraw` / `Update` / `Rooms`の
拡張インターフェースです。実際の公開一覧サーバー・自動マッチメイク・アカウント認証は含みません。
公開一覧が必要なゲームは自分の提供元を接続し、選んだ`NetworkRoom`を`JoinRoom`へ渡します。
エンジンが外部サービスへ勝手に問い合わせることはありません。

## 別々のPC・別々の回線で接続する

Directで別回線への通信は実装されていますが、ホストへ到達できる回線設定が必要です。
接続情報だけでNATやファイアウォールを通過できるわけではありません。

| ホスト側の回線 | 準備 |
| --- | --- |
| 公開IPv4 + 対応ルーター | 待受`0.0.0.0`。任意のUPnP、または手動でTCP待受ポートをホストPCへ転送 |
| 公開IPv4 + 非対応ルーター | 手動ポート転送と公開IPv4の指定 |
| 到達可能なIPv6 | 待受`::`またはホストのIPv6。ルーター・WindowsでゲームのTCP受信を許可 |
| CGNAT・共有IPv4・二重NATなど | ホストへの到達設定を回線側で確保するか、到達可能なIPv6を使う。Directだけでは自動解決しない |

IPv4ではホストの「相手に伝える接続先」にLAN IPまたは公開IPを入れ、
接続情報をコピーします。IPv6は`[IPv6アドレス]:port`です。名前解決はなく数値IPを指定します。
`0.0.0.0`と`::`は待受指定であり参加先には使いません。
手動指定もUPnP公開先もない場合、ワイルドカード待受の接続情報は同じPC用のloopbackになります。
その情報をそのまま別PCへ渡しても接続できません。

「ルーターの自動ポート設定を利用する（UPnP）」は既定OFFです。
ONのDirectホストだけがWindowsのUPnP Control Pointで対応IPv4ルーターを検索し、
120秒のTCPポート転送を要求します。既存の転送があるポートは使用せず、
作成後は45秒ごとに自分の設定を確認して更新します。
終了時は自分の識別子・PC・ポートに一致する転送だけを解除します。
永久転送しか扱わない機器は非対応です。異常終了時の失効はルーターがleaseを守ることが前提です。
検索と応答待ちは別スレッドで行い、終了操作で画面を待たせません。
OS・機器の応答待ち中は解除が遅れる場合があり、短期leaseの期限も使って後始末します。
Windowsファイアウォールは自動変更しません。

「公開先を確認できた」は、別回線のプレイヤーから到達できたという意味ではありません。
接続が失敗したら、公開接続先・ポート・ファイアウォール・共有IPv4・二重ルーターを確認します。
UDP hole punching、STUN、TURN、中継、PCP、NAT-PMPはDirectには実装していません。
実ルーターと別回線の2台での動作確認は未実施です。

## SceneとPrefabを同期対象にする

既存のScene内オブジェクトにはInspectorの「コンポーネントを追加」から
`Network Identity`を追加し、Scene Keyを`world.box`などの一意な値にします。
キーは英数字と`._-`の64文字以内で、両端末の同じオブジェクトに同じ値を付けます。
通信IDと所有者は接続中に割り当てられ、Sceneファイルへ保存しません。
Scene内の複製では重複しないキーへ変更します。

ネットワーク対象はSceneのrootへ置きます。子の配置・描画はPrefab内の階層を使います。
子のTransformを個別に同期する場合は独立したrootオブジェクトにしてください。
Scene Keyを持つ固定オブジェクトはホストが自動登録します。
Prefabの`Network Identity`のScene Keyは空にします。

動的オブジェクトはプロジェクト設定の「同期Prefabの登録」に登録します。
例: キー`player`、Prefabパス`prefabs/player.prefab.json`。パスはassetsからの相対パスです。
ネットワークではキーだけを送信し、相手から受け取ったファイルパスは読み込みません。
接続の両側で登録とPrefabを揃えてください。

`ホストだけでシミュレーション`が既定で有効です。参加者側では対象rootと子の
Rigidbody、Character Controller、Input Mover、移動用Component、Joint、Native Scriptを
無効にし、ホストの結果を表示します。Native Scriptのインスタンス生成時の`Awake`や
破棄時の処理は発生し得るので、ホスト固有の初期化には`IsNetworkHost()`の確認も入れます。
入力と接続UIを担当する管理Scriptは、同期オブジェクトの階層から独立させます。
フラグを外す場合はゲーム側でクライアントのシミュレーションを管理してください。

参加者の表示は`補間時間`（既定0.1秒）でなめらかにします。0なら即時反映です。
予測や巻き戻しはありません。接続終了時は固定オブジェクトの元の状態と無効化した
Componentを復元し、通信で生成したオブジェクトを削除します。

## C++ API

`Application`が`NetworkSession`と`NetworkSceneBridge`を所有し、毎フレーム更新します。
通常のゲームScriptで`Update()`を重ねて呼ぶ必要はありません。接続は明示的に開始します。
プロジェクト設定はエディターと配布ゲームの起動時に読み込まれます。

| API | 動作 |
| --- | --- |
| `Script::HostNetwork(name, address)` | 部屋を作る。LANの既定待受は127.0.0.1 |
| `Script::JoinNetwork(address, name)` | Direct接続情報、LANの数値接続先、EOS部屋IDへ接続 |
| `Script::JoinDirectNetwork(endpoint, key, name)` / `JoinNetworkRoom(room, name)` | ゲーム固有の接続画面や部屋一覧から参加 |
| `Script::StopNetwork()` | 切断し、Scene同期状態を復元 |
| `Script::Network()` / `IsNetworkHost()` | 現在のSession、ホスト判定 |
| `Script::NetworkSpawn(key, transform, owner)` | ホストが登録Prefabを生成 |
| `Script::NetworkDespawn(id)` / `FindNetworkObject(id)` | ホストによる削除、Scene上の検索 |
| `NetworkSession::SendInput(id, name, data)` | 自分の所有物の操作をホストへ送る |
| `NetworkSession::PollEvent(event)` | 参加・退出・操作・ゲームイベント・エラーを取り出す |
| `NetworkSession::BroadcastEvent(name, data)` | ホストが全参加者へゲームイベントを配信 |
| `NetworkSession::SendCommand(name, data)` | 所有オブジェクトが不要な操作要求。送信者IDをホストへ渡す |
| `NetworkSession::SetSessionState(data)` / `SessionState()` | ホストだけが部屋全体の状態を更新。途中参加にも現在値を送る |
| `NetworkIdentityComponent::SetReplicatedData(data)` | ホスト上でゲーム用状態を設定 |
| `NetworkIdentityComponent::IsLocalOwner()` | この端末が所有者か確認 |

イベントを読む管理Scriptは一つにまとめます。`PollEvent`で取り出したイベントは
他のScriptでは再取得できません。ホストは入力の所有者を自動検証しますが、
操作内容、速度、クールダウンなどゲーム固有の検証はゲームコードで行います。
参加者から生成・座標変更・削除・ゲームイベント配信の要求は受け付けません。

### ジャンルに合わせて同期方法を選ぶ

| ゲーム | 主に使う仕組み |
| --- | --- |
| 協力アクション | 所有者の`SendInput`、ホストのScene / Prefab同期、表示の補間 |
| ボード・カード・ターン制 | `SendCommand`、`SetSessionState`、必要なら`BroadcastEvent` |
| 小規模なシミュレーション | 変更されたオブジェクトの同期、操作要求、ゲーム固有の状態 |

`Continuous`は設定したtick頻度で全オブジェクト状態を送り、
`OnChange`は変更されたオブジェクトだけを同じtick頻度で送ります。
`OnChange`も途中参加には全状態を送ります。所有権・Prefab生成・削除は両方式共通です。
部屋全体の状態は変更時に送信し、同じ値の再設定では送信しません。
全データは順序付きで、差分圧縮やUDPの古いスナップショット破棄はありません。
FPSの予測・巻き戻し、大規模RTSの決定論的同期はゲーム専用の追加設計が必要です。

[P2PTurnBasedController.h](../samples/Networking/P2PTurnBasedController.h)は
SceneやPrefabなしで動く3×3の盤面例です。ホストがX、最初の参加者がO、
残りは観戦者になります。手番・送信者・盤面・状態リビジョンをホストで検証し、
古い操作、連打による重複、観戦者の操作を受け付けません。
途中参加は現在の盤面を受け取ります。対戦参加者が抜けると新しい対戦へリセットします。

```cpp
class BoardManager final : public LamaPon::Script
{
public:
    void Update(float) override
    {
        if (auto* session = Network()) m_board.Update(*session);
        // 自分のUIでは m_board.Board() / Turn() / Winner() を表示し、
        // セル選択時に m_board.RequestMove(*Network(), cellIndex) を呼びます。
    }
private:
    LamaPon::Samples::P2PTurnBasedController m_board;
};
LAMAPON_SCRIPT(BoardManager)
```

LamaPonの共通ヘッダーとサンプルヘッダーをゲームへ追加します。
`Script::SendNetworkCommand` / `SetNetworkSessionState`も使えます。
一つの管理Scriptがイベントを読み、ゲーム全体へ振り分けてください。

### 小さな協力プレイのサンプル

[P2PCooperativeController.h](../samples/Networking/P2PCooperativeController.h)は、
各参加者の`player`をホストで生成し、各端末の移動入力を20Hzで送り、ホストが
速度と範囲を制限して移動する例です。送信するのは移動方向だけです。
入力が途絶えると0.3秒で移動を止めます。通信試験で実際のPrefab生成と移動を検証します。

1. rootに`Network Identity`と見えるMeshを付けたPlayer Prefabを保存します。
   Scene Keyは空、ホストだけでシミュレーションは有効のままにします。
2. 同期Prefabキー`player`として登録します。
3. サンプルヘッダーをゲームのソースへ追加し、独立した管理GameObjectのScriptから呼びます。
   ヘッダーのinclude先はゲーム側の配置に合わせてください。

```cpp
#include "LamaPon/LamaPon.h"
#include "P2PCooperativeController.h"

class CoopManager final : public LamaPon::Script
{
public:
    void Update(float dt) override
    {
        auto* session = Network();
        auto* bridge = LamaPon::ActiveNetworkSceneBridge();
        if (!session || !bridge) return;
        const auto& input = Graphics().Input();
        m_game.Update(*session, *bridge, dt,
            input.Value("MoveHorizontal"), input.Value("MoveVertical"));
    }
private:
    LamaPon::Samples::P2PCooperativeController m_game;
};
LAMAPON_SCRIPT(CoopManager)
```

エディターの接続ボタンで部屋を作って試します。配布ゲームではゲームのUIから
`HostNetwork()`、`JoinNetwork()`、`StopNetwork()`を呼びます。
このサンプルはイベントを消費するため、ゲームイベントの追加もこの管理Scriptで行います。

## EOSを有効にする

EOS SDKはリポジトリに含めません。SDKなしでもDirect・LANとエディターをビルドできますが、
EOSを選ぶと明確なエラーになります。EOS SDKは公式配布ページから取得できます。

1. [公式SDK配布ページ](https://onlineservices.epicgames.com/sdk)からC SDKを取得し、
   [Epic Developer Portal](https://dev.epicgames.com/portal/)で製品を用意します。
   `Include`、`Lib`、`Bin`が入ったSDKの場所を指定します。
2. 製品のProduct ID、Sandbox ID、Deployment ID、ゲームクライアント用Client IDを
   プロジェクト設定へ入力します。両端末で同じDeploymentを使います。
3. [Client Policy](https://dev.epicgames.com/docs/epic-online-services/eos-fundamentals/client-and-client-policy/client-policy-guide)を
   ゲームクライアント向けに限定し、Connectによる端末ログイン・ユーザー作成とP2Pに必要な権限を設定します。
   管理者・バックエンド用資格情報をゲームへ入れないでください。
4. ゲームクライアント用Client Secretは起動環境の`LAMAPON_EOS_CLIENT_SECRET`へ設定します。
   設定ファイルには環境変数名だけを保存し、値は書き込みません。
   環境変数は実行環境の設定方法であり、配布クライアント内の資格情報を秘密に保つ仕組みではありません。
5. x64 Visual Studio Developer Command Promptで構成・ビルドします。

```bat
cmake --preset windows-debug -DLAMAPON_EOS_SDK_ROOT="C:/SDK/EOS/SDK"
cmake --build --preset windows-debug
ctest --preset windows-debug -R OnlineP2P
```

Releaseにも同じ`LAMAPON_EOS_SDK_ROOT`を指定します。SDKのヘッダー・import library・DLLが
揃っていなければ構成時に失敗します。SDKを外す場合は空文字を指定して再構成します。
SDK対応ビルドは`EOSSDK-Win64-Shipping.dll`を実行ファイルの横へコピーします。
Windows書き出しではこのDLLも配布先へコピーします。EOS設定なのにSDK非対応またはDLLが
無い場合は書き出しを中止します。EOSのライセンス・配布条件は公式SDKの条件を確認してください。

端末IDを使う[EOS Connect](https://dev.epicgames.com/docs/epic-online-services/eos-fundamentals/connect-interface)でログインするため、
Epicアカウントのログイン画面は出しません。同じPC上の複数プロセスは同一端末ユーザーになり得ます。
EOSの実動作確認には異なる2台のPCを使います。

ホストの部屋IDは`ProductUserId:ランダムなSocket名`です。参加者へそのまま共有します。
部屋IDを知る人が参加できる仕組みです。アカウント招待・フレンド制限・パスワードは未実装です。
ログイン有効期限の更新を行い、ログイン失効・P2P切断・通信タイムアウトは接続エラーへ反映します。

## 上限と対象範囲

- Windows x64、2〜4人、同期rootオブジェクト128個、Prefab登録64個。
- 通信パケット1100バイト、操作・ゲーム用データ256バイト。文字列はUTF-8です。
- ホストが受け取るSessionメッセージは接続ごとに毎秒128件のtoken bucketで制限します。過剰送信・不正なJSON・ID・所有者を拒否します。
- イベントキュー512件。Direct / LAN送信キューは接続ごとに256KiB、EOSは送受信各256KiB。
  処理しきれない参加者は切断し、キューを無制限に増やしません。
- 既定は20Hzの全オブジェクト状態送信。変更時だけの同期も選べます。
  大量のオブジェクト・高頻度設定では帯域が増えます。
  差分圧縮、UDP向けのスナップショット破棄、クライアント予測はありません。
- 所有者の退出でその所有物を削除します。ホスト退出では全員が切断されます。
  ホスト引き継ぎ、再接続時の進行復元、Scene遷移同期はありません。
- Animator内部状態、子Transform、音声、物理内部状態は自動同期しません。
  必要な情報をゲーム用データ・イベントで表し、参加者側の表示へ反映してください。
- WebGLへの通信移植、対戦用の巻き戻し、ボイスチャット、チート対策は対象外です。

公開型の変更によりGame Module APIは78、Session通信プロトコルは2です。
全参加者を同じ版へ更新し、ゲーム用DLLを再ビルドしてください。
既存プロジェクトはLAN方式・定期同期の既定を保ち、自動ポート設定とLAN公開はOFFです。
接続を開始しなければ追加設定なしで従来どおり起動します。

## 検証

```bat
cmake --build --preset windows-debug
ctest --preset windows-debug
cmake --build --preset windows-release
ctest --preset windows-release
```

`OnlineP2PSession`は実際のloopback TCPソケットで、2〜4人参加、満員、ゲーム設定不一致、
128個の途中参加状態、所有権、イベント、退室、タイムアウト、不正な通信、分割受信、
送信過多、再接続を確認します。`OnlineP2PScene`はScene保存・補間・Prefab生成/削除、
参加者の物理とScriptの抑制、停止時復元、Scene差し替え、即時再接続、協力プレイ例を確認します。

`OnlineP2PDirect`は本物のloopback IPv4 / IPv6ソケットで、共有キー認証、暗号化通信、
4人参加、満員、128個の途中参加状態、古いキーの拒否、平文接続の拒否、部屋状態・コマンド、
変更時だけの送信、盤面の勝敗と観戦者を確認します。同じPCの実UDP検索と部屋選択も確認します。
暗号部品はRFC 4231のHMAC、RFC 5869のHKDF、NISTのAES-GCM既知ベクトルを使い、
ECDHの一致・不正形式、改ざん・連番リプレイ拒否を検証します。
UPnPは偽のゲートウェイで、lease作成・更新・解除、既存設定保護、
他の所有者への変更、永久leaseの拒否、照会失敗を確認します。実機のUPnP対応確認ではありません。
`OnlineP2PScene`の協力プレイ例はDirect + OnChangeでもPrefab生成と移動を検証します。

EOS SDKを無効にしたDebug・Release構成で全体ビルドが成功し、
両方とも77 / 77件の回帰テストが成功しています。
同じPCの通信試験は別PC・別回線の到達性を検証したものではありません。
実機が揃ったら、手動IPv4・UPnP・IPv6の各経路で
部屋作成→参加→操作→途中参加→切断→再作成を確認してください。
現時点では実ルーターの設定変更、別回線の通信、エディター接続画面の目視確認は未実施です。

EOS SDK 1.19.2.1でDebug・Releaseのコンパイルを確認しています。
SDK対応ビルドでは、本物のEOS DLLで資格情報の未設定と、不正な部屋ユーザーIDを使った
SDKの読み込み・初期化・終了・再開始も確認します。この試験は資格情報もインターネット接続も使いません。
自動テストのプレイヤー間通信はDirect / LAN方式の検証です。EOSのインターネット接続を検証したことにはなりません。
SDK導入後は、異なる回線の2台で、部屋作成→参加→移動→途中参加→切断→再作成を確認し、
直接接続・中継・回線断の結果を記録してください。

1台のPCでEOSへの認証とホストの開始・停止・再作成だけを確認する場合は、
SDK対応の試験プログラムを明示的なオプションで起動します。これは通常のCTestには含まれません。
実行プロセスの環境に次の値を設定してください。Client Secretをチャット、Git、ログへ載せないでください。

| 環境変数 | 値 |
|---|---|
| `LAMAPON_EOS_PRODUCT_ID` | 製品のProduct ID |
| `LAMAPON_EOS_SANDBOX_ID` | Sandbox ID |
| `LAMAPON_EOS_DEPLOYMENT_ID` | Deployment ID |
| `LAMAPON_EOS_CLIENT_ID` | ConnectとP2Pを許可したゲーム用Client ID |
| `LAMAPON_EOS_CLIENT_SECRET` | そのクライアントのClient Secret |

```bat
out\build\windows-debug\LamaPonNetworkSessionTests.exe --eos-host-smoke
out\build\windows-release\LamaPonNetworkSessionTests.exe --eos-host-smoke
```

実際のConnectログイン後にホストがReadyへ到達すること、部屋IDの形式、停止時の解放、
再開始時の新しい部屋IDを2回の起動で確認します。ログには資格情報と部屋IDを出しません。
EOSへ接続し端末ユーザーを作成する試験ですが、参加者同士の通信、NAT越え、中継は検証しません。
その確認には異なる端末と回線が必要です。

## Directの通信形式

接続情報`LPD1`と鍵交換`LPDH1`はDirect transportの版であり、
ゲームSessionの`protocol: 2`とは別です。平文LANへの自動フォールバックはしません。
TCPの4バイトbig-endian長さとフレーム境界を使います。

1. Client Hello: ASCII `LPDH1` + ランダム32バイト + CNG P256公開鍵blob72バイト。
2. Host Helloと暗号化された`LamaPon.Direct.Host.1`の証明を返す。
3. Clientが暗号化された`LamaPon.Direct.Client.1`を返した後、ホストがSession接続を受け付ける。
4. アプリケーションフレームを各方向の鍵と連番で暗号化する。

公開鍵blobは`BCRYPT_ECCKEY_BLOB`のlittle-endianヘッダーと、big-endianの32バイトX / Yです。
CNGの`BCRYPT_KDF_RAW_SECRET`で得たlittle-endian共有秘密をHKDFへ渡します。
saltは`HMAC-SHA256(roomKey, ClientHello || HostHello)`、
HKDF infoは`LamaPon.Direct.1.client-to-host`と`LamaPon.Direct.1.host-to-client`です。
鍵交換の秘密鍵は導出後に破棄します。
暗号フレームはtype byte `2` + big-endian uint64連番 + ciphertext + 16バイトtagです。
nonceは4バイトの0 + 8バイト連番、AADはtypeと連番です。
鍵交換の証明も同じ連番を消費します。各方向で厳密な連番を確認し、不一致や改ざんで切断します。
平文は最大1100バイト、暗号フレームは最大1125バイト、認証前の接続は5秒で期限切れです。
