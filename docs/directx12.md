# DirectX 12 Experimental

LamaPonのWindows向け書き出しゲームは、プロジェクト設定からDirectX 11と
DirectX 12 Experimentalを選択できます。

DirectX 12は実際のゲーム描画に使用できますが、現時点ではExperimentalです。
既存プロジェクトの安全性を優先し、既定値と自動選択はDirectX 11のままです。

[← ドキュメント一覧へ戻る](index.md)

## DirectX 12を有効にする

1. エディターで「ファイル」→「プロジェクト設定とビルド...」を開きます。
2. 「Graphics」の「Rendering API」で「DirectX 12 Experimental」を選びます。
3. 設定を保存します。
4. エディターまたはゲームを再起動します。
5. Windows向けにゲームを書き出して起動します。

描画APIは実行中には切り替わりません。設定を変えると、再起動が必要であることを
Project Settingsに表示します。

設定は`.lamapon/project.json`の`graphics.renderingApi`へ保存されます。

```json
{
  "graphics": {
    "renderingApi": "DirectX12Experimental"
  }
}
```

指定できる値は`Auto`、`DirectX11`、`DirectX12Experimental`です。
古いプロジェクトで項目が無い場合と、不明な文字列が指定された場合は
DirectX 11として読み込みます。

## 起動時の選択とフォールバック

| 設定 | Windows書き出しゲーム | エディター／CLI |
|---|---|---|
| Auto | DirectX 11 | DirectX 11 |
| DirectX 11 | DirectX 11 | DirectX 11 |
| DirectX 12 Experimental | DirectX 12。初期化失敗時はDirectX 11 | DirectX 12。初期化失敗時はDirectX 11 |

DirectX 12を選んだゲーム、エディター、CLIでDevice、SwapChainなどの初期化に失敗した
場合は、DirectX 12の資源を解放してDirectX 11へ自動的にフォールバックします。
フォールバックした理由はログへ出力されます。

エンジンを直接組み込む場合も、`Application::Initialize`または
`GraphicsDevice::Initialize`へ`RenderingApi::DirectX12Experimental`を渡すだけで
DirectX 12を起動できます。初期実装時の`GraphicsStartupProfile`付きoverloadは
ソース互換用に残っていますが、現在は追加のopt-inとして指定する必要はありません。

エディターでは、Dear ImGui、Viewport／アセットのテクスチャ表示、モデルプレビュー、
gridやgizmoのデバッグライン、GPU ProfilerもDirectX 12で描画します。

## 現在の対応範囲

DirectX 12の書き出しゲームでは、次の主要機能を実際に描画できます。

- Cube、Sphere、Cylinder、Plane、Procedural Meshとワイヤーフレーム表示
- PBR Material、Ambient／Directional／Point／Spot Light、Forward+
- Directional／Spot／Point Lightの影
- glTF／GLB／FBX／CMO／SDKMESH／VBO Model
- スキニング、アニメーション、LOD、静的ModelとMeshのインスタンス描画
- PNG／JPEG、RGBA8／BGRA8／BGRX8、R8／RG8、16／32-bit float、
  BC1～BC7（sRGB・signed形式を含む）のDDS
  （2D、2D array、cubemap、cube array、volume）とmip列
- Sky、太陽円盤、IBL、Reflection Probe、照度ボリューム、Fog
- ParticleSystem、Sprite、Text、Imageなどの2D／UI
- HDR、SSAO、SSR、TAA、Volumetric Light、Depth of Field、Motion Blur
- Bloom、Screen Space Lens Flare、自動露出、Tone Mapping、Color Grading
- Screen Outline、FXAA
- Material、Sprite、Particle、ScreenEffect、ComputeEffectのcustom HLSL

custom HLSLの入口、利用できるregister、描画状態については
[カスタムShader](shaders.md)を参照してください。各グラフィック機能の設定方法は
[グラフィックス](graphics.md)で説明しています。

## D3D11互換について

書き出しゲームのユーザー向け描画機能をDirectX 11とDirectX 12のどちらでも利用できる
「runtime feature parity」には到達しています。主要なScene／Editor／CLI描画経路を移植し、
WARPによるD3D11／D3D12の画素比較、両APIでのresource生成、debug layerを使った
回帰テストを継続しています。

ただし、「すべてのD3D11低レベル呼び出しと一対一で同じ」という意味での完全互換では
ありません。DirectX 12 Experimentalのままにしている境界と差は次のとおりです。

