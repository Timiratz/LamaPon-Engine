# 起動音をモノラル16bit WAVで生成します(OutputPath: 出力先、省略時は組み込み音声)。
param(
    [string]$OutputPath
)

# projectRoot: エンジンrepository root。
$projectRoot = Split-Path -Parent $PSScriptRoot
# 出力path未指定時は組み込み音声の既定先にします。
if ([string]::IsNullOrWhiteSpace($OutputPath))
{
    $OutputPath = Join-Path $projectRoot 'assets\audio\startup.wav'
}

# sampleRate: 1秒あたりのaudio sample数。
$sampleRate = 44100
# durationSeconds: 生成するaudioの秒数。
$durationSeconds = 0.65
# sampleCount: 出力するsample総数。
$sampleCount = [int]($sampleRate * $durationSeconds)
# channelCount: WAVのaudio channel数。
$channelCount = 1
# bitsPerSample: 1 sampleあたりのbit数。
$bitsPerSample = 16
# blockAlign: 1 frame分の全channel byte数。
$blockAlign = $channelCount * ($bitsPerSample / 8)
# byteRate: 1秒あたりのWAV data byte数。
$byteRate = $sampleRate * $blockAlign
# dataSize: WAV data chunkのbyte数。
$dataSize = $sampleCount * $blockAlign

# directory: 出力fileの親directory。
$directory = Split-Path -Parent $OutputPath
# WAV出力先の親directoryを作成します。
New-Item -ItemType Directory -Force -Path $directory | Out-Null

# stream: WAVを書き込むfile stream。
$stream = [System.IO.File]::Open(
    $OutputPath,
    [System.IO.FileMode]::Create,
    [System.IO.FileAccess]::Write)
# writer: WAV headerとPCM dataのbinary writer。
$writer = [System.IO.BinaryWriter]::new($stream)

# sample loop全体でstreamを保持し、finallyで閉じます。
try
{
    $writer.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
    $writer.Write([int](36 + $dataSize))
    $writer.Write([System.Text.Encoding]::ASCII.GetBytes('WAVE'))
    $writer.Write([System.Text.Encoding]::ASCII.GetBytes('fmt '))
    $writer.Write([int]16)
    $writer.Write([int16]1)
    $writer.Write([int16]$channelCount)
    $writer.Write([int]$sampleRate)
    $writer.Write([int]$byteRate)
    $writer.Write([int16]$blockAlign)
    $writer.Write([int16]$bitsPerSample)
    $writer.Write([System.Text.Encoding]::ASCII.GetBytes('data'))
    $writer.Write([int]$dataSize)

    # index: 出力sample番号を順に処理します。
    for ($index = 0; $index -lt $sampleCount; ++$index)
    {
        # time: 現sampleの再生時刻（秒）。
        $time = $index / $sampleRate
        # attack: 音声開始時のfade-in倍率。
        $attack = [Math]::Min(1.0, $time / 0.018)
        # release: 音声終了へ向けたfade-out倍率。
        $release = [Math]::Pow(
            [Math]::Max(0.0, 1.0 - ($time / $durationSeconds)),
            2.2)
        # envelope: attackとreleaseを合成した音量倍率。
        $envelope = $attack * $release
        # fundamental: C5基音のsample値。
        $fundamental = [Math]::Sin(2.0 * [Math]::PI * 523.251 * $time)
        # fifth: G5のsample値。
        $fifth = [Math]::Sin(2.0 * [Math]::PI * 783.991 * $time)
        # sparkle: C6のsample値。
        $sparkle = [Math]::Sin(2.0 * [Math]::PI * 1046.502 * $time)
        # sample: 3音を合成した正規化PCM値。
        $sample = (
            0.55 * $fundamental +
            0.30 * $fifth +
            0.15 * $sparkle) * $envelope * 0.58
        $sample = [Math]::Max(-1.0, [Math]::Min(1.0, $sample))
        $writer.Write(
            [int16][Math]::Round(
                $sample * 32767.0))
    }
}
# finally: 成否にかかわらずWAV writerとstreamを閉じます。
finally
{
    # writer・streamの両方を解放します。
    $writer.Dispose()
    $stream.Dispose()
}

Write-Host "Generated $OutputPath"
