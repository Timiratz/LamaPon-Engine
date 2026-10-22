# iOSゲーム出力とXtool導入計画

[← ドキュメント一覧へ戻る](index.md)

## 目標と現在の状態

Windowsエディターで制作した小さなゲームをiPhoneで実行し、TestFlightを経由してApp Storeで有料公開できる出力経路を整備します。エディター自体のiOS／macOS移植は対象外です。

実験用のiOS設定生成とランタイムの移植準備を実装しています。iOSビルド、署名、実機起動、TestFlight、販売用提出は未検証です。エディターには「iOS（実験・ビルド設定）」として表示し、対応済みのゲーム出力とは区別します。

## 今回の制約と実装状況

費用は一切かけません。有料Developer Program、クラウドMac、有料CIなどは利用しません。検証用iPhoneと有料会員アカウントは用意されていないため、実機・TestFlight・App Store販売は今回完了できません。以下の工数表は販売までの将来計画であり、今回完了した作業を示すものではありません。

実装済みの範囲:

- `export_native.py --platform ios`による互換性検査と実験用CMake設定生成。
- `export.native.ios`のBundle ID、バージョン、ビルド番号、最低OS、画面方向の検査。
- arm64専用のアプリバンドル、`Info.plist`、最低OSのCMake設定生成。
- `ios-arm64`のパッケージ選択。ソース・静的ライブラリーを候補とし、実行時共有ライブラリーは拒否。
- ランタイムのiOS用GLESヘッダー、GLES 3.0 context、GLESシェーダー、バンドル内アセット読込の分岐。
- Windowsエディターの実験用iOS設定出力と、出力プロセスの対象OS照合。
- アプリ情報とarm64 Mach-Oヘッダーの検査関数。SDKの対象プラットフォーム・署名・リンク依存・ストア適合性の保証は含まない。

未実装・未検証の範囲:

- XtoolのSDKを使ったCMake／C++／SDL3の実ビルド・パッケージ連携。汎用`build_native.py`のiOSビルドは未対応。
- iPhoneの描画、タッチ、音声、保存、回転、セーフエリア、復帰。
- 開発用・販売用署名、IPA生成、TestFlightとApp Store提出。
- 販売用アイコン、ゲームごとのプライバシー情報、商品ページ。

上記を実機で通すまで「iOSゲームが完成」「Macなしで販売できる」とは扱いません。

### プロジェクト設定例

既存プロジェクトの`export.native`に次の`ios`オブジェクトを追加します。Bundle IDはゲーム所有者が選ぶ識別子に置き換えてください。

```json
{
  "ios": {
    "bundleIdentifier": "org.example.game",
    "version": "1.0.0",
    "buildNumber": "1",
    "minimumVersion": "15.0",
    "orientation": "landscape"
  }
}
```

設定例はプロジェクトファイル全体ではありません。既存のソース選択などを維持して追加します。

読取専用の検査:

```text
python -B tools/export_native.py --project <既存プロジェクト文書> --platform ios
```

実験用のビルド設定を生成する場合は、許可済みの新規／空フォルダーを`--generate-build-project`で指定します。生成だけではiOSバイナリーを作りません。実ビルドには既存のApple SDK、iOS向けツールチェーン、SDL3が別途必要です。

### 検証と後片付け

- 既存出力検査: `python -B tests/NativeExportToolTests.py`、75件成功。
- iOS情報・生成・パッケージ選択・Mach-Oヘッダー検査: `python -B tests/NativeIosToolTests.py`、7件成功。生成物とバイナリーはメモリー内の模擬データであり、iOSビルド検証ではありません。
- 既存ビルドツール検査: `python -B tests/NativeBuildToolTests.py`、30件成功。
- MSVCによる変更したエディターコードのコンパイル検査に成功。
- `NativeExportJobTests`をMSVCでビルドして実行。iOSを含む対象OS・Unicodeパス・プロセス境界検査に成功。
- 既存SDL3ソースを参照し、Portableの`NativeExportSmoke`をWindows向けReleaseでビルド。隠しウィンドウ・ダミー音声・専用保存先で8フレームの起動・描画確認に成功。iOSの検証ではありません。MSBuildの一時保存先警告と既存コードの型変換警告は残っています。
- WSL導入を試みましたが完了せず中止。Ubuntu登録・アプリパッケージと`ios-toolchain`保存先は確認されていません。Swift・Xtool・Apple SDKは導入していません。

今回の検証出力は専用ワークツリーの`build/ios-tests`内に限定しました。削除を試みましたが、自動承認レビューがポリシーにより拒否したため、約68MiB（71,523,063 bytes）が残っています。対象を確認した絶対パス指定でも拒否されました。削除完了とは扱いません。ソース・設計書・正式なテストコードはブランチの変更として保持します。

残存検証出力は次のフォルダーです。手動削除すると検証バイナリー・設定・ログ・保存データをまとめて除去できます。

```powershell
Remove-Item -LiteralPath 'C:\Users\it232252\.codex\worktrees\ios-xtool\LamaPon-Engine\build\ios-tests' -Recurse -Force
```

XtoolはSwiftPMを中心としたビルド・署名・端末インストールの候補です。既存CMakeのC++／SDL3ランタイムをそのまま扱えるとは仮定しません。WindowsではWSLを使用する方式を最初に調査します。販売用署名・アップロードの成立は、開発用署名による実機起動とは別に確認します。

