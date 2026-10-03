# ErrorActionPreference: 予期しないPowerShellエラーを停止理由にする。
﻿$ErrorActionPreference = "Stop"
# NonInteractive: CI実行時に入力待ちを省略する。
$NonInteractive = $args -contains "-NonInteractive"

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
# OutputEncoding: 外部コマンドへUTF-8を渡す。
$OutputEncoding = [System.Text.Encoding]::UTF8

# repoDir: スクリプトが属するrepository root。
$repoDir = Split-Path -Parent $PSScriptRoot
# buildDir: Release buildの出力先。
$buildDir = Join-Path $repoDir "build-release"
# installDir: ユーザー向けengineの配置先。
$installDir = Join-Path $env:LOCALAPPDATA "Programs\LamaPon"

# Fail(message: エラー説明): エラーを表示してスクリプトを終了する。
function Fail([string]$message) {
    Write-Host ""
    Write-Host "[エラー] $message" -ForegroundColor Red
    Write-Host "==============================================="
    Write-Host " 失敗しました。上のメッセージを確認してください。"
    Write-Host "==============================================="
    # 対話起動の場合だけキー入力を待つ。
    if (-not $NonInteractive) {
        Read-Host "終了するには Enter キーを押してください"
    }
    exit 1
}

# CloseRunningLamaPonProcesses: 指定時だけ既存LamaPon processを終了する。
$CloseRunningLamaPonProcesses =
    $args -contains "-CloseRunningLamaPonProcesses"
# WaitForProcessId: 待機対象Editorのprocess ID。
$WaitForProcessId = 0
# argumentIndex: WaitForProcessId optionの検索位置。
for ($argumentIndex = 0;
    $argumentIndex -lt $args.Count;
    $argumentIndex++) {
    # PIDオプション以外の引数を飛ばす。
    if ($args[$argumentIndex] -ne "-WaitForProcessId") {
        continue
    }
    # PID値が続かない引数を拒否する。
    if ($argumentIndex + 1 -ge $args.Count) {
        Fail "-WaitForProcessId には待機するプロセスIDを指定してください。"
    }
    # parsedProcessId: 解析した待機process ID。
    $parsedProcessId = 0
    # validProcessId: PIDを正の整数として解析できたか。
    $validProcessId = [int]::TryParse(
        [string]$args[$argumentIndex + 1],
        [ref]$parsedProcessId)
    # 正の整数でないPIDを拒否する。
    if (-not $validProcessId -or $parsedProcessId -le 0) {
        Fail "-WaitForProcessId には正のプロセスIDを指定してください。"
    }
    $WaitForProcessId = $parsedProcessId
    $argumentIndex++
}

