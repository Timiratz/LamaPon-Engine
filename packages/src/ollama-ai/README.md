# Ollama AI (Local LLM)

このPCで動く [Ollama](https://ollama.com/) のローカルモデルと、ゲームの中で会話するためのパッケージです。
入力欄に書いた文をモデルへ送り、返答をテキストとして表示します。NPCのせりふ作りなどに使えます。

**Windows専用**です。Web書き出しでは動きません（通信とスレッドの仕組みがWindows向けのためです）。

## 無料の範囲だけを使います

このパッケージが使うのは、自分のPCで動かすローカル実行だけです。
有料の Ollama Cloud は、無料枠も含めて使いません。次の方法で、コードの側から止めています。

| 守ること | 方法 |
| --- | --- |
| このPC以外へ送らない | 接続先は `http://127.0.0.1:<ポート>` に固定です。設定できるのはポートだけで、ホストの欄はありません。送る直前にもURLを確かめます |
| APIキーを扱わない | キーの欄がなく、環境変数も読みません。認証用のヘッダーを送りません |
| クラウドのモデルを指定させない | `gemma4:cloud` や `xxx-cloud` のように、名前に `cloud` が付くモデルは設定の読み込みで拒否します |
| 名前で見分けられないモデルも止める | 送る前に `/api/show` でモデルを調べ、`remote_host` / `remote_model` があれば送りません |
| 一覧からも除く | モデルを自動で選ぶとき、`/api/tags` の一覧から `remote_host` 付きのモデルを除きます |
| 届いた結果も確かめる | `/api/chat` の応答に `remote_host` / `remote_model` があれば、結果を捨てて失敗にします |
| サインインが必要な機能を使わない | 呼ぶAPIは `/api/tags`・`/api/show`・`/api/chat` の3つだけです。Web検索・Web取得は使いません |

名前に `cloud` という語が入っていれば、ローカルモデルでも拒否します。取りこぼすより安全な側に倒しているためです。

## 準備

### 1. Ollamaを入れる

1. <https://ollama.com/download> からWindows版をダウンロードしてインストールします。
2. インストールするとOllamaが起動し、タスクトレイにアイコンが出ます。
3. ブラウザーで <http://127.0.0.1:11434> を開き、`Ollama is running` と表示されれば準備完了です。

**サインインは不要です。** アカウントを作らなくても、ローカルモデルは使えます。

### 2. クラウド機能を止める（おすすめ）

Ollama本体のクラウド機能を止めておくと、クラウドのモデルを誤って使うことがなくなります。
次のどちらか一方を設定し、Ollamaを終了してから起動し直してください。

- 環境変数 `OLLAMA_NO_CLOUD=1` を設定する。コマンドプロンプトでは次のとおりです。

  ```bat
  setx OLLAMA_NO_CLOUD 1
  ```

- `~/.ollama/server.json`（Windowsでは `%USERPROFILE%\.ollama\server.json`）に次の内容を書く。

  ```json
  {"disable_ollama_cloud": true}
  ```

設定が効いていると、Ollamaのログに `Ollama cloud disabled: true` と出ます。

### 3. モデルを取得する

コマンドプロンプトで、使いたいモデルを取得します。初回は数GBのダウンロードがあります。

```bat
ollama pull gemma3:4b
```

取得したモデルは `ollama list` で確認できます。名前に `cloud` が付くモデルは取得しないでください。
はじめは小さいモデル（パラメーター数が1B～4B程度）をおすすめします。大きいモデルは返答に時間がかかり、
後で説明する30秒の制限に間に合わないことがあります。

## インストール

1. 「拡張機能 > パッケージを探す...」で **Ollama AI (Local LLM)** を選び、「インストール」を押します。
   Zipを直接受け取った場合は「拡張機能 > Zipから読み込む...」で `ollama-ai-0.1.0.zip` を選びます。
2. Game Moduleのビルドが終わるのを待ちます。Scriptとデータアセット型が登録されます。
3. `assets/packages/ollama-ai/scenes/OllamaChatDemo.scene.json` を開いて再生します。
4. 入力欄に文を書き、Enterか「送信」を押します。「考え中...」のあとに返答が表示されます。

## 自分のSceneで使う

1. 空のGameObjectへ、Script「Ollamaチャット」を追加します。
2. 入力欄（UI Input Field）と、返答を表示するテキスト（Text Renderer）を用意します。
3. Scriptの「入力欄のGameObject名」「返答を表示するGameObject名」に、それぞれの名前を入れます。
4. 送信ボタンを置く場合は、UI Buttonの「クリックイベント名」を `Ollama.Send` にします。

### Scriptのプロパティ

| プロパティ | 既定 | 内容 |
| --- | --- | --- |
| Ollama設定アセット | `profiles/Default.asset.json` | モデルや返答の長さを決めるデータアセット |
| 入力欄のGameObject名 | `Ollama Input` | UI Input Fieldを持つGameObject。Enterで送信します。空にするとイベントだけを受け付けます |
| 返答を表示するGameObject名 | `Ollama Reply` | Text Rendererを持つGameObject。空にすると表示しません |
| 返答待ちの表示 | `考え中...` | 返答を待つ間に表示する文 |
| 送信イベント | `Ollama.Send` | このイベントで送信します |
| 返答イベント | `Ollama.Reply` | 返答が届いたときに発行します |
| 失敗イベント | `Ollama.Error` | 失敗したときに発行します |

### 設定アセット（Ollama設定アセット）

アセットウィンドウの「新規データアセット」から作るか、`profiles/Default.asset.json` を複製して使います。

| 項目 | 既定 | 内容 |
| --- | --- | --- |
| model | 空 | `ollama list` に出る名前。空なら、一覧の最初のローカルモデルを使います |
| systemPrompt | 短く答える指示 | 役柄や口調の指示 |
| temperature | 0.7 | 返答のゆらぎ（0～2）。小さいほど毎回同じような返答になります |
| maxTokens | 128 | 返答の最大トークン数（Ollamaの `num_predict`。1～1024） |
| port | 11434 | Ollamaのポート。接続先のホストは `127.0.0.1` 固定です |
| fallbackReply | （いまは返事ができません） | 失敗したときに、返答の代わりに表示する文 |
| historyLimit | 8 | 覚えておく会話の件数（0～64）。質問と返答をそれぞれ1件と数えます |

範囲外の値、クラウドのモデル名、表にない項目（`host` や `apiKey` など）があるアセットは、
1項目も適用せずに拒否し、Consoleへ理由を出します。

### イベント

別のScriptから文を送るには、`EventArgs.text` へ文を入れて `Ollama.Send` を発行します。
`text` が空のときは、入力欄の文を送ります。

```cpp
LamaPon::EventArgs args;
args.text = "こんにちは";
Emit("Ollama.Send", std::move(args));
```

返答と失敗は、イベントで受け取れます。

```cpp
On("Ollama.Reply", [this](const LamaPon::EventArgs& args)
{
    m_line = args.text;  // モデルの返答
});
On("Ollama.Error", [this](const LamaPon::EventArgs& args)
{
    if (args.text == "NotRunning") { /* Ollamaを起動するよう案内する */ }
});
```

### 失敗の種類

失敗したときは、設定アセットの `fallbackReply` を表示し、`Ollama.Error` を発行します。
理由はConsoleにも出ます。

| `EventArgs.text` | `number` | 意味 | 直し方 |
| --- | --- | --- | --- |
| `NotRunning` | 1 | Ollamaが起動していない | Ollamaを起動する。ポートを変えている場合は設定アセットの `port` を合わせる |
| `ModelMissing` | 2 | モデルがこのPCにない | `ollama pull <モデル名>` で取得する |
| `Timeout` | 3 | 30秒以内に返答が届かなかった | はじめの1回はモデルの読み込みで間に合わないことがあるので、もう一度送る。続くなら `maxTokens` を小さくするか、小さいモデルを使う |
| `CloudRejected` | 4 | クラウドのモデルを拒否した | ローカルモデルを指定する |
| `Failed` | 5 | 上のどれでもない失敗 | Consoleの理由を確認する |

失敗した会話は履歴に入れません。

## 制限

- **返答は30秒以内に届く必要があります。** エンジンのHTTP通信は受信を30秒で打ち切り、返答を少しずつ
  受け取ることもできません。そのため、返答は1回でまとめて受け取り、`maxTokens` で長さを抑えています。
  はじめの1回はモデルの読み込みで時間がかかります。先に `ollama run <モデル名>` で読み込んでおくと安定します。
- **同時に送れるのは1件です。** 返答を待っている間の送信は無視します。
- **通信中に再生を止めたり、Game Moduleを差し替えたりすると、通信が終わるまで待ちます**（最大で約30秒）。
  通信を途中で止められないため、スレッドの終了を待ってからScriptを破棄しています。
- `model` が空のときは、一覧の最初のローカルモデルを使います。埋め込み専用のモデルが最初にあると
  会話に失敗するので、その場合は `model` に名前を書いてください。
- 考える過程を出力するモデルには、考える過程を省くよう指示しています（`think: false`）。
  それでも返答が空になる場合は、`maxTokens` を大きくしてください。
- 書き出したゲームを配る場合、遊ぶ人のPCにもOllamaとモデルが必要です。モデルはゲームに同梱されません。

## モデルのライセンス

モデルの利用条件は、モデルごとに違います。商用利用ができないものや、利用規約への同意が必要なものがあります。
ゲームに使う前に、必ずモデルのライセンスを確認してください。

```bat
ollama show gemma3:4b --license
```

このパッケージにモデルは含まれていません。モデルの利用条件は、このパッケージのライセンスとは別です。

## ライセンス

MITライセンスです。コードとアセットをコピー・編集・再配布できます。
