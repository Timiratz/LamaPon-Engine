# プラットフォーム移植タスク

[← ドキュメント一覧へ戻る](index.md) / [設計方針](platform-roadmap.md)

エディターはWindowsを維持します。ゲームの対象はWindows、Linux／Steam Deck、Web、Androidです。
macOSは対象外、Steam連携は保留です。

## 今回の共通基盤

トップレベルCMakeに`LAMAPON_PLATFORM_CORE_ONLY`を追加しました。
有効にするとエディター、DirectX、ゲーム出力を構成せず、
`LamaPonPlatformCore`と共通基盤のテストだけをビルドします。
既定値はOFFのため、既存のWindowsエンジンビルドを選びます。
このモードはLinux・Androidのゲームランタイムではありません。

- `PathUtils.h`のWindows SDK依存をWindowsだけに限定。
- UTF変換を標準C++へ移し、WindowsのUTF-16とLinux／AndroidのUTF-32に対応。
- Linuxのキャッシュキーで大文字小文字を区別し、UTF-8のバイトを符号なしで処理。
- Linuxのキャッシュ先は絶対パスの`XDG_CACHE_HOME`、`HOME/.cache`、OS一時領域の順で解決。共通基盤テストは絶対XDG、HOMEへのfallback、相対XDG／HOMEの無視と一時領域fallbackを確認します。環境変数だけを変更し、ディレクトリは作成しません。このPOSIX分岐の実行確認はLinux CIで行う必要があり、Windowsでの共通基盤テストでは代替できません。
- Linuxの実行ファイルの場所は`/proc/self/exe`から取得。
- Androidは生成Activityの`getCacheDir()`を共通PathUtilsへ渡し、ゲームのセーブには`getFilesDir()`を使う（APKでの実行は未検証）。
- 既存の`JobSystem`と`VersionCompare`を共通ライブラリに含める。
- キーボード・ポインター・ゲームパッドの状態とフレーム境界を`PortableInputState`へ分離。Web入力もこの層を使用。

