# 複数プラットフォームとSteam連携の設計

[← ドキュメント一覧へ戻る](index.md)

このページは複数プラットフォーム対応の進捗と計画です。Linux／Androidの出力設定生成とWindowsエディターからの操作経路は実装済みですが、Linux実ビルド、APK生成、対象OSでの実行確認は未完了です。Steam連携も未実装です。
macOSは今回の対応対象から除外します。Steamの初期対象機能は実績・統計・Steam Cloud・オーバーレイです。
ネイティブ出力の検査APIとCLIはWindows・Linux・Androidだけを受け付け、macOSやSteamを出力先として指定すると検査前に拒否します。
エディターを動かすOSと、ゲームを書き出す対象は別に管理します。
エディターはWindowsを維持し、ゲームの出力先をWindows・Linux／Steam Deck・Web・Androidへ広げます。
Steam連携は保留し、OS移植を先に進めます。

共通基盤のビルド方法とOS別の作業一覧は[プラットフォーム移植タスク](platform-tasks.md)を参照してください。

## 現状と候補

| 対象 | 現状 | 次の作業 |
|---|---|---|
| Windowsゲーム | 共有SDL Portable出力と従来出力をビルド・起動検証済み | 別環境・物理GPUを含む追加確認 |
| Webゲーム | Chrome上のWebGL1／WebGL2とPortable Smoke、保存動作を確認済み | モバイル実機でタッチ・復帰を確認 |
| Linuxゲーム | CMake設定生成、診断、WindowsエディターのWSLビルド経路、Linux CIを実装。設定生成は確認済み | 実Linux／WSLでビルド・起動し、CI結果を確認 |
| Steam Deck | Steam Runtime 4 CI構成を実装。CIとDeck実機は未確認 | CI結果、Proton、Linuxネイティブ版、Deck操作を別々に検証 |
| Androidゲーム | Gradle設定生成、SDK事前検査、Android CIと16KB Emulator検証構成を実装。設定生成は確認済み | CIでAPK生成・起動を確認し、実機でタッチ・音声・復帰を検証 |
| エディター | Windowsを維持。Releaseビルドと既存描画／出力テストを確認済み | 対象ゲームの移植を継続 |

Linux・Androidの追加は、出力先の選択肢を増やすだけでは完了しません。
Windowsエディターを維持しつつ、ゲーム出力用にSDL3・OpenGL／OpenGL ES・保存領域・OS別パッケージ検査を分けています。
共通基盤のWindowsテスト、Linux／Androidの設定生成、Webブラウザーの実行確認は通りましたが、Linux上での実ビルドとAndroid APK／実機確認はまだ完了していません。現在のタスクと検証結果は[プラットフォーム移植タスク](platform-tasks.md)を参照してください。

## 共通ゲームコードの境界

Scene、Prefab、ゲームスクリプトからOSや配布先のSDK型を直接扱わない構成を目指します。

- ウィンドウ・入力・音声・ファイル保存先・サスペンド／復帰をプラットフォーム実装へ分離する。
- 描画資源とアップロードの公開契約からDirectXの型を段階的に取り除く。
- アセット変換とShaderの出力を描画バックエンドごとに管理する。HLSLの既存資産の扱いも決める。
- パッケージ依存を対象OS／CPU／配布先ごとに解決し、WindowsのDLLやlibを他の出力に混ぜない。
- 使えない機能は出力前に説明する。対応済み表示は実際に出力・実行の検証を通してから有効にする。

Portableネイティブ出力では、`assets/packages/*/package.json`の
`nativeVariants`からWindows x86-64、Linux x86-64、Android arm64-v8a／x86-64を選び、
ヘッダー、ライブラリ、ランタイム、ライセンスをリンク・同梱します。
従来の`native`宣言はWindows向け互換設定です。実際のOS別SDKを用いたビルドは未検証です。

