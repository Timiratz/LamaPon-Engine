param(
    [string]$OutputPath
)

$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath))
{
    $OutputPath = Join-Path $projectRoot 'assets\audio\startup.wav'
}

$sampleRate = 44100
$durationSeconds = 0.65
$sampleCount = [int]($sampleRate * $durationSeconds)
$channelCount = 1
$bitsPerSample = 16
$blockAlign = $channelCount * ($bitsPerSample / 8)
$byteRate = $sampleRate * $blockAlign
$dataSize = $sampleCount * $blockAlign

$directory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $directory | Out-Null

$stream = [System.IO.File]::Open(
    $OutputPath,
    [System.IO.FileMode]::Create,
    [System.IO.FileAccess]::Write)
$writer = [System.IO.BinaryWriter]::new($stream)

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

    for ($index = 0; $index -lt $sampleCount; ++$index)
    {
        $time = $index / $sampleRate
        $attack = [Math]::Min(1.0, $time / 0.018)
        $release = [Math]::Pow(
            [Math]::Max(0.0, 1.0 - ($time / $durationSeconds)),
            2.2)
        $envelope = $attack * $release
        $fundamental = [Math]::Sin(2.0 * [Math]::PI * 523.251 * $time)
        $fifth = [Math]::Sin(2.0 * [Math]::PI * 783.991 * $time)
        $sparkle = [Math]::Sin(2.0 * [Math]::PI * 1046.502 * $time)
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
finally
{
    $writer.Dispose()
    $stream.Dispose()
}

Write-Host "Generated $OutputPath"