- API非依存の動的頂点buffer更新・bindは両Backendで利用できます。D3D11 Effect向けの
  pixel shader resource直接bindだけは、D3D12ではroot signature固有のdescriptor tableを
  Backend固有の描画サービスからbindする設計へ置き換えており、一対一の低レベル入口では
  ありません。ユーザー向けのMaterial、Sprite、Particle、post-process、custom shaderは
  いずれもこのD3D12専用経路を使用します。
- DDSはUNORM／SNORM／sRGB、BC1～BC7、BC6H、16／32-bit float、
  R11G11B10／RGB9E5 HDR、packed RGB／YUY2と、DirectXTK11が認識する
  legacy headerを両APIで読みます。DX10 headerの対応typeless storageは、
  DirectXTK11と同じ既定のshader-readable形式へ正規化します。
  YUY2はD3D12 driver間の対応差を避けるため、読み込み時にRGBA8へ変換します。
  24-bit RGBやpalettized形式などDirectXTK11自体が拒否する形式は、両APIとも
  事前に`texconv`等で現在のDXGI形式へ変換する必要があります。
- GPU、driver、浮動小数点精度の違いにより、両APIの画素が常に完全一致するとは限りません。
- 回帰テストは主要経路を検証しますが、すべてのGPUとSceneの組み合わせを保証するものでは
  ありません。

互換性は「同じ実装を共有すること」ではなく、「同じプロジェクト設定とアセットから、
同じユーザー向け機能と意図した見た目を得られること」を基準に進めます。

## 検証の目安

変更時は少なくともD3D11／D3D12 WARPの描画回帰、Editor GUI、Backend選択、
書き出しゲーム起動、Game Module ABIのテストを実行します。Release構成の全ターゲットを
ビルドし、CTest全体も確認します。実機GPUでは、代表的なSceneについて影、半透明、
custom shader、post-process、Editor Viewportを両APIで見比べてください。

## 問題が起きた場合

1. 設定保存後にゲームを再起動したか確認します。
2. `.lamapon/project.json`の`renderingApi`が`DirectX12Experimental`か確認します。
3. GPU driverを更新し、ゲームのログでD3D12初期化エラーやフォールバック理由を確認します。
4. 「DirectX 11」へ戻して再起動し、API固有の問題か確認します。
5. GPUを利用できない環境では`--warp`を付けてWARPで起動します。

```powershell
.\MyGame.exe --warp
```

WARPはCPUで描画するため低速ですが、機能確認や問題の切り分けに利用できます。

## 任意バックエンドパッケージへの移行

将来、D3D12実装本体を通常のエンジン配布から分離して、必要なプロジェクトだけが
`directx12-renderer`パッケージとして導入できるようにするための互換性境界を用意して
います。パッケージの`package.json`は次の宣言を持ちます。

```json
{
  "name": "directx12-renderer",
  "version": "1.0.0",
  "minimumEngineVersion": "0.1.0",
  "activation": "Restart",
  "graphicsBackend": {
    "api": "DirectX12Experimental",
    "abiVersion": 1,
    "runtimeLibrary": "runtime/LamaPonGraphicsD3D12.dll"
  }
}
```

起動前の検査では、最低エンジンバージョン、バックエンドABI、DLLの存在、パッケージ
外を参照しない相対パスを確認します。不足・破損・非互換の場合はDLLをロードせず、
DirectX 11へ安全に戻せる結果を返します。インストールや更新は実行中にDLLを交換せず、
再起動後に反映します。

`LamaPonGraphicsD3D12.dll`は起動時に実際にロードされ、ABI versionと対応APIを検証
します。パッケージが存在するのに破損・非互換ならDirectX 11へフォールバックします。
物理分離の移行期間に既存プロジェクトを壊さないため、パッケージがまったく無い場合だけは
組み込みD3D12を引き続き利用します。

現段階ではD3D12描画処理本体はまだ`LamaPonRuntime`へ組み込まれており、パッケージDLLは
capability／ABIプロバイダーとして組み込み実装へ委譲します。次段階で同じABI境界へBackend
factoryを追加し、重い実装本体をDLLへ移します。プロジェクト固有のcustom HLSLは従来どおり
プロジェクト側でコンパイル・キャッシュし、共通バックエンドだけを配布対象にする方針です。

プロジェクト設定とWindows書き出しの詳しい手順は
[プロジェクト管理とビルド](project.md)を参照してください。
