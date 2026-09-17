# Discord Rich Presence (Social SDK)

LamaPonの **Discord Rich Presence** を、Discord公式の **Social SDK** で
実際にDiscordへ表示できるようにするアダプターです。

LamaPon本体はDiscord Rich Presenceの仕組みを持っていますが、SDKを
同梱していないため、標準のままでは常に「利用不可（Unavailable）」に
なります。このパッケージを入れてSDKを配置すると、実際に表示されます。

> **Discordログイン（アカウント連携）とは無関係です。**
> このアダプターはDiscordのOAuthを一切行いません。ログインしなくても
> Rich Presenceだけを使えます。access tokenもrefresh tokenも扱わず、
> 保存もしません。

## 1. SDKを入手する

**ライセンス上、SDK本体をこのパッケージへ同梱できません。**
お手数ですが、ご自身でDiscordから入手してください。

1. [Discord Developer Portal](https://discord.com/developers/applications)
   を開き、ゲーム用のApplicationを作る（すでにあればそれを使う）
2. そのApplicationのページからSocial SDKをダウンロードする
3. Windows x64向けの中身を取り出す

## 2. SDKを配置する

このパッケージの `sdk` フォルダーへ、次の3つを置きます。
フォルダーはあらかじめ用意してあります。

```
assets/packages/discord-presence-sdk/sdk/
├── include/discordpp.h
├── lib/discord_partner_sdk.lib
└── bin/discord_partner_sdk.dll
```

ダウンロードしたSDKの中では、おおむね次の場所にあります。

| 置くファイル | SDK内の場所 |
| --- | --- |
| `sdk/include/discordpp.h` | `include/discordpp.h` |
| `sdk/lib/discord_partner_sdk.lib` | `lib/release/discord_partner_sdk.lib` |
| `sdk/bin/discord_partner_sdk.dll` | `bin/release/discord_partner_sdk.dll` |

配置し忘れたままビルドすると、リンクエラーではなく
「どのパッケージの何が足りないか」を名指しした案内で止まります。

`sdk` フォルダーに置いたSDKは、あなたのゲームのビルドに使われるだけで、
LamaPonが再配布することはありません。

## 3. Application IDを設定する

エディターの **プロジェクト設定 → オンラインサービス →
Discord Rich Presence** を開き、

- 「Discord Rich Presenceを使う」をオンにする
- **Application ID** に、Developer PortalのApplication IDを入れる
- 必要なら既定の画像キー・画像テキストを入れる

画像キーはDeveloper Portalの **Rich Presence → Art Assets** へ
登録した名前です。

ここに入れた内容は `project.json` に保存され、ゲームにも書き出されます。
**公開して問題ない情報だけ**を入れてください。client secretや
access tokenは入れません（LamaPonはそもそも保存しません）。

## 4. 表示内容をゲームから変える

Scriptから次のように書けます。Discordの用語を覚える必要はありません。

```cpp
void Start() override
{
    // 1行目・2行目だけを変える簡易版
    SetDiscordActivity("Chapter 3", "Boss Battle");
}
```

細かく指定したい場合は `DiscordActivity` を使います。

```cpp
#include "LamaPon/Online/DiscordPresence.h"

void Start() override
{
    LamaPon::DiscordActivity activity;
    activity.details = "Chapter 3";       // 1行目
    activity.state = "Boss Battle";       // 2行目
    activity.largeImageKey = "chapter-3"; // Art Assetの名前
    activity.largeImageText = "Chapter 3";
    // 経過時間を出す（0は「指定なし」）
    activity.startTimestamp = LamaPon::DiscordPresenceUnixTime();
    SetDiscordActivity(activity);
}
```

消すときは `ClearDiscordActivity()` です。状態を見たいときは
`IsDiscordPresenceAvailable()` / `DiscordPresenceStatus()` /
`DiscordPresenceError()` が使えます。

`details` と `state` に何を入れるかはゲーム次第です。LamaPonは
ジャンルを前提にしません（`Stage 5` / `Year 12` / `Puzzle 48` など）。

## 自分のコードからSDKを直接使いたいとき

`discordpp.h` をそのままincludeすれば使えます。ただし
**`DISCORDPP_IMPLEMENTATION` は定義しないでください。**

`discordpp.h` はヘッダーオンリーのラッパーで、このマクロを定義した
ちょうど1つの `.cpp` が実装を持つ決まりです。このパッケージでは
`DiscordSocialSdkImplementation.cpp` がその役をしています。
もう一度定義すると多重定義でリンクできなくなります。

## 覚えておくこと

- **Discordが起動していなくてもゲームは普通に動きます。** 警告ログを
  1回出して、Rich Presenceなしで続行します。あとからDiscordを起動
  すれば自動でつながります。
- **更新は15秒に1回です。** Discordの仕様で、それより速い更新は
  黙って捨てられます。LamaPonは最新の指定だけを覚えておいて、この
  間隔でまとめて送ります。毎フレーム呼んでも問題ありません。
- **文字数の制限があります。** `details` と `state` は2〜128バイト、
  画像キーは256バイトまでです。超える指定はLamaPon側で弾かれ、
  `DiscordPresenceError()` に理由が入ります。
- **C++のゲームモジュールが必要です。** このパッケージはScriptを
  ビルドする仕組みの上で動きます。

## ライセンス

このパッケージ（アダプターのソース）はMITライセンスです。
`LICENSE` を見てください。

**Discord Social SDK本体はこれに含まれず、Discordの規約に従います。**
SDKの入手・利用条件はDiscord Developer Portalで確認してください。
