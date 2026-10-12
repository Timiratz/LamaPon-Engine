# LamaPonのインストール

Windows 10/11でx64アプリを実行できるPC向けです。

## 配布する人

[ブランチに同梱したSetup.exe](../installers/LamaPon-0.1.0-windows-x64-Setup.exe?raw=1)をダウンロードできます。ファイルのサイズとSHA-256は[配布ファイルの情報](../installers/README.md)を確認してください。

`LamaPon-<version>-windows-x64-Setup.exe`を渡してください。受け取る側はエンジンをビルドする必要はありません。実行ファイルだけを抜き出すと、DLL、SDK、シェーダーなどが不足します。

## 初めて使う人

インストーラーは日本語のみで表示します。使用許諾画面にはMITライセンスの日本語訳を表示し、原文の`LICENSE`と第三者ライセンスは配布物に保持します。

1. Setup.exeを実行し、保存先とショートカットを選びます。
2. 完了画面の「C++自動ビルドに必要なソフトを確認・セットアップする」を実行します。
3. 既に必要な環境がある場合は診断だけで終了します。不足する場合は、容量・管理者権限・ダウンロードの案内を確認し、Microsoftの画面でBuild Toolsを導入します。
4. Windowsの再起動を求められた場合は再起動します。その後、スタートメニューの「自動ビルド環境セットアップ」を再実行して確認します。
5. 「LamaPon Hub」を起動し、プロジェクトを作成または追加します。

既定の保存先は、そのPCで操作しているユーザーの`%LOCALAPPDATA%\Programs\LamaPon`です。LamaPon本体のインストールには管理者権限を要求しません。別のWindowsユーザーも利用する場合は、そのユーザーでSetup.exeを実行してください。

起動に必要なVC++とXAudio2のランタイムDLLを同梱します。C++の編集・自動ビルドにはMSVC、Windows SDK、CMake 3.25以上、Ninjaが必要です。環境セットアップは、対応するVisual Studioが既にある場合にはその環境へ不足機能を追加します。未導入・古い環境にはVisual Studio Build Tools 2026のC++ワークロード（推奨コンポーネントを含む）とCMakeツールを導入します。配布SDKを作成したMSVC以上のツールセットが必要です。インターネット接続と管理者権限、数GB～十数GBの空き容量が必要です。起動用EXEは`%LOCALAPPDATA%\LamaPon\BuildToolsDownload`に保存し、処理終了時に削除します。

学校・会社のPCで導入が制限されている場合は、管理者にC++ビルド環境の導入を依頼してください。本体のインストールと、ビルド済みのSceneを試す操作は別に利用できます。

## 3つの起動ショートカット

- **LamaPon Hub**：プロジェクトの作成・選択。
- **LamaPon Engine**：Editorを直接起動。
- **LamaPon 自動ビルド**：エンジン本体のソースを再ビルドして、このインストール先へ更新。初回は完全なLamaPon-Engineソースのフォルダーを選択します。配布SDKだけではエンジン本体の再ビルドはできません。ビルド先は選択したソース内の`build-release`で、容量は数GB以上です。

ゲーム用C++ Scriptの自動ビルドはEditor内で動作します。Scriptを保存するとGame Moduleがビルドされます。「LamaPon 自動ビルド」はエンジン本体を開発する人向けです。

完全なソースは[GitHubリポジトリ](https://github.com/Timiratz/LamaPon-Engine)から取得します。Gitを使う場合はGitを別途インストールしてください。

カスタム配置のVisual Studioを利用していて診断が不足を示す場合、Editorが検索する`C:\Program Files\Microsoft Visual Studio`配下へのBuild Tools導入を利用してください。複数のVisual Studioがある場合は、最新のMSVCツールセットを持つ環境にCMakeとNinjaも必要です。

## 更新・アンインストール

新しいSetup.exeを実行すると、同じ保存先へ上書き更新できます。使用中のLamaPonは作業を保存して終了してください。Windowsの「インストールされているアプリ」からLamaPonを削除すると、インストーラーが配置したファイルとショートカットを削除します。別の場所に保存したゲームプロジェクトや、Visual Studio Build Toolsは削除しません。

## インストーラーの生成

開発者は、MSVCのx64開発者プロンプトでRelease版を構成・ビルドし、[Inno Setup 6.3以上](https://jrsoftware.org/isdl.php)を用意します。

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
powershell -NoProfile -ExecutionPolicy Bypass -File tools\BuildInstaller.ps1 -BuildDir out\build\windows-release -Compiler "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
```

既定では`out\installer\payload`に配布一式、`out\installer`にSetup.exeを生成します。既存のpayloadがある場合は、確認して削除するか、`-OutputDir`で新しい出力先を指定してください。Debug版の配布は拒否します。署名を付ける場合は完成したSetup.exeに配布者のコード署名を付けてください。

自動導入では、`Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`を使用できます。この場合は追加ビルドツール導入とHub起動を行いません。必要に応じ、配布先の`tools\SetupBuildEnvironment.ps1 -CheckOnly`で診断してください（準備完了: 0、不足: 1）。

導入方式は[Microsoftのコマンドライン導入仕様](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio)と[Inno Setupのユーザー別導入仕様](https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm)に基づいています。
