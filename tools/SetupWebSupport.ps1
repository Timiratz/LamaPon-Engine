# The editor obtains consent before passing -Install -AllowDownloads.
# Check-only and dot-source modes never create files or download anything.
param(
    [switch]$Install,
    [switch]$AllowDownloads,
    [string]$Root = '',
    [string]$ResultPath = ''
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

function Get-WebSupportRoot {
    if ($Root) { return [IO.Path]::GetFullPath($Root) }
    return Join-Path $env:LOCALAPPDATA 'LamaPon\toolchains\web'
}

function Test-WebSdk([string]$Sdk) {
    return $Sdk -and (Test-Path -LiteralPath (Join-Path $Sdk '.emscripten') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Sdk 'upstream\emscripten\emcmake.py') -PathType Leaf)
}

function Find-WebPython([string]$Sdk, [string]$ManagedRoot, [string]$Configured = '') {
    $candidates = [Collections.Generic.List[string]]::new()
    if ($Configured -and $Configured -notmatch '\\WindowsApps\\') { $candidates.Add($Configured) }
    foreach ($base in @((Join-Path $ManagedRoot 'emsdk-6.0.9\python'), $(if ($Sdk) { Join-Path $Sdk 'python' }))) {
        if ($base -and (Test-Path -LiteralPath $base)) {
            Get-ChildItem -LiteralPath $base -Directory | Sort-Object Name -Descending | ForEach-Object {
                $candidates.Add((Join-Path $_.FullName 'python.exe'))
            }
        }
    }
    $candidates.Add((Join-Path $ManagedRoot 'python-3.13.3\python.exe'))
    $onPath = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($onPath -and $onPath.Source -notmatch '\\WindowsApps\\') { $candidates.Add($onPath.Source) }
    $launcher = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($launcher) {
        $actual = & $launcher.Source -3 -I -S -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0 -and $actual) { $candidates.Add([string]@($actual)[-1]) }
    }
    $pythonBase = Join-Path $env:LOCALAPPDATA 'Programs\Python'
    if (Test-Path -LiteralPath $pythonBase) {
        Get-ChildItem -LiteralPath $pythonBase -Directory | Sort-Object Name -Descending | ForEach-Object {
            $candidates.Add((Join-Path $_.FullName 'python.exe'))
        }
    }
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        & $candidate -I -S -c 'import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)' 2>$null
        if ($LASTEXITCODE -eq 0) { return $candidate }
    }
    return ''
}

function Get-WebSupportTools {
    $managed = Get-WebSupportRoot
    $saved = @{}
    $settings = Join-Path $env:LOCALAPPDATA 'LamaPon\web-export-tools.json'
    if (Test-Path -LiteralPath $settings) {
        try { $saved = Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json } catch { $saved = @{} }
    }
    $sdk = ''
    foreach ($candidate in @((Join-Path $managed 'emsdk-6.0.9'), $saved.emsdk, $env:EMSDK,
        (Join-Path $env:USERPROFILE 'Tools\emsdk'), (Join-Path $env:USERPROFILE 'emsdk'))) {
        if (Test-WebSdk $candidate) { $sdk = $candidate; break }
    }
    if (-not $sdk) {
        $entry = Get-Command emcmake -ErrorAction SilentlyContinue
        if ($entry) {
            $candidate = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $entry.Source))
            if (Test-WebSdk $candidate) { $sdk = $candidate }
        }
    }
    $python = Find-WebPython $sdk $managed $saved.python
    $cmake = ''; $ninja = ''
    $cmakeCandidates = @((Join-Path $managed 'cmake-3.31.6-windows-x86_64\bin\cmake.exe'))
    $ninjaCandidates = @((Join-Path $managed 'ninja-1.12.1\ninja.exe'))
    foreach ($name in @('cmake','ninja')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) {
            if ($name -eq 'cmake') { $cmakeCandidates += $command.Source }
            else { $ninjaCandidates += $command.Source }
        }
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vs = & $vswhere -latest -products '*' -property installationPath
        if ($vs) {
            $base = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
            $cmakeCandidates += Join-Path $base 'CMake\bin\cmake.exe'
            $ninjaCandidates += Join-Path $base 'Ninja\ninja.exe'
        }
    }
    foreach ($candidate in $cmakeCandidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $version = & $candidate --version 2>$null
        if ($LASTEXITCODE -eq 0 -and ($version -join "`n") -match 'cmake version (\d+\.\d+\.\d+)' -and [version]$Matches[1] -ge [version]'3.25.0') {
            $cmake = $candidate; break
        }
    }
    foreach ($candidate in $ninjaCandidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $null = & $candidate --version 2>$null
        if ($LASTEXITCODE -eq 0) { $ninja = $candidate; break }
    }
    return [ordered]@{ format='lamapon.web-support'; version=1; ready=[bool]($sdk -and $python -and $cmake -and $ninja)
        python=$python; emsdk=$sdk; cmake=$cmake; ninja=$ninja }
}

