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
| 3D描画 | 基本メッシュ、モデル、PBRマテリアル、深度に対応 |
| 入力 | キーボード、マウス、標準ゲームパッド、タッチに対応 |
| 音声 | WAVと3D定位をWeb Audioで再生。圧縮音声はWAVへ変換 |
| 物理 | 基本的な重力、AABB衝突、トリガー、Box／Mesh Raycastに対応 |
| 画像 | WebPを使用。PNG、JPEG、GIF、BMP、DDS、TGA、TIFFは自動変換 |
| 3Dモデル | FBX、glTF、GLB、OBJなどをGLB 2.0へ自動変換 |
| HLSL、Direct3D、XAudio2、Win32 API | Web版では使用不可 |
| 高度なシェーダー、影、ポスト処理 | 未対応。機能に応じて警告または拒否 |
| `std::thread`／pthreads | 現在のプロファイルでは動作を保証しない |

`webgl2-basic-2d`は2D向け、`webgl2-basic-3d`は基本的な3D向けです。
WebGL2を優先し、対応状況に応じてWebGL1またはソフトウェア描画へ切り替えます。
ブラウザー、GPU、タブの状態、自動再生制限によってWindows版と結果が異なる場合があります。

WebGLの圧縮テクスチャはブラウザーとGPUによって対応状況が異なるため、Windows版の
BC圧縮データをそのまま使いません。詳しくは
[Khronos WebGL Extension Registry](https://registry.khronos.org/webgl/extensions/)を参照してください。
出力用のコピーだけをWebPへ変換し、プロジェクト内の原本は変更しません。

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

## アセット変換

変換は`.lamapon/web-generated-assets`の出力用コピーに対して行います。仮想パスと
拡張子は維持し、ブラウザー側はファイルシグネチャから実際の形式を判定します。
C++文字列やシーンJSONの参照を変更する必要はありません。

| 入力 | Web版の形式 | 変換ツール |
| --- | --- | --- |
| PNG、JPEG、GIF、BMP、DDS、TGA、TIF／TIFF | ロスレスWebP | ImageMagick |
| MP3、OGG、FLAC、M4A、AAC、WMA | PCM WAV | FFmpeg |
| FBX、glTF、GLB、OBJ、DAE、3DSなど | GLB 2.0 | Assimp |

TTF、OTF、WOFF、WOFF2は変換せずに埋め込みます。表示を端末間で揃える場合は、
OSのフォント名だけに依存せず、フォントアセットをプロジェクトへ含めてください。

## シーンコンポーネント

| 区分 | コンポーネント |
| --- | --- |
| 対応 | `NativeScript`、`Camera`、`DirectionalLight`（影を除く）、`AudioSource`／`AudioListener`、`MeshRenderer`、`ModelRenderer`、`BoxCollider3D`、`Rigidbody`の基本機能、`ParticleSystem`、`SpriteRenderer`／`SpriteMask`／`SpriteAnimator`、`TextRenderer`、`UIRectTransform`、`TransformAnimator`の直接クリップ、`Rotator`、`InputMover`、`ParallaxLayer`、`RenderCulling` |
| 拒否 | `PointLight`／`SpotLight`、`ReflectionProbe`、影、高度なCollider／Joint／`CharacterController`、NavMesh、LOD／Billboard、Tilemap、Light2D／2D Physics、`SpriteParticles2D`、`UIRectTransform`以外のUI Canvas系コンポーネント、RenderTexture、カスタムシェーダー、Animator Controller、Root Motion |

対応表にないコンポーネントも拒否します。対応を追加する場合は、ポータブル版の処理と
互換性テストを用意してから許可リストへ登録します。

## レポート

互換性検査が完了すると、`web-compatibility-report.json`へ結果を保存します。
互換性判定で拒否した場合も理由と対処方法を記録します。成功時には`web-export-manifest.json`も生成し、
入力のSHA-256、使用したレンダラー／プロファイル／モジュール、成果物のサイズと
SHA-256を記録します。

配布前にはHTMLのCanvasとスクリプト、空ファイル、単一HTMLから漏れた外部ファイル、
宣言モジュールと実際のリンク内容を検査します。
