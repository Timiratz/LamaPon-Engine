# はじめてのLamaPon

ゲーム制作が初めての人向けに、C++コードを読み、Sceneを編集して最初のゲームを動かす手順を案内します。所要時間は30〜60分です。

[← ドキュメント一覧へ戻る](index.md)

## 0. 準備

配布ZIPを使う場合は、展開して`LamaPonHub.exe`を起動します。エンジン本体をビルドする必要はありません。

C++ Scriptを変更してビルドする段階では、Windows 10/11とVisual Studio 2022以降の「C++によるデスクトップ開発」が必要です。まだ入っていなくても、学習SceneのWASD移動、Inspectorでの改造、企画・デザイン課題までは試せます。

ソースからエンジンを作る場合は、リポジトリ直下で次を実行します。

```bash
cmake --preset windows-release
cmake --build --preset windows-release
```

## 1. 学習プロジェクトを作る

1. `LamaPonHub.exe`を起動します。
2. 保存場所はLamaPonリポジトリの外を選びます。Hubは誤ってエンジン本体の中へゲームを作る操作を止めます。
3. テンプレートは「3D学習」または「2D学習」を選び、「作成して開く」を押します。
4. Editorが開いたら、C++ Game Moduleの初回ビルドがバックグラウンドで始まります。移動はビルド完了前でも試せます。

プロジェクト直下の`LEARNING.md`がこのゲーム専用の入口です。

## 2. まず遊ぶ

1. Editor上部の再生ボタンを押します。
2. Game Viewをクリックします。
3. WASDまたはゲームパッド左スティックで、青い`Player`を黄色い`Goal`へ動かします。
4. SpaceまたはゲームパッドAで、Playerの大きさが変わることを確認します。

Spaceだけ反応しない場合はC++ Game Moduleがまだビルド中です。Consoleの完了表示を待つか、後述の`LearningPlayer.cpp`を保存してください。

ここでは操作だけで終わらせず、ゲームの動作とC++の接点を確認することが目的です。

- どのSceneとC++ Scriptがこの画面を作っているか？
- WASDはどのComponentとC++処理で移動へ変わるか？
- 毎秒回り続ける処理は`Update`のどこで呼ばれるか？

遊べたら`LamaPonCli.exe learn complete --project "."`でステップを完了します。

## 3. Componentを止めて観察する

1. 再生を停止し、Hierarchyの`Player`を選びます。
2. Inspectorで`Input Mover`を一度オフにして再生します。
3. 動かなくなることを確認し、停止してオンへ戻します。
4. `speed`を変えてもう一度遊びます。

SceneはGameObjectの一覧で、GameObjectの振る舞いはComponentの組み合わせです。`Transform`は位置・回転・大きさ、`Mesh Renderer`は見た目、`Input Mover`は入力による移動を担当します。

## 4. C++のゲームループを読む

Asset Browserから`assets/scripts/LearningPlayer.cpp`を開きます。重要なのは4か所だけです。

```cpp
class LearningPlayer final : public LamaPon::Script
{
public:
    void Start() override
    {
        // Play開始時に1回
    }

    void Update(const float deltaTime) override
    {
        // 1フレームに1回
        GetTransform().Rotate(
            { 0.0f, 1.0f, 0.0f },
            SpinSpeed * deltaTime);
    }

private:
    static constexpr float SpinSpeed = 1.2f;
};

LAMAPON_SCRIPT(LearningPlayer);
```

- `Script`を継承するとゲームループから呼ばれます。
- `Start`は最初の`Update`直前に1回です。
- `Update`は毎フレームです。
- `deltaTime`は前フレームからの秒数です。速度へ掛けるとPCのFPSが違っても1秒あたりの動きが揃います。
- `LAMAPON_SCRIPT`がクラスをGame Moduleへ登録します。

## 5. C++を1か所だけ改造する

1. `SpinSpeed`を`1.2f`から`3.0f`へ変えて保存します。
2. 自動ビルドとHot Reloadの完了を待ちます。
3. 再生し、変更前より速く回るか比較します。
4. 次に`BoostScale`も変え、Spaceの反応を比較します。

失敗してもEngine本体は変わりません。編集しているのはHubが作った独立プロジェクトの`assets`だけです。コンパイルエラーはConsoleと`.lamapon/game-module-build.log`へ出ます。

## 6. ゲームエンジニアとして伸ばす

### エンジニア

`SpinSpeed`、`BoostScale`、入力Actionのどれかを変え、保存→ビルド→再生の一往復を行います。

### プランナー

`learning/design-note.md`へ、誰が・何をすると・どうなれば成功かを書きます。SceneへObstacleかGoalを1つ増やし、30秒で伝わるか遊び直します。

### デザイナー

PlayerとGoalの色、Directional Light、Main Cameraのうち2つ以上を変えます。きれいかだけでなく、Goalが見つけやすくなったかを比べます。

学習では、C++コードを読み、実際にゲームを変更し、検証・デバッグ機能で結果を確かめる流れを繰り返します。詳しい道筋は[学習ロードマップ](learning-path.md)にあります。

## 7. 再現できるか確かめる

CLIを使うと、学習状態とプロジェクトをJSONで検証できます。

```powershell
LamaPonCli.exe learn doctor --project "C:\Projects\MyGame"
LamaPonCli.exe learn status --project "C:\Projects\MyGame"
LamaPonCli.exe validate --project "C:\Projects\MyGame"
LamaPonCli.exe build --project "C:\Projects\MyGame"
```

`learn doctor`は教材と参照ファイル、`validate`はScene構造、`build`はC++を確認します。自分のPCだけで偶然動いた状態を減らすための手順です。

## 8. 保存とエクスポート

- Sceneはメニューから保存します（`assets/scenes/*.scene.json`）。
- 「ファイル」→「プロジェクト設定とビルド...」で起動Scene、解像度、品質、入力、タグ、ゲームアイコンを設定できます。
- 同じ画面の「ビルドプロファイル」からWindows／Webを選んで配布物を作れます。Windowsで「配布用ZIPも作成」を選べば友達へ渡せるZIPも生成されます。
- 動作がおかしいときは、ゲーム中にF1でデバッグオーバーレイを表示できます。

次は、作りたいゲームに合わせて[C++スクリプティング](scripting.md)、[グラフィックス](graphics.md)、[UIと2D](ui-2d.md)、[物理](physics.md)へ進んでください。
