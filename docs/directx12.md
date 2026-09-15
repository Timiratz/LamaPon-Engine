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
| DirectX 12 Experimental | DirectX 12。初期化失敗時はDirectX 11 | DirectX 11 |

DirectX 12を選んだゲームでDevice、SwapChainなどの初期化に失敗した場合は、
DirectX 12の資源を解放してDirectX 11へ自動的にフォールバックします。
フォールバックした理由はログへ出力されます。

エディターとCLIは、Dear ImGuiやモデルプレビューを含む完全なエディター描画経路が
DirectX 12へ移植されるまでDirectX 11で起動します。プロジェクトの設定値は失われず、
書き出したゲームの起動時に使用されます。

## 現在の対応範囲

DirectX 12の書き出しゲームでは、次の主要機能を実際に描画できます。

- Cube、Sphere、Cylinder、Plane、Procedural Meshとワイヤーフレーム表示
- PBR Material、Ambient／Directional／Point／Spot Light、Forward+
- Directional／Spot／Point Lightの影
- glTF／GLB／FBX／CMO／SDKMESH／VBO Model
- スキニング、アニメーション、LOD、静的ModelとMeshのインスタンス描画
- PNG／JPEG、RGBA8／BGRA8／BC1／BC3／BC5の2D DDS、DDS cubemap、mip列
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

現在の目標は、書き出しゲームのユーザー向け描画機能をDirectX 11とDirectX 12の
どちらでも利用できる「runtime feature parity」です。主要なScene描画経路は移植済みで、
WARPによるD3D11／D3D12の画素比較とdebug layerを使った回帰テストを追加しています。

ただし、まだ「完全なD3D11互換」とは表記していません。残っている差は次のとおりです。

- エディターGUIとモデルプレビューはDirectX 11で動作します。
- D3D11の即時Contextを前提にした一部の低レベル描画入口は、D3D12のゲーム描画経路では
  使用せず、Backend固有の描画サービスへ置き換えています。すべての低レベル入口が
  一対一で利用できる状態ではありません。
- DDSのarray／volumeなど、一部のtexture種別と形式にはAPI間の対応差があります。
- GPU、driver、浮動小数点精度の違いにより、両APIの画素が常に完全一致するとは限りません。
- 回帰テストは主要経路を検証しますが、すべてのGPUとSceneの組み合わせを保証するものでは
  ありません。

互換性は「同じ実装を共有すること」ではなく、「同じプロジェクト設定とアセットから、
同じユーザー向け機能と意図した見た目を得られること」を基準に進めます。

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

プロジェクト設定とWindows書き出しの詳しい手順は
[プロジェクト管理とビルド](project.md)を参照してください。
