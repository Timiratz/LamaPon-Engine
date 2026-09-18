$ErrorActionPreference = "Stop"
# デスクトップのショートカットから実行する場合は従来どおり確認し、
# CIや自動検証から実行する場合だけ入力待ちを省略します。
$NonInteractive = $args -contains "-NonInteractive"

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

$repoDir = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoDir "build-release"
$installDir = Join-Path $env:LOCALAPPDATA "Programs\LamaPon"

function Fail([string]$message) {
    Write-Host ""
    Write-Host "[エラー] $message" -ForegroundColor Red
    Write-Host "==============================================="
    Write-Host " 失敗しました。上のメッセージを確認してください。"
    Write-Host "==============================================="
    if (-not $NonInteractive) {
        Read-Host "終了するには Enter キーを押してください"
    }
    exit 1
}

# Native Game Moduleは、インストール済みエンジンのテンプレートと公開SDKを
# 参照してビルドされます。Runtimeだけが更新されSDKが古いままだと、DLL自体は
# 正常にビルドできてもABIバージョン不一致で読み込みに失敗します。
# CMakeのinstall規則に加え、ゲームモジュールが直接参照するSDKをハッシュで
# 検証しながら同期して、実行ファイルとSDKの世代を必ずそろえます。
function Sync-InstalledSdkDirectory(
    [string]$sourceRelativePath,
    [string]$destinationRelativePath,
    [string]$filter = "*") {
    $sourceDirectory = Join-Path $repoDir $sourceRelativePath
    $destinationDirectory = Join-Path $installDir $destinationRelativePath
    if (-not (Test-Path $sourceDirectory -PathType Container)) {
        Fail "Game Module SDKのコピー元フォルダーが見つかりません: $sourceDirectory"
    }

    Get-ChildItem -LiteralPath $sourceDirectory -Recurse -File -Filter $filter | ForEach-Object {
        $relativeFilePath = $_.FullName.Substring($sourceDirectory.Length).TrimStart('\')
        $destination = Join-Path $destinationDirectory $relativeFilePath
        $destinationParent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null

        $sourceHash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
        $destinationHash = if (Test-Path $destination -PathType Leaf) {
            (Get-FileHash -Algorithm SHA256 $destination).Hash
        }
        else {
            ""
        }
        if ($sourceHash -ne $destinationHash) {
            try {
                Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
            }
            catch {
                Fail "Game Module SDKを同期できません: $destination`n$($_.Exception.Message)"
            }
        }

        $installedHash = (Get-FileHash -Algorithm SHA256 $destination).Hash
        if ($sourceHash -ne $installedHash) {
            Fail "インストール後のGame Module SDKが一致しません: $destination"
        }
    }
}

function Sync-InstalledSdkFile([string]$relativePath) {
    $source = Join-Path $repoDir $relativePath
    $destination = Join-Path $installDir $relativePath
    if (-not (Test-Path $source -PathType Leaf)) {
        Fail "Game Module SDKのコピー元ファイルが見つかりません: $source"
    }

    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    $sourceHash = (Get-FileHash -Algorithm SHA256 $source).Hash
    $destinationHash = if (Test-Path $destination -PathType Leaf) {
        (Get-FileHash -Algorithm SHA256 $destination).Hash
    }
    else {
        ""
    }
    if ($sourceHash -ne $destinationHash) {
        try {
            Copy-Item -LiteralPath $source -Destination $destination -Force
        }
        catch {
            Fail "Game Module SDKを同期できません: $destination`n$($_.Exception.Message)"
        }
    }

    $installedHash = (Get-FileHash -Algorithm SHA256 $destination).Hash
    if ($sourceHash -ne $installedHash) {
        Fail "インストール後のGame Module SDKが一致しません: $destination"
    }
}

Write-Host "==============================================="
Write-Host " LamaPon Editor 再ビルド & インストール"
Write-Host "==============================================="
Write-Host "リポジトリ    : $repoDir"
Write-Host "ビルド先      : $buildDir"
Write-Host "インストール先: $installDir"
Write-Host ""

$hubProc = Get-Process -Name "LamaPonHub" -ErrorAction SilentlyContinue
$editorProc = Get-Process -Name "LamaPonEditor" -ErrorAction SilentlyContinue
if ($hubProc -or $editorProc) {
    Write-Host "[警告] LamaPon Hub / LamaPon Editor が起動中です。" -ForegroundColor Yellow
    Write-Host "保存していない作業がある場合は失われます。"
    if (-not $NonInteractive) {
        Read-Host "続行するには Enter キーを押してください（中止する場合はこのウィンドウを閉じてください）"
    }
    if ($hubProc) { $hubProc | Stop-Process -Force }
    if ($editorProc) { $editorProc | Stop-Process -Force }
    Start-Sleep -Milliseconds 500
}

Write-Host ""
Write-Host "Visual Studio のビルド環境を検索しています..."
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail "vswhere.exe が見つかりません。Visual Studio がインストールされているか確認してください。"
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsPath) {
    Fail "Visual Studio のインストール先を特定できませんでした。"
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Fail "vcvars64.bat が見つかりません: $vcvars"
}

$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

# vcvars64.bat はcmd.exe用の環境変数設定バッチなので、
# 一度cmd経由で実行してその結果の環境変数をこのPowerShellプロセスに取り込む。
$cmdOutput = cmd /c "`"$vcvars`" 2>nul && set"
foreach ($line in $cmdOutput) {
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

# ヘッダーの依存関係を正しく追跡するため、Ninjaを使用します。
# 既存のビルドフォルダーが別のジェネレーターで構成されている場合は、
# キャッシュを削除して再構成します。
$cacheFile = Join-Path $buildDir "CMakeCache.txt"
$usingNinja = (Test-Path $cacheFile) `
    -and (Select-String -Path $cacheFile -Pattern "^CMAKE_GENERATOR:INTERNAL=Ninja$" -Quiet)

