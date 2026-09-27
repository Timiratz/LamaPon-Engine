# Network Session Workflow

通信条件をデータアセットにまとめ、Sceneの「通信セッション管理」Scriptから参照するサンプルです。
UnityのScriptableObjectとSceneの管理コンポーネントを組み合わせる感覚で使えます。
アプリが持つ既存のP2PセッションとScene Bridgeを使い、通信APIや同期コンポーネントはそのまま使えます。

## インストール

1. 「拡張機能 > Zipから読み込む」で `network-session-workflow-0.1.0.zip` を選びます。
2. Game Moduleをビルドします。初回はScriptとデータアセット型の登録が必要です。
3. Sceneの専用GameObjectへ、Script「通信セッション管理」を追加します。
4. 「通信設定アセット」に `profiles/DirectLocal.asset.json` を指定します。

ソースをコピーして使う場合は、このフォルダーの内容を
`assets/packages/network-session-workflow/` へ置き、Game Moduleをビルドしてください。
`NetworkSessionController.cpp` と `NetworkProfile.h` を別の場所へ置く場合も同じフォルダーに置きます。
プロファイル参照はassetsからの相対パスへ変更します。外部SDKの同梱はありません。

## 設定の置き場所

| 設定・操作 | 置き場所 |
| --- | --- |
| P2PゲームID、通信バージョン、EOS共通IDと資格情報の環境変数名 | プロジェクト設定 > オンライン |
| 接続方式、人数、送信頻度、同期方式、ポート、部屋名、検索公開、同期Prefab | 通信設定アセットのInspector |
| アセット参照、プレイヤー名、ホスト待受先、ホスト・参加・退出のイベント名 | Sceneの通信セッション管理Script |
| NetworkIdentity / NetworkTransform / NetworkStateなど | 同期対象のGameObject |
| 接続の動作確認、部屋検索、接続情報コピー、Discordテスト表示 | ウィンドウ > オンライン診断 |
| アカウント連携のURL・環境ID、Discord Application ID | プロジェクト設定 > オンライン |

アセットの条件はScriptが有効化されたときに適用します。再生開始だけでは接続しません。
無効化・破棄・Scene終了時は担当する接続を終了します。アプリのセッションは作り直しません。
管理Scriptは1つだけ置き、NetworkIdentityを持つGameObjectやその子には置かないでください。
重複するScriptと、既に接続中のセッションへの後付けは拒否し、既存接続を止めません。
アセットを変更したら接続を終了し、管理Scriptを再度有効化するか再生し直してください。
Project Settingsを保存しても再生中の通信条件は上書きしません。

## ホスト・参加・退出

`scenes/NetworkDemo.scene.json` はホスト・退出ボタンの例です。
ボタンのクリックイベント名を、それぞれ `Network.Host` / `Network.Leave` にします。
ボタンの移動先Sceneは空にします。UI Button、別のScript、または診断ウィンドウから接続を試せます。

参加には実行時の接続情報を `EventArgs.text` で渡します。別のScriptからの例です。

```cpp
LamaPon::EventArgs args;
args.text = connectionCodeFromYourJoinUi;
Emit("Network.Join", std::move(args));
```

Directの接続情報は診断ウィンドウの「接続情報をコピー」で取得できます。
秘密キーを含むため、参加者へだけ共有し、Scene・プロファイル・ログへ保存しないでください。
参加用コードやアクセスキーを保存するプロパティは用意していません。
ゲーム内の接続状態は既存の `Network()->State()` / `Members()` / `LastError()` で表示できます。
接続はApplicationが実時間で更新します。このScriptで `Network()->Update()` を重ねて呼ばないでください。

## 同じPCで試す

1. 共通のゲームIDと通信バージョンを保存し、両方で同じプロファイルを使います。
2. ホストの待受先は `127.0.0.1`、DirectLocalのポートは `0`（自動選択）のまま再生します。
3. ホスト側で `Network.Host` を発行するか、診断ウィンドウの「部屋を作成」を押します。
4. 接続情報をコピーし、もう一方のゲームの参加UIか診断ウィンドウへ渡します。
5. `Network.Leave` または診断ウィンドウで接続を終了します。

| 同梱プロファイル | 内容 |
| --- | --- |
| DirectLocal | 暗号化直接接続、4人、20Hz、定期同期、ポート自動 |
| LanLocal | 従来LAN方式、4人、20Hz。信頼するLAN向け |
| TurnBased | Direct、変更時の同期、10Hz |
| EOS | EOS方式。共通IDとEOS対応ビルドを別途用意 |

Directの外部接続は待受先をIPv4なら `0.0.0.0`、IPv6なら `::` へ変更し、到達可能な接続先を用意します。
UPnPとLAN検索への公開はすべての例でOFFです。必要な場合だけアセットで変更します。
LAN公開・UPnP・EOSの仕様と同期APIの詳細はエンジンの `docs/online-p2p.md` を参照してください。

## 既存プロジェクトから移す

プロジェクト設定の「従来のP2P設定」は折りたたみ欄として残っています。
管理ScriptがないSceneと既存の `HostNetwork` / `JoinNetwork` / `Configure` のコードは引き続き動作します。
アセットへ移す場合は、新しい通信設定アセットを作るか同梱プロファイルを複製し、
従来設定のScene ID・人数・同期方式・ポート・検索・Prefab一覧をInspectorへ写してください。
共有するゲームID・通信バージョン・EOS設定は引き続きプロジェクト設定に保存します。
不正な型・人数・ポート・Prefab登録は接続前に拒否し、部分的な設定は適用しません。

MITライセンスです。コードとアセットをコピー・編集・再配布できます。
