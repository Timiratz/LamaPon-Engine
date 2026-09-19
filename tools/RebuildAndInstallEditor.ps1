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

# エディターから起動した再インストールでは、自分自身が終了するまで
# 待ってからコピーします。明示的な指定なしに他のLamaPonプロセスを
# 非対話で終了しないよう、強制終了も専用フラグに限定します。
$CloseRunningLamaPonProcesses =
    $args -contains "-CloseRunningLamaPonProcesses"
$WaitForProcessId = 0
for ($argumentIndex = 0;
    $argumentIndex -lt $args.Count;
    $argumentIndex++) {
    if ($args[$argumentIndex] -ne "-WaitForProcessId") {
        continue
    }
    if ($argumentIndex + 1 -ge $args.Count) {
        Fail "-WaitForProcessId には待機するプロセスIDを指定してください。"
    }
    $parsedProcessId = 0
    $validProcessId = [int]::TryParse(
        [string]$args[$argumentIndex + 1],
        [ref]$parsedProcessId)
    if (-not $validProcessId -or $parsedProcessId -le 0) {
        Fail "-WaitForProcessId には正のプロセスIDを指定してください。"
    }
    $WaitForProcessId = $parsedProcessId
    $argumentIndex++
}

Write-Host "==============================================="
Write-Host " LamaPon Editor 再ビルド & インストール"
Write-Host "==============================================="
Write-Host "リポジトリ    : $repoDir"
Write-Host "ビルド先      : $buildDir"
Write-Host "インストール先: $installDir"
Write-Host ""

if ($WaitForProcessId -gt 0) {
    Write-Host "エディターの終了を待機しています（PID: $WaitForProcessId）..."
    $waitDeadline = [DateTime]::UtcNow.AddSeconds(30)
    while (Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue) {
        if ([DateTime]::UtcNow -ge $waitDeadline) {
            Fail "指定されたエディターが30秒以内に終了しませんでした。"
        }
        Start-Sleep -Milliseconds 250
    }
}

$hubProc = Get-Process -Name "LamaPonHub" -ErrorAction SilentlyContinue
$editorProc = Get-Process -Name "LamaPonEditor" -ErrorAction SilentlyContinue
if ($hubProc -or $editorProc) {
    Write-Host "[警告] LamaPon Hub / LamaPon Editor が起動中です。" -ForegroundColor Yellow
    Write-Host "保存していない作業がある場合は失われます。"
    if ($NonInteractive -and -not $CloseRunningLamaPonProcesses) {
        Fail "LamaPon Hub / LamaPon Editor が起動中です。終了してから再実行してください。"
    }
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

# インストール済みエディターから「再インストール」を選んだときに、
# ビルド可能な元リポジトリを確実に見つけるための情報です。SDKとRuntime
# はこのスクリプトが同じCMake install規則から更新するため、世代が揃います。
$desktopBuildSourcePath = Join-Path $installDir "desktop-build-source.json"
$desktopBuildSource = [ordered]@{
    format = "LamaPonDesktopBuildSource"
    version = 1
    sourceRoot = [System.IO.Path]::GetFullPath($repoDir)
} | ConvertTo-Json -Compress
try {
    [System.IO.File]::WriteAllText(
        $desktopBuildSourcePath,
        $desktopBuildSource,
        [System.Text.UTF8Encoding]::new($false))
}
catch {
    Fail "デスクトップ再インストール情報を保存できません: $desktopBuildSourcePath`n$($_.Exception.Message)"
}

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
