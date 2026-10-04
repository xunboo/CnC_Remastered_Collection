param(
    [string]$OutputDirectory = '',
    [string]$MSBuildPath = ''
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repositoryRoot 'build\redalert' }
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

if (!$MSBuildPath) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswherePath)) { throw 'Visual Studio with C++ build tools is required.' }
    $installationPath = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$installationPath) { throw 'No Visual Studio C++ installation was found.' }
    $MSBuildPath = Join-Path $installationPath 'MSBuild\Current\Bin\MSBuild.exe'
}

$buildStart = New-Object System.Diagnostics.ProcessStartInfo
$buildStart.FileName = $MSBuildPath
$buildStart.UseShellExecute = $false
$buildStart.WorkingDirectory = $repositoryRoot
$outputPath = $OutputDirectory.Replace('\', '/') + '/'
$arguments = @(
    'REDALERT/RedAlert.vcxproj', '/t:Build', '/p:Configuration=Release', '/p:Platform=Win32',
    ('/p:OutDir=' + $outputPath), ('/p:IntDir=' + $outputPath + 'obj/'),
    '/m:4', '/nologo', '/v:quiet', '/fl', ('/flp:logfile=' + $outputPath + 'build.log;verbosity=normal')
)
$buildStart.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '

# Windows environment keys are case-insensitive. Normalize their spelling to
# avoid MSBuild failing when its parent supplies both PATH and Path.
$environmentCopy = @{}
foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
    $environmentCopy[$entry.Key.ToUpperInvariant()] = $entry.Value
}
$buildStart.Environment.Clear()
foreach ($entry in $environmentCopy.GetEnumerator()) { $buildStart.Environment[$entry.Key] = $entry.Value }

Write-Output ('Building RedAlert.dll in ' + $OutputDirectory)
$buildProcess = [System.Diagnostics.Process]::Start($buildStart)
$buildProcess.WaitForExit()
if ($buildProcess.ExitCode -ne 0) { throw ('Build failed. See ' + (Join-Path $OutputDirectory 'build.log')) }
Write-Output ('Built ' + (Join-Path $OutputDirectory 'RedAlert.dll'))
