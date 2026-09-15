# Discordログイン、クラウドセーブ、Rich Presence

LamaPonは、Windows x64ゲームでDiscordアカウントを使ったログインと、
ログインしたプレイヤーごとのクラウドセーブ同期を扱えます。
さらに、プレイ中の状況をDiscordのプロフィールへ表示する
**Rich Presence**を、ログインなしでも利用できます。

```text
OnlineServices
├─ DiscordAuth      アカウント連携（ログイン）
├─ CloudSave        プレイヤー単位のセーブ同期
└─ DiscordPresence  Rich Presence表示
```

この3つは独立しています。Rich Presenceだけを使う、ログインとクラウドセーブ
だけを使う、両方使う、どれも選べます。Rich Presenceの説明は
[Discord Rich Presence](#discord-rich-presence)にあります。

> [!IMPORTANT]
> この機能だけでオンラインサービスは完成しません。LamaPonが提供するのは
> **ゲーム側クライアント、通信契約、ローカル保存、同期UI**です。
> Discord OAuthを処理してプレイヤーを管理し、セーブデータを永続化する
> **ゲーム開発者のバックエンド**は別に用意してください。

## できることと、できないこと

| 項目 | 現在の対応 |
|---|---|
| Discordでログイン | Windows x64で対応。既定ブラウザーを使う非同期OAuthフロー |
| 初回ログイン時のゲームアカウント作成 | バックエンド側で実装。Discord利用者と内部プレイヤーIDを関連付ける |
| 次回起動時のログイン復元 | 対応。保護した更新トークンから非同期で復元 |
| PlayerPrefsとJSONスロットのクラウド同期 | 対応。ログインした内部プレイヤー単位で同期 |
| オフライン中の保存 | 対応。ローカルへ先に保存し、通信復旧後に再試行 |
| 複数端末の同時更新 | 検出して競合として停止し、ローカル版／クラウド版を選択 |
| Discordへのセーブ保存 | 非対応。Discordは本人確認に使い、データは自前サーバーへ保存 |
| バックエンドサーバーの同梱 | 非対応。このリポジトリにはサーバー実装を含まない |
| Rich Presence（プレイ状況の表示） | 対応。Discordログインは不要 |
| Rich Presenceのアダプター同梱 | 非対応。Discord SDKはこのリポジトリに含めない |
| Webエクスポート | オンラインランタイムは未対応 |

Discordログインはゲーム専用パスワードを保存しなくてよい利点がありますが、
不正ログインを完全に防ぐものではありません。Discordアカウント、端末、バックエンド、
トークンのどれかが侵害されれば被害は起こり得ます。バックエンドで短命なアクセストークン、
更新トークンのローテーション、失効、レート制限、監査ログを実装してください。

## 推奨するアカウント設計

Discordを唯一のアカウント回復手段にはしない設計を推奨します。

1. バックエンドでゲーム固有の内部プレイヤーIDを発行する。
2. 仮アカウント、メール、プラットフォームアカウントなど、ゲーム側の主アカウントまたは
   回復手段を用意する。
3. Discordは、その内部プレイヤーへ関連付けるログイン手段の一つとして扱う。
4. セーブの所有者はDiscord IDではなく、バックエンドが発行した内部プレイヤーIDにする。

Discordの公式ガイドも、まずゲーム側アカウントを作り、後からDiscordを関連付ける構成を
勧めています。Discordだけを主認証にすると、Discord側でのBAN、関連付け解除、認可取消、
更新トークン失効などによりゲームデータへ到達できなくなるおそれがあります。
[DiscordのAccount Linkingガイド](https://docs.discord.com/developers/discord-social-sdk/development-guides/account-linking-with-discord)と
[Account Linking概要](https://docs.discord.com/developers/platform/account-linking)も確認してください。

## 全体構成

```text
LamaPonゲーム
  ├─ 既定ブラウザー ── Discord OAuth
  └─ HTTPS ── ゲーム開発者のバックエンド
                 ├─ Discord OAuthのclient secret
                 ├─ Discord利用者 ↔ 内部プレイヤーID
                 ├─ LamaPon用access/refresh token
                 └─ セーブDBまたはオブジェクトストレージ
```

LamaPonゲームはDiscordの`client_secret`を受け取りません。ブラウザーでの認可完了を
バックエンドへ問い合わせ、バックエンドが発行したLamaPon用の短命アクセストークンで
クラウドセーブへ接続します。DiscordのアクセストークンをクラウドセーブAPIへ渡さないでください。

## プロジェクト設定

「ファイル」→「プロジェクト設定とビルド...」→「オンラインサービス」で設定します。

- **Discordアカウント連携を有効にする**: オンライン機能を有効化
- **サービスURL**: ゲーム開発者が運用するバックエンドのHTTPSベースURL
- **ゲームID**: 名前変更後も変えないゲーム固有ID
- **環境ID**: `production`、`staging`などの接続環境
- **認証ページをブラウザーで開く**: ログイン開始時に既定ブラウザーを開く
- **ローカルHTTPを許可**: 開発時のloopback接続だけを許可

`.lamapon/project.json`では次の形式です。

```json
{
  "online": {
    "enabled": true,
    "serviceBaseUrl": "https://online.example.com",
    "gameId": "com.example.my-game",
    "environmentId": "production",
    "allowInsecureLoopback": false,
    "openAuthorizationBrowser": true
  }
}
```

Rich Presenceの設定は同じ`online`オブジェクトの`discordPresence`に入ります
（[Discord Rich Presence](#discord-rich-presence)を参照）。
`discordPresence`が無い古い`project.json`は、Rich Presence無効として読み込みます。

`gameId`は1〜128文字、`environmentId`は1〜64文字で、ASCIIの英数字、`.`、`_`、`-`だけを
使えます。有効化時はURL、ゲームID、環境IDがすべて必要です。通常はHTTPSだけを許可します。
HTTPを使えるのは`allowInsecureLoopback`を有効にした`127.0.0.1`、`localhost`、`[::1]`だけです。
安全でないloopback設定を有効にしたままWindowsゲームを書き出すことはできません。

この設定は公開情報です。Discordの`client_secret`、DiscordまたはLamaPonのaccess token／
refresh token、データベース認証情報、API秘密鍵を`project.json`や`LamaPonGame.json`へ
入れないでください。

## エディターで確認する

「ウィンドウ」→「セーブデータ」を開くと、次を確認できます。

- アカウント状態、ログイン開始／中止、ログアウト
- クラウド同期状態、再試行までの秒数、手動同期
- 競合したPlayerPrefsまたはセーブスロットと、ローカル版／クラウド版の選択
- 中断された永続化処理の復元／破棄

競合解決と復旧データの破棄はデータを上書きするため、確認ダイアログを表示します。
パネルは内部トークン、ETag、ハッシュ、実保存パス、未加工のプレイヤーIDを表示しません。

## C++スクリプトからログインする

`Script`のオンラインAPIは非同期です。コールバックを保持せず、毎フレーム状態を確認するため、
Game ModuleのHot Reload中にも使えます。

```cpp
void Start() override
{
    if (!IsOnlineSignedIn())
    {
        SignInWithDiscord();
    }
}

void Update(float) override
{
    switch (OnlineState())
    {
    case LamaPon::OnlineAccountState::SignedIn:
        // バックエンドの内部プレイヤー名。Discord IDではありません。
        m_playerName = OnlinePlayerName();
        break;
    case LamaPon::OnlineAccountState::Error:
        LamaPon::Logger::Instance().Error(OnlineError());
        break;
    default:
        break;
    }
}
```

`openAuthorizationBrowser`が`false`の場合や、自動起動できなかった場合は、
`OnlineAuthorizationUrl()`で認証URLを取得してゲーム側のUIから案内できます。

主なメソッドは次の通りです。

| メソッド | 用途 |
|---|---|
| `SignInWithDiscord()` | ログイン開始要求。受理されたときtrue |
| `CancelDiscordSignIn()` | 進行中のログインを中止 |
| `SignOutOnline()` | バックエンドへログアウトを通知し、端末のセッションを削除 |
| `OnlineState()` | `SignedOut`、`WaitingForAuthorization`、`SignedIn`などの状態 |
| `IsOnlineSignedIn()` | サインイン済みか確認 |
| `OnlinePlayerId()` / `OnlinePlayerName()` | バックエンドの公開プロフィール |
| `OnlineAuthorizationUrl()` | ブラウザーで開く認証URL |
| `OnlineError()` | 利用者へ表示できる最後のエラー |

すべての公開メソッドは、`Application`を動かす同じスレッドから呼んでください。

## セーブと同期

サインイン後も、通常どおり`Preferences()`、`SaveInteger()`、`SaveText()`、
`Application::Saves()`などへ保存します。LamaPonはローカルへ耐久的に保存してから変更を
ジャーナルへ記録し、バックグラウンド通信でクラウドへ同期します。

- 未ログイン時の既存データは従来どおり`%LOCALAPPDATA%/LamaPon/<ゲーム名>/`に残ります。
- ログイン中は、内部プレイヤーIDをSHA-256で不可逆なフォルダー名へ変換した
  `OnlineProfiles`配下の専用領域を使います。
- ゲストと各オンラインアカウントのセーブは分離されます。
- ゲストセーブはログイン時に自動インポートされません。
- 通信失敗やレート制限は保存済みジャーナルから再試行します。
- 更新トークンは現在のWindowsユーザーだけが復号できるよう保護し、制限ACLの領域へ保存します。

手動同期は次のように要求できます。

```cpp
const auto result = RequestCloudSync();
if (result == LamaPon::OnlinePersistenceOperationResult::Busy)
{
    // すでに同期中です。
}

const auto status = CloudSyncStatus();
if (status.state == LamaPon::OnlineCloudSyncState::Conflict)
{
    for (const auto& conflict : CloudConflicts())
    {
        // 実際のゲームでは、内容と選択の意味をUIで確認します。
    }
}
```

`CloudConflicts()`の`id`は現在のプロセス内だけで有効なランダム識別子です。
保存したり、次回起動で再利用したりしないでください。

競合解決は必ず利用者に確認してから実行します。

```cpp
ResolveCloudConflict(
    conflict.id,
    LamaPon::OnlineCloudConflictResolution::UseLocal);
```

- `UseLocal`: 競合検出時のローカル版を、現在のクラウド版に対する新しい更新として再送
- `UseRemote`: クラウド版でローカルを上書き

## Discord Rich Presence

Rich Presenceは、プレイ中の状況をDiscordのプロフィールとフレンド一覧へ
表示する機能です。

```text
My Awesome Game

Chapter 3
Boss Battle

[ゲームアイコン]

00:18:42
```

> [!IMPORTANT]
> **Rich PresenceとDiscordアカウント連携は別の機能です。**
> Rich Presenceを使うために、Discordログインもクラウドセーブも
> バックエンドも必要ありません。逆に、ログインとクラウドセーブを使いながら
> Rich Presenceを切っておくこともできます。

LamaPonは表示内容を決め打ちしません。`details`と`state`へ何を入れるかは
ゲーム制作者が決めます。

| ジャンル | `details` | `state` |
|---|---|---|
| アクション | `Stage 5` | `Boss Battle` |
| シミュレーション | `Year 12` | `Population 120000` |
| RPG | `Royal Capital` | `Quest: The Lost Sword` |
| パズル | `Puzzle 48` | `87% Complete` |
| レース | `Circuit A` | `Time Attack` |

### 1. Discord Applicationを作る

Rich Presenceは、ゲームごとに用意したDiscord Applicationとして表示されます。
LamaPonは特定のApplication IDを強制しません。

```text
ゲームA → Discord Application A
ゲームB → Discord Application B
```

1. [Discord Developer Portal](https://discord.com/developers/applications)で
   **New Application**を作り、Discordへ表示したいゲーム名を付けます。
   この名前がプロフィールの1行目になります。
2. **General Information**の**Application ID**を控えます。
   これは公開情報で、秘密情報ではありません。
3. **Rich Presence** → **Art Assets**で画像を登録します。ここで付けた名前が
   `largeImageKey` / `smallImageKey`になります（例: `game_icon`）。
   登録後、Discord側へ反映されるまで時間がかかることがあります。

> [!CAUTION]
> Application IDだけをLamaPonへ設定します。**OAuth2のClient Secret、Bot Token、
> access token、refresh tokenは、ゲームにも`project.json`にも入れないでください。**
> 配布ファイルから誰でも取り出せます。LamaPonはASCII数字以外のApplication IDを
> 拒否するので、秘密情報を貼り付けた場合は保存時にエラーになります。

### 2. プロジェクト設定

「ファイル」→「プロジェクト設定とビルド...」→「オンライン」→
「Discord Rich Presence」で設定します。

- **Discord Rich Presenceを有効にする**: Rich Presenceの利用可否
- **Application ID**: 上で控えたID（ASCII数字、最大32文字）
- **既定の大画像キー**: Activity側で指定しなかったときに使うArt Asset名
- **既定の大画像テキスト**: 画像へカーソルを合わせたときの説明（最大128バイト）

同じ画面の「動作確認」から、エディターのままテスト表示を送れます。
ここで入力したDetails / Stateは`project.json`へ保存しません。

`.lamapon/project.json`では次の形式です。

```json
{
  "online": {
    "enabled": false,

    "discordPresence": {
      "enabled": true,
      "applicationId": "123456789012345678",
      "defaultLargeImageKey": "game_icon",
      "defaultLargeImageText": "My Awesome Game"
    }
  }
}
```

この例のように`online.enabled`が`false`でもRich Presenceは動きます。
`discordPresence`が無い古い`project.json`は、Rich Presence無効として
読み込みます（既存プロジェクトの挙動は変わりません）。
`applicationId`が空のまま`enabled`を`true`にしても保存はできますが、
実行時に警告を出して安全に無効化します。

### 3. C++スクリプトからActivityを設定する

```cpp
#include "LamaPon/Scripting/Script.h"

class BossRoom final : public LamaPon::Script
{
public:
    void Start() override
    {
        LamaPon::DiscordActivity activity;
        activity.details = "Chapter 3";
        activity.state = "Boss Battle";
        activity.largeImageKey = "game_icon";
        activity.largeImageText = "My Awesome Game";
        // 経過時間（00:18:42）をDiscordへ表示します。
        activity.startTimestamp =
            LamaPon::DiscordPresenceUnixTime();
        static_cast<void>(SetDiscordActivity(activity));
    }

    void OnDestroy() override
    {
        ClearDiscordActivity();
    }
};
```

`details`と`state`だけでよければ簡易版を使えます。

```cpp
static_cast<void>(
    SetDiscordActivity("Chapter 3", "Boss Battle"));
```

主なメソッドは次の通りです。

| メソッド | 用途 |
|---|---|
| `SetDiscordActivity(activity)` | 表示内容を設定。受理され、いま反映できたときtrue |
| `SetDiscordActivity(details, state)` | 上の簡易版 |
| `ClearDiscordActivity()` | 表示を消す。何度呼んでも安全 |
| `IsDiscordPresenceAvailable()` | いまDiscordへ反映できるか |
| `DiscordPresenceStatus()` | `Disabled` / `Unavailable` / `Ready` / `Active` |
| `DiscordPresenceError()` | 直前の失敗理由（利用者向けではありません） |

`DiscordActivity`のフィールドは次の通りです。

| フィールド | 内容 | 上限 |
|---|---|---|
| `details` | 1行目。例: `Chapter 3` | 2〜128バイト |
| `state` | 2行目。例: `Boss Battle` | 2〜128バイト |
| `largeImageKey` | 大画像のArt Asset名 | 256バイト |
| `largeImageText` | 大画像の説明 | 2〜128バイト |
| `smallImageKey` | 小画像のArt Asset名（大画像が必要） | 256バイト |
| `smallImageText` | 小画像の説明 | 2〜128バイト |
| `startTimestamp` | 開始時刻（Unix秒）。経過時間を表示 | 0で指定なし |
| `endTimestamp` | 終了時刻（Unix秒）。残り時間を表示 | 0で指定なし |

上限を超える値、制御文字、負のタイムスタンプ、`startTimestamp`より前の
`endTimestamp`は送信せずに`false`を返します。`largeImageKey`を空にすると、
プロジェクト設定の既定値で補います。

### 4. タイムスタンプ

`startTimestamp`と`endTimestamp`は**Unix秒**です。`0`は「指定なし」で、
Discordは経過時間を表示しません。LamaPonは`0`を勝手に現在時刻へ
置き換えません（Discordの仕様に合わせています）。経過時間を出したいときは
明示的に現在時刻を入れてください。

```cpp
// 経過時間（数え上がり）
activity.startTimestamp = LamaPon::DiscordPresenceUnixTime();

// 残り時間（数え下がり）— 例: 5分のタイムアタック
activity.startTimestamp = 0;
activity.endTimestamp =
    LamaPon::DiscordPresenceUnixTime() + 5 * 60;
```

同じ`startTimestamp`を渡し続けるかぎり、Discord側の経過時間は
リセットされません。ステージを移るたびに計り直したいときだけ、
新しい時刻を入れてください。

### 5. 更新の頻度

Discordは1クライアントあたり**15秒に1回**しかActivity更新を受け付けず、
超過分を黙って捨てます。LamaPonは最新の要求だけを保持し、次の更新枠で
まとめて送ります。毎フレーム`SetDiscordActivity()`を呼んでも安全ですが、
Discordへ届くのは最後の内容です。

通信の進行は`Application::Update` → `OnlineServices::Update`から行います。
Rich Presenceのために独自スレッドは作りません。`DiscordActivity`を扱う
すべてのメソッドは、`Application`を動かす同じスレッドから呼んでください。

### 6. Discordが起動していない場合

Rich Presenceは、Discordが入っていない・起動していない・Presenceアダプターが
未導入といった場合でも、**ゲームを止めません**。

```text
Presence初期化 → 失敗 → 警告ログ → Presence無効化 → ゲームは通常動作
```

ログには次のように出ます。

```text
Discord Rich Presenceを利用できません。
Discord Activityなしでゲームを続行します: <理由>
```

このとき`IsDiscordPresenceAvailable()`は`false`、`DiscordPresenceStatus()`は
`Unavailable`を返します。`SetDiscordActivity()`も`false`を返しますが、
最後に渡した内容は保持され、Discordへ接続できた時点で送られます。
接続はエンジン側が定期的にやり直すので、ゲームを起動した後にDiscordを
起動した場合も表示されます。

### 7. Presenceアダプター

LamaPonはDiscordのSDKを同梱しません。Discord SDKの利用条件により、SDKを
LamaPonのリポジトリへ取り込んだり、改変したり、単体で再配布したりできない
ためです。非公式のDiscord RPCライブラリにも依存しません。そのため、標準の
ビルドではRich Presenceは常に`Unavailable`になります（ゲームは正常に動きます）。

Discordへ実際に表示するには、
[Discord Social SDK](https://discord.com/developers/docs/discord-social-sdk/overview)を
各自でDiscordから入手し、`DiscordPresenceBackend`を実装したアダプターを
登録します。Discord固有の型はこのアダプターの内側だけに閉じ込めます。

```text
Game / Script
  ↓
LamaPon API（DiscordActivity）
  ↓
DiscordPresenceBackend（アダプター）
  ↓
Discord SDK / API
```

```cpp
#include "LamaPon/Online/DiscordPresence.h"

class MyDiscordBackend final
    : public LamaPon::DiscordPresenceBackend
{
public:
    bool Initialize(std::string_view applicationId) override;
    void Shutdown() noexcept override;
    bool SetActivity(
        const LamaPon::DiscordActivity& activity) override;
    void ClearActivity() noexcept override;
    void Tick(float elapsedSeconds) noexcept override;
    bool IsAvailable() const noexcept override;
};

// Applicationを作った後、Presenceを設定する前に一度だけ登録します。
LamaPon::SetDiscordPresenceBackendFactory(
    []
    {
        return std::make_unique<MyDiscordBackend>();
    });
```

アダプターの約束事は次の通りです。

- `Initialize()`は接続できなければ`false`を返します。**例外を投げない**でください。
- `Tick()`は毎フレーム呼ばれます。**ブロックしない**でください。再接続や
  受信処理はここで少しずつ進めます。
- `IsAvailable()`が`false`になると、LamaPonは表示内容を保持したまま待ち、
  `true`へ戻った時点で送り直します。
- LamaPonが渡す`DiscordActivity`は検証済みで、15秒間隔にまとめられています。

アダプターは[パッケージ](packages.md)として配れます。`package.json`の
`native`へSDKのインクルードパス・`.lib`・`.dll`を書けば、Game Moduleの
ビルドとゲームの書き出しへ自動で反映されます。SDK本体は再配布できない
ため、パッケージには空の`sdk/`フォルダーと配置手順だけを入れ、利用者が
Discordから自分でダウンロードして置く形にします。詳しくは
[ネイティブライブラリを含むパッケージ](packages.md#ネイティブライブラリを含むパッケージ)を
参照してください。

`LamaPon::Detail::FakeDiscordPresenceBackend`
（`LamaPon/Online/DiscordPresenceTesting.h`）は、Discordを起動せずに
Presenceの動作を確かめるためのbackendです。単体テストはこれだけで動くので、
CIにDiscordクライアントは必要ありません。

### 8. 今回の範囲

このバージョンは**表示だけ**です。Party、Secrets、Join、Ask to Join、Invite、
Spectate、Matchmaking、Achievements、Overlay制御は含みません。
`DiscordActivity`は、これらを将来フィールドとして追加できる形にしています
（Game Module DLLのレイアウトを壊さないよう、新しいフィールドは必ず末尾へ
追加します）。

## 中断した保存処理を復旧する

強制終了やディスク障害のあとに復旧対象が見つかると、通常のログイン・同期を止めます。
`PersistenceRecoveryStatus()`を取得し、その`revision`を使って復元または破棄します。

```cpp
const auto recovery = PersistenceRecoveryStatus();
if (recovery.state
    != LamaPon::OnlinePersistenceRecoveryState::None)
{
    const auto result = RestorePersistence(recovery.revision);
}
```

`revision`は確認画面を出した後に対象が変化していないことを保証するCASガードです。
古い値を渡すと`Stale`になり、操作しません。`DiscardPersistence()`は復旧対象を破棄するため、
必ず利用者へデータ損失の可能性を示してください。

LamaPonの保存APIは同じ対象への同時更新をロックとリビジョンで調停します。
ゲーム外から保存ファイルを直接編集する、またはLamaPonの対象ロックを使わない別プロセスから
書き込む運用はサポートされません。

## バックエンド通信契約 v1

ここからはバックエンド実装者向けです。すべてJSON UTF-8、リダイレクトなしで実装し、
本番ではHTTPSを必須にしてください。認証要求にもゲームIDと環境IDが設定されている場合は
`X-LamaPon-Game-Id`と`X-LamaPon-Environment-Id`が付きます。

### 認証エンドポイント

| メソッドとパス | 要求 | 成功応答 |
|---|---|---|
| `POST /v1/auth/login/start` | `{"provider":"discord","platform":"windows","protocolVersion":1}` | 201、`transactionId`、`pollToken`、`authorizationUrl`、`expiresIn`、`pollInterval` |
| `POST /v1/auth/login/complete` | `{"transactionId":"...","pollToken":"..."}` | 202 pending、200 authorized、403 denied、410 expired |
| `POST /v1/auth/session/refresh` | `{"refreshToken":"..."}` | 200、更新済みsession |
| `POST /v1/auth/session/logout` | Bearer LamaPon token、`{}` | 200または204 |

pending応答は`{"status":"pending","retryAfter":1}`です。authorized応答とrefresh応答の
sessionは次のフィールドを含めます。

```json
{
  "status": "authorized",
  "accessToken": "short-lived-lamapon-token",
  "refreshToken": "rotated-refresh-token",
  "expiresIn": 900,
  "player": {
    "id": "internal-player-id",
    "displayName": "Player",
    "avatarUrl": "https://cdn.example.com/avatar.png",
    "linkedProvider": "discord"
  }
}
```

`expiresIn`は30〜86400秒です。ログイン開始の`expiresIn`は30〜900秒、`pollInterval`と
`retryAfter`は1〜10秒として扱われます。`player.id`はDiscord IDではなく、推測しにくい
ゲーム内部IDを返してください。更新トークンは利用のたびにローテーションし、過去の値を
失効させる構成を推奨します。

### クラウドセーブエンドポイント

| メソッドとパス | 用途 |
|---|---|
| `GET /v1/cloud-saves/manifest` | プレイヤーが持つ全リソースのメタデータ一覧 |
| `POST /v1/cloud-saves/read` | PlayerPrefsまたは1スロットを取得 |
| `PUT /v1/cloud-saves/item` | JSONデータを作成またはCAS更新 |
| `DELETE /v1/cloud-saves/item` | 既存リソースをCAS削除 |

全要求に次のヘッダーを付けます。

```http
Authorization: Bearer <LamaPonバックエンドの短命token>
X-LamaPon-Game-Id: com.example.my-game
X-LamaPon-Environment-Id: production
```

書き込みと削除には正規形式のUUIDを`Idempotency-Key`として付けます。新規作成は
`If-None-Match: *`、更新と削除は強いETagを`If-Match`へ指定します。CASが一致しない場合は
412と現在のsnapshotを返してください。同じIdempotency-Keyの再送は同じ結果になるようにします。

リソースは次のどちらかです。

```json
{"kind":"preferences"}
```

```json
{"kind":"save_slot","slot":"slot1"}
```

read要求は`{"protocolVersion":1,"resource":{...}}`、PUT要求はそれに`byteLength`、
32-byte SHA-256のpaddingなしbase64url値`sha256`、JSONバイト列のbase64url値`content`を
加えます。応答にも`protocolVersion: 1`、`resource`、引用符を含む強い`etag`、`deleted`、
`byteLength`を返し、未削除なら`sha256`と`content`も返します。manifestは
`{"protocolVersion":1,"items":[...]}`です。削除済み項目は本文なしのtombstoneとして残します。

所有者を要求本文の`playerId`から決めてはいけません。Bearer tokenの`sub`と、検証済みの
ゲームID／環境IDからサーバー側で決定します。トークンのaudience、issuer、期限、署名も検証し、
ゲームIDと環境IDを別テナントとして分離してください。

バックエンドは少なくとも次の上限と、正しいJSONであることを検証します。

| 対象 | LamaPonクライアント上限 |
|---|---:|
| PlayerPrefs | 256 KiB |
| セーブスロット1個 | 1 MiB |
| スロット数 | 32 |
| 1アカウント合計 | 16 MiB |

サーバー側は同じか、より厳しい上限にしてください。401はセッション更新、404はread時の
未存在、412は競合、429は`Retry-After`に基づく再試行に使います。408、425、5xxは一時障害として
再試行されます。応答スキーマ、ETag、ハッシュ、サイズが一致しない応答は安全のため拒否されます。

## UnityのDiscord SDKとの違い

ネットで見かけるUnity対応は、Discordの**Social SDK Unity package**についての情報です。
公式にはUnity 2021.3以降を対象に、認証、フレンド、プレゼンスなどを提供しています。
[Unity導入ガイド](https://docs.discord.com/developers/discord-social-sdk/getting-started/using-unity)と
[対応プラットフォーム](https://docs.discord.com/developers/discord-social-sdk/core-concepts/platform-compatibility)を参照してください。

LamaPonはUnityではなくC++エンジンであり、このブランチはSocial SDKを同梱していません。
ログインは、Discordが案内する標準OAuth2のWeb Flowを自前バックエンド経由で使う構成です。
Rich Presenceは、Social SDKのアダプターを差し込める形だけをLamaPonが用意します
（[Presenceアダプター](#7-presenceアダプター)を参照）。
[Discord OAuth2ドキュメント](https://docs.discord.com/developers/topics/oauth2)も参照してください。
このため、Social SDK側が対応するプラットフォームと、LamaPonのオンライン機能の対応範囲は別です。
現在のLamaPonオンライン機能はWindows x64だけを対象とします。

## 配布前チェック

- Discord OAuthのredirect URIをバックエンドの実URLへ限定した
- `client_secret`とデータベース認証情報はサーバーの秘密管理へ置いた
- LamaPon tokenの署名、issuer、audience、期限、失効を検証した
- Discord利用者と内部プレイヤーIDを一対一で安全に関連付けた
- アカウント回復とDiscord関連付け解除の導線を用意した
- セーブ所有者をBearer tokenから決め、本文のIDを信用していない
- ETag、Idempotency-Key、容量制限、レート制限をサーバーで強制した
- ログへtoken、認証URLの秘密部分、セーブ本文を出していない
- バックアップと復元手順、削除・プライバシーポリシーを用意した
- Windows x64の複数端末、オフライン、強制終了、競合を実機で確認した

Rich Presenceを使う場合は次も確認してください。

- Discord Developer Portalでゲーム専用のApplicationを作り、そのIDだけを設定した
- `client_secret`、Bot Token、access tokenを`project.json`とゲームへ入れていない
- 表示するdetails / stateに、利用者の本名やメールなど公開したくない情報を入れていない
- Discordを起動していない状態でゲームが通常どおり動くことを確認した
- Discordを終了・再起動しても表示が壊れず、ゲームが落ちないことを確認した
- Rich PresenceだけをONにした構成（Discordログインなし）で動作を確認した

関連ページ: [プロジェクト設定](project.md)、[Sceneとセーブデータ](scenes.md)、
[C++スクリプティング](scripting.md)、[エディター](editor.md)、
[書き出したゲームの保護](export-protection.md)
