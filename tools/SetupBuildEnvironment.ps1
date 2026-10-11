# CheckOnly: 導入せず診断する。
param([switch]$CheckOnly)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# Get-LamaPonBuildEnvironment: Editorが検索する配置のビルド環境を診断する。
function Get-LamaPonBuildEnvironment {
    # minimumToolset: 配布SDK以上のコンパイラー。
    $minimumToolset = [version]'14.30.0'
    # buildInfoPath: 配布元のビルド情報。
    $buildInfoPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'build-info.json'
    if (Test-Path -LiteralPath $buildInfoPath) {
        # buildInfo: SDKを生成した環境。
        $buildInfo = Get-Content -LiteralPath $buildInfoPath -Raw | ConvertFrom-Json
        if ($buildInfo.compiler -match 'MSVC 19\.(\d+)\.') {
            $minimumToolset = [version]("14.$($Matches[1]).0")
        }
    }
    # vsRoot: Editorの検索基準。
    $vsRoot = Join-Path $env:ProgramFiles 'Microsoft Visual Studio'
    # candidates: VS環境候補。
    $candidates = @(Get-ChildItem -LiteralPath $vsRoot -Directory -ErrorAction SilentlyContinue |
        Get-ChildItem -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'Common7\Tools\VsDevCmd.bat') })
    # environments: MSVC版数付き候補。
    $environments = @(
        # candidate: 検査中のVS。
        foreach ($candidate in $candidates) {
            # toolset: 最新MSVCの配置。
            $toolset = Get-ChildItem -LiteralPath (Join-Path $candidate.FullName 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
                Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
            if ($toolset) {
                [pscustomobject]@{ Root = $candidate.FullName; Toolset = $toolset.FullName; Version = [version]$toolset.Name }
            }
        }
    )
    # selected: Editorと同じ最新環境。
    $selected = $environments | Sort-Object Version, Root -Descending | Select-Object -First 1
    # missing: 不足ツールの一覧。
    $missing = [System.Collections.Generic.List[string]]::new()
    if (-not $selected) {
        $missing.Add('MSVC x64 C++ビルドツール')
        $missing.Add('Visual Studio同梱CMake / Ninja')
    }
    else {
        if ($selected.Version -lt $minimumToolset) {
            $missing.Add("配布SDKに対応するMSVC（$minimumToolset 以上）")
        }
        if (-not (Test-Path (Join-Path $selected.Toolset 'bin\Hostx64\x64\cl.exe'))) {
            $missing.Add('MSVC x64 C++コンパイラー')
        }
        # cmakeRoot: VS同梱CMake一式。
        $cmakeRoot = Join-Path $selected.Root 'Common7\IDE\CommonExtensions\Microsoft\CMake'
        # cmakePath: 検査するCMake。
        $cmakePath = Join-Path $cmakeRoot 'CMake\bin\cmake.exe'
        if (-not (Test-Path $cmakePath)) {
            $missing.Add('Visual Studio C++ CMake tools')
        }
        else {
            # cmakeVersion: CMakeの版数出力。
            $cmakeVersion = & $cmakePath --version
            if ($LASTEXITCODE -ne 0 -or ($cmakeVersion -join "`n") -notmatch 'cmake version (\d+\.\d+\.\d+)' -or [version]$Matches[1] -lt [version]'3.25.0') {
                $missing.Add('CMake 3.25以上')
            }
        }
        if (-not (Test-Path (Join-Path $cmakeRoot 'Ninja\ninja.exe'))) {
            $missing.Add('Visual Studio同梱Ninja')
        }
    }
    # sdkRoot: Windows SDKの基準。
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    # usableSdk: ヘッダーとx64ライブラリが揃うSDK。
    $usableSdk = @(Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            (Test-Path (Join-Path $_.FullName 'um\Windows.h')) -and
            (Test-Path (Join-Path $_.FullName 'ucrt\stdio.h')) -and
            (Test-Path (Join-Path $sdkRoot "Lib\$($_.Name)\um\x64\kernel32.lib")) -and
            (Test-Path (Join-Path $sdkRoot "Lib\$($_.Name)\ucrt\x64\ucrt.lib"))
        })
    if ($usableSdk.Count -eq 0) { $missing.Add('Windows 10/11 SDK') }
    return [pscustomobject]@{ Ready = ($missing.Count -eq 0); Missing = $missing.ToArray(); VisualStudio = $selected; MinimumToolset = $minimumToolset }
}

# ドットソース時は診断関数だけを公開する。
if ($MyInvocation.InvocationName -eq '.') { return }