# Sync-InstalledSdkDirectory(sourceRelativePath: SDK元, destinationRelativePath: 配置先, filter: 対象名): SDKファイルをハッシュ検証して同期する。
# RuntimeとSDKの世代がずれると、ABI不一致でGame Moduleを読み込めない。
function Sync-InstalledSdkDirectory(
    [string]$sourceRelativePath,
    [string]$destinationRelativePath,
    [string]$filter = "*") {
    # sourceDirectory: repository内のSDK source folder。
    $sourceDirectory = Join-Path $repoDir $sourceRelativePath
    # destinationDirectory: SDKのinstall folder。
    $destinationDirectory = Join-Path $installDir $destinationRelativePath
    # SDK元フォルダーの存在を確認する。
    if (-not (Test-Path $sourceDirectory -PathType Container)) {
        Fail "Game Module SDKのコピー元フォルダーが見つかりません: $sourceDirectory"
    }

    # SDK元ファイルを順に同期・検証する。
    Get-ChildItem -LiteralPath $sourceDirectory -Recurse -File -Filter $filter | ForEach-Object {
        # relativeFilePath: SDK sourceからのrelative path。
        $relativeFilePath = $_.FullName.Substring($sourceDirectory.Length).TrimStart('\')
        # destination: SDK fileのinstall先。
        $destination = Join-Path $destinationDirectory $relativeFilePath
        # destinationParent: SDK fileの親folder。
        $destinationParent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null

        # sourceHash: SDK元fileのSHA-256。
        $sourceHash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
        # destinationHash: 既存配置fileのSHA-256。
        $destinationHash = if (Test-Path $destination -PathType Leaf) {
            (Get-FileHash -Algorithm SHA256 $destination).Hash
        }
        else {
            ""
        }
        # ハッシュが異なるファイルだけコピーする。
        if ($sourceHash -ne $destinationHash) {
            # SDKファイルの同期を試す。
            try {
                Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
            }
            # コピー失敗を明確なエラーへ変換する。
            catch {
                Fail "Game Module SDKを同期できません: $destination`n$($_.Exception.Message)"
            }
        }

        # installedHash: コピー後fileのSHA-256。
        $installedHash = (Get-FileHash -Algorithm SHA256 $destination).Hash
        # 配置後の内容が元ファイルと一致するか確認する。
        if ($sourceHash -ne $installedHash) {
            Fail "インストール後のGame Module SDKが一致しません: $destination"
        }
    }
}

# Sync-InstalledSdkFile(relativePath: SDK相対パス): 単一SDKファイルをハッシュ検証して同期する。
function Sync-InstalledSdkFile([string]$relativePath) {
    # リポジトリ内のSDK元ファイル
    $source = Join-Path $repoDir $relativePath
    # インストール先のSDKファイル
    $destination = Join-Path $installDir $relativePath
    # SDK元ファイルの存在を確認する。
    if (-not (Test-Path $source -PathType Leaf)) {
        Fail "Game Module SDKのコピー元ファイルが見つかりません: $source"
    }

    # 配置先の親フォルダーを作成する。
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    # sourceHash: SDK元fileのSHA-256。
    $sourceHash = (Get-FileHash -Algorithm SHA256 $source).Hash
    # destinationHash: 既存配置fileのSHA-256。
    $destinationHash = if (Test-Path $destination -PathType Leaf) {
        (Get-FileHash -Algorithm SHA256 $destination).Hash
    }
    else {
        ""
    }
    # ハッシュが異なるファイルだけコピーする。
    if ($sourceHash -ne $destinationHash) {
        # Sync-InstalledSdkFile: 単一SDK fileを同期する。
        try {
            Copy-Item -LiteralPath $source -Destination $destination -Force
        }
        # コピー失敗を明確なエラーへ変換する。
        catch {
            Fail "Game Module SDKを同期できません: $destination`n$($_.Exception.Message)"
        }
    }

    # installedHash: コピー後fileのSHA-256。
    $installedHash = (Get-FileHash -Algorithm SHA256 $destination).Hash
    # 配置後の内容が元ファイルと一致するか確認する。
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

# エディターから引き継いだプロセスの終了を待つ。
if ($WaitForProcessId -gt 0) {
    Write-Host "エディターの終了を待機しています（PID: $WaitForProcessId）..."
    # waitDeadline: 指定processの終了待ち期限。
    $waitDeadline = [DateTime]::UtcNow.AddSeconds(30)
    # 指定PIDが終了するまで確認する。
    while (Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue) {
        # 30秒の待機期限を超えた場合を検出する。
        if ([DateTime]::UtcNow -ge $waitDeadline) {
            Fail "指定されたエディターが30秒以内に終了しませんでした。"
        }
        Start-Sleep -Milliseconds 250
    }
}

# hubProc: 起動中のHub process。
$hubProc = Get-Process -Name "LamaPonHub" -ErrorAction SilentlyContinue
# editorProc: 起動中のEditor process。
$editorProc = Get-Process -Name "LamaPonEditor" -ErrorAction SilentlyContinue
# インストール前に終了が必要なプロセスがあるか調べる。
if ($hubProc -or $editorProc) {
    Write-Host "[警告] LamaPon Hub / LamaPon Editor が起動中です。" -ForegroundColor Yellow
    Write-Host "保存していない作業がある場合は失われます。"
    # 非対話実行で明示許可なくプロセスを終了しない。
    if ($NonInteractive -and -not $CloseRunningLamaPonProcesses) {
        Fail "LamaPon Hub / LamaPon Editor が起動中です。終了してから再実行してください。"
    }
    # 対話起動では続行を確認する。
    if (-not $NonInteractive) {
        Read-Host "続行するには Enter キーを押してください（中止する場合はこのウィンドウを閉じてください）"
    }
    # 起動中のHubを終了する。
    if ($hubProc) { $hubProc | Stop-Process -Force }
    # 起動中のEditorを終了する。
    if ($editorProc) { $editorProc | Stop-Process -Force }
    Start-Sleep -Milliseconds 500
}

Write-Host ""
Write-Host "Visual Studio のビルド環境を検索しています..."
# vswhere: Visual Studio検索toolのpath。
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
# Visual Studio検索ツールの存在を確認する。
if (-not (Test-Path $vswhere)) {
    Fail "vswhere.exe が見つかりません。Visual Studio がインストールされているか確認してください。"
}

# vsPath: MSBuildを備えたVisual Studioのpath。
$vsPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
# Visual Studioの検出結果を確認する。
if (-not $vsPath) {
    Fail "Visual Studio のインストール先を特定できませんでした。"
}

# vcvars: x64 compiler環境設定batchのpath。
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
# コンパイラ環境設定バッチの存在を確認する。
if (-not (Test-Path $vcvars)) {
    Fail "vcvars64.bat が見つかりません: $vcvars"
}

# ninja: Visual Studio同梱Ninjaのpath。
$ninja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

# vcvars64.batをcmdで実行し、設定された環境変数を取り込む。
# cmdOutput: 初期化後のcmd環境変数一覧。
$cmdOutput = cmd /c "`"$vcvars`" 2>nul && set"
# cmd環境変数をPowerShellへ反映します。
# line: cmd環境変数一覧の1行。
foreach ($line in $cmdOutput) {
    # 変数名と値に分かれた行だけ反映する。
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

# ヘッダー依存を追跡するNinja構成を使う。
# 別ジェネレーターのキャッシュは削除して再構成する。
# cacheFile: CMake generatorを確認するcache path。
$cacheFile = Join-Path $buildDir "CMakeCache.txt"
# usingNinja: build folderがNinja generatorか。
$usingNinja = (Test-Path $cacheFile) `
    -and (Select-String -Path $cacheFile -Pattern "^CMAKE_GENERATOR:INTERNAL=Ninja$" -Quiet)

# Ninja構成でない場合はビルドフォルダーを再構成する。
if (-not $usingNinja) {
    Write-Host ""
    Write-Host "ビルド設定をNinja（信頼性の高いビルドツール）に切り替えています..."
    Write-Host "（初回のみ時間がかかります）"
    # 既存キャッシュを含むビルドフォルダーを削除する。
    if (Test-Path $buildDir) {
        Remove-Item $buildDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

    # Ninja実行ファイルの存在を確認する。
    if (-not (Test-Path $ninja)) {
        Fail "ninja.exe が見つかりません: $ninja"
    }

    Push-Location $buildDir
    # Ninja構成を作成し、終了時は必ず作業場所を戻す。
    try {
        & cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$ninja" $repoDir
        # configureResult: CMake configureの終了code。
        $configureResult = $LASTEXITCODE
    }
    # Ninja構成の処理後に元の場所へ戻る。
    finally {
        Pop-Location
    }
    # Ninja構成の失敗を検出する。
    if ($configureResult -ne 0) {
        Fail "ビルド設定に失敗しました。上のログを確認してください。"
    }
}

# BuildInfoへ最新のGit revisionを含めるためビルド前に再構成する。
Push-Location $buildDir
# CMake構成を更新し、終了時は作業場所を戻す。
try {
    & cmake .
    # configureResult: CMake reconfigureの終了code。
    $configureResult = $LASTEXITCODE
}
    # CMake再構成後に元の場所へ戻る。
    finally {
    Pop-Location
}
# CMake再構成の失敗を検出する。
if ($configureResult -ne 0) {
    Fail "ビルド設定の更新に失敗しました。上のログを確認してください。"
}

Write-Host ""
Write-Host "[1/3] クリーンビルド中...（数分かかることがあります）"
Push-Location $buildDir
# cleanとビルド後に作業場所を戻す。
try {
    # 古いオブジェクトによるABI不一致を避けるためcleanする。
    & cmake --build . --target clean
    # buildResult: cleanまたはbuildの終了code。
    $buildResult = $LASTEXITCODE
    # cleanに成功した場合だけ必要な配布ターゲットを生成する。
    if ($buildResult -eq 0) {
        # cmake --install対象をすべて生成する。
        # ターゲット追加時はinstall規則とこの一覧をそろえる。
        & cmake --build . --target `
            LamaPonRuntime LamaPonEditorApp LamaPonHub `
            LamaPonGame LamaPonCli
        # 配布ターゲットのビルド終了コード
        $buildResult = $LASTEXITCODE
    }
}
    # ビルド後に元の場所へ戻る。
    finally {
    Pop-Location
}

# クリーンビルドの失敗を検出する。
if ($buildResult -ne 0) {
    Fail "ビルドに失敗しました。上のログを確認してください。"
}

Write-Host ""
Write-Host "[2/3] インストール先へコピーしています..."
# インストール先フォルダーの存在を確認する。
if (-not (Test-Path $installDir)) {
    Fail "インストール先フォルダが見つかりません: $installDir"
}

# Runtime、CLI、公開ヘッダーなどをinstall規則で更新する。
& cmake --install $buildDir --prefix $installDir
# CMake installの失敗を検出する。
if ($LASTEXITCODE -ne 0) {
    Fail "インストールに失敗しました。上のログを確認してください。"
}

# CMake install後もEXEとRuntime DLLの世代をハッシュで照合する。
# 不一致なら同期し、古い起動ファイルを残さない。
# runtimeFiles: install先と照合する実行file・Runtime一覧。
$runtimeFiles = @(
    "LamaPonRuntime.dll",
    "LamaPonRuntime.lib",
    "LamaPonEditor.exe",
    "LamaPonHub.exe",
    "LamaPonGame.exe",
    "LamaPonCli.exe"
)
# 各ビルド成果物をインストール先と照合する。
# fileName: 照合中の実行file名。
foreach ($fileName in $runtimeFiles) {
    # source: build folder内の成果物path。
    $source = Join-Path $buildDir $fileName
    # destination: install先の成果物path。
    $destination = Join-Path $installDir $fileName
    # ビルド成果物の存在を確認する。
    if (-not (Test-Path $source)) {
        Fail "ビルド成果物が見つかりません: $source"
    }

    # sourceHash: build成果物のSHA-256。
    $sourceHash = (Get-FileHash -Algorithm SHA256 $source).Hash
    # destinationHash: 既存install fileのSHA-256。
    $destinationHash = if (Test-Path $destination) {
        (Get-FileHash -Algorithm SHA256 $destination).Hash
    }
    else {
        ""
    }
    # ハッシュが異なる成果物だけコピーする。
    if ($sourceHash -ne $destinationHash) {
        # 実行ファイルの同期を試す。
        try {
            Copy-Item -LiteralPath $source -Destination $destination -Force
        }
        # コピー失敗を明確なエラーへ変換する。
        catch {
            Fail "実行ファイルを同期できません: $destination`n$($_.Exception.Message)"
        }
    }

    # installedHash: コピー後install fileのSHA-256。
    $installedHash = (Get-FileHash -Algorithm SHA256 $destination).Hash
    # 配置後の内容がビルド成果物と一致するか確認する。
    if ($sourceHash -ne $installedHash) {
        Fail "インストール後の実行ファイルが一致しません: $destination"
    }
}

# ProjectGameModuleが参照するSDKと全公開ヘッダーをRuntime同様に同期・検証する。
# 公開APIや同梱依存の追加時は同期対象一覧も更新する。
Write-Host "Game Module SDKを同期・検証しています..."
Sync-InstalledSdkDirectory "tools\ProjectGameModule" "tools\ProjectGameModule"
Sync-InstalledSdkDirectory "src\LamaPon" "src\LamaPon" "*.h"
Sync-InstalledSdkDirectory "third_party\nlohmann" "third_party\nlohmann" "*.hpp"
Sync-InstalledSdkDirectory "third_party\DirectXTK\include" "third_party\DirectXTK\include"
Sync-InstalledSdkDirectory "third_party\XAudio2Redist\include" "third_party\XAudio2Redist\include"
Sync-InstalledSdkFile "cmake\LamaPonMsvcDependencies.cmake"

# インストール済みEditorが再ビルド元を見つけるための情報を保存する。
# RuntimeとSDKは同じ実行で更新し、互換世代をそろえる。
# desktopBuildSourcePath: 再install用repository情報file。
$desktopBuildSourcePath = Join-Path $installDir "desktop-build-source.json"
# desktopBuildSource: install済みEditorへ渡すrepository情報JSON。
$desktopBuildSource = [ordered]@{
    format = "LamaPonDesktopBuildSource"
    version = 1
    sourceRoot = [System.IO.Path]::GetFullPath($repoDir)
} | ConvertTo-Json -Compress
# 再インストール元情報を保存する。
try {
    [System.IO.File]::WriteAllText(
        $desktopBuildSourcePath,
        $desktopBuildSource,
        [System.Text.UTF8Encoding]::new($false))
}
# 保存失敗を明確なエラーへ変換する。
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
# 対話起動の場合だけ完了後の入力を待つ。
if (-not $NonInteractive) {
    Read-Host "終了するには Enter キーを押してください"
}
