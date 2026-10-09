# Webエクスポート

LamaPonは、ポータブル版で対応している機能を使ったC++ゲームをHTMLへ出力できます。
エクスポート前に互換性を検査し、未対応機能が見つかった場合は理由を表示して停止します。
Windows専用機能を含むゲームが、そのままブラウザーで動くことを保証する機能ではありません。

[← ドキュメント一覧へ戻る](index.md)

## エディターから出力する

1. 「ゲームをエクスポート」を開きます。
2. 出力形式で「Web（HTML）」を選びます。
3. 出力先とWebビルド環境を確認し、「エクスポート」を押します。

既定の出力先は`dist/LamaPonWeb`です。手入力または参照ボタンで選んだ出力先は、
Windows（EXE）とWeb（HTML）を切り替えても保持されます。

Webビルド環境は保存済みの設定を優先します。未設定の場合、Emscripten SDKは
`EMSDK`環境変数または`PATH`上の`emcmake`から、Python 3.11以降とCMakeは`PATH`から自動検出します。
SDKフォルダー内のPythonも検出対象です。自動検出できない場合だけ、インストールして有効化した
Emscripten SDKのフォルダー、またはPython実行ファイルを指定してください。これらのPC固有設定は
`%LOCALAPPDATA%/LamaPon/web-export-tools.json`へ保存され、プロジェクトには入りません。

ビルド中も編集を続けられますが、エディターを終了すると実行中のビルドも終了します。
進行状況と結果はステータス表示と出力ダイアログで確認できます。失敗しても既存の出力は
保持されます。成功後は「出力フォルダーを開く」からHTMLを確認できます。

## コマンドラインから出力する

配布SDKには`tools/export_web.py`、`cmake/LamaPonWeb.cmake`、ポータブル版の
ソースと公開ヘッダーが含まれます。Python 3.11以降、CMake、
[Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)を用意し、
`emsdk_env.bat`（Windows）または`source emsdk_env.sh`でSDKを有効化してから実行します。

```sh
python tools/export_web.py /path/to/MyGame/.lamapon/project.json --generator Ninja
```

Emscripten 6.0.9とChromeを使ったコンパイル・起動テストを行っています。

## 出力されるファイル

既定のファイル名は`LamaPonWebGL-プロジェクト名.html`です。表示用のゲームタイトルとは
別に、プロジェクトフォルダー名から決まります。`singleFile`が有効な場合はJavaScript、
WebAssembly、アセットを1つのHTMLへ埋め込みます。複数ファイル出力を選んだ場合は、
生成されたフォルダー全体を配布してください。

`export.web.buildSystem`が`lamapon`のプロジェクトでは、プロジェクト側の
`CMakeLists.txt`は不要です。エクスポーターがソース、モジュール、起動シーン、
アセットを調べ、一時的なCMakeターゲットを生成します。プロジェクトのソースや設定は
書き換えません。

## 対応範囲

ポータブル版はWindows版と別のランタイムです。Windows版はWin32、Direct3D 11、
XAudio2を使い、Web版はEmscripten、WebGL、Web Audioへ置き換えます。

| 領域 | Webでの扱い |
| --- | --- |
| ゲームループ、時間、ログ | 対応 |
| 2D描画 | スプライト、テキスト、マスク、アルファ合成に対応 |
| 3D描画 | 基本メッシュ、モデル、PBRマテリアル、深度、点光源・スポットライト（合計8灯まで）に対応 |
| 入力 | キーボード、マウス、標準ゲームパッド、タッチに対応 |
| 音声 | WAVと3D定位をWeb Audioで再生。圧縮音声はWAVへ変換 |
| 物理 | 基本的な重力、AABB衝突、トリガー、Box／Mesh Raycastに対応 |
| 画像 | WebPを使用。PNG、JPEG、GIF、BMP、DDS、TGA、TIFFは自動変換 |
| 3Dモデル | FBX、glTF、GLB、OBJなどをGLB 2.0へ自動変換 |
| Discordログイン、クラウドセーブ | 未対応。Windows x64ランタイムだけで利用可能 |
| HLSL、Direct3D、XAudio2、Win32 API | Web版では使用不可 |
| 高度なシェーダー、影、ポスト処理 | 未対応。機能に応じて警告または拒否 |
| `std::thread`／pthreads | 現在のプロファイルでは動作を保証しない |