try {
    # status: 現在の環境診断。
    $status = Get-LamaPonBuildEnvironment
    if ($CheckOnly) {
        $status | ConvertTo-Json -Depth 4
        if ($status.Ready) { exit 0 } else { exit 1 }
    }
    Add-Type -AssemblyName System.Windows.Forms
    if ($status.Ready) {
        [System.Windows.Forms.MessageBox]::Show(
            'C++自動ビルドに必要な環境は揃っています。LamaPonを再起動して利用してください。',
            'LamaPon ビルド環境', 'OK', 'Information') | Out-Null
        exit 0
    }
    # message: 導入前の確認文。
    $message = "不足しているもの：`n" + ($status.Missing -join "`n") +
        "`n`nMicrosoft公式のVisual Studio Build Tools 2026を導入・更新します。" +
        "`nインターネット接続、管理者権限、数GB～十数GBの空き容量が必要です。" +
        "`nダウンロードする起動用ファイルは終了後に削除します。" +
        "`nMicrosoftのライセンス条件を確認してからインストールしてください。続行しますか？"
    if ([System.Windows.Forms.MessageBox]::Show($message, 'LamaPon ビルド環境セットアップ', 'YesNo', 'Question') -ne 'Yes') { exit 2 }
    # existingInstaller: 既存VSの変更ツール。
    $existingInstaller = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    if ($status.VisualStudio -and $status.VisualStudio.Version -ge $status.MinimumToolset -and (Test-Path -LiteralPath $existingInstaller)) {
        # workload: 導入済み製品のC++機能。
        $workload = if ((Split-Path -Leaf $status.VisualStudio.Root) -eq 'BuildTools') { 'Microsoft.VisualStudio.Workload.VCTools' } else { 'Microsoft.VisualStudio.Workload.NativeDesktop' }
        # modifyArguments: 現在のVSへ不足を追加する。
        $modifyArguments = 'modify --norestart --installPath "' + $status.VisualStudio.Root + '" --add ' + $workload + ' --includeRecommended --add Microsoft.VisualStudio.Component.VC.CMake.Project'
        # modifyProcess: 既存環境の変更画面。
        $modifyProcess = Start-Process -FilePath $existingInstaller -ArgumentList $modifyArguments -Verb RunAs -PassThru -Wait
        if ($modifyProcess.ExitCode -eq 3010) {
            [System.Windows.Forms.MessageBox]::Show('Windowsを再起動してから、ビルド環境セットアップを再実行してください。', 'LamaPon', 'OK', 'Information') | Out-Null
            exit 3010
        }
        if ($modifyProcess.ExitCode -ne 0 -or -not (Get-LamaPonBuildEnvironment).Ready) {
            throw 'Visual Studioの変更が完了していません。C++、Windows SDK、CMakeツールを選択して導入してください。'
        }
        [System.Windows.Forms.MessageBox]::Show('自動ビルド環境のセットアップが完了しました。LamaPonを再起動してください。', 'LamaPon', 'OK', 'Information') | Out-Null
        exit 0
    }
    # downloadDir: 起動用ファイルの保存先。
    $downloadDir = Join-Path $env:LOCALAPPDATA 'LamaPon\BuildToolsDownload'
    # bootstrapper: Microsoftの起動用EXE。
    $bootstrapper = Join-Path $downloadDir 'vs_buildtools.exe'
    New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-Host 'Microsoft公式のビルドツールを取得しています...'
        Invoke-WebRequest -UseBasicParsing -Uri 'https://aka.ms/vs/stable/vs_buildtools.exe' -OutFile $bootstrapper
        # signature: 起動前の署名検証。
        $signature = Get-AuthenticodeSignature -LiteralPath $bootstrapper
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(?:,|$)') {
            throw 'Microsoftの有効な署名を確認できないため、ダウンロードしたファイルは実行しません。'
        }
        # installPath: Editorが検出できる配置。
        $installPath = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\18\BuildTools'
        # parameters: ライセンス確認を残す導入引数。
        $parameters = '--wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.CMake.Project --installPath "' + $installPath + '"'
        # installer: 管理者権限のVS導入処理。
        $installer = Start-Process -FilePath $bootstrapper -ArgumentList $parameters -Verb RunAs -PassThru -Wait
        if ($installer.ExitCode -eq 3010) {
            [System.Windows.Forms.MessageBox]::Show('ビルドツールの導入にはWindowsの再起動が必要です。再起動後にこのセットアップをもう一度実行してください。', 'LamaPon', 'OK', 'Information') | Out-Null
            exit 3010
        }
        if ($installer.ExitCode -ne 0) { throw "ビルドツールの導入が完了しませんでした（終了コード: $($installer.ExitCode)）。" }
        $status = Get-LamaPonBuildEnvironment
        if (-not $status.Ready) {
            throw ("必要なツールがまだ不足しています：`n" + ($status.Missing -join "`n") + "`nVisual Studio Installerで、使用している最新のVisual StudioにもC++とCMakeツールを追加してください。")
        }
        [System.Windows.Forms.MessageBox]::Show('自動ビルド環境のセットアップが完了しました。LamaPonを再起動してください。', 'LamaPon', 'OK', 'Information') | Out-Null
    }
    finally {
        if (Test-Path -LiteralPath $bootstrapper) { Remove-Item -LiteralPath $bootstrapper -Force }
    }
}
catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    if (-not $CheckOnly) {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'LamaPon セットアップ失敗', 'OK', 'Error') | Out-Null
    }
    exit 1
}
