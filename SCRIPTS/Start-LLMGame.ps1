[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameExecutable,
    [string]$GameArguments = '',
    [ValidatePattern('^[A-Za-z0-9_-]{1,63}$')]
    [string]$Channel = 'v1'
)

$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $GameExecutable).Path
if (!(Test-Path -LiteralPath $executablePath -PathType Leaf) -or
    [System.IO.Path]::GetExtension($executablePath) -ine '.exe') {
    throw 'GameExecutable must name the actual installed game .exe.'
}

# The official launcher creates both the front-end ClientG and its local
# InstanceServerG simulation process. Starting ClientG alone can reach the
# menus but leave skirmish loading waiting for a server that was never started.
if ([System.IO.Path]::GetFileName($executablePath) -ieq 'ClientG.exe') {
    $executablePath = Join-Path (Split-Path -Parent $executablePath) 'ClientLauncherG.exe'
    if (!(Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw 'ClientLauncherG.exe is required beside ClientG.exe to start the local simulation server.'
    }
}
if ([System.IO.Path]::GetFileName($executablePath) -ine 'ClientLauncherG.exe') {
    throw 'GameExecutable must name ClientLauncherG.exe (or ClientG.exe beside that launcher).'
}
if ($GameArguments -match '(?i)(?:^|\s)TIBERIANDAWN(?:\s|$)') {
    throw 'This launcher starts Red Alert; remove TIBERIANDAWN from GameArguments.'
}

# ClientG is shared by both games. The launcher passes this standalone token
# to select Red Alert; an empty command line starts Tiberian Dawn instead.
$launchArguments = $GameArguments.Trim()
if ($launchArguments -notmatch '(?i)(?:^|\s)REDALERT(?:\s|$)') {
    $launchArguments = ('REDALERT ' + $launchArguments).Trim()
}

# Set the opt-in on the game child only. An already-running Steam process does
# not inherit environment changes made in this PowerShell session.
$gameStart = New-Object System.Diagnostics.ProcessStartInfo
$gameStart.FileName = $executablePath
$gameStart.Arguments = $launchArguments
$gameStart.UseShellExecute = $false
$gameStart.WorkingDirectory = Split-Path -Parent $executablePath
$gameEnvironment = @{}
foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    $gameEnvironment[$entry.Key.ToUpperInvariant()] = $entry.Value
}
$gameStart.Environment.Clear()
foreach ($entry in $gameEnvironment.GetEnumerator()) {
    if ($entry.Key -ne 'OPENAI_API_KEY') { $gameStart.Environment[$entry.Key] = $entry.Value }
}
$gameStart.Environment['AIBOOST_LLM'] = '1'
$gameStart.Environment['AIBOOST_LLM_CHANNEL'] = $Channel
if (!$PSCmdlet.ShouldProcess($executablePath, ('Start Red Alert launcher and simulation server with arguments "' + $launchArguments + '" and LLM channel ' + $Channel))) {
    return
}

# An existing launcher/server may retain an environment without AIBOOST_LLM.
# Require a fresh launch instead of handing this request to an older instance.
$existingGames = @(Get-Process -Name ClientG,ClientLauncherG,InstanceServerG -ErrorAction SilentlyContinue)
if ($existingGames.Count -gt 0) {
    $existingNames = ($existingGames | ForEach-Object { $_.ProcessName } | Sort-Object -Unique) -join ', '
    throw ('Close the game and its launcher before starting LLM mode. Still running: ' + $existingNames)
}
$gameProcess = [System.Diagnostics.Process]::Start($gameStart)
Write-Output ('Started Red Alert launcher PID ' + $gameProcess.Id + ' with LLM channel ' + $Channel)
