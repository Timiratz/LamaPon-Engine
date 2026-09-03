# オーディオ

効果音とBGM、3Dサウンド、ストリーミング、ミキサーバスの使い方を説明します。

[← ドキュメント一覧へ戻る](index.md)

## 1分でためす

1. 音を鳴らしたいGameObjectを選び、Inspectorの「コンポーネントを追加」から
  **オーディオソース**を追加します。
2. Asset BrowserのWAV／OGGファイルをInspectorへドラッグして割り当てます。
3. 「ゲーム開始時に自動再生」をONにしてPlayすると、音が鳴ります。

スクリプトから鳴らす場合（ジャンプ効果音の例）:

```cpp
#include "LamaPon/LamaPon.h"

class JumpSound final : public LamaPon::Script
{
public:
    void Start() override
    {
        // 同じGameObjectのオーディオソースを取得
        m_audio = GetComponent<LamaPon::AudioSourceComponent>();
    }

    void Update(float deltaTime) override
    {
        // Jump（既定はSpaceキー）を押した瞬間に1回だけ鳴らす
        if (m_audio != nullptr
            && Graphics().Input().WasPressed("Jump"))
        {
            m_audio->PlayOneShot();
        }
    }

private:
    LamaPon::AudioSourceComponent* m_audio{};
};

LAMAPON_SCRIPT(JumpSound);
```

BGMをコードから流す場合:

```cpp
auto& bgm = AddComponent<LamaPon::AudioSourceComponent>();
bgm.SetAudioPath("audio/bgm.ogg"); // assetsからの相対パス
bgm.SetLoop(true);
bgm.SetStreaming(true);  // 長い曲はメモリへ全展開せず再生
bgm.SetBus(LamaPon::AudioBus::Music);
bgm.SetVolume(0.6f);
bgm.Play();
```

`Play`（最初から再生）、`PlayOneShot`（重ねて1回）、`Pause`／`Resume`、`Stop`が使えます。

## オーディオソースの設定

InspectorではAsset BrowserのWAV／OGGファイルをドラッグして音声を割り当て、再生、One Shot、一時停止、再開、停止に加えて、音量、ピッチ、左右パン、ループ、ゲーム開始時の自動再生を設定できます。
設定はシーンJSONへ保存され、GameObjectの複製にも引き継がれます。

長いBGMは「ストリーミング再生」をONにすると、圧縮データだけをメモリへ持ち、再生しながらデコードします。
ミキサーバス（Effects／Music）を選ぶと、バスごとの音量をまとめて調整できます（効果音だけ小さくする等）。

## 曲の途中から鳴らす（試聴・サビ出し）

ストリーミング音源は`SetStartFrame()`で**再生を始める位置**を決められます。
単位は各チャンネル共通のsample frame（44.1kHzなら`秒 × 44100`）です。
`Play()`は毎回この位置から鳴り直します。

```cpp
auto& source = object.AddComponent<LamaPon::AudioSourceComponent>();
source.SetStreaming(true);
source.SetAudioPath("audio/bgm/song.ogg");
source.SetLoop(true);
source.SetStartFrame(53 * 44100);   // 53秒地点（サビ）から
source.Play();
```

BGMセレクトの試聴のように「曲の一番おいしいところだけ聞かせたい」ときや、
**長すぎるイントロを飛ばして本編から鳴らしたい**ときに、
**音源を別途切り出す必要がなくなります**。
音源の長さを超える値を渡した場合は先頭（0）として扱います。
`SetLoopRegionFrames()`と併用すると、開始位置からループ終端までを一度鳴らしてから
ループ区間の反復へ入ります。

## 音に合わせて動く波形を出す（レベルメーター）

`SetLevelMeterEnabled(true)`にすると、デバイスへ送っているPCMそのものを
12帯域のバンドパスで追いかけ、**いま鳴っている位置**の強さを0〜1で返します。
音楽ビジュアライザ、リズムに合わせて光るUI、口パクの当たりなどに使えます。

```cpp
source.SetLevelMeterEnabled(true);

// 毎フレーム。低域→高域の順に入ります。
std::array<float, LamaPon::AudioSourceComponent::LevelBandCount> bands{};
source.ReadLevelBands(bands.data(), bands.size());
```

- 返るのは**先読み済みのPCMではなく再生位置に合わせた値**です。
  画面の棒と耳で聞こえる音がずれません。
- 帯域ごとに自動利得がかかります。低域と高域では音量の桁が違うので、
  生の値のままだと高域の棒がほとんど動かないためです。