## ソース調査で確認した境界

| 箇所 | 現状 | iOS対応に必要な調査・変更 |
|---|---|---|
| `CMakeLists.txt` | 通常構成はWindows以外を拒否。共通基盤のみの別構成あり | WindowsエディターとiOSゲームの構成を分離する |
| `cmake/LamaPonNative.cmake` | `APPLE`を拒否。Windows／Linux／Androidのみ | iOSの識別、arm64、リンク、アプリ資源配置を追加する。macOS全般を許可しない |
| `src/LamaPon/Native/NativeGL.h` | AndroidのみGLES3ヘッダーを使用 | iOS SDKのOpenGL ESヘッダーとリンク方式を確認する |
| `src/LamaPon/Native/NativeGame.cpp` | AndroidはGLES 3.0、それ以外はデスクトップGL 3.3 | iOSでGLESを選択する。SDLアプリコールバックと復帰処理を実機で検証する |
| `src/LamaPon/Web/WebRenderer3D.cpp` | Portable描画とWeb描画を共有。Android条件分岐あり | デスクトップGL向け分岐を監査し、iOSのフレームバッファ・シェーダーを確認する |
| `docs/platform-roadmap.md` | ネイティブ出力の対象はWindows／Linux／Android | 実装進捗に合わせてiOS計画を追加。対応済み表示は実行確認後に行う |

SDL3を使う入力・音声・保存・ゲームループは再利用候補です。iOSでの動作は未確認です。初期検証は既存GLES描画を候補とし、Metalの新規実装は別途見積もります。

## 実装方針と最初の判断地点

1. 依存ライブラリとゲームスクリプトのiOS向けコンパイル・リンク条件を調べる。
2. Xtool／SwiftPMからC++／SDL3をビルドする方式と、CMakeで作った静的ライブラリをアプリへ組み込む方式を比較する。
3. iOS arm64で最小描画・タッチを実機確認し、採用方式を決める。
4. Portableランタイム、ゲーム資源、音声、保存を接続する。
5. 動作成立後にWindowsエディターの出力操作へ統合する。

判断地点は「WindowsからC++／SDL3アプリをiPhoneで動かせること」です。ここで成立しない場合は原因と不足機能を記録し、クラウドMacを使う代替方式の工数・費用を比較します。Xtoolだけで販売まで完結できるとは約束しません。

## 段階工数

1人日を約8時間とし、1人専任の概算です。審査待ち、ゲーム本編制作、アプリ内課金・広告・Game Center、大規模な描画追加は含みません。

| 段階 | 成果・完了条件 | 概算 |
|---|---|---:|
| 1. 現状調査・設計 | 移植対象、依存条件、ビルド候補を整理 | 2〜4人日 |
| 2. 環境準備 | 最小iOSアプリをWindowsから実機へインストール | 2〜4人日 |
| 3. C++／SDL3接続 | 実機で描画・タッチが動く | 4〜8人日 |
| 4. ランタイム移植 | 小さなラマポンシーンで描画・入力・音声・保存が動く | 8〜15人日 |
| 5. iPhone動作整備 | セーフエリア、回転、復帰、音声中断が安定 | 5〜10人日 |
| 6. エディター統合 | iOS設定、ビルド操作、依存検査、エラー表示を提供 | 5〜8人日 |
| 7. TestFlight | 販売用ビルドをApp Store Connectが受理し配布可能 | 3〜6人日 |
| 8. 販売準備 | 商品ページ・契約・審査対応を完了し有料公開 | 3〜7人日 |
| 合計 | 技術検証後に再見積もり | 32〜62人日 |

## 検証項目と必要な準備

- arm64ビルド、端末接続、開発用署名、インストール、起動。
- 2D／3D描画、シェーダー、画面のピクセル寸法、タッチ座標、画面回転・セーフエリア。
- アプリ内資源の読込、書込可能な保存先、終了・再起動後の保存復元。
- ホーム画面移動・復帰、画面ロック、音声中断、メモリー不足時の処理。
- パッケージのiOS／arm64依存解決とライセンス表示。Windows用バイナリーを混入させない。
- 配布用証明書・プロビジョニング、アイコン、必要なプライバシー情報、SDK提出要件。
- TestFlightの受理・起動とApp Store審査。有料契約・銀行・税務情報は販売者が設定する。

iPhone実機、Appleアカウント、Appleから入手するSDK、販売段階ではApple Developer Program加入が必要です。秘密鍵・認証情報・SDKはリポジトリーへ保存しません。

追加の検証用プロジェクトや一時フォルダー・一時ファイルを作る前に、作成理由、保存先、概算容量、削除方法を説明して明示的な許可を得ます。環境導入時のSDK・ビルド成果物の容量は、採用バージョンと構成を確定してから見積もります。

## 参考資料

- [Xtool公式リポジトリー](https://github.com/xtool-org/xtool)
- [Xtool Linux／Windows導入](https://xtool.sh/documentation/xtool/installation-linux/)
- [SDL3 iOS資料](https://wiki.libsdl.org/SDL3/README-ios)
- [Apple Developer Program](https://developer.apple.com/programs/)
- [App Store Connectへのビルドアップロード](https://developer.apple.com/help/app-store-connect/manage-builds/upload-builds/)
