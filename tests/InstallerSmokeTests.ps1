# InstallerPath: 配布EXE、TestRoot: 試験ファイルの保存先。
param([Parameter(Mandatory = $true)][string]$InstallerPath, [string]$TestRoot = 'out\installer\smoke')

$ErrorActionPreference = 'Stop'
# repoRoot: テストの基準。
$repoRoot = Split-Path -Parent $PSScriptRoot
# outputRoot: 試験出力の絶対パス。
$outputRoot = if ([IO.Path]::IsPathRooted($TestRoot)) { [IO.Path]::GetFullPath($TestRoot) } else { [IO.Path]::GetFullPath((Join-Path $repoRoot $TestRoot)) }
# executable: 試験するSetup.exe。
$executable = (Resolve-Path -LiteralPath $InstallerPath).Path
# installRoot: 日本語と空白を含む配置。
$installRoot = Join-Path $outputRoot '日本語 path\LamaPon'
# uninstallKey: 製品固有の登録先。
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{C252A97B-7C30-4572-87D3-3DAC6BAF931A}_is1'
if (Test-Path -LiteralPath $uninstallKey) { throw '既存のLamaPonインストールを保護するため、試験は中止しました。別のWindowsユーザーで実行してください。' }
if (Test-Path -LiteralPath $installRoot) { throw "試験先が既に存在します: $installRoot" }
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
# originalTemp: 元の一時出力先。
$originalTemp = $env:TEMP
# originalTmp: 元の一時出力先。
$originalTmp = $env:TMP
# installed: 導入が成功したか。
$installed = $false
try {
    $env:TEMP = $outputRoot
    $env:TMP = $outputRoot
    # setupArguments: 本体のみの無人導入。
    $setupArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOICONS', '/TASKS=""', ('/DIR="' + $installRoot + '"'), ('/LOG="' + (Join-Path $outputRoot 'install.log') + '"'))
    # setup: 試験用導入プロセス。
    $setup = Start-Process -FilePath $executable -ArgumentList $setupArguments -WindowStyle Hidden -PassThru -Wait
    if ($setup.ExitCode -ne 0) { throw "インストール失敗: $($setup.ExitCode)" }
    $installed = $true
    if (-not (Test-Path -LiteralPath $uninstallKey)) { throw 'Windowsのアンインストール登録がありません。' }
    # payloadRoot: 比較元の配布一式。
    $payloadRoot = Join-Path (Split-Path -Parent $executable) 'payload'
    # relativePath: コピーの完全性を調べるファイル。
    foreach ($relativePath in @('LamaPonHub.exe', 'LamaPonEditor.exe', 'LamaPonGame.exe', 'LamaPonGameLauncher.exe', 'LamaPonCli.exe', 'LamaPonRuntime.dll', 'LamaPonRuntime.lib', 'vcruntime140.dll', 'msvcp140.dll', 'xaudio2_9redist.dll', 'tools\SetupBuildEnvironment.ps1', 'tools\StartAutoBuild.ps1', 'tools\RebuildAndInstallEditor.ps1', 'src\LamaPon\LamaPon.h', 'tools\ProjectGameModule\CMakeLists.txt', 'docs\installation.md', 'licenses\LamaPon.txt')) {
        if ((Get-FileHash -LiteralPath (Join-Path $installRoot $relativePath)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $payloadRoot $relativePath)).Hash) { throw "配置ファイルの内容が異なります: $relativePath" }
    }
    # cliOutput: ランタイム読込を伴う版数表示。
    $cliOutput = & (Join-Path $installRoot 'LamaPonCli.exe') --version
    if ($LASTEXITCODE -ne 0) { throw "CLIを起動できません: $cliOutput" }
    Write-Host ($cliOutput -join "`n")
    # diagnostic: 配布スクリプトの診断出力。
    $diagnostic = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $installRoot 'tools\SetupBuildEnvironment.ps1') -CheckOnly
    if ($LASTEXITCODE -ne 0 -or -not (($diagnostic -join "`n" | ConvertFrom-Json).Ready)) { throw '配布スクリプトのビルド環境診断が失敗しました。' }
    Write-Host 'PASS: 日本語パスへの導入、SDK・DLLの完全性、CLI起動、環境診断'
    $setup = Start-Process -FilePath $executable -ArgumentList $setupArguments -WindowStyle Hidden -PassThru -Wait
    if ($setup.ExitCode -ne 0) { throw "上書き更新に失敗しました: $($setup.ExitCode)" }
    if (-not (Test-Path -LiteralPath (Join-Path $installRoot 'LamaPonHub.exe'))) { throw '更新後にHubが見つかりません。' }
    Write-Host 'PASS: 同じ保存先への上書き更新'
}
finally {
    if ($installed) {
        # uninstaller: 試験導入を解除する処理。
        $uninstaller = Start-Process -FilePath (Join-Path $installRoot 'unins000.exe') -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG="' + (Join-Path $outputRoot 'uninstall.log') + '"')) -WindowStyle Hidden -PassThru -Wait
        if ($uninstaller.ExitCode -ne 0) { throw "試験導入のアンインストール失敗: $($uninstaller.ExitCode)" }
        if (Test-Path -LiteralPath $uninstallKey) { throw '試験用アンインストール登録が残っています。' }
        if (Test-Path -LiteralPath (Join-Path $installRoot 'LamaPonRuntime.dll')) { throw 'アンインストール後に配布DLLが残っています。' }
        Write-Host 'PASS: アンインストールと登録の解除'
    }
    $env:TEMP = $originalTemp
    $env:TMP = $originalTmp
}
