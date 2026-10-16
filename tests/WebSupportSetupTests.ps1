param([string]$WorkRoot)
$ErrorActionPreference='Stop'
$Root=Join-Path $WorkRoot ('web-support-setup-'+$PID)
. (Join-Path $PSScriptRoot '..\tools\SetupWebSupport.ps1') -Root $Root
if (Test-Path -LiteralPath $Root) { throw 'Test directory must start absent.' }
$ready=Get-WebSupportTools
if (Test-Path -LiteralPath $Root) { throw 'Check-only detection must not create files.' }
try {
    Install-WebArchive 'https://example.invalid/tools.zip' ('0'*64) (Join-Path $Root 'tools') 'denied'
    throw 'Missing download consent was accepted.'
}
catch { if ($_.Exception.Message -notmatch 'ダウンロードの許可') { throw } }
if (Test-Path -LiteralPath $Root) { throw 'Refusing downloads must not create files.' }

# Reusing an already installed toolchain must not access the network.
function Invoke-WebRequest { throw 'Unexpected network access in reuse test.' }
if ($ready.ready) {
$installed=Install-WebSupport
if (-not $installed.ready -or -not (Test-Path -LiteralPath (Join-Path $Root 'web-support.json'))) {
    throw 'Existing tools were not registered.'
}
if ($installed.emsdk -ne $ready.emsdk) { throw 'Existing SDK should be reused without copying it.' }
}

# Corrupt archives must be rejected before extraction or activation.
$AllowDownloads=$true
function Invoke-WebRequest { param([switch]$UseBasicParsing,$Uri,$OutFile); [IO.File]::WriteAllText($OutFile,'corrupt') }
$caught=$false
try { Install-WebArchive 'https://example.invalid/tools.zip' ('0'*64) (Join-Path $Root 'untrusted') 'corrupt' }
catch { $caught=$_.Exception.Message -match '検証に失敗' }
if (-not $caught -or (Test-Path -LiteralPath (Join-Path $Root 'untrusted'))) {
    throw 'A corrupt archive must not be extracted.'
}
Write-Output 'Web support setup: check-only, consent, reuse, manifest and checksum tests passed.'
