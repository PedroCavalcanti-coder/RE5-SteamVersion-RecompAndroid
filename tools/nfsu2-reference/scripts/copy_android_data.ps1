param(
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [string]$Adb = 'adb',
    [string]$Serial = ''
)
$ErrorActionPreference = 'Stop'
$resolvedRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$adbArgs = @()
if ($Serial) { $adbArgs = @('-s', $Serial) }
function Invoke-Adb([string[]]$Arguments) {
    & $Adb @adbArgs @Arguments
    if ($LASTEXITCODE -ne 0) { throw "ADB failed: $($Arguments -join ' ')" }
}
$entries = @('CARS','CREDITS','FRONTEND','GLOBAL','LANGUAGES','MOVIES','NIS',
    'SDATA','SOUND','SUBTITLES','TRACKS','memcard','SPEED2.EXE','00000000.016',
    '00000000.256','filelist.txt','foobar','server.cfg','server.dll')
$expected = 'f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7'
if ((Get-FileHash -LiteralPath (Join-Path $resolvedRoot 'SPEED2.EXE') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'This SPEED2.EXE is not the validated game build.'
}
foreach ($entry in $entries) {
    if (-not (Test-Path -LiteralPath (Join-Path $resolvedRoot $entry))) { throw "Missing source: $entry" }
}
Invoke-Adb @('shell','mkdir','-p','/sdcard/nfsu2')
foreach ($entry in $entries) {
    # Do not overwrite personal progress when updating game assets.
    if ($entry -eq 'memcard') {
        & $Adb @adbArgs shell test -d /sdcard/nfsu2/memcard
        if ($LASTEXITCODE -eq 0) { Write-Output 'Keeping existing device memcard'; continue }
    }
    Invoke-Adb @('push',(Join-Path $resolvedRoot $entry),'/sdcard/nfsu2/')
}
Write-Output 'Game files copied to Internal storage/nfsu2. PC installers/DirectX are omitted.'
