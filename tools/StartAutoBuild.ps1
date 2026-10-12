$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

try {
    # installRoot: この起動スクリプトの配布先。
    $installRoot = Split-Path -Parent $PSScriptRoot
    # sourceRoot: 利用者が指定するソース。
    $sourceRoot = ''
    # sourceInfo: 再ビルド元の記録。
    $sourceInfo = Join-Path $installRoot 'desktop-build-source.json'
    if (Test-Path -LiteralPath $sourceInfo) {
        $sourceRoot = (Get-Content -LiteralPath $sourceInfo -Raw | ConvertFrom-Json).sourceRoot
    }
    if (-not $sourceRoot -or -not (Test-Path -LiteralPath (Join-Path $sourceRoot 'tools\RebuildAndInstallEditor.ps1'))) {
        # picker: ソースフォルダー選択画面。
        $picker = [System.Windows.Forms.FolderBrowserDialog]::new()
        try {
            $picker.Description = 'LamaPon-Engineのソース一式を選択してください。ゲーム用プロジェクトのC++自動ビルドはEditor内で動作します。'
            $picker.ShowNewFolderButton = $false
            if ($picker.ShowDialog() -ne 'OK') { exit 2 }
            $sourceRoot = $picker.SelectedPath
        }
        finally { $picker.Dispose() }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt')) -or
        -not (Test-Path -LiteralPath (Join-Path $sourceRoot 'src\LamaPon\LamaPon.h')) -or
        -not (Test-Path -LiteralPath (Join-Path $sourceRoot 'tools\RebuildAndInstallEditor.ps1'))) {
        throw 'LamaPon-Engineのソース一式が見つかりません。ソースを取得してから、そのフォルダーを選択してください。'
    }
    . (Join-Path $PSScriptRoot 'SetupBuildEnvironment.ps1')
    if (-not (Get-LamaPonBuildEnvironment).Ready) {
        [System.Windows.Forms.MessageBox]::Show('先にスタートメニューの「自動ビルド環境セットアップ」を実行してください。', 'LamaPon', 'OK', 'Information') | Out-Null
        exit 1
    }
    # confirmation: 出力作成前の同意。
    $confirmation = "エンジン本体を再ビルドして更新します。`nソース: $sourceRoot`nビルド先: $sourceRoot\build-release`nインストール先: $installRoot`nビルド用に数GBの空き容量が必要です。続行しますか？"
    if ([System.Windows.Forms.MessageBox]::Show($confirmation, 'LamaPon 自動ビルド', 'YesNo', 'Question') -ne 'Yes') { exit 2 }
    & (Join-Path $PSScriptRoot 'RebuildAndInstallEditor.ps1') -SourceRoot $sourceRoot -InstallRoot $installRoot
}
catch {
    [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'LamaPon 自動ビルド失敗', 'OK', 'Error') | Out-Null
    exit 1
}