- **ストリーミング音源専用**です（効果音側は対象外）。
- **既定は無効で、無効な間は解析もメモリ確保もしません。**
  有効にしたときだけ1音源あたり約29KBのリングを持ち、
  44.1kHzで12本のフィルタを回します。

## 3Dサウンド

3D音声を使う場合は、通常メインカメラへ「オーディオリスナー」を追加し、AudioSourceの「3D空間オーディオ」を有効にします。
音源とリスナーの位置・向きはGameObjectのワールドTransformへ追従し、最小距離までは等音量、そこから線形に減衰して最大距離で無音になります。
3Dモードでは左右パンをTransformから自動計算します。

サンプルシーンはメインカメラにAudioListenerを持ち、右前方に置いた`assets/audio/startup.wav`を3D音声として開始時に一度再生します。
サンプル音源は次のコマンドで再生成できます。

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\GenerateSampleAudio.ps1
```

## 低音を持ち上げる

`SetBassBoost(gainDb, cornerHz)` で、cornerHzより下をgainDbだけ持ち上げます。
スマホや車内のスピーカーで痩せる低域を、**音源を作り直さずに**補うためのものです。
0dB（既定）なら一切処理しません。

```cpp
source.SetBassBoost(6.0f, 110.0f);   // 110Hzより下を+6dB
```

中では、int16のPCMが振り切れないよう**全体をgainDbぶん下げてから棚を掛け、
下げたぶんを再生音量側で自動的に戻します**。呼ぶ側は`SetVolume`をそのままに
できます（音量は内部で1.0を超えますが、PCMを同じだけ下げてあるので
元の振幅は超えません）。

**すでに大きくマスタリングされた音源では、低域が持ち上がったぶんピークが
上がります。** どれだけ上がるかは曲によるので（実測では+6dBの棚で
ピークの上昇は1.5〜2.7dB程度）、大音量で鳴らすなら実際に測ってから
音量を決めてください。

## 再生位置と波形

`PlaybackFrame()`は**いま鳴っている位置**（音源内のsample frame）を返します。
ループ区間の折り返しも追うので、進捗バーや歌詞の同期に使えます。
`TotalFrames()`と`SampleRate()`で秒や進捗率へ直せます。

```cpp
const double seconds =
    static_cast<double>(source.PlaybackFrame()) / source.SampleRate();
```

`ReadPeakEnvelope()`は音源全体を区間ごとの最大振幅（0〜1）にして返します。
波形を絵にするための値です。

```cpp
std::vector<float> peaks(1024, 0.0f);
source.ReadPeakEnvelope(peaks.data(), peaks.size());
```

**`ReadPeakEnvelope`は重い呼び出しです。** 音源全体をデコードし直すので5分のOGGで
0.3秒ほど止まります。読み終えたら再生位置は元へ戻しますが、鳴らしている最中に呼ぶと
その間の供給が滞って音が途切れます。停止中に一度だけ作るか、波形表示用に別の音源
インスタンスを用意してください。

## よくあるつまずき

- **音が鳴らない** — 音声ファイルを割り当てたか、音量が0でないかを確認。
  自動再生がOFFの場合はスクリプトから`Play()`を呼ぶ必要があります。
- **3Dにしたら聞こえなくなった** — カメラ（または聞き手役のGameObject）に
  **オーディオリスナー**が必要です。
  最大距離が音源との距離より小さくても無音になります。
- **効果音が「ダダダ」と連打される** — `IsDown`（押している間ずっとtrue）ではなく
  `WasPressed`（押した瞬間の1フレームだけtrue）で鳴らします。
- **BGMでメモリを大量に使う** — 「ストリーミング再生」をONに。効果音のような
  短い音はOFFのまま（すぐ鳴らせる）が向いています。
- **音量調整のUIを作りたい** — 個別の音量ではなくバス音量
  （`LamaPon::AudioBus::Music`など）を操作すると、まとめて変えられます。
- **レベルメーターの値が全部0のまま** — 音声デバイスが無い環境では
  `State()`が`PLAYING`を返してもPCMは要求されません（解析する音が
  流れていない）。ヘッドレス実行や仮想マシンではこうなります。
- **`SetStartFrame`にしたのに曲頭から鳴る** — 音源の長さを超える値は
  先頭（0）として扱われます。秒ではなくsample frameで渡しているか、
  ストリーミング再生がONかを確認してください。
