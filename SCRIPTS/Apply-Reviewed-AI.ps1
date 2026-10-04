param([Parameter(Mandatory=$true)][string]$TargetDirectory)

$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $workspaceRoot 'review\manifest.json') -Raw | ConvertFrom-Json
$targetRoot = (Resolve-Path -LiteralPath $TargetDirectory).Path
if (![string]::Equals($targetRoot, $manifest.source_root, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'This reviewed patch was prepared for a different project directory.'
}
$targetPrefix = $targetRoot.TrimEnd('\') + '\'
$backupRoot = Join-Path $targetRoot 'review\backup-2026-10-03'

# Validate every source and destination before changing any file.
foreach ($entry in $manifest.files) {
    $targetPath = [System.IO.Path]::GetFullPath((Join-Path $targetRoot $entry.path))
    if (!$targetPath.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Destination leaves the requested project.' }
    $workspacePath = Join-Path $workspaceRoot $entry.path
    if ((Get-FileHash -LiteralPath $workspacePath -Algorithm SHA256).Hash -ne $entry.updated_sha256) { throw ('Working file changed: ' + $entry.path) }
    if ($null -eq $entry.original_sha256) {
        if (Test-Path -LiteralPath $targetPath) { throw ('A new destination file already exists: ' + $entry.path) }
    } else {
        if (!(Test-Path -LiteralPath $targetPath)) { throw ('Original file is missing: ' + $entry.path) }
        if ((Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ne $entry.original_sha256) { throw ('Original file changed during review: ' + $entry.path) }
    }
}

foreach ($entry in $manifest.files) {
    if ($null -ne $entry.original_sha256) {
        $backupPath = Join-Path $backupRoot $entry.path
        if (Test-Path -LiteralPath $backupPath) { throw ('A backup already exists: ' + $backupPath) }
        New-Item -ItemType Directory -Path (Split-Path -Parent $backupPath) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $targetRoot $entry.path) -Destination $backupPath
    }
}
foreach ($entry in $manifest.files) {
    $targetPath = Join-Path $targetRoot $entry.path
    New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $workspaceRoot $entry.path) -Destination $targetPath
    if ((Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash -ne $entry.updated_sha256) { throw ('Copied file verification failed: ' + $entry.path) }
}
Write-Output ('Applied and hash-verified ' + $manifest.files.Count + ' files in ' + $targetRoot)
Write-Output ('Original source backups: ' + $backupRoot)