SDL3はWindows・Linux・Androidのウィンドウ、入力、音声に使い、Portable描画はOpenGL／OpenGL ESを使用します。
Windowsエディターと既存Direct3D出力は維持しています。Linux／Android上の実行確認は引き続き必要です。
[SDL3対応プラットフォーム](https://wiki.libsdl.org/SDL3/README-platforms)

## Steam連携の契約

Steam連携はゲームランタイムの任意機能にします。初期の実装対象はWindowsで、
Linuxへ移植するときにも同じゲーム向けAPIを使う構成です。
Steamを使わないビルドではSteamworks SDK、ライブラリ、ランタイムの配置を不要にします。
Steam用ビルドでも初期化失敗を取得できるようにし、各ゲームが必須条件や表示を決められるようにします。

公開APIにはSteam SDKの型を含めず、次の状態と結果を区別します。

- 連携無効、初期化失敗、準備中、利用可能。
- 実績・統計の読み込み待ち、変更の受理、保存要求中、保存完了、保存失敗。
- Steam Cloudの有効／無効、ファイル操作の成功／失敗。
- オーバーレイ利用可否、開閉状態。

Steamの初期化・コールバック処理・終了をゲームランタイムが管理します。
コールバックはゲームループの同じスレッドで処理し、オーバーレイ表示やゲームの一時停止中も処理を続けます。
エディターでの通常のプレビューから本番の実績や統計を変更しない構成にします。
[Steamworks API概要](https://partner.steamgames.com/doc/sdk/api)

| 機能 | 初期実装の責務 | 確認する動作 |
|---|---|---|
| 実績 | API名で照会・解除し、保存結果を通知 | 未準備時、解除済み、再起動後の状態、保存失敗 |
| 統計 | 整数／浮動小数値の照会・設定と保存 | 定義との型整合性、保存失敗、再試行 |
| Steam Cloud | 保存データだけを同期対象とし、Steamユーザー単位で分離 | 別PC、Windows／Linux間、オフライン、ユーザー切替 |
| オーバーレイ | 利用可否、表示要求、開閉通知 | Steamからの起動、実際の描画、入力／ポーズの復帰 |

Steamworks SDKは未入手のため、まずSDKを必要としない共通設計を進めます。
Steamworks管理画面で実績・統計の定義とCloud設定も必要です。
SDK入手後、保存先とゲームのApp IDを確認してからアダプターの実装・実機検証へ進みます。
開発用の`steam_appid.txt`を配布ビルドに含めないよう、出力時に検査します。

### SDKなしで固めるAPI設計

以下の名前はAPI案であり、現在利用できるC++ APIではありません。

| 契約案 | 内容 |
|---|---|
| `DistributionServices` | Applicationが所有し、初期化・フレームごとの更新・終了を管理する窓口 |
| `DistributionCapabilities` | 実績、統計、Cloud、オーバーレイごとの利用可能状態。初期化成功だけで全機能を利用可能にしない |
| `ServiceOperationResult` | `Unavailable`、`NotReady`、`InvalidArgument`、`Accepted`、`Failed`を区別する操作結果 |
| `ServiceRequestId` | 非同期の保存要求を識別するID。成功／失敗通知を要求元と対応させる |
| `Achievements` | API名による照会と解除要求。未取得を「未解除」として返さない |
| `Statistics` | 整数／浮動小数の型付き照会・設定と、まとめた保存要求 |
| `CloudStorage` | 保存用の論理ファイル名による読み書き。OSの絶対パスを公開契約に含めない |
| `Overlay` | 利用可否、表示要求、開閉通知。開く要求の受理と実際の表示を区別する |

ゲームのウィンドウや描画型をこの窓口に渡さず、各配布先アダプター内部で必要な情報を受け取ります。
最初はSteamアダプターと機能を持たないアダプターを想定します。
機能を持たないアダプターは`Unavailable`を返し、実績保存やCloud同期の成功を装いません。
通常のローカルセーブはこの窓口とは独立して動かします。

Applicationがサービスより先にゲームスクリプトを破棄し、終了後は要求と通知を受け付けません。
サービスへの操作と通知はゲームスレッドへ限定し、通知中の終了やアダプター交換は行いません。
オーバーレイの開閉は通知だけを提供し、ゲームの一時停止はゲーム側が決定します。

実績・統計の変更を受理した段階ではローカルの未保存状態として扱います。
保存通知で成功が確認された時点で未保存状態を解消します。
保存中の追加変更を失わないよう、保存要求ごとに変更の世代を記録します。
再試行は待機時間を設け、毎フレーム保存する設計を避けます。
Steamユーザーが変わった場合は、前のユーザーの未保存状態やCloudファイルを新しいユーザーへ流用しません。

SDKなしの検証では、代替アダプターで初期化失敗、準備待ち、保存失敗、ユーザー切替、終了後の通知を再現します。
これはSteamそのものの動作確認とは別です。SDK導入後に本物のクライアントで4機能を確認します。

### セーブとSteam Cloud

既存のLamaPonバックエンドのクラウド同期とSteam Cloudは別の同期先です。
同じ保存ファイルを両方が同時に更新する構成を避け、ゲーム設定で同期先を選択します。
Steam Cloud導入時にも保存形式とバージョン管理は共通にし、画質や端末固有設定は同期対象から外します。

初期方式は既存のセーブ保存先を調べたうえで、Steam Auto-Cloudと
`ISteamRemoteStorage`のどちらを使うか決めます。
Auto-CloudでOSをまたぐ場合は共通RootとRoot Overridesを設定します。
Steam Deckのサスペンド中の同期を扱う場合は、Dynamic Cloud Syncによるローカル変更への対応も検証します。
[Steam Cloud公式仕様](https://partner.steamgames.com/doc/features/cloud)

## 導入順と完了条件

1. **共通基盤とLinux出力**：Windows版を維持し、共通公開ヘッダーからWindows／DirectX依存を分離する。LinuxでScene・描画・音声・入力・保存を検証する。
2. **Steam Deck**：ProtonとLinux版をそれぞれ確認し、コントローラー操作、UI可読性、サスペンド／復帰を検証する。
3. **Android出力**：NDKとパッケージングを整え、実機でタッチ、画面回転、バックグラウンド復帰、音声、保存を検証する。
4. **Steam連携（保留）**：SDK導入後、実績・統計・Cloud・オーバーレイをSteamから起動したゲームで検証する。

ProtonはWindows実行ファイルをSteam Deck上で動かす経路ですが、互換性は実際のゲームで確認する必要があります。
Protonでの動作と、LamaPonのLinuxネイティブ出力の対応状況は別に報告します。
[Steam Deck互換性審査](https://partner.steamgames.com/doc/steamhardware/compat)

LinuxやAndroidのテスト環境・SDKを新設する場合は、保存先と容量を決めてから作成の許可を得ます。
既存のWeb検証用一時ファイルの許可を、別の移植用プロジェクトやSDKの作成に流用しません。