function Install-WebArchive([string]$Url, [string]$Sha256, [string]$Destination, [string]$Name) {
    if (-not $AllowDownloads) { throw 'ダウンロードの許可がありません。準備画面で確認してから実行してください。' }
    $base = Get-WebSupportRoot
    $resolved = [IO.Path]::GetFullPath($Destination)
    if ($resolved -ne $base -and -not $resolved.StartsWith($base.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid managed tool directory.' }
    $cache = Join-Path $base 'downloads'
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $archive = Join-Path $cache ($Name+'.zip')
    Write-Host "$Name をダウンロードしています..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $archive
    # 取得ZIPの読み取りストリーム
    $archiveStream = [IO.File]::OpenRead($archive)
    # 標準.NETのSHA-256計算器
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        # 検証用の16進ハッシュ
        $actualHash = [BitConverter]::ToString($hasher.ComputeHash($archiveStream)).Replace('-', '')
    }
    finally { $hasher.Dispose(); $archiveStream.Dispose() }
    if ($actualHash -ne $Sha256) { throw "$Name の検証に失敗しました。再試行してください。" }
    New-Item -ItemType Directory -Path $resolved -Force | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            $target = [IO.Path]::GetFullPath((Join-Path $resolved $entry.FullName))
            if (-not $target.StartsWith($resolved.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Archive contains an invalid path.' }
            if (-not $entry.Name) { [IO.Directory]::CreateDirectory($target) | Out-Null }
            else {
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
            }
        }
    }
    finally { $zip.Dispose() }
    Remove-Item -LiteralPath $archive -Force
}

function Install-WebSupport {
    $base = Get-WebSupportRoot
    $state = Get-WebSupportTools
    if (-not $state.python) {
        Install-WebArchive 'https://www.python.org/ftp/python/3.13.3/python-3.13.3-embed-amd64.zip' '59ff76e16e6597de47474fb22be69e7191a89116910d728ab735079b078e52db' (Join-Path $base 'python-3.13.3') 'Python'
        $state.python = Join-Path $base 'python-3.13.3\python.exe'
    }
    if (-not $state.emsdk) {
        $commit = '5eb0bde7585670252e8ba05e9d361627bffd08b5'
        Install-WebArchive "https://github.com/emscripten-core/emsdk/archive/$commit.zip" '3275c97800ed4cd31ae5281e497d175124012c4aa4d8ae05f22bc1c61168ab5c' (Join-Path $base 'sdk-source') 'Emscripten'
        $sdk = Join-Path $base 'emsdk-6.0.9'
        $source = [IO.Path]::GetFullPath((Join-Path $base "sdk-source\emsdk-$commit"))
        if (-not $source.StartsWith($base.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase) -or
            -not $sdk.StartsWith($base.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid SDK installation path.' }
        if (-not (Test-Path -LiteralPath $sdk)) { Move-Item -LiteralPath $source -Destination $sdk }
        $env:EMSDK_PYTHON = $state.python
        foreach ($action in @('install','activate')) {
            Write-Host "Webコンパイラーを準備しています（$action）..."
            & $state.python (Join-Path $sdk 'emsdk.py') $action '6.0.9' | ForEach-Object { Write-Host $_ }
            if ($LASTEXITCODE -ne 0) { throw 'Webコンパイラーの準備に失敗しました。ログを確認して再試行してください。' }
        }
    }
    if (-not $state.cmake) {
        Install-WebArchive 'https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip' 'd163cd3ab4959b0a53fa8988f2ddbd2e6c501658201e6a154386bad9dbe4f836' $base 'CMake'
    }
    if (-not $state.ninja) {
        Install-WebArchive 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' 'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a' (Join-Path $base 'ninja-1.12.1') 'Ninja'
    }
    $state = Get-WebSupportTools
    if (-not $state.ready) { throw 'Web出力サポートの確認に失敗しました。ログを確認して再試行してください。' }
    $env:EMSDK_PYTHON = $state.python
    $env:EM_CONFIG = Join-Path $state.emsdk '.emscripten'
    $null = & $state.python (Join-Path $state.emsdk 'upstream\emscripten\emcc.py') --version
    if ($LASTEXITCODE -ne 0) { throw 'Webコンパイラーを起動できませんでした。準備を再実行してください。' }
    New-Item -ItemType Directory -Path $base -Force | Out-Null
    $manifest = Join-Path $base 'web-support.json'
    $temporary = $manifest+'.tmp'
    [IO.File]::WriteAllText($temporary, ($state | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $manifest -Force
    return $state
}

if ($MyInvocation.InvocationName -eq '.') { return }
try {
    if ($Install) { $state = Install-WebSupport } else { $state = Get-WebSupportTools }
    $result = @{ok=[bool]$state.ready; message=$(if ($state.ready) {'Web出力サポートの準備ができました。出力先を選んでエクスポートできます。'} else {'Web出力サポートの準備が必要です。'}); tools=$state}
}
catch { $result = @{ok=$false; message=$_.Exception.Message} }
if ($ResultPath) { [IO.File]::WriteAllText($ResultPath, ($result | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false)) }
$result | ConvertTo-Json -Depth 5
if ($result.ok) { exit 0 } else { exit 1 }