`AudioSource.streaming`はPortable版では逐次読み込みにならず、再生前に音声全体をデコードしてメモリーへ保持します。`AudioSource.bus`の個別ミキサーも未対応で、既定以外のバス指定は出力診断で警告します。SDLネイティブ版の3D定位は距離減衰とステレオ定位で近似し、Web AudioのHRTFとは異なるため、ネイティブ出力診断に警告を表示します。
Windows専用のCoroutine／タイマー、ウィンドウ変更、Discord・ネットワーク・クラウド同期、数値保存メソッドはPortable Scriptに実装されていません。Portable Script派生クラスから直接呼ぶ場合はNative／Web共通の出力前検査で拒否します。
Windows版Sceneにだけ存在する公開メソッドを直接呼び出す場合も、Native／Web共通の互換性検査でPortable未対応として診断します。

`webgl2-basic-2d`は2D向け、`webgl2-basic-3d`は基本的な3D向けです。
WebGL2を優先し、対応状況に応じてWebGL1またはソフトウェア描画へ切り替えます。
ブラウザー、GPU、タブの状態、自動再生制限によってWindows版と結果が異なる場合があります。

プロジェクト設定でオンラインサービスを有効にしても、ポータブルWebランタイムへ認証・同期機能は
追加されません。Web版を配布する場合は現在オンライン機能を無効にし、Windows版だけで利用して
ください。対応範囲とバックエンド構成は[Discordログインとクラウドセーブ](online-services.md)を
参照してください。

## セーブデータ

Portableスクリプトの`SaveText`／`SaveInteger`は、Webではブラウザーの`localStorage`へ保存します。
保存は現在のオリジン（プロトコル・ホスト・ポート）に紐付き、同じオリジンを次回開くと読み戻せます。
ゲーム側で例外を捕捉すれば、保存拒否を画面へ表示したり、再試行を案内したりできます。
`LoadText`はキーがない場合や読み込みが拒否された場合に既定値を返します。

ブラウザーのサイトデータ消去、プライベートブラウズ、容量制限、ブラウザー側の保存方針によって、
保存が拒否されたり、後から消去されたりすることがあります。Web版の保存はOS上のゲーム保存先と
同じディレクトリ・容量・保持保証を持ちません。大切なデータには、ゲーム内のエクスポート機能など
別のバックアップ手段を用意してください。Windows/Linux/Androidのネイティブ出力は、OSごとの
アプリ専用データ領域へ保存します。

