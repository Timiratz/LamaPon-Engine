# Web／Windows出力試作

配布SDKにも `tools/export_web.py`、`cmake/LamaPonWeb.cmake`、Web／Portableの
ソースと必要なヘッダーを同梱しています。ソースリポジトリの別途取得は不要です。
Python 3.11以降、CMake、[Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
を用意し、`emsdk_env.bat`（Windows）または `source emsdk_env.sh` で有効化してから、
SDK内の `python tools/export_web.py <プロジェクトのproject.json> --generator Ninja`
を実行します。Webの対応範囲は以下の契約に従います。
Emscripten 6.0.9でコンパイルとChrome上の回帰テストを検証しています。

この文書は、既存のWindows／DirectX 11 Runtimeを保ったまま、C++で書いた小さなゲームをWindowsとWebへ出力するための最初の設計判断を記録します。

## 現状の境界

現行RuntimeはWindows専用です。`CMakeLists.txt` が `WIN32` を要求し、Runtimeは次のOS／APIへ直接依存しています。

| 領域 | 現在の依存 | Web試作での扱い |
| --- | --- | --- |
| Window／メインループ | Win32 `HWND`、メッセージループ | `Platform` backendへ分離 |
| Renderer | D3D11、DXGI、D3DCompiler、DirectXMath、DirectXTK | `LamaPon::Web::Renderer3D` のWebGL2 backendを別Runtimeとして置く |
| Input | Win32メッセージ、DirectXTK `Keyboard`／`Mouse` | キー状態の共通APIとbackend別イベント入力 |
| Audio | DirectXTK Audio、XAudio2 Redistributable | `LamaPon::Web::WebAudioRuntime` でWeb Audioへ置換。自動再生制限を扱う |
| FileSystem／Assets | Win32ファイル、`std::filesystem`、WIC、DDS、ディスクキャッシュ | 最初は静的配布アセット。将来はURL／仮想ファイルシステムへ分離 |
| Editor／Hub | Win32、Dear ImGui Win32／D3D11 backend | ホストアプリとしてWindows専用のまま。Web SDKとExport設定を管理 |

特に `GraphicsDevice`、Renderer系Component、Model／Texture loaderのヘッダーにD3D11型とDirectXMath型が現れるため、既存Runtime全体を条件コンパイルでWeb化するのは最初の一歩として採用しません。先に、ユーザーコードがbackendの型を見ない最小APIを確立します。

## Web backendの順序

最初の主対象は **WebGL2** とします。2Dの基礎検証に加え、今回の試作では3D描画・軽量Physics・Audioを同じWebGL/Emscriptenターゲットで検証します。WebGL2を第一選択、WebGL1をGPU互換経路とし、HTMLはゲームを起動するシェルとして出力します。

- EmscriptenのOpenGL ES 3／WebGL2経路で、2Dの頂点バッファ・テクスチャ・アルファブレンドを小さく検証できる。
- Emscriptenでは `-sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=2` とし、WebGL2を優先しながらWebGL1対応端末でもGPU描画を維持する。
- WebGPUは、EmscriptenではEmdawnwebgpuという外部ポートを介した `webgpu.h` 経路であり、WGSLと明示的なGPUリソース／非同期初期化を前提とする。高機能3D、Compute、将来の高機能Rendererには有力だが、SDKの配布範囲と初期化境界を固定できるまで、最初の互換性プロファイルには含めない。

WebGL2を最初に選ぶことは、将来WebGPUを採用しないという意味ではありません。`IRenderer2D`相当の共通APIを先に決め、WebGL2／D3D11／WebGPUが同じゲームコードを受けられるようにします。

## 提案するモジュール境界

```text
LamaPon::Core
  ├─ Platform       Window / main loop / clock
  ├─ Input          key, pointer, text events
  ├─ FileSystem     packaged assets / URL or native files
  ├─ Renderer2D/3D  portable draw requests / backend resources
  ├─ Physics3D      fixed-step portable simulation
  └─ Audio          optional Web Audio / native backend

Host tools
  ├─ Hub             SDK, Emscripten, runtime package, cache
  └─ Editor          target settings, export, preview
```

Renderer／Input／FileSystem／Audioは抽象インターフェースを公開し、backendは各ターゲットのパッケージに閉じ込めます。Coreの公開ヘッダーには `HWND`、`ID3D11*`、`DirectX::XMFLOAT*`、Win32型を出しません。

Webの最小2Dモジュールは `core + renderer2d + input`、今回のCarGame向けPortable Game Moduleは `core + renderer2d + renderer3d + input + physics3d + audio + particles3d` です。`LamaPon::Web::IWebApplication` と `WebRuntime` がEmscriptenのメインループ、固定更新、入力フレーム、実行状態、フレーム計測を共通管理します。Renderer、Physics、Audio、ParticleはProjectが要求したときだけ `cmake/LamaPonWeb.cmake` によりリンクされます。

CarGameでは完全コース、手続き生成MR-S、テクスチャ付き路面・壁・縁石、霧と照明、車両挙動、タイヤ痕・スモーク、Web Audioを移植対象に含めます。HLSLの実行時コンパイル、Compute、PostProcess、DirectXTKのSpriteBatch、XAudio2、ネイティブPhysics DLL自体はブラウザで実行せず、同じゲーム結果を作るWeb backendへ置換します。

## WindowsとWebの互換性プロファイル

`.exe`で動くことを、Webで動くことの根拠にはしません。Webプロファイルは用途ごとに許可するものを少なく定義します。`webgl2-basic-2d` は2D専用、`webgl2-basic-3d` は今回のCarGame向け3D試作です。

| 機能・表現 | Windows／D3D11 | Web初期プロファイル | 方針 |
| --- | --- | --- | --- |
| ゲームループ、時間、ログ | 可 | 可 | Core APIへ分離 |
| 2D矩形・スプライト・アルファブレンド | 可 | 可 | WebGL2 backendで保証 |
| キーボード入力 | Win32メッセージ | 可 | Platform／Inputで吸収 |
| キーボード、ゲームパッド、タッチ | 可 | 可 | 共通Input Actionへ自動接続。タッチは左側スティック／右側アクセル・ブレーキ |
| マウス／Pointer | 可 | 可 | 座標、移動量、Wheel、5 Buttonのdown／pressed／releasedを共通Inputへ接続 |
| IME／文字入力 | 可 | 未対応 | ゲーム操作Actionとは分離し、Portable Text Input APIができるまでUI入力Componentを拒否 |
| WebP／JSONアセット | WIC等 | 可 | WebPはそのまま静的配布または仮想ファイルシステム |
| PNG／JPEG／GIF | 可 | 自動変換 | 全画像をロスレスWebPへ統一。GIFはNative同様に先頭FrameをTexture化し、元の参照Pathを維持 |
| BMP／DDS／TGA／TIFF | 可 | 自動変換 | Hubの画像変換ModuleでロスレスWebP化。元の参照Pathは維持 |
| FBX／glTF／GLB／OBJほか3D形式 | 可 | 自動変換 | AssimpでGLB 2.0へ統一し、頂点・法線・UV・Node階層をPortable Runtimeで読む |
| スキン／ボーンAnimation | 可 | 可 | glTFのLINEAR／STEP／CUBICSPLINEをCPU Skinningで再生 |
| PBR Material | 可 | 基本対応 | Albedo、Normal、Metallic、Roughness、AO、Emissive、Alpha、UnlitをWebGLへ変換 |
| 高度glTF Material／Morph | 可 | 拒否 | Transmission、Clearcoat、別UV、UV Transform、Morph等は完全再現Backend追加まで停止 |
| HLSL／D3DCompiler／DirectXTK | 可 | 不可 | GLSL ES 3.00または将来のShader IRへ移行 |
| 3D頂点メッシュ、法線、深度 | 可 | `webgl2-basic-3d`で可 | WebGL2の固定シェーダー経路へ変換 |
| 影、PostProcess | 可 | 警告して自動無効化 | Basic Profileでは未実装。壊れた再現を成功扱いしない |
| Compute、Tessellation、Geometry Shader | 可（一部機能） | 不可 | WebGL2基本プロファイルでは拒否 |
| XAudio2／ネイティブDLL／Win32 API | 可 | 不可 | Web Audio／Web API／Wasm moduleへ置換 |
| 基本的な重力、AABB衝突／Trigger、Box／Mesh Raycast | 可 | `webgl2-basic-3d`で可 | 固定ステップの軽量Web Physics。回転BoxはWorld AABB、摩擦／反発／CCDは警告または拒否 |
| Project独自Input Action | 可 | 自動変換 | `.lamapon/project.json` のKeyboard／Mouse／標準Gamepad BindingをWeb Mapへ生成。未知Controlは拒否 |
| WAV／Audio loop／pitch／volume／pan／3D定位 | 可 | 可 | Web Audioへ置換。Listener、HRTF、距離減衰を接続し初回操作でunlock |
| MP3／OGG／FLAC／M4A／AAC／WMA | 可 | 自動変換 | Hubの音声変換ModuleでPCM WAV化。元の参照Pathは維持 |
| `std::filesystem`のホストファイル前提 | 可 | 不可 | ブラウザ仮想ファイルシステムの範囲だけ許可 |
| `std::thread`／pthreads | 可 | 未保証 | SharedArrayBuffer等を含む別プロファイルで扱う |

WebGLの圧縮テクスチャはS3TC等が拡張として登録されており、ブラウザの実装・GPUで一律に保証できません。[Khronos WebGL Extension Registry](https://registry.khronos.org/webgl/extensions/)。そのため、Windows側でBC5へ最適化されたテクスチャをそのままWebへ流用しません。Web Package内の画像形式はWebPへ統一し、PNG／JPEG／DDS等は出力用StageだけをロスレスWebPへ自動変換します。

WebGL2のShaderはGLSL ES 3.00形式です。既存のHLSLを文字列置換だけで移植するのではなく、Rendererが共通の描画要求を受け、backendごとにShaderを持つ形にします。[Emscripten WebGL guidance](https://emscripten.org/docs/optimizing/Optimizing-WebGL.html)

ファイル読み込みも、ブラウザのホストファイルを直接読む仕組みではありません。Emscriptenの仮想ファイルシステムへ事前にパッケージするか、URL／非同期Asset APIを使います。[Emscripten Runtime Environment](https://emscripten.org/docs/porting/emscripten-runtime-environment.html)

スレッド、SIMD、ブラウザの自動再生制限、タブの非アクティブ化、GPUドライバー差、Canvasの解像度とCSSサイズの違いも、Windowsとの完全一致を保証しない項目です。[Emscripten Portability Guidelines](https://emscripten.org/docs/porting/guidelines/portability_guidelines.html)

### Export時の拒否条件

`tools/export_web.py` はPortable Gameで厳格検査を常時実行します。検査結果は `AUTO`（安全な自動代替）、`INFO`（設計情報）、`WARNING`（出力できるが差をPreviewで確認）、`REJECT`（壊れた出力を作らず停止）の4段階です。

| 検査層 | 検査内容 | REJECTの例 |
| --- | --- | --- |
| C++／API | 選択ソースとヘッダー、Portable API許可表、Windows／D3D11依存、入力Action、Script登録 | 未実装 `LamaPon::` API、Win32型、未登録Script、未定義Action |
| Module | API・Scene Componentが要求するModuleとProfileの許可Moduleを照合 | `renderer2d` の宣言漏れ、Web backendがないModule |
| Scene | JSON形式、Object ID、親参照と循環、Main Camera、Component、NativeScript、環境Effect | 壊れた階層、未対応Component、存在しないCamera／Script |
| Asset | C++／Scene参照、Package対象、拡張子、空ファイル、PNG／JPEG／WebP／WAV／JSONの基本整合性 | 欠落Texture、除外済みAsset、破損画像、FBX／HLSL／DLL |
| Build／Output | Emscriptenコンパイル、Moduleリンク一致、HTML Shell、Canvas／Script、空成果物、単一HTML、標準名 | コンパイル不能、Wasm漏れ、未展開Shell、誤った出力名 |

検査は次の順で失敗を早く見つけます。

1. C++／Module／Scene／Assetの静的契約検査
2. Emscriptenによる元C++のコンパイルとリンク
3. 生成Packageの構造・ファイル名・埋め込み検査
4. `web-compatibility-report.json` とSHA-256付きManifestの生成

現在のPortable Runtimeは「2D専用Scene」と「3Dシーン＋2D HUD」の両方を実装します。2D専用Targetはcore、renderer2d、inputだけを宣言し、SpriteRenderer、SpriteMask、TextRendererを同じScene JSONとC++ Scriptから実行できます。Canvas／frame初期化コードは3D Targetと共有しますが、Model、Physics、Audioなど未宣言Moduleはリンクしません。

### Asset自動変換

Exporterは変換可能なAssetを見つけると、Project内の原本には触れず、`.lamapon/web-generated-assets` の出力用コピーだけを変換します。変換後も仮想Pathと拡張子を維持し、RuntimeがファイルSignatureから実形式を判定するため、C++文字列やScene JSONの参照を書き換えません。

| 入力 | Web Runtime形式 | Hub Tool |
| --- | --- | --- |
| PNG、JPEG、GIF、BMP、DDS、TGA、TIF／TIFF | ロスレスWebP | ImageMagick |
| MP3、OGG、FLAC、M4A、AAC、WMA | PCM WAV | FFmpeg |
| FBX、glTF、GLB、OBJ、DAE、3DSほか | GLB 2.0 | Assimp |

TTF、OTF、WOFF、WOFF2は変換せずPackageへ埋め込み、SceneのfontAssetが
指定されたTextRendererではFontFace APIで読み込みます。OSにしかないフォント名は
端末ごとに字形や幅が変わるため、完全一致が必要なUIではフォントAssetをProjectへ
含めます。

ProjectのInput Actionは `.lamapon/project.json` から
`lamapon-input-actions.json` を出力Stageへ生成します。Action名をEngineへ
ハードコードする必要はありません。Keyboard、Mouse、標準Gamepadの対応Controlは
そのまま移し、対応不能なControlが実際にゲームコードまたはSceneから使われる場合は
Exportを拒否します。Move／Accelerate／Brake等にはTouch操作も合成します。
Editorが外部変更されたProject設定を再読み込みした場合も、Exporterは実行のたびに
現在の `.lamapon/project.json` をディスクから読み直すため、以前のPreviewで生成した
Input Action Mapを使い回しません。

### Scene Component対応表

| 区分 | Component |
| --- | --- |
| 実装済み | NativeScript、Camera、DirectionalLight（影以外）、AudioSource／Listener、MeshRenderer、ModelRenderer、BoxCollider3D、Rigidbody基本Subset、ParticleSystem、SpriteRenderer／Mask／Animator、TextRenderer、UIRectTransform、TransformAnimator直接Clip、Rotator、InputMover、ParallaxLayer、RenderCulling |
| Export時に明示拒否 | Point／Spot Light、ReflectionProbe、Shadow、各種高度Collider／Joint／CharacterController、NavMesh、LOD／Billboard、Tilemap、Light2D／2D Physics、SpriteParticles2D、UIRectTransform以外のUICanvas系Component、RenderTexture、Custom Shader／Binding、Animator Controller、Root Motion |

この表にない未知Componentも拒否します。Native側のComponent語彙をExporter内で
一覧管理しているため、「Sceneには入っていたがWeb Runtimeが黙って捨てた」という成功扱いは
行いません。安全に同じ結果を作れるComponentから共通Runtimeへ追加します。

変換した一覧はPackage内の `lamapon-asset-conversions.json` に記録します。必要な変換ModuleがHubに無い場合や変換Commandが失敗した場合は、原形式を黙ってPackageせずExportを拒否します。高度な設定では `export.web.converterTools.imageMagick` と `export.web.converterTools.ffmpeg` でTool Pathを固定できますが、初心者向けEditorではHub管理SDKを自動選択します。

主な自動検査は次の通りです。

- `export.modules` に初期プロファイル外のモジュールがあれば拒否レポートを出す。
- Web対象ソースのWin32／D3D11／DXGI／XAudio2型・ヘッダーを検出したら拒否レポートを出す。`XMFLOAT2/3/4/4X4`、`XMMATRIX`、`XMStoreFloat4x4`、`XM_PI` は公開API互換の軽量数学型へ自動置換する。
- `assets` 以下の変換可能画像はWebP、音声はWAV、対応ModelはGLBへ変換する。標準LamaPonLit HLSLは組み込みGLSLへ置換し、Custom HLSL、ネイティブバイナリ、未対応Model機能は拒否レポートを出す。
- Browser内の存在確認・Directory列挙を行う `std::filesystem` と、`std::thread`／`std::async` は警告を出す。単純なAsset path保持は警告しない。
- `WindowsMain.cpp` など一般的なWindowsエントリーポイントは自動判定し、ゲームコードのWindows依存は理由を表示して停止する。

Exportに成功すると、Hubは自動対応・注意事項を `web-compatibility-report.json` に記録します。出力できない場合も同じファイルへ `status: "rejected"` と理由・対処方法を書き、C++のコンパイルを開始する前に停止します。レポートには厳格Portable契約を使ったかどうかと、各判定レベルの件数Summaryも入ります。初心者は通常このファイルを読む必要はありませんが、配布前の確認やサポート問い合わせで「何が自動対応され、何が未対応か」を追跡できます。

成功時はさらに `web-export-manifest.json` を生成します。ソース入力のSHA-256指紋、選択Renderer／Profile／Module、Build種別、単一HTML設定、各成果物のサイズとSHA-256を記録します。Exporterは配布前にHTMLのCanvas／Script、未展開Shell、空ファイル、単一HTMLから漏れた外部JavaScript／Wasmを検査します。Project設定で宣言したModuleとCMakeが実際にリンクするModuleが一致しない場合もビルド時に停止します。

この拒否は「将来もWebで使えない」という意味ではありません。Web backend・Asset変換・互換性テストが揃ったときに、別プロファイルまたは許可リストを追加します。無理に出力して実行時に壊れるより、Export時に理由を出して止める方がLamaPonの「不具合やバグが超少ない」という方針に合います。

## Project／Export設定案

初心者が書く設定は、ターゲットの選択だけにします。互換性プロファイル、SDK、CMakeターゲット、Windows専用ファイルの判定はHub／Editorが既定値として管理します。

```json
{
  "export": {
    "targets": ["windows", "web"]
  }
}
```

HubはSDKとWeb Runtimeをバージョン固定して管理し、Editorはこの設定を使ってExport・Previewを呼び出します。Project側は「どの出力を作るか」だけを保存し、エンジン本体へ全モジュールを静的リンクする設計にはしません。高度なプロジェクトだけが、追加のWeb設定やモジュールを明示します。

Web成果物のファイル名は設定で変更せず、必ず `LamaPonWebGL-プロジェクト名.html` とします。表示用のゲームタイトルとは分離し、通常のLamaPonProjectではプロジェクトフォルダー名、外部ソースを使うWeb Targetでは `projectName` を使用します。CarGameの出力は `LamaPonWebGL-CarGame.html` です。

## C++からWebAssemblyへ

Emscriptenの `em++`／CMake toolchainでC++をWebAssemblyへ変換します。通常のビルド出力は `.html`、`.js`、`.wasm` ですが、初心者向けの配布では `SINGLE_FILE` と埋め込みアセットを使い、HTMLへJavaScript・Wasm・小さなアセットをまとめられます。Web出力はHubがEmscripten SDKのバージョンとビルドフラグを固定し、ユーザーは通常のC++／`LamaPon::` APIだけを記述する形を目指します。

今回の試作では `tools/export_web.py` がこのHub側処理の最小版です。CMake Webターゲットを持つプロジェクトにはWebGL2・互換性プロファイル・標準ソース構成を適用して、`emcmake cmake` → `cmake --build` を実行します。通常のLamaPonProjectも、Projectファイルを変更せずにC++ソース、起動Scene、必要Module、Runtime Asset、Input Actionを推定し、一時Portable Targetを自動生成して同じ経路でビルドします。

`export.web.buildSystem` が `lamapon` のWeb Targetでは、Projectごとの `CMakeLists.txt` は不要です。Exporterが `sources`、`modules`、`shellFile`、`assetDirectory`、`singleFile` から一時CMake Targetを生成し、共通の `lamapon_add_web_game()` を呼び出します。CarGameもこの自動生成経路で再出力されており、手書きCMakeは出力処理に使用しません。

`prototypes/cargame-portable` は `singleFile: true` を指定しており、最終パッケージは自己完結したHTMLです。WebGL2 Rendererを第一選択、WebGL1をGPU互換経路とし、GPU API自体が無効な埋め込みプレビューではCanvas 2D、さらにCanvas 2Dも使えない場合はSVGソフトウェア3Dへ自動切り替えます。Canvas 2D経路も960x540のソフトウェア深度バッファ、近クリップ面での三角形分割、透視補正UV、頂点照明補間を使用します。表示時は1280x720のゲーム画面とHUDを一体で等比拡縮し、ブラウザの縦横比による変形を防ぎます。描画backendが変わっても、C++側のゲームループ・Physics・Input・Audio APIは同じです。

```sh
python3 tools/export_web.py prototypes/cargame-portable/project.json
```

### 既存のLamaPonProject

`.lamapon/project.json` をそのまま渡せます。ゲーム名、起動Scene、`assets/scripts` のC++、Scene Component、Runtime Assetから必要Moduleと入力を推定し、WebGL2プロファイルで厳格検査してからPortable Emscripten Targetを生成します。Projectに `export.web` があれば高度な設定だけ上書きでき、無ければ単一HTML、WebGL2、起動Scene、Asset Directoryを自動設定します。

```sh
python3 tools/export_web.py /path/to/CarGame/.lamapon/project.json
```

通常のLamaPonProject（例: CarGame本体）をそのまま渡した場合は、3D Renderer、2D HUD、Physics、Audio、Particle等を自動選択し、元C++をWasmへコンパイルします。Windows/DX11エントリーポイントや未対応APIが残る場合は、壊れたHTMLを作らず理由を示して拒否します。HLSLやDirectX APIを危険な文字列置換でJavaScriptへ書き換えることもしません。既定の生成物はProjectの `.lamapon` 以下だけに置かれ、`assets` とProject設定は書き換えません。完全に外部へ出したい場合は `--output` と `--build-dir` を指定できます。

`portableGame: true` のTargetでは、ExporterがWeb用の `LamaPon/LamaPon.h` を公開インターフェースとして先に解決し、元のゲームスクリプトをEmscriptenで直接コンパイルします。Scene／GameObject／Component／Script、手続きメッシュ、Renderer 3D、固定ステップ、Raycast、Input、Audio、Particle、HUD、保存状態をWeb backendへ接続します。ゲームロジックのコピーやJavaScriptへの再実装は行いません。

`prototypes/cargame-portable` はCarGameの回帰試験設定として残しますが、通常の変換には専用設定は不要です。CarGame本体の `.lamapon/project.json` を直接入力する経路でも、元の `assets/scripts/*.cpp` 9ファイルを変更せずWasmへリンクし、必要なScene JSON、テクスチャ、音声を出力用ステージへ複製できることを検証済みです。Projectへ追加された `ToggleView` もInput Action設定から自動変換され、Keyboard C／Gamepad Yで動作します。`?autopilot=1` では元C++の決定論テスト経路を使い、ブラウザで走行・ゴールまで検証します。

これは全LamaPonゲームの全APIを保証する完成版ではありません。Portable公開APIに未実装機能があればExport時のコンパイルエラーまたは互換性レポートで明示し、対応をRuntimeへ追加していきます。重要なのは、ゲームごとの手移植ではなく、一度追加した互換機能を次のゲームでも共有できる構造になったことです。

## 既存の未コミット変更

既存ツリーの変更はテクスチャ圧縮（BC5含む）、モデル埋め込み画像のロード統合、TextureLoaderテストに限定されています。Web試作ツリーには取り込まず、Renderer／Platform境界の検討と独立させます。将来Asset／Texture backendをWebへ広げる段階で、これらの変更を別途レビューして統合します。
