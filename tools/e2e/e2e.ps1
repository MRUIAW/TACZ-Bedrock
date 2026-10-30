# Helpers for manual e2e runs against one already-running headless instance.
param(
    [Parameter(Mandatory)][string]$InstanceRoot,
    [Parameter(Mandatory)][string]$BdsRoot,
    [Parameter(Mandatory)][string]$ShotsDir,
    [string]$HeadlessPipe,
    [int]$ServerProcessId
)
$script:Root = (Resolve-Path $InstanceRoot).Path
$script:BdsRoot = (Resolve-Path $BdsRoot).Path
$Instance = Split-Path $script:Root -Leaf
Import-Module "$script:Root\mods\LeviHeadlessInput\HeadlessInput.psm1" -Force
if (-not $HeadlessPipe) { $HeadlessPipe = 'LeviHeadlessInput-' + ($Instance -replace '^.*-(hi\d+)$', '$1') }
Set-HeadlessPipe $HeadlessPipe
$script:Log = Join-Path $script:Root 'logs\latest.log'
$script:Shots = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ShotsDir)
$script:ConsoleReader = Join-Path $PSScriptRoot 'read-console.ps1'
New-Item -ItemType Directory -Force $script:Shots | Out-Null

function Mark { (Get-Content $script:Log).Count }

# TACZ gameplay lines logged after $mark, without per-frame/diagnostic noise.
function Since([int]$mark, [string]$pattern = '\[(bolt|reload|shoot|hand|anim)\]|fired at|refused|recoil-shot|Script |failed') {
    Get-Content $script:Log | Select-Object -Skip $mark | Select-String -Pattern $pattern |
        Where-Object { $_.Line -notmatch 'frame hook rate|hit block|expired after' } |
        ForEach-Object { $l = $_.Line; $l.Substring(12, [Math]::Min(150, $l.Length - 12)) }
}

function Chat([string]$text) {
    Send-HeadlessKey T; Start-Sleep -Milliseconds 700
    Send-HeadlessText $text; Start-Sleep -Milliseconds 300
    Send-HeadlessKey ENTER; Start-Sleep -Milliseconds 900
}

function Shot([string]$name) { Save-HeadlessScreenshot (Join-Path $script:Shots "$name.png") | Out-Null; Join-Path $script:Shots "$name.png" }

function Fire([int]$holdMs = 80) { Send-HeadlessRaw down -Button left; Start-Sleep -Milliseconds $holdMs; Send-HeadlessRaw up -Button left }

# Dedicated-server log (shared-read; the server keeps it open).
$script:SrvLog = Join-Path $script:BdsRoot 'logs\latest.log'
function SrvLines {
    $fs = [IO.File]::Open($script:SrvLog, 'Open', 'Read', 'ReadWrite')
    try { $sr = [IO.StreamReader]::new($fs); ($sr.ReadToEnd() -split "`n") } finally { $fs.Dispose() }
}
function SrvMark { (SrvLines).Count }
function SrvSince([int]$mark, [string]$pattern = '\[(bolt|reload|shoot)\]|refused|Script |failed') {
    SrvLines | Select-Object -Skip $mark | Select-String -Pattern $pattern |
        ForEach-Object { $l = $_.Line.TrimEnd(); $l.Substring(12, [Math]::Min(150, $l.Length - 12)) }
}
function Resume { Send-HeadlessMouse click -X 377 -Y 345; Start-Sleep -Seconds 1 }

# Live server console text (the log file is buffered). Lines after the given HH:mm:ss.fff.
$script:SrvPid = if ($ServerProcessId) { $ServerProcessId } else {
    (Get-Process -Name bedrock_server_mod -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($script:BdsRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) } |
        Select-Object -First 1).Id
}
function SrvConsole([string]$after = '00:00:00.000', [string]$pattern = '\[(bolt|reload)\]|refused|Script |failed', [int]$lines = 300) {
    pwsh -NoProfile -File $script:ConsoleReader -ProcessId $script:SrvPid -Lines $lines |
        ForEach-Object { $_ -replace '\s+\S?$', '' } |
        Where-Object { $_ -match '^\d\d:\d\d:\d\d\.\d{3}' -and $_.Substring(0, 12) -gt $after -and $_ -match $pattern } |
        ForEach-Object { $_.Substring(0, [Math]::Min(140, $_.Length)) }
}
function Now { (Get-Date).ToString('HH:mm:ss.fff') }
function Give([string]$gun) {
    Chat '/clear @s'; Chat "/tacz give gun $gun"; Start-Sleep -Seconds 2
    Send-HeadlessKey 2; Start-Sleep -Milliseconds 400; Send-HeadlessKey 1; Start-Sleep -Seconds 2
}