if (-not $usingNinja) {
    Write-Host ""
    Write-Host "ビルド設定をNinja（信頼性の高いビルドツール）に切り替えています..."
    Write-Host "（初回のみ時間がかかります）"
    if (Test-Path $buildDir) {
        Remove-Item $buildDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

    if (-not (Test-Path $ninja)) {
        Fail "ninja.exe が見つかりません: $ninja"
    }

    Push-Location $buildDir
    try {
        & cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$ninja" $repoDir
        $configureResult = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($configureResult -ne 0) {
        Fail "ビルド設定に失敗しました。上のログを確認してください。"
    }
}

# クラッシュレポートに表示するBuild revisionは、configure時に
# Gitから生成するbuild-info.jsonを使います。古いリビジョンを
# 埋め込まないよう、ビルド前に毎回再構成します。
Push-Location $buildDir
try {
    & cmake .
    $configureResult = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($configureResult -ne 0) {
    Fail "ビルド設定の更新に失敗しました。上のログを確認してください。"
}

Write-Host ""
Write-Host "[1/3] クリーンビルド中...（数分かかることがあります）"
Push-Location $buildDir
try {
    # ヘッダー変更後の古いオブジェクトが残ると、クラスサイズの不一致などで
    # 実行時クラッシュにつながる。配布用ビルドでは速度より確実性を優先する。
    & cmake --build . --target clean
    $buildResult = $LASTEXITCODE
    if ($buildResult -eq 0) {
        # install(TARGETS ...)に含まれる実行ファイルをすべて生成します。
        # clean後に一部のターゲットだけをビルドすると、cmake --installが
        # 必要な成果物を見つけられません。ターゲットを追加するときは、
        # この一覧とインストール対象をそろえてください。
        & cmake --build . --target `
            LamaPonRuntime LamaPonEditorApp LamaPonHub `
            LamaPonGame LamaPonCli
        $buildResult = $LASTEXITCODE
    }
}
finally {
    Pop-Location
}

if ($buildResult -ne 0) {
    Fail "ビルドに失敗しました。上のログを確認してください。"
}

Write-Host ""
Write-Host "[2/3] インストール先へコピーしています..."
if (-not (Test-Path $installDir)) {
    Fail "インストール先フォルダが見つかりません: $installDir"
}

# Runtime、CLI、公開ヘッダー、CRTなどの配布物を同じ規則で更新するため、
# CMakeLists.txtのinstall規則を使用します。
& cmake --install $buildDir --prefix $installDir
if ($LASTEXITCODE -ne 0) {
    Fail "インストールに失敗しました。上のログを確認してください。"
}

# CMakeが成功を返しても、Windows側で以前の実行ファイルが残ると、
# EXEとRuntime DLLの世代が食い違って「エントリ ポイントが
# 見つかりません」になる。起動に使うバイナリはビルド成果物との
# ハッシュを照合し、違っていれば明示的に同期する。
$runtimeFiles = @(
    "LamaPonRuntime.dll",
    "LamaPonRuntime.lib",
    "LamaPonEditor.exe",
    "LamaPonHub.exe",
    "LamaPonGame.exe",
    "LamaPonCli.exe"
)
foreach ($fileName in $runtimeFiles) {
    $source = Join-Path $buildDir $fileName
    $destination = Join-Path $installDir $fileName
    if (-not (Test-Path $source)) {
        Fail "ビルド成果物が見つかりません: $source"
    }

    $sourceHash = (Get-FileHash -Algorithm SHA256 $source).Hash
    $destinationHash = if (Test-Path $destination) {
        (Get-FileHash -Algorithm SHA256 $destination).Hash
    }
    else {
        ""
    }
    if ($sourceHash -ne $destinationHash) {
        try {
            Copy-Item -LiteralPath $source -Destination $destination -Force
        }
        catch {
            Fail "実行ファイルを同期できません: $destination`n$($_.Exception.Message)"
        }
    }

    $installedHash = (Get-FileHash -Algorithm SHA256 $destination).Hash
    if ($sourceHash -ne $installedHash) {
        Fail "インストール後の実行ファイルが一致しません: $destination"
    }
}

# ProjectGameModuleはインストール先のSDKを直接使うため、Runtime DLLと同様に
# 明示的に同期・検証する。公開ヘッダーは将来のABI変更を取りこぼさないよう
# 全ヘッダーを対象とし、ゲームモジュールが使用する同梱依存も併せて同期する。
Write-Host "Game Module SDKを同期・検証しています..."
Sync-InstalledSdkDirectory "tools\ProjectGameModule" "tools\ProjectGameModule"
Sync-InstalledSdkDirectory "src\LamaPon" "src\LamaPon" "*.h"
Sync-InstalledSdkDirectory "third_party\nlohmann" "third_party\nlohmann" "*.hpp"
Sync-InstalledSdkDirectory "third_party\DirectXTK\include" "third_party\DirectXTK\include"
Sync-InstalledSdkDirectory "third_party\XAudio2Redist\include" "third_party\XAudio2Redist\include"
Sync-InstalledSdkFile "cmake\LamaPonMsvcDependencies.cmake"

Write-Host ""
Write-Host "[3/3] 完了しました。LamaPon Hub を起動します..."
Start-Process (Join-Path $installDir "LamaPonHub.exe")

Write-Host ""
Write-Host "==============================================="
Write-Host " インストールが完了しました。"
Write-Host "==============================================="
if (-not $NonInteractive) {
    Read-Host "終了するには Enter キーを押してください"
}
