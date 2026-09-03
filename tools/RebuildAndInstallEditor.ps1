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

# NMake Makefiles（CMakeの既定ジェネレーター）はヘッダーの依存関係の
# 追跡が不完全で、更新されていない古いオブジェクトファイルが
# そのままリンクされてしまうことがある（実際に一度、ヒープ破損クラッシュの
# 原因になった）。Ninjaは依存関係の追跡が正確なので、切り替える。
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

# build-info.json（クラッシュレポートの Build revision の出どころ）は
# configure時にgitから作られる。上のNinja切り替えを通らないと
# 再構成されず、古いリビジョンを名乗るバイナリが入る（実際、
# 2026-08-18のクラッシュ追跡で別コミットを見させられた）。
# 数百ミリ秒なので毎回通す。
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
        # CMakeListsの install(TARGETS ...) と**同じ面々**をビルドする。
        # 以前はLamaPonHubだけをビルドしていたが、LamaPonCliはその
        # 依存に入っていない。手コピー時代はCLIをコピーして
        # いなかったので露呈しなかったが、cmake --install へ移した
        # とたん「LamaPonCli.exe が見つからない」で失敗するように
        # なった（--target clean が前回の成果物も消すため）。
        # ターゲットを足すときは install(TARGETS ...) も揃えること。
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

# 以前はここで7ファイルだけを手でコピーしていましたが、
# CRTのDLL・LamaPonCli・配布ヘッダ・build-info.jsonがリストに
# 無く、更新されないまま取り残されていました。
# 2026-08-18にこれが原因で、ツールチェーンをVS2022からVS18へ
# 移したとたん、古い14.36のCRTが残ったまま14.51のバイナリが
# 入り、エディターがログを1行も書かないまま落ちるように
# なりました。CMakeListsの install 規則を唯一の正として使います。
& cmake --install $buildDir --prefix $installDir
if ($LASTEXITCODE -ne 0) {
    Fail "インストールに失敗しました。上のログを確認してください。"
}

Write-Host ""
Write-Host "[3/3] 完了しました。LamaPon Hub を起動します..."
Start-Process (Join-Path $installDir "LamaPonHub.exe")

Write-Host ""
Write-Host "==============================================="
Write-Host " 成功しました！"
Write-Host "==============================================="
if (-not $NonInteractive) {
    Read-Host "終了するには Enter キーを押してください"
}
