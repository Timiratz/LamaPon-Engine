# プレイヤーがホストになるオンライン通信（P2P）

Windows x64ゲームで、プレイヤーのPCをホストにして2〜4人で接続します。
ホストがゲームの進行と物理を計算し、参加者は操作を送ります。位置・回転・拡縮、
有効状態、最大256バイトのゲーム用データ、Prefabの生成・削除をホストから同期します。
途中参加では、現在の参加者と全同期オブジェクトを受け取ってから接続完了になります。

| 接続方式 | 用途 | 必要な準備 |
| --- | --- | --- |
| LAN | 同じPCでの開発、同じネットワーク内でのプレイ | 標準ビルド。IPv4とポートを指定 |
| EOS | インターネット越しのプレイ | 公式EOS SDK、Epic Developer Portalの製品設定、SDK対応ビルド |

専用ゲームサーバーは用意しません。EOS方式ではEpicの認証・接続支援・中継サービスを
利用します。[EOSのP2Pインターフェース](https://dev.epicgames.com/docs/epic-online-services/multiplayer/nat-p2p-interface)は
直接接続を試み、必要な場合に中継します。この実装は`AllowRelays`を使います。
部屋IDをコピーして相手に伝える方式で、公開ロビー一覧や自動マッチメイクはありません。
Discordログイン、クラウドセーブ、Rich Presenceの設定は独立した既存機能です。

## LANで接続を試す

1. 両方に同じScene・Prefab・ゲームコードを用意します。
2. エディターの「プロジェクト設定」で「プレイヤー同士の通信（P2P）」を開き、
   接続方式をLANにします。ゲームID・通信バージョン・シーンIDは両方で揃えて保存します。
3. 再生を開始し、ホスト側で「部屋を作成」を押します。
   同じPCから接続するときは待受IPv4を`127.0.0.1`にします。
4. 別PCのLAN参加を受け付けるときは待受IPv4を`0.0.0.0`にし、
   ホストPCのLAN IPv4とポート（例`192.168.1.20:27840`）を相手に伝えます。
   表示された`0.0.0.0`は待受指定なので、参加先には使いません。
5. 参加側で接続先とプレイヤー名を入力し「部屋に参加」を押します。
   必要な場合はWindowsファイアウォールで、このゲームのプライベートネットワーク通信を許可します。

同じPCでホストを複数起動するときは別ポートを指定するか、ポート`0`で空きポートを
選びます。LANはTCPによる順序付き通信です。暗号化・端末認証は提供しないため、
信頼できるLAN参加者向けです。インターネットでの利用にはEOS方式を使います。

接続状態・部屋ID・参加者・往復時間・同期オブジェクト数・最後のエラーを同じ画面で確認できます。
再生停止、Scene切り替え、ホスト終了で接続も終了します。ホストが最小化されても
通信とゲーム進行は続きます。エディターの一時停止はゲーム進行を止めます。

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
| `Script::JoinNetwork(address, name)` | IPv4:portまたはEOS部屋IDへ接続 |
| `Script::StopNetwork()` | 切断し、Scene同期状態を復元 |
| `Script::Network()` / `IsNetworkHost()` | 現在のSession、ホスト判定 |
| `Script::NetworkSpawn(key, transform, owner)` | ホストが登録Prefabを生成 |
| `Script::NetworkDespawn(id)` / `FindNetworkObject(id)` | ホストによる削除、Scene上の検索 |
| `NetworkSession::SendInput(id, name, data)` | 自分の所有物の操作をホストへ送る |
| `NetworkSession::PollEvent(event)` | 参加・退出・操作・ゲームイベント・エラーを取り出す |
| `NetworkSession::BroadcastEvent(name, data)` | ホストが全参加者へゲームイベントを配信 |
| `NetworkIdentityComponent::SetReplicatedData(data)` | ホスト上でゲーム用状態を設定 |
| `NetworkIdentityComponent::IsLocalOwner()` | この端末が所有者か確認 |

イベントを読む管理Scriptは一つにまとめます。`PollEvent`で取り出したイベントは
他のScriptでは再取得できません。ホストは入力の所有者を自動検証しますが、
操作内容、速度、クールダウンなどゲーム固有の検証はゲームコードで行います。
参加者から生成・座標変更・削除・ゲームイベント配信の要求は受け付けません。

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

EOS SDKはリポジトリに含めません。SDKなしでもLANとエディターをビルドできますが、
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
- ホストへの入力は接続ごとに毎秒128件まで。過剰送信・不正なJSON・ID・所有者を拒否します。
- イベントキュー512件。LAN送信キューは接続ごとに256KiB、EOSは送受信各256KiB。
  処理しきれない参加者は切断し、キューを無制限に増やしません。
- 既定は20Hzの全オブジェクト状態送信。大量のオブジェクト・高頻度設定では帯域が増えます。
  差分圧縮、UDP向けのスナップショット破棄、クライアント予測はありません。
- 所有者の退出でその所有物を削除します。ホスト退出では全員が切断されます。
  ホスト引き継ぎ、再接続時の進行復元、Scene遷移同期はありません。
- Animator内部状態、子Transform、音声、物理内部状態は自動同期しません。
  必要な情報をゲーム用データ・イベントで表し、参加者側の表示へ反映してください。
- WebGLへの通信移植、対戦用の巻き戻し、ボイスチャット、チート対策は対象外です。

公開型の変更によりGame Module APIは77です。ゲーム用DLLを再ビルドしてください。
EOSに接続しない既存プロジェクトは追加設定なしで従来どおり起動します。

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

EOS SDK 1.19.2.1でDebug・Releaseのコンパイルを確認しています。
SDK対応ビルドでは、本物のEOS DLLで資格情報の未設定と、不正な部屋ユーザーIDを使った
SDKの読み込み・初期化・終了・再開始も確認します。この試験は資格情報もインターネット接続も使いません。
自動テストのプレイヤー間通信はLAN方式の検証です。EOSのインターネット接続を検証したことにはなりません。
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