パス取得関数はフォルダーを作成しません。
POSIX上の`UsesNetworkDrive`は現在falseを返し、NFS等の検出には未対応です。
Androidの`ExecutableDirectory`はホストプロセスの場所で、APK内アセットの場所として使用できません。
[XDG仕様](https://specifications.freedesktop.org/basedir/latest/)

## 共通基盤のビルド

以下はビルド生成物を作成する手順です。保存先の作成許可が必要な環境では、実行前に許可を得てください。

Windows／Linux:

```text
cmake -S . -B <許可済みのビルド先> -DLAMAPON_PLATFORM_CORE_ONLY=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build <許可済みのビルド先> --config Release --parallel 2
ctest --test-dir <許可済みのビルド先> -C Release --output-on-failure --no-tests=error
```

WindowsではVisual StudioのC++開発環境、LinuxではC++20コンパイラーとCMakeが必要です。
AndroidはNDKの`build/cmake/android.toolchain.cmake`を`CMAKE_TOOLCHAIN_FILE`へ指定し、
`ANDROID_ABI=arm64-v8a`または`x86_64`、`ANDROID_PLATFORM=android-26`でビルドします。
Androidのテスト実行ファイルはホストCTestに登録しません。CIでは別途、両ABIのdebug APKを生成して検査し、
x86_64 APKは16KBページサイズのAndroid Emulatorで起動します。実機テストはまだ含みません。
[Android NDKのCMake手順](https://developer.android.com/ndk/guides/cmake)

`.github/workflows/platform-core-ci.yml`はWindows／Linuxの共通基盤テストに加え、arm64-v8aとx86_64の
Portableゲームdebug APKを生成する構成です。対象パスを含む`codex/**`ブランチへのpushでも、Web CIとともに起動します。
これにより、ドラフトPRを作る前に作業ブランチ上で対象OSのCI結果を確認できます。AndroidジョブはSDK Managerのライセンスを受諾してから、API 36、Build Tools 36.0.0、
NDK 30.0.16248370（最新LTS）とCMake 3.31.6を導入し、APK出力ツールが使う版と揃えます。テスト用ゲームは`test-output/platform-core`へ
用意し、SDK Manager以外の取得物であるSDLソースは公式アーカイブのSHA-256を検査して使います。
Java 17とGradle 9.6.0も公式セットアップActionで明示し、ホストのプリインストール版に依存しません。
開発PCへのAndroid SDKインストールは求めません。
APKに含まれるActivity・ゲーム／SDL／C++ランタイムのABI・16KB ELF配置・選択アセット・ライセンス・
ZIP配置・署名を検査します。x86_64 APKはAndroid 15の16KBページサイズEmulator上で起動し、
ページサイズ・Scene描画・UIButtonのタッチクリックイベントとsender・同一プロセスでのActivity前景復帰・ディスプレイサイズ変更後の描画継続・
アプリ専用領域への保存と、強制終了後の保持を検査します。
さらにADB画面キャプチャをPNGとして解析し、Portable Sceneの緑色UIが1,000画素以上表示されることを確認します。
この画像判定は描画の存在を確認するもので、材質・画質や全端末での表示品質を保証しません。
Android用SmokeゲームはUIButtonの`clickEvent`をScriptで受け、タッチ後のイベント数と送信元を専用保存領域へ記録する構成です。
同じゲームはWindowsネイティブでコンパイル・起動しましたが、Emulatorのタッチ入力とAndroid APKはまだ未検証です。
検査を含むAndroid APK／Emulator CIはまだ未実行です。
Androidターゲットには`GLESv3`のリンクと16KBページ指定を追加し、未解決シンボルを許可しない設定にしました。
このゲーム用検証構成はWindowsで実際にビルド・2回起動し、日本語セーブとPE依存を確認しました。
設定を追加した段階で、ビルド・起動成功を示すものではありません。
タッチ・音声の実機検証と機種固有のSurface復帰は別途必要です。
[NDKのOpenGL ESリンク](https://developer.android.com/ndk/guides/stable_apis#opengl_es)、
[16KBページ対応](https://developer.android.com/guide/practices/page-sizes?hl=ja)
CIを追加しただけでは実行済みとはしません。現時点のローカル検証はWindowsのみです。
LinuxにはSDL 3.4.18の公式ソースをSHA-256で検査し、実ゲームをビルドしてXvfb／Mesa上で
描画・入力・リサイズ・保存を検査するCIも追加しています。これは未実行の構成で、Steam Deck実機検証ではありません。

## ネイティブ実行基盤の実装中コード

`src/LamaPon/Native`と`cmake/LamaPonNative.cmake`に、SDL3を用いるPortableゲーム実行基盤を追加しています。
WindowsではSDL 3.4.18をリンクしたMSVC Releaseビルドと、実際のゲーム起動テストが通りました。
SDLのキー変換・フォーカス喪失、タッチの座標・解除・キャンセル、画面サイズ変更後のUI・3D描画ピクセル、
日本語の保存値がプロセス再起動後も残ることを確認しています。
日本語の起動シーン名とBMP画像名も実ファイルで確認し、WindowsのANSI文字コードに依存していた読み込みを修正しました。
共有Portableコードの修正後、ChromeでWeb版の起動検査も通っています。Linux／Androidのビルド・実機動作は未検証です。
WindowsエディターのLinux／Steam Deck出力には、CMake設定だけを生成するモードと、既存WSLディストリビューション内でLinuxゲームをビルドするモードがあります。
WSLモードは指定したSDL3ソースを使い、生成したELF・選択アセット・ライセンス・共有ライブラリ依存を検査します。WSLやOSパッケージは自動インストールしません。
AndroidはCMake／Gradle設定生成とdebug APKビルドの両方を選べます。
どの出力も実行確認を自動で成功扱いにしません。
エディターの変更箇所はMSVCでコンパイルし、生成processの非同期実行・日本語パス・対象OS不一致の拒否を検査済みです。
エディター全体のReleaseビルドと出力ダイアログ5形式のWARP描画テストは通っています。
Linuxの「WSL内でLinuxゲームまでビルドする」を選ぶと、既存ディストリビューション・SDL3ソース・Python 3.11以降・CMake 3.25以降・C++20コンパイラーを使ってLinux ELFを生成します。
WSLのドライブ共有を通じてプロジェクトと出力先へアクセスします。依存のインストールや取得は行いません。
Windows側のパス変換、プロセス引数、失敗応答、出力先外のELF拒否をPythonテストとMSVCプロセス境界テストで確認しました。
このPCにはWSLディストリビューションがないため、実WSLのLinuxビルドとAndroidの実APKビルドは未検証です。

Androidには「debug APK」も選べる出力経路を追加しています。
既存SDK・JDK・Gradle・SDL3の場所を入力し、「debug APKをビルド」を押すと診断・設定生成・ビルドを非同期で実行します。
SDKは自動取得せず、Gradle依存の取得もチェックを入れた場合だけ許可します。
生成物とキャッシュは出力先の`build`配下へ置き、成功したAPKのフォルダーを開けます。
成功表示にはビルド・配布物検査の成功フラグと、予定先のAPKファイルの存在が必要です。
変更した画面コードとジョブはMSVCでコンパイル済みです。子プロセス境界のテストでは、
日本語・空白・`&`を含むSDKパス、APK欠落、検査未完了の拒否、他の出力形式への切り替えを確認しました。
このプロセステストはスタブであり、Gradle・APK署名・実機動作の成功を示しません。
実際のAPKビルドと、エディター画面からの操作確認はまだ未完了です。

- SDLのウィンドウ、キー、マウス、タッチ、ゲームパッドから共通入力状態へ接続。
- 既存Portable Scene・Script・物理と、OpenGL 3.3／Android OpenGL ES 3.0描画を接続。
- PNG／JPEG／BMPの画像、TTFの文字、WAV／Ogg Vorbisの音声を読むアダプターを実装。
- ゲーム専用の保存先へJSONを保存し、書き込み後の置換と次回起動時の読み込みを実装。
- バックグラウンドで入力を解除し、音声・更新を停止。SDLのGLコンテキスト交換通知でGPU資源を再構築する。

Portable AudioSourceの`streaming`指定は現状フルデコード再生です。ネイティブSpatial audioは距離減衰とステレオ定位で、Web AudioのHRTFとは異なることを出力診断に反映しています。

`NativeAudioTests`で、SDLの実際のミキサーコールバックを音声ストリームへ接続してPCMを検査しました。
Windowsでは日本語名のステレオWAV、24kHzモノラルから48kHzステレオへの変換、主音量・音源音量・
左右定位・3D定位と距離減衰・ピッチ変更中のループ・停止・非ループ音源の完了・256音源の上限が通りました。
欠落ファイルと音声以外のファイルは再生ハンドルを返さず、SDLデバイスの一時停止・復帰も確認しました。
無効な音源／リスナー位置が通常の音量で再生される問題を再現し、NaN／無限大を含む位置は無音になるよう修正しました。
有効な位置に戻したときに再生が続くことも検証しています。
既存のプロジェクト所有素材`tests/fixtures/audio/startup.ogg`も実際にデコードし、48kHzステレオへの変換、
ループ境界をまたぐPCMの信号と非ループ再生の完了を確認しました。
NaN／無限大を含む浮動小数WAVが再生ハンドルを返す問題を再現し、デコード後に拒否するよう修正しました。
拒否後のミキサー出力が無音で、有効な音声を続けて再生できることも検証しています。
日本語・絵文字・空白・`&`を含むアセット／保存先フォルダーでも同じWAV・Vorbisテストが通りました。
デコード拒否の修正後、配布SDKから音声テスト用フックを含まない共有SDL版Windowsゲームを再ビルドし、
同梱依存検査と2回の起動、日本語セーブの継続を確認しました。
ダミー音声デバイスとPCMの数値検査であり、実スピーカー、Linux／Android端末での音声出力は未検証です。
Windows CIとLinuxの静的／共有SDLのCIに同じ音声テストを追加しました。CI自体の実行結果はまだ確認していません。
修正後のWindowsゲームの描画・入力・GLコンテキスト交換・ライフサイクル・セーブ再起動テストも通りました。

アプリの背面状態とウィンドウの最小化状態を別々に管理し、両方の解除を待って再開します。
SDLの通知が別スレッドから届いても、入力・Scene・GLの操作はメインスレッドで処理します。
Windowsの実ゲームで、背面中のウィンドウ復元、最小化中の前面通知、別スレッドからの復帰通知を
注入して確認しました。修正前の早期再開・GLコンテキスト復帰失敗も同じテストで再現済みです。
これはAndroid／Steam DeckのOSサスペンドやSurface再作成を実機確認した結果ではありません。

Windowsでは、共有資源を持たない新しいGLコンテキストを作り、以前のコンテキストを実際に破棄するテストも通しました。
既存のSceneとMesh IDを維持して頂点・Index・Shaderを再生成し、画像と文字のTexture IDも維持して再アップロードします。
交換後も3Dメッシュと日本語ファイル名の画像が描画され、ボタン文字の画素列が交換前と一致することを確認しました。
スクリプトの再起動やSceneの再読み込みは行わず、保存カウンターが各プロセスで1回だけ増えることも検査しています。
再構築用に画像・文字テクスチャのRGBAデータをCPU側へ保持するため、その分のメモリーを使用します。
実際のAndroid Surface再作成やSteam DeckのGPU復帰はまだ未検証です。

画像のバイト列からテクスチャを生成する`Renderer3D::CreateTextureEncoded`を追加しました。
Windowsではメモリー上のPNGをデコードし、GPUのRGBA画素を確認しています。呼び出し元のバイト列を
変更した後と、共有資源のないGLコンテキストへの交換後も、同じハンドルと画素で復元されました。
ChromeではWebGL1・WebGL2・Canvas2Dをそれぞれ指定し、同じPNGの画素とキーボード／マウス／ゲームパッドのUI操作を確認しました。
WebGLのコンテキスト喪失・復帰通知にも接続しました。喪失中は更新を停止して保持入力を解除し、
復帰時にプログラム・Uniform・メッシュバッファ・WebGL1の32ビットIndex拡張を再構築します。
画像は圧縮データをBlobで保持し、復帰後に再デコードして同じ画像IDへ登録します。
画像の復帰も非同期のため、GPU資源の復帰通知だけで全画像のデコード完了を保証しません。
既定のシェルは復帰を待つ表示へ切り替え、10秒経っても復帰しない場合は再読み込みを案内します。
ChromeのWebGL1／2で`WEBGL_lose_context`により2回続けて喪失・復帰を起こし、
メモリーPNGの同じハンドルの画素と、埋め込み画像付きglTF／GLBの描画画素が戻ることを確認しました。
描画資源の所有者を破棄した際の未完了画像とコンテキスト通知の解放も、既定・別Canvasで検査しました。
Canvas2Dの既存描画・入力と、WindowsのGL交換・復帰・セーブの2回起動検証も通っています。
AndroidのSurface再作成やSteam Deck実機での復帰を確認した結果ではありません。
[Emscriptenのコンテキスト通知](https://emscripten.org/docs/api_reference/html5.h.html#webgl-context-event-callbacks)
Webのデコードは非同期なので、返されたハンドルだけでは読み込み完了を保証しません。
ソフトウェア描画では非表示Canvasへ入力が登録されたままになる問題も再現し、標準シェルでは表示面に
共通する`stage`へ入力を登録するよう修正しました。独自シェルの入力先指定は引き続き利用します。
`RunBrowserTests.py --renderer webgl1|webgl2|canvas2d`は実際の描画方式も検査し、Web CIへ各方式の検証を接続しました。
この画像経路をglTFのbase64 data URI画像と、GLBのbufferView画像へ接続しました。
アルベド・法線・金属度粗さ・遮蔽・発光の5経路に実装し、同じモデル内の画像バイト列を共有します。
モデルのPNG画像はWindowsとChromeのWebGL1／2・Canvas2Dで、実モデルの描画画素まで検証しました。
WindowsではGLコンテキスト交換後のモデル描画も通っています。JPEG埋め込みのコード経路は追加済みですが、
埋め込みJPEGと5経路すべての材質再現、Linux／Androidでのモデル実行はまだ未検証です。

PortableのボタンUIに、ゲームパッドの十字キー・左スティックとキーボードの矢印キーによる空間的な選択を追加しました。
Tab／Shift-Tabは有効なボタンを順番に選択し、端では反対側へ移ります。
選択したボタンはホバー色で表示し、Submit（既定は南側ボタン／Enter／Space）でクリック、Cancelで選択を解除します。
選択がない状態のSubmitはクリックを発生させないため、ゲーム中のジャンプ入力が最初のボタンを押すことを防ぎます。
マウスを動かすか押すと選択を解除し、マウス操作へ戻ります。
無効・非操作対象・サイズのないボタンは飛ばし、選択したオブジェクトの破棄後も別ボタンへ決定を転送しません。
Portableの`UIButtonComponent::SetNavigationEnabled(false)`、またはScene JSONの`navigationEnabled: false`で
個別ボタンを選択対象から外せます。`IsFocused()`で現在の選択を取得できます。
方向入力の押下アクションは`UIUp`／`UIDown`／`UILeft`／`UIRight`、順送りは`UINext`／`UIPrevious`です。
プロジェクトの入力割り当てでこれらの押下入力とSubmit／Cancelを変更できます。
左スティックは各方向へ0.55以上倒すと1回選択を移動し、0.35未満へ戻すと次の入力を受け付けます。
境界付近の揺れや保持による繰り返し移動を防ぎ、フレーム終了・フォーカス喪失・切断で入力状態を解除します。
WindowsのSDL仮想ゲームパッドで四方向の選択・無効ボタンのスキップを検証しました。
Web入力も共通のゲームパッド状態処理を使い、Chromeの模擬Gamepad APIで左スティックによる選択・保持・
ニュートラル復帰後の再入力・決定・切断を検証しました。WebGL1／WebGL2／Canvas2Dの3経路で通っています。
これは物理コントローラーやSteam Deckでの検証ではありません。
押し続けた際の自動リピートとスクロールUIとの連携はまだ含みません。

SDL入力アダプターには`NativeInput::SelectGamepad(id)`を追加しました。0は最初の接続機器を自動選択し、
機器を指定した場合は切断後に別機器へ勝手に切り替えません。別機器を再選択すれば入力を再開します。
この選択APIは入力アダプター用で、エディターやゲーム内の機器選択画面は未実装です。
Windowsの実ゲームで、SDL仮想コントローラーを接続して選択・決定・キャンセル・切断・再選択を確認しました。
既存XInput機器と同時に接続した状態でも、指定した検証機器の入力を使うことを確認しました。
ボタンの無効化・破棄、フォーカスのない決定、Tabの循環、マウスへの切り替え、分割更新の入力抑制も検査しています。
Chromeでは実際のDOMキーイベントでTab／Enter操作、その後のマウスクリックと描画を検証しています。
変更後のネイティブ出力関連Pythonテスト38件とWeb出力テスト38件も通っています。
この結果はLinux／Steam Deck／Androidの実機でのコントローラー操作を確認したものではありません。

ネイティブ配布物にはエンジンのライセンス文を`licenses/LamaPon.txt`へ同梱します。
ソースの`LICENSE`、またはインストール済みSDKの`licenses/LamaPon.txt`を参照します。
Windowsで生成設定から実行ファイルを再ビルドし、同梱した文書が元の`LICENSE`と同一であることを確認しました。
再ビルドしたゲームも2回起動し、日本語アセットの読み込みと既存セーブの保持を確認済みです。
AndroidではNDKの`NOTICE`と`NOTICE.toolchain`を使用中のNDKから取得し、APKの`assets/licenses`へ含めます。
これらの文書が不足する場合は事前検査または配布物検査で失敗します。NDK・Gradleでの実ビルド確認は未完了です。

SDL3は3.4以上が必要です。依存ライブラリは自動取得しません。
デスクトップでは既存のSDL3 CMake package、または`SDL_SOURCE_DIRECTORY`で既存ソースを渡します。
AndroidではSDLActivityが使う共有ライブラリとActivity／Gradleの生成設定を実装していますが、APKのビルド・実行は未検証です。
Steam Deck／Android実機の入力・音声・復帰は未検証です。
標準のProggyCleanフォントは日本語表示を保証しません。必要なグリフを含むTTFの指定が必要です。
Portable APIの制約は残るため、既存Windowsランタイムとの全機能一致も未完了です。
[SDL3画像読み込み](https://wiki.libsdl.org/SDL3/SDL_LoadSurface_IO)、[SDL3 Android構成](https://wiki.libsdl.org/SDL3/README-android)

`tests/native`は実際のSDLゲームとしてSceneを読み込み、SDLのキー・タッチ変換、解像度変更後を含むUI・3Dメッシュの描画ピクセル、保存を検査します。
`tests/RunNativeTests.py`はそのゲームを2回起動し、保存値とUTF-8文字列が再起動後も残ることを検査します。
検査コードを追加しただけでは、検査に合格したことを意味しません。
検証時は許可済みのビルド先と、既存の空の保存先を明示してください。

```text
cmake -S tests/native -B <許可済みのビルド先> -DLAMAPON_SDL_SOURCE_DIRECTORY=<既存のSDLソース>
cmake --build <許可済みのビルド先> --config Release --parallel 2
python -B tests/RunNativeTests.py --game <NativeSmokeの絶対パス> --data-dir <許可済みの空の保存先>
```

`--probe`は`--data-dir`を必須にし、検証用ゲームが既定のアプリ保存先を作らないようにしています。
テストランナーはフォルダーを作成・削除しません。ゲームが保存先に`values.json`を作成し、置換時に`values.pending`を使用します。
デスクトップのビルドでは同梱依存と標準フォントのライセンスを`licenses`へ配置します。
SDLをCMake packageから使う場合は、配布元のライセンスを`SDL_LICENSE_FILE`へ指定してください。

## ネイティブ出力の診断とビルド設定生成

`tools/export_native.py`は`export.native`の設定を読み、既存Portable APIの契約、Scene、
モデル、アセット形式、参照ファイル、入力割り当てを検査します。
既定の動作は読み取りだけです。SDL／NDKやコンパイラーを取得・起動しません。
モデルのPNG／JPEG埋め込み画像とbase64 data URIを受け付けます。
画像のbufferView・buffer番号・宣言された範囲・MIME・base64を検査し、不正な参照や未対応形式を拒否します。
URI画像のヘッダーも確認しますが、静的な検査だけでは画像の完全なデコードや実行を保証しません。
Linux／Android向けではWindows上でもディレクトリの実際の名前を調べ、ソース、起動シーン、
Scene／Materialのアセット参照、glTFの外部参照の大文字小文字の違いを出力前に拒否します。
この検査は静的に見つかる参照が対象です。ゲームが実行時に組み立てるパスは実機でも確認してください。

```text
python -B tools/export_native.py --project <既存のproject.json> --platform linux
```

対象は`windows`、`linux`、`android`です。正常終了はビルド・実行の成功を示しません。
静的な診断を通った場合も、結果の`verified`はfalseです。
`export.native`には`sources`、`assetDirectory`、`assetIncludePaths`、`scenePath`を指定できます。
省略時はプロジェクトの`assets/scripts`と既定のアセットフォルダーを使います。
Web用の変換済みアセットや設定を暗黙に流用しません。
`assets/packages/*/package.json`のPortable用`nativeVariants`は、Windows x86-64、
Linux x86-64、Android arm64-v8a／x86-64ごとに解決します。ヘッダー・静的／共有ライブラリ・
ランタイム・任意のライセンスファイルを生成設定へ結び、Windows既存`native`は互換入力として扱います。
未定義ABI、パス逸脱、形式違い、同梱名の衝突は生成前に拒否します。
この経路でOS別SDKを使った実ビルドはまだ行っていません。
エディターでは「ゲームをエクスポート」ダイアログの「Linux／Steam Deck（ビルド設定）」または
「Android（ビルド設定）」を選び、新しい出力先とPython実行ファイルを指定して「ビルド設定を生成」を実行します。
LinuxでWSLビルドを使う場合は同じ画面でチェックを入れ、既存WSLディストリビューションとWindowsから参照できるSDL3ソースを指定します。
Androidは「Android（debug APK）」で既存SDK・NDK・JDK・Gradle・SDL3を指定します。生成処理のログと互換性エラーをダイアログへ表示します。
既存のWindows EXEとWeb HTMLの出力は引き続き別の選択肢です。
SDKの同梱設定にはネイティブ実装ソース、出力ツール、フォントと依存ライセンスを追加しています。
SDKを実際にインストールした配置から、共有SDL版WindowsゲームのReleaseビルドと2回の起動を検証しました。
同じSDKでLinux／Androidの設定生成も通りましたが、対象OSでのビルド・実行は未検証です。
CLIの標準出力・標準エラーはUTF-8に統一しました。WindowsのCP932／CP1252環境でも診断JSONと
日本語・絵文字を含むエラーが読めることを確認し、エディター用コマンドの成功／失敗メッセージも検証しました。
関連Pythonテストはネイティブ62件、Web出力45件が通っています。

以下は新しい出力フォルダーにビルド設定を作成する操作です。保存先の作成許可が必要な環境では実行前に許可を得てください。

```text
python -B tools/export_native.py --project <既存のproject.json> --platform linux --generate-build-project <許可済みの新規または空の出力先>
```

生成先にはCMake設定、入力割り当てJSON、診断結果、ビルド手順だけを置きます。
エンジンやプロジェクトを複製せず、実行可能なゲームや独立したソース配布物は生成しません。
別のOSでは既存エンジン・プロジェクトの場所を`LAMAPON_ENGINE_ROOT`と`LAMAPON_PROJECT_ROOT`で指定し直す必要があります。
LinuxのバイナリにはLinuxのビルド環境が必要です。AndroidはNDKライブラリとGradleアプリの生成設定を追加していますが、APK生成の実行確認は未完了です。

Windowsでは`tests/native/ExportSmoke.cpp`の実ソースとScene、メモリー上のプロジェクト設定を使い、
このツールが生成したCMake設定からReleaseビルド・ゲーム起動を確認しました。
2回のプロセス起動で日本語の保存値とカウンターの継続、入力設定・アセット・依存ライセンスの配置も検査済みです。
この結果は検証用のPortableゲームに限られ、個別プロジェクトの診断JSONは引き続き`verified: false`を返します。
この設定生成ツールのテストは既存Sceneとメモリー上の設定を使い、一時ファイルを作成しません。
ネイティブ描画・音声・入力の実行テストは別途必要です。

### Androidアプリの生成設定

`--platform android --generate-build-project <出力先>`は`android`配下にActivity、Manifest、Gradle設定を生成します。
ゲームのソースとエンジンは参照し、SDLのJava側もビルド時に同じ既存SDLソースから読みます。
Gradle WrapperやSDKは取得しません。
選択アセット、入力割り当て、標準フォント、ライセンスをGradleのビルド先へ配置し、APKへ同梱する設定です。
Activityは`getFilesDir()`の保存先を渡し、ManifestはOpenGL ES 3.0を要求します。

```json
{
  "export": {
    "native": {
      "android": {
        "applicationId": "com.example.mygame",
        "minSdk": 26,
        "targetSdk": 36,
        "compileSdk": 36,
        "abis": ["arm64-v8a", "x86_64"],
        "versionCode": 1,
        "versionName": "1.0"
      }
    }
  }
}
```

設定を省略するとAPI 26以上、compile／target API 36、両ABIを使い、プロジェクトのフォルダー名からアプリIDを作ります。
配布前には固有のアプリIDを指定してください。SDK範囲、ABI、バージョン番号、アプリIDの形式を生成前に検査します。
生成設定はAGP 9.4.0、Gradle 9.6.0以上、JDK 17、NDK 30.0.16248370、CMake 3.31.6を使用します。
AGPの互換性とNDKの16KBページ対応の仕様に基づく設定であり、APKの16KB整列や端末動作を実測したものではありません。
[AGP 9.4の互換性](https://developer.android.com/build/releases/agp-9-4-0-release-notes)、[16KBページ対応](https://developer.android.com/guide/practices/page-sizes)

既存のSDKとGradleを準備し、ダウンロード・ビルド・キャッシュの保存先が許可された環境でのみ実行してください。
Gradleビルドはプラグイン依存を取得する場合があります。生成設定では`android.builder.sdkDownload=false`を指定し、SDKの自動取得を無効にします。
APKビルドツールも同じ設定を起動引数に付けます。SDK・NDK・CMake・build-toolsは先に用意してください。
SDK Managerを使う場合は、保存先を確認してから次のパッケージを導入します（初回はライセンス同意も必要です）。

```text
sdkmanager --sdk_root=<既存SDKのルート> "platforms;android-36" "build-tools;36.0.0" "ndk;30.0.16248370" "cmake;3.31.6"
sdkmanager --sdk_root=<既存SDKのルート> --licenses
```

```text
gradle --no-daemon -PlamaponSdlRoot=<既存SDLソース> :app:assembleDebug
```

`android`を作業フォルダーに使います。別のソース配置では`-PlamaponEngineRoot`と`-PlamaponProjectRoot`も指定します。
debug APKの予定出力先は`app/build/outputs/apk/debug/app-debug.apk`です。
releaseは未署名です。ストア配布用の署名設定、APKのビルド確認、実機テストは未完了です。
エディターからのdebugビルド起動は接続済みですが、実環境でのAPKビルドはまだ確認していません。
メモリー上の生成内容のテストはManifestのXML、Activityの保存先、ABI・SDL共有ライブラリ、アセット配置を確認します。
このテストはGradleやJavaコンパイラーを実行しません。

## 共通タスク

### 生成設定から実行ファイルをビルドする

`tools/build_native.py`は、`export_native.py`が作成した`native-build-project.json`とCMake設定を使います。
既存のツールを指定し、作成が許可されたビルド先へ出力してください。SDKは自動インストールしません。
選択した素材が`lamapon-default-font.ttf`／`lamapon-input-actions.json`と同じ配置先を使う場合は、
設定生成前に`reserved-native-asset`で拒否します。素材の名前を変更するか、出力対象から除外してください。
同名のディレクトリ配下にある素材も対象にし、Windowsでは大文字・小文字を区別せずに検査します。
この検査はWindows／Linux／Android共通で、出力しない素材は対象外です。
新しい衝突・対象外・大文字小文字の検査を含むネイティブ出力関連Pythonテスト61件が通っています。
Windows／Linuxの実行ファイルは、それぞれの対象OSでビルドします。

```text
python -B tools/build_native.py --project-directory <生成設定フォルダー> --build-directory <ビルド先> --sdl-source-directory <既存SDLソース>
```

CMakeがPATHにない場合は`--cmake`で実行ファイルを指定します。
既存のSDL CMakeパッケージを使う場合は、SDLソースの代わりに`--sdl-cmake-package`と`--sdl-license-file`を指定します。
別のマシンへ生成設定を渡す場合は`--engine-root`と`--game-root`で既存ソースの場所を指定します。
ビルド先は空のフォルダー、または同じ生成設定に属する既存ビルドに限定し、ソースやアセットとの重複を拒否します。

Windows上で生成設定からRelease実行ファイルをビルドし、実際に2回起動しました。
起動シーン、日本語の画像ファイル名、日本語の保存値と再起動後のカウンター保持を確認済みです。
これはPortableランタイムの検証で、既存D3D版の回帰検証やLinux／Androidの実行確認ではありません。

Android用設定では、さらに`--android-sdk`、`--java-home`、`--gradle-home`と`--sdl-source-directory`を指定します。
この経路はdebug APKを対象とし、APKビルド自体はまだ未検証です。
Gradle依存の取得は既定でオフラインです。取得が許可された環境では`--allow-downloads`を指定します。
NDK・SDK・CMake・build-toolsは先に準備する必要があります。
アプリの生成物はビルド先の`app`、CMakeの生成物はその隣の`native`へ分けて出力します。
Gradleキャッシュと一時ファイルの保存先もビルド先へ指定しますが、実際のGradle実行による保存先の確認は未完了です。

ビルド先に`native-build.log`と`native-build-result.json`を記録します。
実行ファイルでは起動シーン・入力設定・フォント・ライセンスの配置を検査します。
デスクトップの素材配置は毎回のビルドで実行し、素材だけの変更も反映します。
生成設定のversion 2には素材ディレクトリと出力対象を記録し、配布物のファイル一覧と
各素材・標準フォント・入力設定のSHA-256を元ファイルと照合します。
対象外の古い素材が残った場合は成功扱いせず、新しい空のビルド先を案内します。既存ファイルの自動削除は行いません。
旧version 1の生成設定は再生成が必要です。
WindowsのMSVC出力には、CMakeで検出したVisual Studioの再配布用ReleaseランタイムDLLを
実行ファイルの横へ配置します。共有SDL3を使う場合もSDL3のDLLを配置する設定です。
`tools/native_windows.py`で実行ファイルのPEを読み、通常・遅延ロードの依存DLLを再帰的に検査します。
Windows 10以降のシステムDLL／APIセット以外は同梱を必須とし、ファイル構造、CPU種別の不一致、
欠けた間接依存、Debug版Visual C++ランタイムを拒否します。Debugランタイムを必要とするWindows出力はReleaseで再ビルドしてください。
ステージされたDLLは静的／遅延依存だけでなく全件を検査し、パッケージの`runtimeFiles`にある動的ロードDLLも存在とCPU種別を確認します。
Visual C++依存には配布に関する通知文を含めます。
MicrosoftのランタイムDLLは適用されるMicrosoftの使用条件に従うもので、LamaPonのMITライセンスで再許諾されません。
アプリと同梱したDLLの更新はゲームの配布更新時に行います。
Microsoftが更新しやすい中央配置を推奨する理由と配布条件は、
[公式の配布手順](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)と
[配置方法の選択](https://learn.microsoft.com/en-us/cpp/windows/choosing-a-deployment-method)を参照してください。
Windowsの生成ゲームを実際に起動し、MSVCP140／VCRUNTIME140／VCRUNTIME140_1の3 DLLが
ゲームの出力フォルダーからロードされることと、日本語セーブの継続を確認しました。
PE検査はメモリー上のデータによる不正構造・欠落・遅延依存・CPU不一致のテストも含み、ネイティブ出力関連Pythonテスト46件が通っています。
共有SDL 3.4.18のCMake packageでも実ビルドし、ゲームを2回起動しました。
SDL3とVisual C++の4 DLLがゲームの出力フォルダーからロードされること、日本語セーブと起動回数が継続することを確認しています。
開発環境のない別PCでの動作は未検証です。
`windowsChecks`には同梱DLL・システムDLL・CPU種別を記録し、`cleanMachineVerified`は`false`のままにします。
Windowsの実ビルドで、入力設定だけの編集が反映されること、古い内容・対象外の画像が検査で拒否されることを確認しました。
APKの検査でも、ZIPから直接読み取って同じ素材一覧とSHA-256を照合する実装を追加しました。
メモリ上のZIPを使った欠落・対象外・古い素材の拒否テストは通っていますが、実APKのビルドと端末実行は未検証です。
関連Pythonテスト37件と、変更後のNativeSmokeによる描画・GLコンテキスト再作成・復帰・セーブの2回起動テストも通っています。
APKではそれらに加え、Activity、指定ABIの共有ライブラリと署名を検査します。
共有ライブラリのELFヘッダーを読み、arm64-v8a／x86_64の取り違え、欠けたヘッダー、
16KB未満のLOADセグメント整列、RELRO領域の末尾の不整列を拒否します。
同梱するすべての共有ライブラリを対象とし、RELROのファイル範囲と`p_filesz <= p_memsz`も検査します。圧縮されたライブラリと重複したZIPエントリーも拒否します。
続いてSDKの`zipalign -c -P 16 -v 4`と`apksigner verify`を実行します。
これらは[Androidの16KBページ対応手順](https://developer.android.com/guide/practices/page-sizes)に基づく配布物検査です。
検査コードはメモリー上のELF／ZIPデータでテストしました。実際のNDK製ライブラリ、APK、16KB端末での検証はまだ未完了です。
`androidChecks`にはライブラリごとの整列値と署名・ZIP検査結果を記録し、`pageSizeRuntimeVerified`は`false`のままにします。
成功記録の`executed`は`false`です。起動・実機確認は別途行ってください。
ビルド開始後にコマンドや配置検査が失敗した場合は`built: false`を残し、以前の成功を流用しません。
ツールや設定の事前検査で失敗した場合はビルド先を書き換えず、コマンドの終了コードとエラーメッセージで通知します。

Portableの`Script::LoadText`は、保存済みの空文字列をそのまま返し、未登録の項目だけ既定値にします。
`Script::SaveText`／`SaveInteger`は保存失敗を例外で通知します。呼び出し元が捕捉してUIへ通知・再試行できます。
WebではlocalStorageへの書き込みが拒否された場合、成功のDOM通知を更新せず、`lamaponSaveError`へ失敗を記録します。
ネイティブでは保留ファイルの書き込み・公開が成功した後だけメモリー上の保存値を変更します。
保存ファイルの読み込みに失敗した場合も、正常な設定の再読み込みまで保存を拒否し、既存ファイルを上書きしません。
Windowsの実ファイルで、書き込み失敗時の旧データ保持、再保存、空文字列、壊れた文書を読んだ後の上書き拒否を検証しました。
変更後のNativeSmokeも2回起動し、描画・入力・GL復旧・日本語セーブの再起動保持が通っています。
Chrome上の実Wasmでは、空文字列／未登録の区別とlocalStorageの保存拒否を検証し、捕捉後もゲームが動くことを確認しました。
共有SDLを使う生成済みWindowsゲームも更新後のソースで再ビルドし、2回起動して既存の日本語セーブとカウンターの継続を確認しました。
Linux CIへ同じ保存検証を追加しましたが、Linux／Androidでの実行と電源断への耐久性はまだ未確認です。

WebセーブのChrome回帰テストは、同じ一時プロファイルとオリジンでWasmゲームを二度起動し、
一度目の値が二度目に読み込めることを検証します。ブラウザーによるサイトデータ消去・容量制限・
プライベートブラウズ時の挙動は別の制約として[Web出力の説明](web-export.md)に記載しています。

Portable Sceneのランタイム復元項目を、シリアライザーの出力とローダーが参照するcomponent／material JSON accessorへ照合するテストを追加しました。ParticleSystemの`previewInEditor`はゲームの動作に影響しないため、ランタイム未対応項目とは分けてエディター専用に分類します。

- [x] CORE-01: 描画・エディターを必要としない共通基盤のCMakeターゲット。
- [x] CORE-02: UTF-16／UTF-32、パス、ジョブ処理、バージョンのテスト。Windowsで実行済み。
- [x] CORE-03: Windows／Linuxテスト、AndroidクロスコンパイルのCI構成追加（CI実行結果は未確認）。
- [x] CORE-04: SDLウィンドウ、入力、音声、ライフサイクルの共通アダプターを実装（Linux／Android実行は未検証）。
- [x] CORE-05: Portable描画をD3D11／HWND／DirectXMathから分離し、OpenGL／OpenGL ESバックエンドを実装（対象OS実行は未検証）。
- [ ] CORE-06: Scene・Prefab・スクリプト・UI・物理の共通Portable契約と、未対応機能検査の網羅性確認。
- [x] CORE-07: OS別の保存先、セーブファイルの永続化・置換、Androidアプリ専用領域を実装（Linux／Android実行は未検証）。
- [x] CORE-08: SDK／パッケージ／アセット／Shaderの対象OSとCPU別依存解決（Linux／Androidの実ビルドは未検証）。

Portable WebとSDLネイティブ出力は同じScene／Scriptランタイムと互換性検査を使います。
対応・近似・拒否するコンポーネントは[Portableの対応表](web-export.md)に記載し、
未登録API、未登録スクリプト、未対応Sceneコンポーネントを出力前に拒否します。
`Script::Instantiate`／`Scene::InstantiatePrefab`は、assets内の文字列リテラルPrefabを実行時に生成し、
Scene固有のIDを割り当て、親子階層とScriptの初期ライフサイクルを保ちます。
Prefab参照先のcomponent・Script・素材も出力互換性検査へ含め、動的変数パスは拒否します。
Portableの`RequestLoad`／`RequestReload`は次の更新境界で主Sceneを切り替え、失敗時に現在のSceneを維持します。RequestLoad先は文字列リテラルのScene pathを出力前に検査し、Sceneの階層・Script・component・素材も共通診断へ合流させます。
追加Sceneの読み込み／破棄、非同期読み込みと遷移、保持Object、Prefab override・入れ子Prefab・外部参照の意味差と、Script API全体の網羅性は
引き続き確認が必要です。
Portable公開ヘッダーの最上位型はWeb出力テストでモジュール分類表への網羅性を確認し、
新しい型が互換性検査の対象から漏れないようにしています。DirectX値型と描画バックエンド内部型は対象外として明記しています。
互換性検査とWebモジュール推定は、`LamaPon::Type`、`namespace LP = LamaPon;`の`LP::Type`、
およびnamespace import後の公開型名を追跡し、必要なmoduleの不足と未知の修飾API型を検出します。
`using LamaPon::Type;`とnamespace aliasからの単独importも追跡し、必要moduleと未知型を検査します。
namespace修飾子`::`の前後に空白があるC++表記もAPI・alias・動的スクリプト検査で扱います。
動的な`NativeScriptComponent`追加も同じnamespace表記を認識し、未登録スクリプトを出力前に拒否します。
既定の`LAMAPON_SCRIPT(Type)`も`Game.Type`の登録として解決し、Scene参照と動的追加を共通回帰テストで確認します。
WindowsのPortable Export Smokeにはこの標準マクロを使う追加プローブを置き、起動時に`Game.Type`からFactoryを生成できることを検査します。
Releaseビルド後の隠し起動2回では、共有の名前付きSceneスクリプトと日本語セーブの継続も確認しました。
`Scene::Raycast`と`Scene::WebAudio`のようにcore型経由で呼ぶ物理・音声機能も必要moduleとして推定・検査します。
コメントと文字列リテラルはAPI使用として数えず、例示コードで余分なmoduleを要求しません。
namespace import／alias経由で3Dレンダラーが見落とされるケースと未知型を、共有契約の回帰テストで確認しています。
Portableで実装していない`Scene::RaycastAll`、Sphere／Box／Capsule castとOverlap queryも、Native／Web共通の出力前検査で拒否します。
PythonのJSON読込が受け入れる`NaN`／`Infinity`と範囲外の指数値は、共有契約検査でScene・Material・Animationから拒否します。壊れたMaterial JSONと起動Sceneの不正UTF-8も出力前に位置を示して診断します。C++ランタイムの厳格なJSON読込との差による、対象OS別の出力可否の食い違いを防ぎます。

Portable Sceneのモジュール分類とC++復元分岐も照合しました。分類済みcomponentのうち復元処理がないものは`MeshCollider3D`だけで、WebとWindows／Linux／Androidのネイティブ出力検査が明示的にrejectします。この対応をWeb／ネイティブの回帰テストで固定しました。Sceneシリアライザーが保存する各Portable component設定は、ランタイムが復元する値かPortable非対応として診断する値のどちらかへ分類し、分類漏れを回帰テストで検出します。Script APIとcomponent設定の意味的な互換性監査は続けます。

Scene環境設定とPortableローダーも照合し、有効でも描画されないポスト処理が複数ありました。AO、TAA、SSR、ベイクGI、ボリューメトリックライト、ブルーム、スクリーンアウトライン、レンズフレア、被写界深度、モーションブラー、自動露出、カラーグレーディングを共有互換性診断で警告します。さらに、Portableが使わない霧密度と、簡易グラデーション描画では反映しない空のキューブマップ等も警告します。環境設定JSONのオブジェクト・型・有限な32ビット浮動小数も出力前に検査し、C++ローダーの変換例外を防ぎます。Sceneシリアライザーの全環境設定キーが描画または診断のどちらかに分類されるテストと、Web／ネイティブ共通テストが通っています。これはCORE-06の監査の一部で、全Script APIと全component設定の完了を意味しません。

Portable `Scene::Load`では、Scene値の型変換例外をロード失敗へ変換し、追加途中のGameObjectとIDを巻き戻します。`Camera.nearPlane`へ文字列を入れたシーンをNativeSmokeとPortableSmokeの両方から読み込み、実行中のSceneを保ったまま拒否することをWindowsネイティブとChrome WebGL2で確認しました。さらに、Sceneシリアライザーが保存するPortable component設定を、ランタイム復元値・型検査済みの非対応機能・全面拒否componentに分類し、C++シリアライザーとPortableローダーの項目一覧を照合する回帰テストを追加しました。Component・インライン材質・外部材質の型、整数幅、ベクトル、Objectの親・Transform、SpriteAnimatorのclip設定を出力前に検査します。保存された非対応描画・音声・UI・物理設定の代表ケースが期待する警告／拒否診断に結び付くことも検査します。Web出力テスト68件とNative出力テスト55件が通り、nativeのWindows／Linux／Android各ターゲットも同じ不正シーンを拒否します。Scene／Prefabの挙動差、Script API全体、UI・物理の意味的な互換性監査は残るため、CORE-06は継続します。

Portable公開ヘッダーから利用できるReactiveイベント型`Observable`、`Subscription`、`CompositeSubscription`をcore APIとして分類しました。未登録型として誤って拒否していた購読コードが、Native／Webの出力互換性検査とモジュール推定を通ることを確認しました。また、Sceneの`RaycastAll`、Sphere／Box／Capsule cast、Overlap queryはNative Sceneにだけ存在しPortableでは実装していないため、3対象OSを含むNative出力とWeb出力の共通事前検査で拒否します。変更後のNative出力ツールテスト58件、Web出力ツールテスト76件が通りました。Script API全体と追加Scene／Prefabの意味差は引き続き監査します。

Windows版`Script`にだけあるCoroutine／タイマー、ウィンドウサイズ、オンライン／ネットワーク、数値保存API、GameObject生成APIの呼び出しも監査しました。Portable Scriptクラス内の直接呼び出しを調べ、Portableで提供されないメソッドをNative／Web共通の出力前検査で診断します。namespace alias、複数継承、`this->`呼び出しに対応し、非Scriptクラス・コメント・文字列と同名の独自ヘルパーは誤検出しないテストを追加しました。Windows専用のカメラ切替・環境設定・アセット読み込み・Scene保存・物理統計も検査対象に加えました。Script派生クラスのalias経由Prefab生成も固定パスを要求し、参照先の検査漏れを防ぎます。Windows／Portable Scriptヘッダーを比較し、Windows側にだけある公開メソッドがすべて拒否registryへ分類済みであることを検査します。

Windows `Scene`とPortable `Scene`の公開メソッドもヘッダー同士を比較し、未実装の全メソッドを拒否registryか型付き検査へ分類しました。引数に`Scene&`を取る関数の`Clear()`は拒否し、`EventBus::Clear()`のような同名APIは許可することもNative／Webで検証します。これでScene／Scriptの直接呼び出し一覧は網羅しましたが、関数ポインター・テンプレート経由の呼び出し、SceneとPrefabの意味差、component設定の動作差は引き続きCORE-06で確認します。

Windows `GameObject`の公開メソッドもPortableヘッダーと照合し、`SetName`、`FindChild`、`WorldMatrix`、有効なカリング設定の問い合わせ、component順序変更をPortableへ追加しました。永続Object・Prefab編集メタデータ・ワールド変換変更・Windows側の更新／描画callbackはPortableの契約外として分類し、Scriptの`Owner()`、型付きGameObject変数、修飾メソッド参照からの使用をNative／Web出力前に拒否します。Windowsの生成ゲームを実際に2回起動し、名前変更・階層検索・カリング値・World行列・component順序と日本語セーブの継続を検査しました。これはGameObjectの呼び出し分類と基本動作の確認で、CORE-06のPrefab／Scene動作差、関数ポインターやテンプレート経由、全component設定・物理・UIの意味差を解決するものではありません。
更新後はNative各出力ツールテスト124件、Web出力テスト83件、Android画面検査4件が通り、`git diff --check`も成功しました。

Windowsの`TextRendererComponent`とPortable版を照合し、Portable側で不足していた既定コンストラクター引数、本文・書体・色・寸法・折返し・揃えの取得／設定APIを追加しました。文字サイズの下限とレイアウト寸法の制約もWindows版と同じにし、PortableSmokeと生成ネイティブゲームに既定値・更新値・境界値の検査を追加しました。生成ネイティブゲームをReleaseで再ビルドして2回起動し、API検査とセーブ継続を確認しました。ヘッダー照合テストも通過しています。PortableSmokeを含むWeb C++ビルドは、このPCにEmscripten SDKがないため今回未実行です。これにより同コンポーネントのScript利用差は縮小しますが、CORE-06の全component・物理・UI動作の監査は継続します。

Portable `Component`基底にも`GetTransform`、`IsActiveAndEnabled`、`ScriptInstance`を追加し、Windows版の共通アクセサーに揃えました。Portable NativeScriptは基底ポインターから実体を取得でき、親GameObjectの無効化／再有効化に合わせたcomponent状態もWeb／Nativeスモークへ追加しました。生成ネイティブゲームを再ビルドして2回起動し、Transform参照・有効状態・スクリプト取得とセーブ継続を確認しました。Native出力API照合テストは124件です。Component派生型の個別APIと描画・物理の意味差は引き続きCORE-06で監査します。

Scene直下に保存されるGameObject項目も監査しました。Portableローダーでタグを復元し、Windowsランタイムと同じ`CompareTag`、`Scene::FindGameObjectByTag`／`FindGameObjectsByTag`、Scriptのタグ検索を追加しました。親階層からコンポーネントを検索する`GetComponentInParent`も移植し、Scriptのコンポーネント・Transform便利APIもPortable側へ揃えました。無効な祖先を除外する既定動作と`includeInactive`をNative／Webスモークで確認しました。旧形式のGameObject直下の`alwaysVisible`／`cullingMargin`は`RenderCulling`へ移して値を保持しますが、Portable基本レンダラーではカリングを実行しないため警告し、`persistent`／`persistenceKey`もランタイムで適用されないことを出力前に知らせます。GameObject項目の型違いも拒否します。Web／Native出力テスト、Windows NativeSmoke、Chrome WebGL2 PortableSmokeで確認しました。

今回、Portableの`GameObject::SetParent`にもWindows版と同じ自己参照・循環・別Scene親の拒否と子一覧の追跡を加えました。`GetComponentInChildren`、`GetComponentsInChildren`、`GetComponentsInParent`、`GetScriptInChildren`およびScriptからの対応ラッパーも追加し、深さ優先の検索順と`includeInactive`をWeb／ネイティブで確認しました。子オブジェクトだけの破棄後に親の子一覧へ無効ポインターが残る不具合も修正し、PortableSmokeで検査しています。Windows NativeSmokeはプロセス再起動を含む2回実行、Chrome WebGL2はゲーム起動とブラウザー再起動後のセーブ保持まで通りました。Prefabの実行時生成、ライフサイクルコールバック、同一Sceneの遅延再読み込みを追加し、Web／Native共通診断がRequestLoad先のSceneも走査するようにしました。追加Sceneの読み込み・破棄、UI・物理設定は引き続き監査します。

Windows Script APIの`LateUpdate`もPortableへ追加し、全Script／コンポーネント更新の後に呼ぶようにしました。NativeSmokeとPortableSmokeは次フレームのUpdateから前フレームのLateUpdate完了を検証します。Portableの`CollisionEvent`に相手GameObject参照を加え、通常衝突とトリガーのEnter／Stay／Exitを個別に配送します。直前接触の種別を保持してExitを通知する処理を追加し、両方のスモークでイベント順・相手名・trigger属性を確認しました。さらにPortable Scriptへ`Awake`、`OnEnable`、`OnDisable`、`OnDestroy`を追加し、階層／component有効状態の切替、`Start`の一度だけの実行、破棄前通知を検証しました。Prefabの実行時生成と初期化順はWebGL2とWindowsネイティブで検証し、同一Sceneの遅延再読み込みはWindowsネイティブで実行しました。Chrome WebGL2のセーブ再起動テストとWindows NativeSmokeのランタイム検査も通りました。追加Sceneの読み込み・破棄、UI・物理設定の意味差は引き続き監査します。

CameraComponentのWindows／Portable公開APIを照合し、Portableに欠けていた視野角・近遠クリップ距離のgetterを追加しました。Windows側にも近遠クリップ距離setterを揃え、Portableの既定値とScene JSONに値がない場合の復元値をWindowsと統一しました。Native／Webのスモークにコンストラクター、getter／setter、Scene読込時の既定値検査を追加しました。Windowsネイティブ出力で実行確認し、Web C++ビルドはEmscripten SDKがないため未実行です。

同じ監査で、BoxCollider3DのPortableソルバーが回転形状・摩擦・反発・材質合成・連続衝突を再現しないことを確認しました。ブラウザー専用だった警告をPortable共通の診断へ改め、Windows／Linux／Androidの各出力でも制約を表示する回帰テストを追加しました。

今回、Windows Editor Release全体を再ビルドし、D3D11／D3D12 WARP、バックエンド分離、エクスポートダイアログの4件が通りました。GameExporterはCTestの深い作業パスではShaderキャッシュ書込みに失敗しましたが、許可済みの短い`test-output/platform-core`作業場所から同じ実行ファイルを起動すると合格しました。診断メッセージに失敗時の警告一覧も加えたため、長い作業パスの問題を特定できるようになりました。

ProjectSettingsのphysics設定もPortableゲームでは読まれず、独自重力・固定更新間隔・無効衝突レイヤー・離散衝突速度制限が無視されることを確認しました。これらの有効なカスタム設定をWeb・Windows／Linux／Android共通で警告し、標準設定では不要な警告を出さない回帰を加えました。
局所光源の8灯制限もPortable共通の警告へ統一し、ネイティブの各対象でWeb専用の文言が出ないことを検証しました。

SceneシリアライザーとPortableローダーを照合し、`AudioSource.bus`は保存される一方、Portable音声バックエンドには個別バスのミキサーがないことを確認しました。Portable出力ではバス値を保持し、既定以外の指定をWindows／Linux／AndroidとWebに共通警告し、範囲外・型違いは出力前に拒否します。Script APIと全component設定の監査は継続中です。

Windows Sceneが公開する3D physics queryのうち、Portableで対応している`Scene::Raycast`以外のRaycastAll・SphereCast・BoxCast・CapsuleCast・OverlapBox・OverlapSphere・OverlapCapsuleをPortableランタイムと照合しました。これらを呼ぶコードはNative／Web共通の事前検査で拒否し、Windows・Linux・Android各出力に対して同じ回帰検査を行います。

`Rigidbody.interpolate`もSceneへ保存されますが、Portable描画の`InterpolatedWorldMatrix(alpha)`はalphaを反映せず、最新Transformを直接描画します。有効時の差をWindows／Linux／AndroidとWebで共通警告し、true／false以外の値は出力前に拒否します。Script APIと全component設定の監査は継続中です。

PortableパッケージはWindows x86-64／Linux x86-64／Android arm64-v8a・x86-64別に依存を解決し、選択先に合わないライブラリ形式、欠けたABI、出力対象外の変種、危険なパス、DLL／共有ライブラリ名とAPKライセンスの衝突を拒否します。アセットは対象の大文字小文字まで検査し、PE／ELF／APKの検査でもCPUと同梱範囲を確認します。Shaderは共通PortableのGL／GLES経路に限定し、独自HLSL等は出力前に拒否します。これは検査・解決層の完了で、Linux／Androidターゲットの実ビルド成功を意味しません。

## Windows

- [x] WIN-01: 共通基盤のReleaseビルドとCTest。
- [x] WIN-02: 共通アダプター導入後も、既存のD3D11／D3D12とGame Module／出力が動くことを確認（Windows・WARP、下記の範囲）。
- [x] WIN-03: エディターから外部のLinux／Androidビルド環境へ渡す設定生成・診断と、既存WSL上のLinux実ビルド経路を実装（実WSL／Android SDKでの実行は未検証）。

既存のWindowsゲーム出力は移植中も維持します。

Windows上でVisual Studio 2022のRelease全体ビルドを実行し、エディター・CLI・ゲームを生成しました。
既存D3D12シェーダーの単一文字列がMSVCの長さ制限に達したため、隣接するraw文字列へ分割しました。
変更前とのHLSL本文の一致を確認し、内容は変更していません。
Releaseビルド後、登録済みCTest 78件を短縮ドライブR:経由で全件実行し、すべて通過しました。通常の深い作業パスではWindowsの既定パス長制限により6件が失敗しましたが、R:経由の再実行で解消しました。エディター、Web／Native出力、セーブ・オンライン処理、Game Moduleのビルドとロード、D3D11／D3D12のWARP描画を含みます。
素材を相対パスで参照するテストは、ゲーム実行ファイル横の`assets`を作業場所にして検証しています。

出力ダイアログはWindows・Web・Linuxビルド設定・Androidビルド設定・Android APKの5形式を描画しました。
Game Moduleは実際にビルド・ロードし、高速／通常モードの切り替えと未変更時のキャッシュ再利用も検証しました。
D3D11／D3D12の描画検証はWARPによるソフトウェア描画です。
物理GPU、開発環境のない別PC、Linux／Steam Deck、Android端末での動作確認を意味しません。
検証後、今回作成した`test-output/platform-core/editor-regression`の削除を試みましたが、
自動承認レビューがポリシーにより拒否し、削除は実行されませんでした。
許可済みの同フォルダー内で追加検証を行い、現在は約1,259 MiBの生成物が残っています。

`cmake --install`で配布SDKを生成し、そのSDK内のツール・移植用ソース・ヘッダー・依存ライセンスを
使って共有SDL版WindowsゲームをReleaseビルドしました。PE依存検査と8件の同梱ライセンス確認が通り、
ゲームを2回起動して日本語セーブの継続を確認しました。
追加で、埋め込みPNGを持つglTF／GLBを含むゲームをSDKから生成・ビルドしました。
2回の起動ログで両モデルの描画パーツ読み込みと日本語セーブの継続を確認しました。
この生成ゲームでは画面ピクセルを検査していません。描画ピクセルの確認は別のNativeSmokeとWebの検証です。
ハイフン・ドット・プラスを含むプロジェクト名から生成したCMakeターゲットを、
ビルドツールが拒否していた問題も修正しました。危険な区切り文字やパスを拒否する検査を含め、
関連Pythonテスト65件が通っています。
SDK内の出力ツール群のCLI読み込みと、Windows・Linux・Androidのビルド設定生成も確認しました。
Linux／Androidについては設定の生成までで、対象OSのバイナリやAPKの検証を意味しません。
ネイティブ出力の互換性診断には、Emscriptenの代わりに対象OSのCMake、またはAndroid SDK・NDKでの
ビルドと動作確認を案内します。互換性診断だけでは動作確認済みと記録しません。

## Linux／Steam Deck

- [ ] LINUX-01: Linuxで共通基盤CIの実行結果を確認。
- [x] LINUX-02: 共通ウィンドウ・入力・音声アダプターを実装（CORE-04。Linuxでの動作は未検証）。
- [x] LINUX-03: OpenGL描画バックエンドとShader経路を実装（CORE-05。Linuxでの動作は未検証）。
- [x] LINUX-04: 大文字小文字を区別するアセット参照、保存データ、共有ライブラリの配置／ロードを実装（Linux生成物は未検証）。
- [x] LINUX-05: WindowsエディターからWSLへ渡し、Linux ELF・アセット・ライセンス・共有ライブラリ依存を検査する実ビルド経路を実装（WSLでの実ビルドは未検証）。
- [ ] LINUX-06: 実際のScene・音声・入力・セーブ・終了をLinuxゲームで検証。
- [ ] LINUX-07: Windowsエディターから既存WSLへLinuxゲームをビルドし、生成物をLinux上で起動。
- [ ] DECK-01: Windows出力のProton動作を確認。Linuxネイティブ出力とは別に記録。
- [ ] DECK-02: コントローラーのみの操作、UI可読性、解像度、サスペンド／復帰を実機で確認。
- [x] DECK-03: Valve公式のSteam Runtime 4 SDKコンテナーでLinuxゲームをビルド・起動するCIを追加（CI未実行、Steam Deck実機は未検証）。

Windows上の共通基盤テストだけではLinux対応済みとしません。
Protonの動作確認だけでもLinuxネイティブ出力対応済みとしません。
Linuxの生成ゲームには、配布フォルダーを基準とする`$ORIGIN`の実行時検索パスを設定しました。
共有SDLを使う場合は、SDLのELFが要求するSONAMEで実体をゲームの横へコピーする設定です。
これらのLinux分岐は実Linuxでのビルド・移動後の起動検証が未完了です。
Ubuntu Linux CIには静的／共有SDLのビルドと、共有版の配布フォルダー移動後のロード・描画・復帰・セーブ検証を追加しました。
Steam Deck向けには、Valveが新規ネイティブゲームに推奨するSteam Runtime 4 SDKコンテナーでも、
`export_native.py`と`build_native.py`を通した実際のLinuxゲーム出力と、静的／共有SDLのビルドを行うCIを追加しました。
生成したゲームもXvfb／Mesa上で起動し、Scene・UTF-8画像・保存の再起動保持を検査します。
別途ネイティブ実行基盤のSmokeゲームで入力・復帰・音声・共有SDL移動後のロードを検査します。
これはSDKコンテナーのx86_64 ABI上の検証であり、Steamクライアント経由やDeck実機での互換性検証ではありません。
CIの実行結果はまだ確認していません。[Valve Steam Runtime](https://github.com/ValveSoftware/steam-runtime)
`tools/native_linux.py`はELFを実行せずに読み取り、CPU・実行時ローダー・エントリーポイント、
通常の共有ライブラリ依存とSONAMEを検査します。依存を再帰的に確認し、OSの基本ライブラリ以外が
同梱されていない場合や、同梱ライブラリを読む親の検索パスに`$ORIGIN`がない場合は出力を拒否します。
出力へステージされたすべての共有ライブラリも検査し、パッケージの`runtimeFiles`にある動的ロード用ELFの欠落やCPU・SONAME・間接依存の不一致を拒否します。
ファイル外を指すアドレス、絶対パスの依存、外へ逃げるリンク、CPU・SONAMEの不一致も拒否します。
Linuxビルド成功時には`linuxChecks`へ結果を記録します。
これはリンクに必要なシンボルやglibc／libstdc++のバージョン、SDLが動的に開く音声・画面の依存を保証しません。
`abiCompatibilityVerified`と`cleanMachineVerified`は`false`のままです。
特殊なaudit/filterによる追加ロードは未対応として拒否し、PIE実行ファイルのライブラリ混入も検出します。
メモリー上のELFと依存グラフのテスト11件と、依存検査失敗時にビルド成功を記録しない検査を含むPythonテスト58件はWindowsで通りました。
実Linuxの生成物への検査はCIへ接続しましたが、まだ実行結果を確認していません。
SDL以外の間接依存、glibc／libstdc++の最低バージョンとSteam Deck実機の互換性確認も残っています。

SDL入力ではタッチの機器IDと指IDの組を使い、異なる機器の同じ指IDを混同しないようにしました。
第2の機器での押下やキャンセルが、第1の機器のポインターと移動入力を解除しないことを
Windows上のNativeSmokeで検証しています。Steam Deckのタッチ画面・タッチパッドによる実機確認は未完了です。

## Android

- [ ] ANDROID-01: arm64-v8a／x86_64のdebug APK CIの結果を確認。
- [x] ANDROID-02: Activity／ネイティブ起動、アセット読み込み、アプリ専用のdataDir／cacheDirを接続（生成設定・ソース・契約テスト済み。APK実行は未検証）。
- [x] ANDROID-03: OpenGL ES描画、タッチ／ゲームパッド入力、SDL音声を共通ランタイムへ接続（APK実行は未検証）。
- [x] ANDROID-04: SDLアプリの背景・復帰、GLコンテキスト再作成、画面サイズ変更を実装（Android Emulator／実機は未検証）。
- [x] ANDROID-05: Windowsエディターからdebug APKを出力し、applicationId／minSdk／ABI／debug署名を設定（実APK生成は未検証。ストア用署名は対象外）。
- [ ] ANDROID-06: 実機でScene・描画・音声・入力・保存・復帰を検証。
- [x] ANDROID-07: x86_64 APKのAndroid 15・16KB Emulator起動、Scene描画・UIButtonタッチイベント・Activity復帰・画面サイズ変更・再起動保存を検査するCIを追加（未実行）。

NDKのライブラリがビルドできる状態と、APKを実行できる状態は別に管理します。

生成するActivityは`getFilesDir()`をセーブ先、`getCacheDir()`をキャッシュ先としてSDLへ渡し、
ネイティブ側でPathUtilsへ登録します。Gradleは選択素材をAPKの`assets/`へ収録します。
Portableの素材パスは`assets/...`に解決され、SDL 3.4.18の`SDL_IOFromFile`はAndroidパッケージ素材への
読み込みも処理します。生成設定、SDL側の読み込み経路、PathUtilsの絶対パス検査とUTF-8変換を確認しました。
APKビルド、Activity起動、APK内の実ファイル読み込みは未検証です。

SDLが配信する`SDL_SCANCODE_AC_BACK`を共通のEscape入力へ変換しました。
NativeSmokeでは押下・反復・解除と、UIフォーカスのキャンセル時に誤クリックが発生しないことを確認しています。
更新したWindows版の検証ゲームを2回起動し、描画・GLリソース復帰・再起動後のセーブも通りました。
配布SDKも更新し、通常のWindowsゲーム出力を再ビルドしました。同梱DLLの検査と2回の起動を通し、
既存セーブの起動カウンターが2から4へ進み、日本語の保存値も維持されることを確認しています。
これはSDLイベントを使う入力層の検証で、Androidの戻るジェスチャーやActivityの動作確認ではありません。
SDLの[タッチイベント構造](https://wiki.libsdl.org/SDL3/SDL_TouchFingerEvent)に合わせて機器IDと指IDを保持しています。

## Web

- [x] WEB-01: 共通入力層へ移行し、ChromeでWebGL1／WebGL2／Canvas2DのPortableテストを確認。
- [x] WEB-02: ブラウザー保存の永続化、失敗通知とOSネイティブのセーブ契約を整合（Chrome再起動テストと仕様説明を追加。ブラウザーによる消去・拒否は対象外）。
- [ ] WEB-03: モバイル実機でタッチ、リサイズ、音声解放、バックグラウンド復帰を確認。
- [x] WEB-04: 機能差を出力前の診断とドキュメントへ反映（拒否時はコンパイル前に停止し、単独のJSON診断も可能）。

Web出力は既存機能を持ちますが、ネイティブ版との全機能一致は未完了です。
ChromeのWebテストではCanvasのCSS拡大時のタッチ座標、タッチ開始・終了・取消を検査します。
このテストは合成DOMイベントによる入力層の検証で、スマートフォン実機の画面回転、音声解放、
バックグラウンド復帰を確認した結果ではありません。

配布SDKの通常プロジェクト出力でも、PortableのglTF／GLBをモデル変換ツールなしで同梱する経路を確認しました。
埋め込みPNGを持つglTFとGLBは元のバイト列を維持し、外部参照の欠落や名前の大文字小文字の不一致は拒否します。
既定の起動シーンから3D描画モジュールを推定する処理も修正しています。
Emscripten 6.0.9が日本語ファイル名を埋め込み用のアセンブラー記号へ使う問題には、
ASCIIの内部名で埋め込み、起動前に元のUTF-8パスへ戻す処理を追加しました。
配布SDKにこの補助ツールを含め、素材の変更を次回のビルドへ反映します。

SDKから単独HTMLを生成し、Chrome／SwiftShaderのWebGL2で実際に起動・撮影しました。
日本語名の白い画像を緑色へ着色したUIが14,400ピクセル、glTF／GLBのピンクと緑のモデルが
それぞれ15,376ピクセル描画されることを確認しています。
これは1つの検証シーンの結果で、すべての画像形式・マテリアルや物理GPUでの検証ではありません。
Webのテクスチャ付きUIはRGB色の乗算に対応し、画像パスの変更と画像解除も次の描画へ反映するよう修正しました。
WebGL1／WebGL2／Canvas2DのPortableテスト、WebGLリソースの復帰テストと、
Web出力のPythonテスト74件が通っています。Scriptの`On`／`Off`／`Emit`、
引数なしイベント、ペイロードと既定／明示sender、Script破棄時の購読自動解除もPortableSmokeで確認しています。
共有モデル検査、Rigidbody補間診断、ネイティブ向け診断文言の変更後、ネイティブ出力ツールのPythonテスト53件も通りました。該当するCTestのWebExportTool・EditorWebExport・NativeOutputToolsも3件すべて成功しています。

今回のSDKと撮影結果を含む`test-output/platform-core/editor-regression`は約1,260 MiBです。
このフォルダーの削除は以前の自動承認レビューで「blocked by policy」と拒否されたため、
検証生成物を残して再利用しています。Linux／Steam Deckの実環境とAndroid SDK／実機による検証は引き続き未完了です。

PortableのUIButtonは、Scene JSONの`targetScene`と`reloadCurrentScene`を共通の主Scene要求へ接続しました。
ボタン押下後のScene変更は次のScene更新境界で適用し、失敗時は従来のSceneを保持します。
Web／ネイティブ出力検査は遷移先Sceneを再帰的に読み、欠落・未収録パスや未対応コンポーネントを診断します。
Windows NativeSmokeではポインタークリックからのScene切替・再読込とJSON復元を確認し、
ChromeではWebGL1／WebGL2／Canvas2Dの3経路で切替を確認しました。
Web出力ツール74件、Native出力ツール57件、Linux／Windowsビルド出力ツール55件、
Androidスクリーンショット検査4件、およびWindows Release CTest 78件が通っています。
Portable出力ではUIButton `clickEvent`とScriptの`On`／`Off`／`Emit`が同じSceneイベントバスを使います。
キーボード・ポインター・SDL仮想ゲームパッド・Chromeの模擬Gamepad APIで、クリックイベントが一度だけ発行され、
送信元が選択したボタンになることを確認しました。物理コントローラーやSteam Deck実機の検証ではありません。
互換性表でも`clickEvent`・`targetScene`・`reloadCurrentScene`を対応済みとして分類し、`loadTargetAdditive`だけを未対応として残しました。
Additive Sceneは引き続き未対応として拒否します。

WindowsのクリーンReleaseフルビルドも行い、`LamaPonEditor.exe`、Runtime、Game Module、
D3D12プロバイダーを再生成しました。D3D12 WARP描画比較と外部Game Moduleの構成切替・再ビルド・ロードを含む
CTest 78件は、短い作業パスで全項目が通過しています。CloudSaveSynchronizationは単独で3回連続、
Game Moduleの構成切替・再ビルド・ロードも単独で3回連続通過した後、Release全体78件も再度通過しました。
再リンク検査の失敗診断には構成名・前後の時刻・CMakeビルド出力を含めます。Native出力124件、
Web出力83件、Android画面検査4件、EditorWebExport 7件も通過しました。SDK削除の自動承認レビューは以前に拒否されているため、
`test-output/platform-core/editor-regression`の生成物は残し、以後その削除は再試行しません。

共通C++基盤もWindows Releaseで再ビルドし、`PlatformCore` CTest 1件が成功しました。
これはWindows上での共通基盤検証であり、Linux ABIやAndroid NDKでのコンパイル結果ではありません。

Camera・BoxCollider・Rigidbody・AudioSourceの基本Script APIと既定値をWindows Editor Release／Nativeゲームで再ビルドしました。Release CTestは短縮`R:`パスで77件が成功し、OnlineP2PSceneだけはテスト側の実パス同一性確認が`R:`と`C:`を別表記と判定しました。同テストを元の`C:`パスから単独実行して成功を確認しています。Native出力スモークではカメラの既定値とScene JSON省略値、BoxColliderの寸法・offset・trigger・layer/mask、Rigidbodyの速度・重力・kinematic状態、AudioSourceの基本設定と制限値を検査し、出力ゲームの保存継続も確認しました。Web／Linux／Androidの実行環境による確認は別途必要です。
Windows／Portableの同名Component型すべての公開メソッド差分を照合し、PortableにないAPIをNative／Web共通の出力前検査で拒否する一覧を拡張しました。型付き参照、`GetComponent`／`AddComponent`の取得、直接チェーン、メンバー関数参照を検査し、継承済みAPIとライフサイクルhookは除外します。テストは不足メソッドの分類漏れがないことと、分類した全メソッドがWindows・Linux・Androidのすべてで共通検査により拒否されることを確認します。Scene・Prefabの意味差、間接呼び出し、機能全体の動作一致はCORE-06に残ります。
AudioSourceの開始時自動再生は、設定値と実行済み状態を別に保持するよう修正しました。開始時に音源を再生したあとも`PlayOnStart()`がtrueを返すことを、Windowsの生成Nativeゲームで確認しました。Native出力ツール75件、Web出力ツール83件、Native／Web出力CTest 3件が通過しています。
Portableの同名Component型にWindows側と同じ`TypeName()`を追加し、全型の戻り値を照合するテストを追加しました。`UIRectTransform`にはWindows版と同じアンカー・pivot・位置・寸法の取得APIも追加し、範囲制限を含む読み書きをNativeスモークで検査しました。PortableSmokeにも同じケースを追加しましたが、Emscriptenが未導入のためWeb C++ターゲットの再ビルドは未実行です。最新変更を含むWindows Editor Release全体を再ビルドし、CTest 78件すべてが成功しました。Native／Web出力APIテスト75件／83件と、Native build・Linux／Windows出力・WSL境界・Android画像検査のツールテスト59件も成功しています。Linux実ビルド、Android APK／Emulator、最新PortableSmokeのWasm実行は未検証です。

Linuxキャッシュパスの追加テストでは、絶対`XDG_CACHE_HOME`、`HOME/.cache`へのfallback、相対XDG／HOMEの無視、一時領域へのfallbackを、環境変数だけを変更して確認します。Windows PlatformCoreを再ビルドしCTest 1/1件は成功しましたが、POSIX分岐はWindowsではコンパイル・実行されません。Linux CIでの実行結果はまだありません。