WebGLの圧縮テクスチャはブラウザーとGPUによって対応状況が異なるため、Windows版の
BC圧縮データをそのまま使いません。詳しくは
[Khronos WebGL Extension Registry](https://registry.khronos.org/webgl/extensions/)を参照してください。
出力用のコピーだけをWebPへ変換し、プロジェクト内の原本は変更しません。

## ブラウザー操作と復帰

標準HTMLには全画面切替を用意しています。全画面APIが使えないブラウザーではボタンを隠します。
スマホではゲーム画面上のスクロール・ピンチ操作を抑え、拡縮されたCanvasの入力座標を
ゲーム内の座標へ戻します。ソフトキーボードなどで表示領域が移動した場合も画面を中央に配置します。

ウィンドウがフォーカスを失ったときはキー・マウス・タッチの保持状態を解除します。
Canvas外でのマウスボタン解放も検知します。タブが非表示の間は更新を止め、復帰直後は
非表示期間の固定更新をまとめて実行しません。WebGLコンテキストを失った場合は更新を止め、
画面に再読込ボタンを表示します。再読込はゲームを最初から開始します。

## UIと局所照明

`UICanvas`の基準解像度と幅・高さの一致率を、子の`UIRectTransform`へ反映します。
Scriptから`UIRectTransform`の最小／最大アンカー、pivot、位置、寸法差を読み取り、変更できます。アンカーとpivotはWindows版と同じ0〜1の範囲に制限し、アンカー同士の前後関係も維持します。
`UIImage`は通常のテクスチャまたは単色に対応します。9分割画像とRenderTextureは拒否します。
`UIButton`は通常・重なり・押下・無効の色、ラベル、矩形または楕円のヒット領域に対応します。
スクリプトは`WasClicked()`または`ConsumeClick()`でクリックを取得できます。
シーンJSONに保存されたUIButtonの`targetScene`と`reloadCurrentScene`は、Portableランタイムがクリック時に主Scene要求へ変換します。
`targetScene`は`scenes/*.scene.json`のasset pathで指定し、出力検査は遷移先Sceneとその階層・Script・素材を再帰的に調べます。
Scriptからの主Scene切り替えは`GetScene().Scenes().RequestLoad("scenes/next.scene.json")`、
現在のScene再読込は`RequestReload()`を使えます。`clickEvent`はPortable Scriptの`On`／`Emit`で受け取れます。
Additive Sceneは未対応のため、主Sceneの遷移先を指定してください。変数経由のScene path、非同期遷移、Prefab override操作もPortable出力前に拒否します。

点光源とスポットライトは毎フレームのワールド位置・方向・有効状態を反映し、距離で減衰します。
スポットライトは内側・外側の半角で円錐の境界を制御します。WebGL1／WebGL2では拡散・鏡面反射、
ソフトウェア描画では頂点ごとの簡易拡散照明を使います。同時に使えるのは合計8灯です。
影とポスト処理は引き続き未対応です。

## Scene環境設定

Portableの基本レンダラーは環境色・基本的な方向光と局所光、上空／地平線色の空、開始／終了距離による霧を描画します。
霧の`density`と、空のキューブマップ・地面色・強度・IBL・太陽連動は反映しないため、設定している場合は出力診断で警告します。
有効なアンビエントオクルージョン、時間的アンチエイリアス、スクリーンスペース反射、ベイク済みグローバルイルミネーション、
ボリューメトリックライト、ブルーム、スクリーンアウトライン、レンズフレア、被写界深度、モーションブラー、
自動露出、カラーグレーディングもPortable描画では再現せず、出力前に警告します。
この診断はWebとSDLネイティブ出力で共有されます。警告を確認し、対象環境の見た目に合わせて設定を調整してください。
環境設定のJSON構造とPortableローダーが使う値の型・数値範囲も検査し、実行時に復元できないSceneは出力前に拒否します。

Portable物理は既定の下向き重力と1/60秒の固定更新を使います。プロジェクト独自の重力・固定更新間隔、衝突レイヤー行列、離散衝突速度制限は現在のPortableゲームへ渡らないため、設定されている場合はWeb／ネイティブ共通の警告を表示します。
スクリプトから利用できるSceneの3D物理問い合わせは`Raycast`です。Windows版の`RaycastAll`、Sphere／Box／Capsule cast、Overlap queryはPortableに未実装のため、Web／ネイティブ共通の出力前検査で拒否します。

## 再出力の速度と容量

画像・音声の変換結果は`.lamapon/web-generated-assets`に保持し、入力・変換ツール・出力の
SHA-256が一致した場合に再利用します。管理情報は`.lamapon/web-asset-cache.json`へ保存します。
入力変更、変換ツールの変更、出力の破損は再変換の対象です。外部画像・バッファを参照できるモデルは
毎回変換します。選択から外れたアセットは生成先から除外します。

同じ内容の生成CMake設定・メタデータは更新時刻を保持し、不要な再リンクを避けます。
Releaseビルドは`-Oz`で容量を優先し、Emscriptenのアサーションを無効にします。
診断が必要な場合はコマンドラインの`--build-type Debug`を使用してください。
キャッシュを消す場合は生成先のアセットフォルダーと管理JSONを削除すると、次回出力で作り直します。

## 互換性検査

`tools/export_web.py`はコンパイル前にプロジェクトを検査します。結果は次の4段階です。

| レベル | 意味 |
| --- | --- |
| `AUTO` | 安全な代替処理を自動で適用 |
| `INFO` | 出力内容に関する情報 |
| `WARNING` | 出力可能だが、ブラウザーでの確認が必要 |
| `REJECT` | 正しく再現できないため出力を停止 |

検査する主な項目は次のとおりです。

1. C++コードで使用するAPIとプラットフォーム依存
2. モジュールと互換性プロファイルの組み合わせ
3. シーン階層、コンポーネント、スクリプト登録
4. 参照アセットの存在と形式
5. Emscriptenのコンパイル結果と出力パッケージ

未知のコンポーネントや、実装のないポータブルAPIは拒否します。検査で判定できる
既知の未対応機能は無視せず、出力を停止します。

HTMLをビルドせずに診断だけ実行するには、`--report-only`を使います。
指定した出力先へ`web-compatibility-report.json`を保存し、拒否項目には理由と対処方法を含めます。

```sh
python tools/export_web.py /path/to/MyGame/.lamapon/project.json \
  --output /path/to/MyGame/web-report --report-only
```

## アセット変換

変換は`.lamapon/web-generated-assets`の出力用コピーに対して行います。仮想パスと
拡張子は維持し、ブラウザー側はファイルシグネチャから実際の形式を判定します。
C++文字列やシーンJSONの参照を変更する必要はありません。

| 入力 | Web版の形式 | 変換ツール |
| --- | --- | --- |
| PNG、JPEG、GIF、BMP、DDS、TGA、TIF／TIFF | ロスレスWebP | ImageMagick |
| MP3、OGG、FLAC、M4A、AAC、WMA | PCM WAV | FFmpeg |
| glTF、GLB（Portableゲーム） | 元のglTF／GLBと必要な参照ファイル | 不要 |
| FBX、OBJ、DAE、3DSなど（および従来ターゲットのモデル変換） | GLB 2.0 | Assimp |

PortableゲームのglTF／GLBは、対応機能を検査して元のバイト列を同梱します。
PNG／JPEGのdata URIとGLB内のbufferView画像を読み込めます。外部の画像・bufferを参照する場合は、
参照先を出力対象へ含め、ファイル名の大文字小文字も一致させてください。
必須拡張や未対応のマテリアル機能は出力前に拒否します。変換ツールが不要でも、
エンジンの全モデル機能への対応を意味しません。

日本語などのアセット名は、埋め込み用のASCII名へ内部的に置き換えてから、
ゲーム起動前に元のUTF-8仮想パスへ戻します。元のプロジェクトの名前や参照は変更しません。
この処理にはPythonが必要です。通常の出力ツールは実行中のPythonをCMakeへ渡します。
直接CMakeを使う場合は、`LAMAPON_WEB_PYTHON_EXECUTABLE`、`EMSDK_PYTHON`、または検出可能なPython 3を用意してください。

TTF、OTF、WOFF、WOFF2は変換せずに埋め込みます。表示を端末間で揃える場合は、
OSのフォント名だけに依存せず、フォントアセットをプロジェクトへ含めてください。

## シーンコンポーネント

| 区分 | コンポーネント |
| --- | --- |
| 対応 | `NativeScript`、`Camera`、`DirectionalLight`（影を除く）、`AudioSource`／`AudioListener`、`MeshRenderer`、`ModelRenderer`、`BoxCollider3D`、`Rigidbody`の基本機能、`ParticleSystem`、`SpriteRenderer`／`SpriteMask`／`SpriteAnimator`、`TextRenderer`、`UICanvas`、`UIRectTransform`、`UIImage`（通常画像・単色）、`UIButton`（クリック状態）、`PointLight`／`SpotLight`（影を除く）、`TransformAnimator`の直接クリップ、`Rotator`、`InputMover`、`ParallaxLayer`、`RenderCulling` |
| 拒否 | `ReflectionProbe`、影、モデル参照を使うScene設定の`MeshCollider3D`を含む高度なCollider／Joint／`CharacterController`、NavMesh、LOD／Billboard、Tilemap、Light2D／2D Physics、`SpriteParticles2D`、UI入力欄・スクロール・レイアウト・トグルなどのUIコンポーネント、RenderTexture、カスタムシェーダー、Animator Controller、Root Motion |

対応表にないコンポーネントも拒否します。対応を追加する場合は、ポータブル版の処理と
互換性テストを用意してから許可リストへ登録します。
`BoxCollider3D`はPortableの基本AABB物理を使い、回転した箱を近似します。摩擦・反発・材質の合成や連続衝突は再現せず、WebとSDLネイティブの両方で出力診断に差を表示します。
Script APIでは`BoxCollider3D`の寸法・中心offset・Trigger・layer・collision maskを読み書きでき、`Rigidbody`では速度・重力・kinematic状態を読み書きできます。これらはPortable物理ソルバーが使用する基本項目で、Windows側のgetter／setterと同じ値を扱います。質量・減衰・回転慣性や衝突材質はPortableで再現しないため、Scene設定の互換性検査で個別に診断します。
`AudioSource`では音源path、音量、pitch、pan、loop、自動再生、空間化、距離、出力busを読み書きできます。音量・pitch・pan・距離の境界値はWindows APIと同じ範囲へ収め、自動再生を実行した後も`PlayOnStart`設定は保持します。Stream再生、解析、bass boost、Pause／Resumeなど未実装のメソッド呼び出しはPortable出力前に拒否します。個別busのミキサーはなく、既定以外のbus指定はScene診断で示します。
WindowsヘッダーとPortable公開ヘッダーに同名Component型がある全型について公開メソッド差分と`TypeName()`の戻り値を監査し、未実装メソッド呼び出しをNative／Web共通の出力前検査で拒否します。検査は型付き参照と`GetComponent`／`AddComponent`による取得、直接チェーン呼び出し、メンバー関数参照を対象とし、継承済みメソッドとライフサイクルhookは重複登録しません。未対応Component型や未登録Portable APIも別の互換性検査で拒否します。これはメソッド名の静的検査であり、関数ポインター・テンプレート越しの間接呼び出しや全機能の動作一致は保証しません。
SDLネイティブ出力も同じPortable Scene／Scriptランタイムと互換性検査を使うため、
このコンポーネントとScript APIの制約を引き継ぎます。OS別のパッケージ検査は別途追加します。

PortableのGameObject階層検索は`GetComponentInChildren`、`GetComponentsInChildren`、
`GetComponentInParent`、`GetComponentsInParent`、`GetScriptInChildren`を使えます。
既定では非アクティブな階層を検索から除き、`includeInactive=true`で含めます。
子孫検索は深さ優先で、Sceneへ登録した子の順序を保ちます。`SetParent`は自己参照、循環、
別Sceneの親指定を拒否し、親変更とGameObject破棄時に親子一覧を更新します。
WebGL2とWindowsネイティブのスモークテストでこの契約を確認しています。
Portable Scriptは`Start`、`FixedUpdate`、`Update`、`LateUpdate`、通常衝突とトリガーの
Enter／Stay／Exitを配送し、`CollisionEvent::other`から接触相手を参照できます。
`Awake`、`OnEnable`、`OnDisable`、`OnDestroy`も共通実装しています。Scriptの初回準備で
`Awake`を呼び、有効なGameObject階層上で有効なScript componentへ状態遷移を通知します。
`Start`は最初に有効になった更新前に一度だけ呼び、GameObject破棄時にはScriptを取り除く前に
`OnDisable`と`OnDestroy`を呼びます。WebGL2とWindowsネイティブのスモークテストで
呼び出し順、無効化・再有効化、破棄を確認しています。

Portableでも`Script::Instantiate`／`Scene::InstantiatePrefab`からPrefabを実行時に生成できます。
パスは`assets/`相対、`assets/...`または`/assets/...`で指定し、Prefab内のsource IDは
Scene内の新しいIDへ割り当て直します。rootを同じSceneの親へ接続でき、生成直後に
`Awake`／`OnEnable`が呼ばれ、`Start`は通常の更新開始時に一度だけ呼ばれます。
Prefab生成はScene環境設定や既定カメラを変更しません。無効な形式、壊れた階層、未登録Scriptは
生成時に失敗し、追加途中のGameObjectとIDを巻き戻します。

Web／ネイティブ出力では、互換性検査がPrefabを参照するパスを文字列リテラルとして解決できる必要があります。
変数経由の動的パスや`assets/`外の参照は拒否します。参照Prefabのcomponent・Script・追加素材も
Sceneと同じ対応検査・アセット収録へ含めます。

## レポート

互換性検査が完了すると、`web-compatibility-report.json`へ結果を保存します。
互換性判定で拒否した場合も理由と対処方法を記録します。成功時には`web-export-manifest.json`も生成し、
入力のSHA-256、使用したレンダラー／プロファイル／モジュール、成果物のサイズと
SHA-256を記録します。

配布前にはHTMLのCanvasとスクリプト、空ファイル、単一HTMLから漏れた外部ファイル、
宣言モジュールと実際のリンク内容を検査します。
