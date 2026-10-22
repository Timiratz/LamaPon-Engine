# Live2D Cubism Runtime

Cubism Coreを使い、モデル3 JSON、MOC3、テクスチャを読み込み、2Dメッシュとして描画するプロジェクト拡張です。モデルのパラメータをC++ Scriptから変更できます。

このZIPにはLive2D Cubism SDK/Coreやモデルデータを含みません。利用者が公式配布SDKを用意して、プロジェクト内へ導入します。

## エディターから導入する

1. エディターの「拡張機能」→「Live2Dをはじめる...」を開き、「Live2D拡張をインストール」を押します。
2. ウィザードのリンクから[Live2D公式サイト](https://www.live2d.com/sdk/download/native/)を開き、利用条件を確認してCubism SDK for Nativeをダウンロード・展開します。
3. 「Cubism SDKの展開先を選ぶ...」で、展開後のSDKルートフォルダーを選択します。選んだフォルダー直下に`Core`があることを確認してください。
4. LamaPonがCoreヘッダー、Windows x64 DLL、MSVC `/MD` import library、描画APIを検査します。検査に通ったら「検査済みSDKをこのプロジェクトへ導入」を押します。必要なファイルの配置とネイティブ設定の更新後、Game Moduleをビルドします。
5. ビルドが終わったら「model3.jsonを選んでシーンへ追加...」からモデルを選びます。同じフォルダー内のMOC・テクスチャを含む素材一式が`assets/Live2D/`へコピーされ、モデルScriptを付けたGameObjectが現在のシーンに作られます。再生して表示を確認してください。

CoreはこのLamaPonパッケージから再配布しません。SDK/Coreの配布・利用条件はSDKに同梱された`LICENSE.md`と[Live2Dの公式案内](https://www.live2d.com/sdk/license/)で確認してください。

導入したSDKはパッケージ内の`sdk/`に保存され、Live2D拡張の更新後も引き継がれます。エディターからパッケージを作り直す場合も、このフォルダーはZIPから除外されるため、SDK/Coreが誤って配布されません。

選択したmodel3 JSONが参照するMOCまたはテクスチャが見つからない場合、素材のコピー前に不足を表示します。モデル3 JSONと参照ファイルは同じモデルフォルダー内へ置いてください。

## コマンドラインでSDKを導入する

エディターのウィザードを使わない場合は、SDKを展開して次のスクリプトを実行できます。

```powershell
powershell -ExecutionPolicy Bypass -File assets/packages/live2d-cubism/tools/SetupCubismSdk.ps1 `
  -SdkRoot "C:\path\to\CubismSdkForNative"
```

このスクリプトもCoreファイルを`assets/packages/live2d-cubism/sdk`へコピーし、マニフェストを更新します。完了後はLamaPonでGame Moduleを再ビルドしてください。

## モデルを表示する

ウィザードを使わない場合は、プロジェクトの`assets`へCubismモデルの`.model3.json`、`.moc3`、テクスチャを置き、GameObjectに`Game.Live2DModel`スクリプトを追加します。スクリプトのプロパティへモデル3 JSONのassets基準パスを設定します。

```json
{
  "modelPath": "Models/Haruka/Haruka.model3.json",
  "positionX": 640.0,
  "positionY": 360.0,
  "pixelsPerUnit": 0.0
}
```

`pixelsPerUnit`の0はCoreがモデルから読み取った値を使います。`positionX`と`positionY`は論理解像度上のモデル原点です。

同じGameObjectに`Game.Live2DParameterExample`を追加すると、`ParamAngleX`を時間に合わせて動かします。ゲームScriptからの利用例です。

```cpp
#include <LamaPonLive2D/Live2DModel.h>

void Update(float deltaTime) override
{
    auto* model = GetScript<LamaPonLive2D::Live2DModel>();
    if (model != nullptr)
    {
        model->SetParameter("ParamAngleX", 15.0f);
    }
}
```

APIは`LoadModel`、`SetPosition`、`SetPixelsPerUnit`、`SetParameter`、`GetParameter`を提供します。独自のモーションや表情を作る場合は各更新で該当パラメータを設定できます。

## 描画対応範囲

- Coreの頂点、UV、テクスチャ、可視状態、不透明度、描画順を使った2Dメッシュ描画に対応します。
- モデルのマスク、Multiply／Additive合成、Cubism 5.3のOffscreen描画は未対応です。これらを必要とするモデルは正確な表示になりません。
- Cubism Frameworkのモーション・物理・自動まばたき処理はまだ含めていません。
- 初版はWindows x64、MSVC、DirectX 11のC++ Game Moduleを対象とします。

## パッケージソースを更新する

リポジトリのパッケージソースを編集した後にZIPと索引を再生成します。

```powershell
python packages/src/build_package.py live2d-cubism --update-index
```

## ライセンス

このパッケージ内のLamaPon用アダプターはMIT Licenseです。`LICENSE`を参照してください。Cubism SDK/Coreとモデルデータには、それぞれの権利者の条件が適用されます。
