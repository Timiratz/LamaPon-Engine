# TestRoot: 診断用の空フォルダー。
param([string]$TestRoot = 'out\installer\diagnostic-tests')

$ErrorActionPreference = 'Stop'
# repoRoot: ソースの基準。
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repoRoot 'tools\SetupBuildEnvironment.ps1')
# outputRoot: 診断データの保存先。
$outputRoot = if ([IO.Path]::IsPathRooted($TestRoot)) { [IO.Path]::GetFullPath($TestRoot) } else { [IO.Path]::GetFullPath((Join-Path $repoRoot $TestRoot)) }
if (Test-Path -LiteralPath $outputRoot) { throw "診断試験の出力先が既にあります: $outputRoot" }
New-Item -ItemType Directory -Path $outputRoot | Out-Null
# originalProgramFiles: 実際のVS検索先。
$originalProgramFiles = $env:ProgramFiles
# originalProgramFilesX86: 実際のSDK検索先。
$originalProgramFilesX86 = ${env:ProgramFiles(x86)}
try {
    $env:ProgramFiles = Join-Path $outputRoot 'program-files'
    ${env:ProgramFiles(x86)} = Join-Path $outputRoot 'program-files-x86'
    # missingStatus: 開発ツールが無いPC。
    $missingStatus = Get-LamaPonBuildEnvironment
    if ($missingStatus.Ready -or $missingStatus.Missing.Count -lt 3) { throw '未導入環境を誤って準備完了と判定しました。' }
    # editionRoot: コンパイラーだけの環境。
    $editionRoot = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\18\BuildTools'
    # fixtureFile: 実行しない検出用ファイル。
    foreach ($fixtureFile in @('Common7\Tools\VsDevCmd.bat', 'VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe')) {
        New-Item -ItemType Directory -Path (Split-Path -Parent (Join-Path $editionRoot $fixtureFile)) -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $editionRoot $fixtureFile), '')
    }
    # partialStatus: CMake・Ninja・SDK不足。
    $partialStatus = Get-LamaPonBuildEnvironment
    if ($partialStatus.Ready -or $partialStatus.Missing -notcontains 'Visual Studio C++ CMake tools' -or $partialStatus.Missing -notcontains 'Visual Studio同梱Ninja' -or $partialStatus.Missing -notcontains 'Windows 10/11 SDK') { throw '部分導入環境の不足を検出できません。' }
    Write-Host 'PASS: 未導入環境とCMake・Ninja・SDK不足の検出（ダウンロードなし）'
}
finally {
    $env:ProgramFiles = $originalProgramFiles
    ${env:ProgramFiles(x86)} = $originalProgramFilesX86
}
