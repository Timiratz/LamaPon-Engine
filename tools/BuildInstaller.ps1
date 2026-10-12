# BuildDir: 構成済みRelease、OutputDir: 配布出力、Compiler: ISCCのパス。
param(
    [string]$BuildDir = 'build-release',
    [string]$OutputDir = 'out\installer',
    [string]$Compiler = ''
)

$ErrorActionPreference = 'Stop'
# repoRoot: ソースの基準。
$repoRoot = Split-Path -Parent $PSScriptRoot
# buildRoot: 構成済みビルド先。
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDir)) { [IO.Path]::GetFullPath($BuildDir) } else { [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir)) }
# outputRoot: 配布物の保存先。
$outputRoot = if ([IO.Path]::IsPathRooted($OutputDir)) { [IO.Path]::GetFullPath($OutputDir) } else { [IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDir)) }
# payloadRoot: CMake配布ファイル一式。
$payloadRoot = Join-Path $outputRoot 'payload'

if (-not $Compiler) {
    # candidate: コンパイラーの既定配置。
    foreach ($candidate in @(
        (Join-Path $repoRoot 'out\installer-tools\compiler\ISCC.exe'),
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 7\ISCC.exe"
    )) {
        if (Test-Path -LiteralPath $candidate) { $Compiler = $candidate; break }
    }
}
if (-not $Compiler -or -not (Test-Path -LiteralPath $Compiler)) {
    throw 'Inno Setup 6.3以上のISCC.exeが必要です。-Compilerで指定してください。'
}
if (-not (Test-Path -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt'))) {
    throw '先にCMakeでReleaseビルドを構成してください。'
}
if (-not (Select-String -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Pattern '^CMAKE_BUILD_TYPE:STRING=Release$' -Quiet)) {
    throw '配布用インストーラーはReleaseビルドから作成してください。'
}
# fileName: 配布に必要な成果物。
foreach ($fileName in @('LamaPonEditor.exe', 'LamaPonHub.exe', 'LamaPonCli.exe', 'LamaPonGame.exe', 'LamaPonRuntime.dll', 'LamaPonRuntime.lib')) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot $fileName))) { throw "ビルド成果物が不足しています: $fileName" }
}
# 不要な古いファイルを再配布しないため、新しい出力先を要求する。
if (Test-Path -LiteralPath $payloadRoot) {
    throw "配布用フォルダーが既に存在します。確認して削除するか、別の-OutputDirを指定してください: $payloadRoot"
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
# installOutput: 失敗時に表示する配置ログ。
$installOutput = & cmake --install $buildRoot --config Release --prefix $payloadRoot 2>&1
if ($LASTEXITCODE -ne 0) { throw ("CMake配布ファイルの作成に失敗しました。`n" + ($installOutput -join "`n")) }
# buildInfo: バージョン情報。
$buildInfo = Get-Content -LiteralPath (Join-Path $payloadRoot 'build-info.json') -Raw | ConvertFrom-Json
# version: 出力名へ使う版数。
$version = [string]$buildInfo.engineVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'build-info.jsonの版数が不正です。' }
# requiredPath: 起動と自動ビルドの必須ファイル。
foreach ($requiredPath in @('vcruntime140.dll', 'msvcp140.dll', 'xaudio2_9redist.dll', 'tools\SetupBuildEnvironment.ps1', 'tools\StartAutoBuild.ps1', 'tools\RebuildAndInstallEditor.ps1', 'tools\ProjectGameModule\CMakeLists.txt', 'src\LamaPon\LamaPon.h', 'docs\installation.md')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot $requiredPath))) { throw "配布ファイルが不足しています: $requiredPath" }
}
& $Compiler /Qp "/DPayloadDir=$payloadRoot" "/DOutputDir=$outputRoot" "/DAppVersion=$version" (Join-Path $PSScriptRoot 'installer\LamaPon.iss')
if ($LASTEXITCODE -ne 0) { throw 'Setup.exeの生成に失敗しました。' }
# installerPath: 完成した配布EXE。
$installerPath = Join-Path $outputRoot "LamaPon-$version-windows-x64-Setup.exe"
Get-Item -LiteralPath $installerPath | Select-Object FullName, Length
Get-FileHash -LiteralPath $installerPath -Algorithm SHA256
