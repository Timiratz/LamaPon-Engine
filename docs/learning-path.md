# LamaPon 学習ロードマップ

LamaPonは、C++でゲームを作りながら、ゲームエンジンの仕組みを学べる環境です。
このロードマップでは、制作と理解を一緒に進める手順を示します。

[← ドキュメント一覧へ戻る](index.md)

## 学習する4つの領域

| 領域 | 学ぶこと |
|---|---|
| ゲームループ | 入力、更新、描画、時間の流れを扱う |
| C++を理解する | `Start`、`Update`、`deltaTime`、Component、Sceneの関係を読む |
| ゲーム制作 | ルール、操作、見た目を変更し、一つのゲームとして完成させる |
| 検証とデバッグ | `validate`、`build`、`doctor`を使い、問題を切り分ける |

4つの領域を別々に進める必要はありません。1つのゲームを作りながら、
ComponentやC++コードを読み、変更し、結果を確かめます。

## 最初に選ぶテンプレート

LamaPon Hubには4種類あります。

| テンプレート | 対象 | 内容 |
|---|---|---|
| 3D学習 | 初めて3Dゲームを作る人 | 遊べるScene、コメント付きC++、8段階の課題、進捗 |
| 2D学習 | 初めて2Dゲームを作る人 | 遊べるScene、コメント付きC++、8段階の課題、進捗 |
| 3D | 作りたい3Dゲームが決まっている人 | Camera、Light、Cube、Groundだけ |
| 2D | 作りたい2Dゲームが決まっている人 | Camera、Spriteだけ |

学習テンプレートには、`LEARNING.md`と`learning/`の教材、コメント付き`LearningPlayer.cpp`が含まれます。進捗の確認・更新は`LamaPonCli learn`コマンドを使います。

空テンプレートや既存プロジェクトにも`LamaPonCli learn init`で教材を後付けできます。既存のSceneは上書きせず、教材、練習用C++、企画メモだけを追加します。

## 共有する教材と個人の進捗

| ファイル | Git共有 | 内容 |
|---|---:|---|
| `LEARNING.md` | する | 人が読む入口 |
| `learning/journey.json` | する | CLIが読む学習手順 |
| `learning/design-note.md` | する | 企画課題と振り返り |
| `assets/scripts/LearningPlayer.cpp` | する | 最小のC++ゲームループ例 |
| `.lamapon/learning-progress.json` | しない | 各自の完了状態とCLIで設定した方向 |

同じ教材をチームで共有しても、他の人の完了チェックを上書きしません。

## 制作の中で試せること

### エンジニア

`Update`、入力、物理、ゲームルール、エラー修正をもう少し触りたい人向けです。次は[C++スクリプティング](scripting.md)、[入力](input.md)、[物理](physics.md)、[コード一覧](code-reference.md)へ進みます。

### プランナー

「何をすると面白いか」、成功条件、難易度、数値の調整を考えたい人向けです。まず`learning/design-note.md`へ30秒で説明できるルールを書き、遊ぶ人に試してもらいます。

### デザイナー

色、形、光、カメラ、UI、分かりやすさを磨きたい人向けです。次は[グラフィックス](graphics.md)、[UIと2D機能](ui-2d.md)、[Animation](animation.md)へ進みます。

エンジニア、プランナー、デザイナーの視点は、4つの領域をゲーム制作の中で
試すための入口です。役割を先に決める必要はありません。まずC++を読み、
ゲームを作り、検証しながら改善します。

## CLIで同じ手順を再現する

進捗は`LamaPonCli`からJSONで操作できます。

```powershell
LamaPonCli.exe learn status --project "C:\Projects\MyGame"
LamaPonCli.exe learn complete --project "C:\Projects\MyGame"
LamaPonCli.exe learn role --project "C:\Projects\MyGame" --role engineer
LamaPonCli.exe learn doctor --project "C:\Projects\MyGame"
```

既存プロジェクトへの追加は`learn init`、個人進捗だけのやり直しは`learn reset`です。詳しい出力は[コマンドライン](cli.md#学習進捗と環境診断)を参照してください。
