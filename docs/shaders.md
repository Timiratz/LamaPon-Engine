# カスタムShader（HLSL）

HLSLで見た目を拡張する方法を説明します。
3Dマテリアル、2Dスプライト／パーティクル、画面全体のポストエフェクトに対応し、いずれも保存するだけで反映できます。

書けるシェーダーステージは**頂点・ピクセル・ハル・ドメイン・コンピュート**の5つです（ハルとドメイン＝[テセレーション](#テセレーションhsmaindsmain)、コンピュートは[ComputeEffect](#コンピュートシェーダーcomputeeffect)）。
ジオメトリシェーダーだけ未対応です。

[← ドキュメント一覧へ戻る](index.md)

**この文書の読み方**

| 知りたいこと | 見る場所 |
|---|---|
| すぐに試す | [1分でためす](#1分でためす3dマテリアルshader) |
| 使える変数・テクスチャ枠 | [使える入力](#3dマテリアルshaderで使える入力) |
| **Inspectorに名前付きの調整UIを出す** | [シェーダー宣言](#シェーダー宣言hlslへ書く設定) |
| **半透明・加算・両面描画にする** | [描画状態を指定する](#描画状態を指定する) |
| **マスクなどテクスチャを増やす** | [テクスチャを増やす](#テクスチャを増やす) |
| スプライトやパーティクルに使う | [2DカスタムShader](#2dカスタムshaderスプライトパーティクル) |
| 画面全体を加工する | [ScreenEffect](#画面全体のポストエフェクトscreeneffect) |
| 保存しても反映されない | [ホットリロード](#ホットリロードとエラー表示)／[よくあるつまずき](#よくあるつまずき) |

Shaderを共有Materialとして保存すれば、Prefabやパッケージで配布できます（マテリアル化の詳細は[グラフィックス](graphics.md)の「Litマテリアル」を参照してください）。

## 1分でためす（3DマテリアルShader）

1. Asset Browserのフォルダー内で右クリック →「**新規カスタムShader**」で
  名前を入力します。
  `VSMain`／`PSMain`を持つ編集可能な雛形（Shader Model 5.0）が作られます。
2. HLSLをダブルクリックしてコードエディターで開き、`PSMain`の返す色を
  少し変えて保存します。
  **実行中でも保存した瞬間に反映されます。**（どのエディターで開くかは「ファイル」→「プロジェクト設定とビルド...」→「スクリプト」で選べます。
  ）
3. HLSLファイルをMesh Renderer／Model RendererのInspectorへドラッグすると、
  そのMaterialのShaderになります。

InspectorからShaderへ値を渡すには「**カスタムShaderパラメーター**」を使います。
例えば雛形の`PSMain`の最後（returnの直前）に1行足すと、Inspectorで設定した色を混ぜられます。

```hlsl
// CustomParameters[0]のRGBへ、wの割合で近づける
// （Inspectorのカスタムパラメーター0で色と強さを操作）
color.rgb = lerp(color.rgb,
    CustomParameters[0].rgb,
    CustomParameters[0].w);
```

## 3DマテリアルShaderで使える入力

雛形と同じ`ObjectBuffer`（`b0`）レイアウトを使います。

| 変数 | 内容 |
|---|---|
| `World` / `ViewProjection` / `WorldInverseTranspose` | 変換行列 |
| `MaterialColor` | ベースカラー×アルファ |
| `CameraPosition` / `CameraForward` | カメラのワールド位置と向き |
| `MaterialParameters` | 粗さ等のマテリアル値 |
| `CustomParameters[8]` | Inspectorから渡す自由な`float4` |
| `MaterialTextureParameters` | PBRマップの有効フラグと遮蔽の強さ |
| `EmissiveParameters` | 発光色（`rgb`）と発光マップの有無（`w`） |

`MaterialTextureParameters`と`EmissiveParameters`は`ObjectBuffer`の**末尾**にあるので、これらの行を持たない既存の自作Shaderもそのまま動きます。

### レジスタの割り当て

どのスロットがエンジン用で、どこが自由に使えるかの一覧です。

| スロット | 用途 |
|---|---|
| `b0` | `ObjectBuffer`（上の表） |
| `b1` | ライティング定数バッファ（`LamaPonLit.hlsl`が正） |
| `b2` | `BoneBuffer`（スキニング用） |
| `t0` / `t1` | アルベド／法線マップ |
| `t2` | 影マップ（`Texture2DArray`） |
| `t3` | 環境マップ（キューブマップ） |
| `t4` / `t5` | スポット影／ポイント影 |
| `t6` | 放射照度（IBLの拡散） |
| **`t7`〜`t10`** | **自由に使える追加テクスチャ**（宣言で割り当て） |
| `t11` / `t12` | 粗さマップ（`.g`）／金属度マップ（`.b`） |
| `t13` | 遮蔽（AO）マップ（`.r`） |
| `t14` | 発光（emissive）マップ |
| `t15` | 画面空間の遮蔽（SSAO、`.r`）。下の「SSAOを受け取る」を参照 |
| `s0` / `s1` | 標準サンプラー／影比較用サンプラー |

自由枠が`t7`〜`t10`で途切れて`t11`からエンジンに戻るのは、自由枠が公開済みの取り決めだからです。
エンジンの追加分を`t7`以降へ詰めると、既存の自作Shaderが**別のテクスチャを黙って読む**ことになるため、番号の連続性より互換性を取っています。

### SSAOを受け取る

SSAO（[グラフィックス](graphics.md#ssao遮蔽による陰り)）は、ライティングの前に用意された遮蔽テクスチャを**Shader側が読んで環境光へ掛ける**方式です。
`LamaPonLit.hlsl`をそのまま使うMeshRenderer／ModelRendererには自動でかかりますが、自作Shaderは次の3行を足すと受け取れます。
読まなくてもエラーにはならず、その場合はそのオブジェクトに陰りがかからないだけです。

```hlsl
Texture2D ScreenAmbientOcclusionTexture : register(t15);

// LightingBuffer（b1）の末尾。x=1/画面幅, y=1/画面高さ, z=有効。
// 末尾に足されているので、この行が無い既存Shaderもそのまま動きます。
float4 ScreenAmbientOcclusionParameters;

// ピクセルシェーダーの中。UVはピクセル座標から作ります。
float occlusion = 1.0f;
if (ScreenAmbientOcclusionParameters.z >= 0.5f)
{
    occlusion = ScreenAmbientOcclusionTexture.Sample(
        MaterialSampler,
        input.Position.xy * ScreenAmbientOcclusionParameters.xy).r;
}
```

掛ける先は**環境光やIBLの項だけ**にしてください。
直接光へ掛けると影の中が二重に暗くなり、汚れのように見えます。

## シェーダー宣言（HLSLへ書く設定）

シェーダーのコメント内へ宣言を書くと、エディターとエンジンが読み取って**パラメーターの名前付け・テクスチャ枠の追加・描画状態の指定**ができます。
HLSLとしてはコメントなので、宣言を消しても動きます。

| 宣言 | 読み取るのは | できること |
|---|---|---|
| `LAMAPON_PROPERTIES` | **エディター**のみ | パラメーターに名前・型・範囲・既定値を付けて、Inspectorへ名前付きUIを出す。テクスチャ枠の追加もここ |
| `LAMAPON_RENDER_STATE` | **エンジン（実行時）** | 半透明・加算合成・カリング・深度書き込みの指定。エクスポートしたゲームでも同じ見た目になる |

### パラメーターに名前を付ける

シェーダーの中へ`LAMAPON_PROPERTIES`宣言を書くと、Inspectorが読み取って**名前付きの調整UI**を出します。
数字の意味がシェーダー自身に書かれるので、Materialとして保存して配っても、受け取った人がそのまま調整できます。

```hlsl
/* LAMAPON_PROPERTIES
[
  { "target": "0.rgb", "type": "color", "name": "着色",
    "default": [1.0, 1.0, 1.0] },
  { "target": "0.a", "type": "float", "name": "着色の強さ",
    "min": 0.0, "max": 1.0, "default": 0.0 },
  { "target": "2.xy", "type": "vector", "name": "UVの拡大",
    "default": [1.0, 1.0] }
]
*/
```

| 項目 | 内容 |
|---|---|
| `target` | `CustomParameters`の「番号.成分」。成分は`x/y/z/w`でも`r/g/b/a`でも書けます（`"3"`のように省略すると`x`） |
| `type` | `float`（1成分）／`color`（3または4成分のカラーピッカー）／`bool`（0か1）／`vector`（2〜4成分の数値入力）／`texture`（画像の割り当て枠） |
| `name` | Inspectorに出す表示名 |
| `min`／`max` | 両方書くとスライダー、省略すると数値入力 |
| `default` | 「既定値に戻す」で書き込む値。数値、真偽値、配列のいずれか |

- 解釈するのは**エディターだけ**です。シェーダーのコンパイル方法や実行時の
  動作は変わりません（コメント内に書くため、HLSLとしても無視されます）。
- **宣言が無いシェーダーは従来どおり**、生の`float4`を8本編集するUIになります。
  途中から宣言を足しても、消しても動きます。
- シェーダーを保存し直すと宣言も読み直されます（ホットリロードと同じ感覚）。
- 宣言に誤りがあるとInspectorへ理由を表示し、生のfloat4編集へ戻ります。

新規作成した`.hlsl`（雛形）には宣言の実例が入っているので、名前と範囲を書き換えるところから始められます。

### テクスチャを増やす

エンジンは`t0`（アルベド）、`t1`（法線）、`t2`（影）、`t3`（環境マップ）、`t4`（スポット影）、`t5`（ポイント影）、`t6`（放射照度）を使います。
**自作Shaderが自由に使える枠は`t7`〜`t10`の4枚**です。

> **`t1`（法線）を読むときの注意:** テクスチャ圧縮が有効だと法線マップはBC5で読み込まれ、**Bチャンネルは0が返ります**。`.xyz`をそのまま使わず、`.xy`から復元してください。非圧縮のRGBでも同じ値になるので、常にこの形で書いて問題ありません。
>
> ```hlsl
> float3 normal;
> normal.xy = NormalTexture.Sample(MaterialSampler, uv).xy * 2.0f - 1.0f;
> normal.z = sqrt(saturate(1.0f - dot(normal.xy, normal.xy)));
> ```
マスク、ランプ、詳細テクスチャなどに使えます。

```hlsl
Texture2D MaskTexture : register(t7);

/* LAMAPON_PROPERTIES
[
  { "target": "t7", "type": "texture", "name": "マスク" }
]
*/
```

宣言するとInspectorに割り当て枠が出て、画像をドロップするか「選択中の画像を割り当て」で設定できます。
**未設定の枠は白テクスチャ**が渡るので、シェーダー側で「設定されているか」を分岐する必要はありません（掛け算するだけで無効時は素通しになります）。

割り当てはMaterialにもRendererにも保存され、画像を移動・改名してもGUIDで参照が追従します。

ライト計算まで自分で行いたい場合は、エンジンのライティング定数バッファ（`b1`）を`LamaPonLit.hlsl`からコピーして使います。
その際は**配列サイズや末尾の`float4 ShadowTexelSizes;`まで正確に一致**させてください（エンジン更新でレイアウトは末尾へ伸びることがあり、`LamaPonLit.hlsl`が常に最新の正です）。

スキニング（ボーン）モデルにもカスタムShaderを使えます。
雛形の`VSSkinnedMain`がボーン変形（`BoneBuffer`、最大72ボーン）を行うので、キャラクターにも自作Shaderが当たります。
エンジンはスキニング用に別途コンパイルし、専用の入力レイアウトを作ります。
`VSSkinnedMain`を消すとそのモデルはDirectXTK描画へフォールバックします。

Shaderを指定していないモデルは標準の`LamaPonLit.hlsl`で描かれます（[グラフィックス](graphics.md)の「モデルのマテリアル取り込み」参照）。

スキニング時のピクセルシェーダーは`PSMain`ではなく`PSSkinnedMain`が呼ばれ、入力は`SkinnedPixelInput`です。
頂点変形はDirectXTKの`SkinnedEffect`が担当し、エンジンはピクセルシェーダーだけを差し替えるため、`PixelInput`とはメンバーの並び（セマンティクス）が違います。
雛形のとおり詰め替えてから共通の陰影関数へ渡してください。

### 描画状態を指定する

半透明や加算合成、両面描画、深度書き込みの有無を**Shader側から**指定できます。
こちらは実行時にも使われる情報なので、ゲームでも同じ見た目になります。

```hlsl
/* LAMAPON_RENDER_STATE
{ "blend": "additive", "cull": "none", "depthWrite": false }
*/
```

| 項目 | 値 |
|---|---|
| `blend` | `opaque`（既定）／`alpha`（ガラス・フェード）／`additive`（発光・エフェクト）／`premultiplied` |
| `cull` | `back`（既定・裏面を描かない）／`front`（内側から見せる箱）／`none`（板ポリゴンの草や旗） |
| `depthWrite` | 深度バッファへ書き込むか。**`blend`が`opaque`以外で省略すると自動で`false`** になります（半透明の重ね合わせで抜けを防ぐため） |
| `depthTest` | 深度テストをするか。`false`で常に手前へ描きます |

宣言が無いShaderは従来どおり（不透明・裏面カリング・深度書き込みあり、ベースカラーのアルファが1未満なら半透明扱い）です。

**GPUインスタンシングでも効きます。** 同じ形状・同じShader・同じテクスチャのMeshRendererは自動で1回のドローコールにまとめられますが（バッチのキーにShaderが含まれるため、まとめられた分の宣言は必ず同じです）、その場合も宣言した合成・カリング・深度が適用されます。
加算合成のエフェクトを大量に並べても1ドローコールで描けます。

**アルファ合成のMeshRendererは前後関係でソートされます。** 不透明を全部描いた後、カメラから遠い順に描くので、置いた順番によらず正しく重なります。
並べ替えの単位はGameObjectなので、1つのオブジェクトが自分自身と交差するような形（ねじれた板1枚など）の中までは並びません。

- **加算合成（`additive`）は並べ替えません。** 足し算は順番によらず結果が
  同じで、並べ替えるとインスタンシングでまとめられる利点だけを失うためです。
  加算のエフェクトは大量に置いても1ドローコールのままです
- アルファ合成のものは**インスタンシングの対象から外れます**（1つずつ順番に
  描く必要があるため）。
  大量に並べるなら、重なりが順序に依存しない`additive`を選んでください
- **インポートしたモデル（ModelRenderer）も対象です。** 半透明パーツを1つでも
  持つモデル（またはベースカラーのアルファを1未満にしたモデル）は、GameObjectごと遠い順の組へ回されます。
  モデルの中では従来どおり不透明パーツ→半透明パーツの順に描かれます。

粒度がGameObject単位なので、**不透明パーツも一緒に後回しになります**。
遠い順に描く中では手前の不透明が奥の半透明を正しく上書きするため絵は破綻しませんが、大部分が不透明でガラスが1枚だけ、というモデルでは早期深度判定の利きが落ちます。
気になる場合はガラスを別のGameObjectへ分けてください

**スキニング（ボーン）モデルでも効きます。** ワイヤーフレーム表示中だけはデバッグ優先で宣言を無視します。

## テセレーション（HSMain／DSMain）

平面をGPU側で細かく割って、頂点を動かせます。
波打つ水面や、近くだけ細かくする地形などに使います。

**`HSMain`（ハルシェーダー）と`DSMain`（ドメインシェーダー）を両方書くと有効になります。** 片方だけだと無視されます。
エンジンが探す入口の名前がこの2つなので、宣言も設定も要りません。

使える条件が2つあります。

- **四角パッチに割れる形状のMesh Rendererだけ**です。今は**Plane**と
  **Cube**（6面をそれぞれ1枚のパッチにします）。
  SphereとCylinder、読み込んだモデルでは効きません（割り当てるとInspectorへ理由が出て、マゼンタの代役で描かれます）
- **変位は面の法線方向**へ入れてください。見本のように制御点から
  `uAxis`／`vAxis`を作って`cross`すれば、Planeでも Cube の各面でも同じコードで正しい向きになります。
  `+Y`固定で書くと、Planeでは正しくてもCubeの側面と底面で横倒しになります
- `LAMAPON_RENDER_STATE`の指定が**効きます**。不透明で深度を書く地形も、
  半透明の水面も作れます。
  宣言が無いときは、通常のメッシュと同じ既定（ベースカラーのアルファが1未満なら半透明）に、両面描画を足したものになります

同梱の見本が`assets/shaders/LamaPonTessellatedTerrain.hlsl`です。
Plane形状のMesh Rendererへドラッグすれば、そのまま波打つ不透明な地面になります（起伏の高さ・細かさ・分割数はInspectorから）。
Cubeへ入れると、6面がそれぞれ外向きに波打ちます。

> **分割数と細かさを割り切れる値にしない。** 例えば分割数8で細かさ4にすると、
> `sin`の周期が生成される頂点の位置とちょうど揃い、**高さが全頂点で0**になります。
> 陰影（傾き）だけは変わるので、「効いているのに形が変わらない」という状態になります。

4つの制御点を持つパッチとして描かれるので、`HSMain`は`InputPatch<..., 4>`を受け取り、`DSMain`は`domain("quad")`になります。
分割数はハルシェーダーのパッチ定数関数で決めるので、カメラからの距離で変える（近いところだけ細かくする）といった調整もそこで行います。

`HSMain`／`DSMain`へ渡っているのは`b0`（`ObjectBuffer`。
変換行列、`MaterialColor`、`CustomParameters`）と`b3`だけです。
**ライトの定数（`b1`）は渡っていません**ので、ライティングは`PSMain`で行ってください。
頂点を動かす材料（時間、波の高さ、分割数）は`CustomParameters`から渡すのが素直です。

## ジオメトリシェーダー（GSMain）

三角形を1枚ずつ受け取って、増やしたり動かしたりできます。
面ごとの押し出し、板ポリの生成、法線の可視化などに使います。

**`GSMain`を書くと有効になります。** 宣言も設定も要りません。
流れは`VSMain` → `GSMain` → `PSMain` です。
テセレーションと組み合わせた場合は、ドメインシェーダーの出力（三角形）が`GSMain`へ入ります。

```hlsl
[maxvertexcount(3)]
void GSMain(
    triangle GeometryInput input[3],
    inout TriangleStream<GeometryInput> stream)
{
    // input[0..2] が1枚の三角形。stream.Append で出力します。
}
```

使える条件と注意点。

- **入力は`triangle`だけ**です。エンジンが流すのは三角形しかないので、
  `point`や`line`を宣言したものは**コンパイルの時点で断ります**（束ねてしまうとD3D11では不正な描画になり、環境によってはドライバーごと落ちるためです）。
  理由はInspectorのShaderエラー欄に出ます
- **`maxvertexcount`より多く出さないでください。** 超えた分は黙って
  捨てられ、形が途中で欠けます
- **スキニングモデル（ボーン付き）では使えません。** 頂点シェーダーが
  DirectXTKのものになるため、`GSMain`が期待する入力と合いません（割り当てても無視されます）
- 輪郭（アウトライン）と遮蔽表示のパスでは束ねません。専用の頂点
  シェーダーで描くので、入力が合わないためです
- **影も`GSMain`を通った形で落ちます。** 深度パスでも同じように
  束ねているので、本体と影がずれません
- 渡っているのは`b0`（`ObjectBuffer`）と`b3`です。テセレーションと
  同じで、**ライトの定数（`b1`）は渡っていません**

同梱の見本が`assets/shaders/LamaPonGeometryExplode.hlsl`です。
三角形を面の向きへ押し出してばらけさせます（押し出す量・縮める量はInspectorから）。

## 2DカスタムShader（スプライト／パーティクル）

Sprite RendererとParticle SystemはInspectorへHLSLをドラッグすると、**ピクセルシェーダーだけ**を差し替えられます（頂点処理はそのまま、エントリポイントは`PSMain`のみ）。
8本の`float4`パラメーターとエラー表示、ホットリロードに対応します。

そのまま使える最小の例（Inspectorのパラメーター0のxを上げると白くフラッシュ）:

```hlsl
Texture2D SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s0);

cbuffer SpriteParameters : register(b0)
{
    float4 CustomParameters[8];
};

float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    float4 pixel =
        SpriteTexture.Sample(SpriteSampler, uv) * color;
    pixel.rgb = lerp(
        pixel.rgb,
        float3(1.0, 1.0, 1.0),
        CustomParameters[0].x);
    return pixel;
}
```

- **引数の並びは`COLOR0` → `TEXCOORD0` → `SV_Position`の順にしてください。**
  差し替えているのはピクセルシェーダーだけで、頂点シェーダーはSpriteBatchのものがこの順で出力します。
  ピクセルシェーダーの引数は宣言順に入力レジスタへ割り当てられるため、`SV_Position`を先頭に書くと**1本ずつずれます**（`color`にUVが入り、`uv`は未定義になる）。
  **エラーも警告も出ず、値だけが静かに壊れます。** 「なぜか色が変」「UVが効かない」ときは、まずこの並びを確認してください。
- スプライトでは`CustomParameters[5]`（tint）、`[6]`（描画矩形: left,
  top, width, height）、`[7]`（UIビューポート: width, height,テクスチャwidth, height）は**エンジンが毎描画設定**します。
  自由に使えるのは`[0]`～`[4]`です。
  色とUVは上の並びで頂点から受け取れるので、`[5]`/`[6]`は画面座標を使いたいときの補助と考えてください。
- Particle SystemはInspectorで**補助テクスチャ**を1枚割り当てられ、
  `t1`から読めます（未設定時は白）。
  ノイズやマスクに便利です。
- Shaderパスとパラメーターはシーンへ保存され、アセットの改名・移動にも
  参照が追従します。

## ノイズ関数

`assets/shaders/LamaPonNoise.hlsli` を `#include` すると、5種類のノイズが使えます。
**C++側の `LamaPon::Noise`（`LamaPon/Core/Noise.h`）と同じ値を返す**ので、C++で地形メッシュを作り、シェーダーで同じノイズから草の分布や色を決める、という組み合わせが成立します。

```hlsl
#include "LamaPonNoise.hlsli"
float height = LamaPonFractalNoise2D(uv * 8.0f, 5, 2.0f, 0.5f);
```

```cpp
const float height =
    LamaPon::Noise::FractalValue2D(x * 0.05f, z * 0.05f, 5);
```

| 関数 | 見た目 | 向いている用途 |
|---|---|---|
| `LamaPonValueNoise2D` / 1D / 3D | 滑らかな斑 | 基本。軽い揺らぎ |
| `LamaPonPerlinNoise2D` | うねる有機的な帯 | 岩肌、水面、風 |
| `LamaPonFractalNoise2D` | 雲 | 地形の高さ、雲、煙 |
| `LamaPonWorleyNoise2D` | セル状の粒 | 石畳、ひび割れ、泡、鱗 |
| `LamaPonCurlNoise2D` | 渦（発散のない流れ） | 煙・水のパーティクル |

**乱数との違い**: 乱数は隣の座標でも値が飛びますが、ノイズは近い座標なら近い値を返します。
この連続性が「自然なムラ」を作ります。

`assets/shaders/LamaPonNoiseSample.hlsl` が見本です。
マテリアルへ割り当てるとInspectorから種類・大きさ・重ねる回数・流れる速さを触れます。

## 水面 — 同梱の見本

`assets/shaders/LamaPonWater.hlsl` を平らな板メッシュのマテリアルへ割り当てるだけで、波が立って空と太陽を映す水面になります。
スクリプトは要りません。

中身は4段だけです。
①ノイズを2枚重ねて波の高さを作る ②高さの傾きから法線を出す（隣を測って引き算するだけ） ③見る角度で映り込みの強さを決める（フレネル）④空の色を映して太陽をGGXで反射する。

**太陽はシーンのDirectional Lightから読んでいます。** ライトを回すと光の帯も動きます。
太陽の「見かけの大きさ」も一緒に読むので、[朝昼夜](graphics.md)と組み合わせると夕日が水面に伸びます。

変更するときは次の3点に注意してください。

- **法線を出すときの測る間隔は、さざ波1つぶんの5%くらいにする。** 粗いと波の山と
  谷をまたいでしまい、傾きが平均化されて鏡のような一枚板になります。
  波の高さが変わっていても、見た目は平らになります。
- **「水面のざらつき」を0に近づけない。** ここは画素より細かくて描き切れない
  さざ波の代わりです。
  完全な鏡にすると、太陽の0.53度は波の散らばり（±40度ほど）に対して狭すぎて、光の帯が数画素へ落ちるか丸ごと消えます。
- **両面描画（`cull: none`）なので、法線を見ている側へ向け直してから使う。**
  裏を向いたままだと、空の映り込みは下向きを引いてずっと地平の色になり、太陽は「水平線の下」と判定されて反射が丸ごと消えます。

## 経過時間

`ObjectBuffer`の末尾に `float4 TimeParameters` を宣言すると、エンジンが毎描画入れた値を読めます（x=秒、y=前フレームからの秒数、z=フレーム数）。
揺れる・流れる・点滅するといった表現に、スクリプトを1本も書かずに済みます。

```hlsl
cbuffer ObjectBuffer : register(b0)
{
    // …（既存の宣言をそのまま）…
    float4 CustomParameters[8];
    float4 MaterialTextureParameters;
    float4 EmissiveParameters;
    float4 TimeParameters;   // x=秒
};
```

秒は**1時間で巻き戻ります**。
floatの仮数は24ビットしかなく、起動から数時間そのまま渡すと下位が丸められて、波のような高い周波数の動きがカクつき始めるためです。

## バリアント（#pragma multi_compile）

キーワードの組み合わせごとに、**別々のシェーダーとしてコンパイル**できます。
実行時の`if`分岐が消えるので速くなります。

```hlsl
#pragma multi_compile _ FOG_ON
#pragma shader_feature _ DETAIL_ON

float4 PSMain(PixelInput input) : SV_Target
{
    float3 color = Shade(input);
#if defined(FOG_ON)
    color = ApplyFog(color, input.WorldPosition);
#endif
    return float4(color, 1.0f);
}
```

- **1行が1グループで、そこから必ず1つが選ばれます。** `_`は「どのキーワードも
  立てない」を表します。
- 選ばれたキーワードは `#define FOG_ON 1` として渡ります
- Inspectorのマテリアル欄に**チェックボックスが自動で出ます**。同じグループの
  ものは排他で、切り替えると対応するバリアントへ差し替わります
- コードからは `SetShaderKeywords` ／ `EnableShaderKeyword` ／
  `DisableShaderKeyword` で操作できます
- **宣言に無いキーワードは無視されます。** シェーダーを差し替えた後の
  マテリアルが、存在しないキーワードでコンパイルを走らせることはありません

`multi_compile`と`shader_feature`の違いは**書き出し時だけ**です。
エディターでの挙動は同じです。

| | 書き出しに入るもの |
|---|---|
| `multi_compile` | **全組み合わせ**。スクリプトから動的に切り替えるものはこちら |
| `shader_feature` | **どこかのマテリアルが実際に使っている組み合わせだけ** |

`shader_feature`で落とした数は書き出しのログへ出ます。
黙って減らすと、「同梱したはずのバリアントが実行時にコンパイルされている」理由を後から追えなくなるためです。

落としたバリアントを実行時に使っても**動きます**（その場でコンパイルされ、少し遅くなるだけ）。
マテリアルに保存せずスクリプトだけで立てるキーワードは`multi_compile`にしてください。

### 組み合わせ数の上限（バリアント爆発）

**`multi_compile`を1行足すたびに組み合わせは倍になります。** 10行書けば1024通り、そのすべてが書き出しのコンパイル時間と配布サイズになります。
バリアント数が増えすぎると、コンパイル時間と配布サイズが大きくなります。

このエンジンは**64通りを上限**にしています。
超える宣言は読み取り時に弾かれ、Inspectorへ理由が出ます（そのシェーダーはキーワード無しの1本として扱われます）。
後から気付くと直すのが大変なので、最初から止めています。

## コンパイルはいつ走るか（キャッシュ）

シェーダーはHLSLのまま配布され、**実行時にコンパイル**されます。
だからホットリロードができ、自作Shaderをテキストで書けます。

コンパイル結果は`%LOCALAPPDATA%\LamaPon\shader-cache`へ保存され、2回目以降は読むだけで済みます。
実測では **3,484ms → 25ms**（53本、検証用VM）。

- **キーはHLSL本体と`#include`した全ファイルの中身のハッシュです。** 1文字でも
  直せば自動的に作り直されるので、ホットリロードは今までどおりです
- **コンパイル失敗も覚えます。** エンジンは`VSOutline`や`HSMain`のような
  「あれば使う」入口を毎回試すので、持っていないシェーダーでは失敗ぶんの時間を毎回払っていました。
  直せばハッシュが変わって試し直されます
- **消しても壊れません。** 次回に作り直されるだけです。おかしいと思ったら
  フォルダーごと消してください
- **書き出したゲームには、コンパイル済みのものが同梱されます**（配布フォルダーの
  `shader-cache`）。
  プレイヤーの初回起動でもコンパイルは走りません。
  書き出し時に自動で作られるので、こちらでする作業はありません

## 配布物からソースを外す

「ファイル」→「プロジェクト設定とビルド…」の**「書き出しでHLSLソースを外す」**を入れると、配布物には**コンパイル済みのバイトコードだけ**が入ります。
**新しいプロジェクトでは既定でオン**です（それ以前に作ったプロジェクトは、保存されている値のままなので、必要なら入れてください）。
なお、同梱するバイトコードは書き出し時に暗号化されます（[書き出したゲームの保護](export-protection.md)）。

- 既定は**オフ**です。オフのままなら配布物にHLSLも入ります
- 入れると事前コンパイルは**全バリアント**を焼きます。`shader_feature`の
  ストリップは止まります。
  両方やると、取りこぼした組み合わせを実行時に作り直せず（ソースが無いので）標準Litへ落ちてしまうためです
- 配布先で自作Shaderを差し替える余地は無くなります（それが狙いでもあります）
- 書き出しに少し時間がかかるようになります

**仕組みの補足**: ふだんのキャッシュのキーは「HLSLソースの中身のハッシュ」です。
ソースが手元にあれば1文字の違いも見分けられて都合が良いのですが、ソースを外すとキーそのものが計算できません。
そこで書き出し時に「パス＋入口＋ターゲット＋キーワード」から引ける索引（`shader-cache/index.txt`）も一緒に置いています。
実行時はまずソースからキーを作り、ソースが無ければこの索引を見ます。

## コンパイル中に止まらない（非同期）

エディターでは**シェーダーのコンパイルを裏で行います**。
出来上がるまでは標準Litで描き、焼き上がった次のフレームで本来のシェーダーへ差し替わります。
コンパイル中は代用シェーダーで描画し、完了後に本来のシェーダーへ切り替えます。

- Inspectorに「Shaderをコンパイル中…」と出ます
- 書き出したゲームでは**常に同期**です。全部事前コンパイル済みなので
  待ち時間が無く、待つ理由がありません
- **1フレームだけ描いて画像を保存する用途では切ってください**。
  焼き上がる前の絵（標準Lit）を撮ってしまいます。
  `GraphicsDevice::SetAsyncShaderCompilationEnabled(false)` で同期になります（エンジンの描画回帰テストはこれを使っています）

## シーンのライトを自作Shaderから読む

`b1` にはエンジンがライトの定数バッファを入れています。
`LamaPonLit.hlsl` の`LightingBuffer` と**同じ並びで、必要なところまで宣言して止める**と読めます。

```hlsl
struct DirectionalLight
{
    float4 DirectionIntensity;  // xyz=光が進む向き, w=強さ
    float4 Color;               // rgb=色, w=太陽の角半径（ラジアン）
};

cbuffer LightingBuffer : register(b1)
{
    float4 Ambient;
    uint4 LightCounts;                  // x=方向光の数
    DirectionalLight DirectionalLights[4];
};                                      // ここで止めてよい
```

**実装で気をつけていること**（改造するとき用）: ハッシュは整数演算です。
`frac(sin(dot(...)))` の定番手法は環境によって精度が違い、CPUと一致しません。
0〜1への変換は上位24ビット÷16777216（floatの仮数に収める）、補間は5次のスムーズステップ（3次だと格子線が見える）。
片方を直したら必ず両方直してください（テストが代表点での一致を検査します）。

## レトロ3D（PS1風）— 同梱の見本

`assets/shaders/LamaPonRetro3D.hlsl` を3Dオブジェクトのマテリアルへ割り当てると、当時のハードウェアが「できなかったこと」を再現できます。
Inspectorから9項目を調整できます。

| 項目 | 何が起きるか |
|---|---|
| テクスチャの泳ぎ | 0で現代の正しい遠近、1で当時どおり |
| 頂点のカクつき／格子数 | 頂点を格子へ丸める。数値が小さいほど粗い |
| 色の段数／ディザの強さ | 色数を落として網目状に混色 |
| テクスチャを補間しない | ドット感（ポイントサンプリング） |

肝は1行です。
UVを渡す変数へ `noperspective` を付けると、GPUが遠近補正をやめて画面空間の線形補間になります。
これがPS1の挙動そのものです。

```hlsl
noperspective float2 AffineTexCoord : TEXCOORD2;
```

**使用時の設定**

- **テクスチャを設定してください。** UVを歪ませるため、模様のない
  マテリアルでは変化を確認できません
- **床は格子状に分割してください。** Plane1枚の巨大な床を浅い角度で
  見ると遠近の歪みが大きくなります。小さいPlaneへ分割すると、
  三角形ごとの歪みを抑えられます
- 解像度自体を落としたいときは、品質設定の「レンダースケール」を
  下げてください（このShaderとは独立に効きます）

**制約**: スキニングモデル（人物など）にはUVの歪みを適用しません。
DirectXTKの頂点シェーダー出力の並びに合わせる必要があり、補間指定を変えられないためです（色の量子化と陰影は効きます）。
静的なモデル、地形、建物には適用できます。深度判定には通常の
深度バッファを使用します。

## 画面全体のポストエフェクト（ScreenEffect）

画面全体を入力テクスチャとして受け取る任意のHLSLを、3D合成へ差し込めます。
C++から**毎フレーム**`QueueScreenEffect`を呼びます（登録はそのフレーム限りなので、Updateで呼び続けます）。

```cpp
void Update(float deltaTime) override
{
    m_time += deltaTime;
    LamaPon::ScreenEffectRequest effect;
    effect.shader = "shaders/sepia.hlsl"; // assets内の相対パス
    effect.customParameters[0] = { 0.8f, 0.0f, 0.0f, 0.0f };
    std::string error;
    if (!Graphics().QueueScreenEffect(effect, nullptr, &error))
    {
        // errorにコンパイルエラー等が入ります
    }
}
```

### どこへ差し込むか（`point`）

`ScreenEffectRequest::point`で、ポスト処理のどこへ入れるかを選べます。
**既定は`AfterToneMapping`**で、指定しなければ従来と同じ位置です。

| `ScreenEffectPoint` | 位置 | 色 | 向いている用途 |
|---|---|---|---|
| `BeforePostProcess` | 3Dを描いた直後（TAAより前） | HDR | 3Dへ足したい光。時間方向にも均されます |
| `BeforeBloom` | 被写界深度・モーションブラーの後 | HDR | **光らせたいもの**。Bloomが滲ませます |
| `BeforeToneMapping` | Bloom・レンズフレアの後 | HDR | 露出の測定に含めたい明るさ |
| `AfterToneMapping` | トーンマップ後（既定） | LDR | 色調整、UI風の重ね描き、ビネット |

```cpp
effect.point = LamaPon::ScreenEffectPoint::BeforeBloom;
```

前へ置くほど「後ろのパスの材料になる」効果になります。
例えば同じ発光を`BeforeBloom`へ置くと滲み、`AfterToneMapping`へ置くと滲みません。
**HDRの地点では1より大きい値を書けます**（`AfterToneMapping`では1で頭打ちです）。

複数を別々の地点へ同時に積めます。
同じ地点のものは`Queue`した順にかかります。

HLSL側は`VSMain`と`PSMain`を持ち、フルスクリーン三角形として実行されます。
`t0`が描画済みの画面、`t1`／`t2`が`ScreenEffectRequest::auxiliaryTextures`（未設定時は白）、`t3`が**シーンの深度**です（下の「深度を読む」を参照）。

```hlsl
cbuffer ScreenParameters : register(b0)
{
    float4 CustomParameters[8];
    float4 ScreenSize; // xy = 画面の幅・高さ
};

Texture2D SceneTexture : register(t0);
SamplerState SceneSampler : register(s0);

struct VertexOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    // 3頂点で画面全体を覆う定番のフルスクリーン三角形
    VertexOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position =
        float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    output.TexCoord = uv;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target
{
    float4 color =
        SceneTexture.Sample(SceneSampler, input.TexCoord);
    // CustomParameters[0].xの割合でセピア化する例
    float3 sepia = float3(
        dot(color.rgb, float3(0.393, 0.769, 0.189)),
        dot(color.rgb, float3(0.349, 0.686, 0.168)),
        dot(color.rgb, float3(0.272, 0.534, 0.131)));
    color.rgb = lerp(color.rgb, sepia, CustomParameters[0].x);
    return color;
}
```

複数回Queueすると登録順に連結され、前の結果が次の入力になります。
2D／UIはポストエフェクトの**後**に合成されるため、ScreenEffectの影響を受けません（UIの色はそのまま保たれます）。

### 深度を読む（アウトライン・自作フォグなど）

`t3`にシーンの深度が刺さっています。
**宣言するだけ**で使えますHLSL側で深度を読むための仕組みです。
C++側の追加設定は要りません。
距離へ直す係数は`ScreenParameters`の**末尾**にある`DepthParameters`で、`x`が射影の`_33`、`y`が`_43`、`z`は深度が有効なら1です。
この行を持たない既存のShaderもそのまま動きます。

```hlsl
cbuffer ScreenParameters : register(b0)
{
    float4 CustomParameters[8];
    float4 ScreenSize;
    float4 DepthParameters; // 末尾へ足すこと
};

Texture2D SceneDepthTexture : register(t3);

// カメラからの距離（メートル）。式はエンジン内部のSSRと同じもの。
float SceneDistance(int2 pixel)
{
    const float deviceDepth =
        SceneDepthTexture.Load(int3(pixel, 0)).r;
    const float denominator = deviceDepth + DepthParameters.x;
    // 何も描かれていない遠平面は0除算になるので逃がします。
    return denominator > -1e-6f
        ? 1e6f
        : DepthParameters.y / denominator;
}
```

ピクセル座標は`input.Position.xy`をそのまま`int2`にします。
`SV_Position`は画面のピクセル座標なので、UVへ直す必要はありません。

### 共有ヘルパー（距離・法線）

上の式は自分で書かなくても、共有実装を取り込めば使えます。
エンジン内部のSSAOも同じファイルを使っているので、**式が食い違う心配がありません**。

```hlsl
#include "LamaPonScreenDepth.hlsli"

// cbufferには DepthParameters に続けて DepthUnprojection も宣言します
//   float4 DepthParameters;   // x=_33, y=_43, z=有効なら1
//   float4 DepthUnprojection; // x=1/_11, y=1/_22（法線の再構成用）

const int2 pixel = int2(input.Position.xy);
const float deviceDepth =
    SceneDepthTexture.Load(int3(pixel, 0)).r;

// カメラからの距離（メートル）
const float distance =
    LamaPonSceneDistance(deviceDepth, DepthParameters);

// ビュー空間の法線（カメラを向いている面は -z）
const float3 normal = LamaPonReconstructViewNormal(
    SceneDepthTexture,
    pixel,
    ScreenSize.zw,
    DepthParameters,
    DepthUnprojection);
```

法線は**深度から組み立てています**（専用のバッファは持っていません）。
上下左右のうち奥行きの段差が小さい側を選んでから外積を取るので、`ddx`/`ddy`の面法線と違って曲面と輪郭に強い代わりに、法線マップの細かい凹凸は出ません。

アウトラインなら、隣の画素との距離差が大きいところを線にします。

```hlsl
float4 PSMain(VertexOutput input) : SV_Target
{
    float4 color = SceneTexture.Sample(SceneSampler, input.TexCoord);
    if (DepthParameters.z < 0.5f)
    {
        return color; // 深度が無い環境では素通し
    }
    const int2 pixel = int2(input.Position.xy);
    const float here = SceneDistance(pixel);
    const float dx = abs(here - SceneDistance(pixel + int2(1, 0)));
    const float dy = abs(here - SceneDistance(pixel + int2(0, 1)));
    // しきい値は距離に比例させます。遠くなるほど1画素あたりの
    // 奥行き差が大きくなるので、固定値だと遠景が線だらけになります。
    const float edge = step(here * 0.02f, max(dx, dy));
    return float4(lerp(color.rgb, float3(0, 0, 0), edge), color.a);
}
```

**`DepthParameters.z`は必ず見てください。** 深度が用意できない経路（プローブのベイク中など）では0になります。
見ずに読むと、深度0＝ニアクリップ面として扱われ、**画面全体が「一番手前」になります**。

深度は**2D／UIが合成される前**のものなので、3Dだけが写っています。

## コンピュートシェーダー（ComputeEffect）

自作のCompute Shaderを走らせて、結果を**名前付きテクスチャ**へ書けます。
書いた絵はSprite RendererやUI Imageの「レンダーテクスチャ」に同じ名前を入れるだけで表示できます（カメラが描いたレンダーテクスチャと同じ登録簿に入るため、表示側の設定は変わりません）。

ピクセルシェーダーとの違いは「1画素につき1回」ではなく「1スレッドにつき1回」で走ることです。
周りの画素をまとめて読む処理（ぼかし、ヒストグラム、粒子の位置更新）が書けます。

```cpp
void Start() override
{
    LamaPon::ComputeEffectRequest request;
    request.shader = "shaders/my-compute.hlsl"; // CSMainを持つHLSL
    request.outputTexture = "noiseField";       // 表示に使う名前
    request.outputWidth = 256;
    request.outputHeight = 256;
    request.customParameters[0] = { 1.0f, 0.0f, 0.0f, 0.0f };
    std::string error;
    if (!Graphics().DispatchComputeEffect(request, &error))
    {
        // errorにコンパイルエラー等が入ります
    }
}
```

ScreenEffectと違って**その場で1回走ります**（毎フレーム呼ぶ必要はありません）。
書き込み先が画面ではなくテクスチャなので、ポスト処理の並びとは無関係だからです。
毎フレーム更新したいならUpdateで呼んでください。

HLSL側の雛形です。

```hlsl
cbuffer ComputeParameters : register(b0)
{
    float4 CustomParameters[8];
    float4 OutputSize; // xy=幅と高さ, zw=その逆数
};

Texture2D InputTexture0 : register(t0);
Texture2D InputTexture1 : register(t1);
SamplerState InputSampler : register(s0);
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // 出力の外へはみ出したスレッドは捨てます。8の倍数でない
    // 大きさだと必ず来るので、この2行は消さないでください。
    if (id.x >= (uint)OutputSize.x || id.y >= (uint)OutputSize.y)
    {
        return;
    }
    OutputTexture[id.xy] = float4(
        (float)id.x * OutputSize.z, 0.0f, 0.0f, 1.0f);
}
```

- スレッドグループは**8x8固定**です。`[numthreads(8, 8, 1)]`と書いてください
- `inputTextures[0]`／`[1]`にassets内のパスを入れると`t0`／`t1`へ入ります
  （未設定なら白）
- 出力の形式は`R16G16B16A16_FLOAT`なので、1.0を超える値も保てます
- **同じディスパッチの中で、他のスレッドが書いた画素を読まないでください。**
  スレッドグループの実行順は決まっていないので、読むと結果が毎回変わります。
  2段階の処理が要るなら`DispatchComputeEffect`を2回呼び、1回目の出力を2回目の`inputTextures`へ渡してください
- 保存すると作り直されます（他の系統と同じホットリロード）

## ホットリロードとエラー表示

3系統とも、HLSLを保存すると実行中でも自動で再コンパイルされます。
コンパイルに失敗しても、Inspector（ScreenEffectは`error`引数）へエラーを表示しながら描画は続きます。
ゲームが止まることはありません。

Shaderがコンパイルできなかったときは、**そこがマゼンタ（明るい紫）で描かれます。** ファイルが見つからないとき、書き間違えたとき、どちらも同じ色です。
3Dマテリアルはオブジェクトごと、2D（スプライト／UI／パーティクル）は元の絵の形を保ったまま塗り替わります。

普通のShaderで描き続けると、「動いているように見えて実は壊れている」状態になります。
Inspectorを開かなければ気付けないので、**見ただけで分かる色**にしています。
直して保存すれば、次のフレームで元の見た目へ戻ります。

2つだけ例外があります。
**コンパイル中**（非同期。
下記）は標準Litで描かれます。
焼き上がるまでの短い間だからです。
そして**画面全体のポストエフェクト（ScreenEffect）はその効果を飛ばすだけ**です。
画面をマゼンタで覆うと何も見えなくなり、直しようがなくなるためで、失敗は`error`引数で受け取ってください。

## よくあるつまずき

- **マゼンタ（明るい紫）になった** — そのShaderがコンパイルできて
  いません。
  InspectorのShaderエラー欄に理由が出ています。
  ファイルを消した・移動した場合も同じ色になります。
  エラー欄の**1行目は日本語の説明**で、その下にHLSLコンパイラの元のメッセージ（行番号付き）が続きます。
- **`entrypoint not found` と出る** — 割り当て先が求める入口が
  ありません。
  よくあるのは2D用のShader（`PSMain`だけ）を3Dマテリアルへ入れた場合です。
  エラー欄の説明がどちらのつもりのShaderかを教えてくれます。
- **編集しても見た目が変わらない** — マゼンタになっていなければ
  コンパイルは通っています。
  書いたつもりの分岐へ入っていない（キーワードの綴り違いなど）か、そもそも別のマテリアルを見ている可能性を先に疑ってください。
- **エントリポイント名を変えたら動かない** — `VSMain`／`PSMain`は
  固定名です（2Dスプライト／パーティクルは`PSMain`のみ）。
- **スキニングモデルだけ `VSSkinnedMain` が無いと言われる** — その
  Shaderは3D用としては正しく書けていて、割り当て先がボーン付きのモデルだった、という意味です。
  `VSSkinnedMain`と`PSSkinnedMain`を雛形から写すと、そのモデルにも使えます。
- **`missing semantics` と出る** — 戻り値や引数の役割（`: SV_Target`
  など）が抜けています。
  2Dは引数を`COLOR0` → `TEXCOORD0` →`SV_Position`の順にします。
- **`undeclared identifier 'ViewProjection'` のように出る** —
  エンジンが渡す値は、使う前にそのShaderの中で`cbuffer`を宣言しておく必要があります。
  `LamaPonLit.hlsl`から**丸ごと**写してください。
- **`LamaPonShaderError` / `LamaPonSpriteError` を選べない** — この
  2本はエンジンが「壊れている印」に使う代役です。
  自分で割り当てられると、マゼンタが「壊れている」のか「そう描きたい」のか区別できなくなるため、一覧・ドラッグ＆ドロップ・「選択Shaderを設定」のどこからも設定できません。
- **b1をコピーしたら影や色が壊れた** — ライティング定数バッファの
  レイアウト不一致です。
  `LamaPonLit.hlsl`から丸ごとコピーし直すのが確実です（エンジン更新後は特に）。
- **スキニングモデルに効かない** — 雛形から`VSSkinnedMain`または
  `PSSkinnedMain`を消していると、DirectXTK描画へフォールバックします。
  ModelRendererの「従来のDirectXTK描画を使う」がオンになっていないかも確認してください（Inspectorの「描画経路」で実際の経路が分かります）。
- **2Dでパラメーター[5]～[7]に書いた値が消える** — その3本はエンジンが
  毎描画上書きします。
  自由用途には`[0]`～`[4]`を使ってください。
- **ScreenEffectが1フレームで消える** — Queueはそのフレーム限りです。
  効果を出し続ける間は`Update`で毎フレーム呼びます。
