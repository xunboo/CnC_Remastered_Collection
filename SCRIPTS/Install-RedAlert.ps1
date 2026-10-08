[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$ModDirectory = 'C:\Program Files (x86)\Steam\steamapps\workshop\content\1213210\2221741447\AIBoost',
    [string]$SourceDirectory = '',
    [string]$MetadataFile = '',
    [string]$LLMConfig = '',
    [switch]$ReplaceLLMConfig
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (!$SourceDirectory) {
    $SourceDirectory = Join-Path $repositoryRoot 'build\redalert'
    if (!(Test-Path -LiteralPath (Join-Path $SourceDirectory 'RedAlert.dll') -PathType Leaf)) {
        $SourceDirectory = Join-Path $repositoryRoot 'AIBoost\Data'
    }
}
$SourceDirectory = [System.IO.Path]::GetFullPath($SourceDirectory)
$ModDirectory = [System.IO.Path]::GetFullPath($ModDirectory)
$targetData = Join-Path $ModDirectory 'Data'
$targetMetadata = Join-Path $ModDirectory 'ccmod.json'
if (!(Test-Path -LiteralPath $targetMetadata -PathType Leaf) -or
    !(Test-Path -LiteralPath (Join-Path $targetData 'RedAlert.dll') -PathType Leaf)) {
    throw 'ModDirectory must name an existing AIBoost mod with ccmod.json and Data\RedAlert.dll.'
}
if ((Get-Content -LiteralPath $targetMetadata -Raw | ConvertFrom-Json).game_type -ne 'RA') {
    throw 'The destination is not a Red Alert mod.'
}

$copies = @()
foreach ($fileName in @('RedAlert.dll', 'RedAlert.pdb')) {
    $sourceFile = Join-Path $SourceDirectory $fileName
    if (!(Test-Path -LiteralPath $sourceFile -PathType Leaf)) { throw ('Missing build file: ' + $sourceFile) }
    $copies += [pscustomobject]@{
        Name = $fileName
        Source = $sourceFile
        Target = Join-Path $targetData $fileName
        SHA256 = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash
    }
}
$bridgeExecutable = Join-Path $SourceDirectory 'LLMBridge.exe'
if (Test-Path -LiteralPath $bridgeExecutable -PathType Leaf) {
    $copies += [pscustomobject]@{
        Name = 'LLMBridge.exe'
        Source = $bridgeExecutable
        Target = Join-Path $targetData 'LLMBridge.exe'
        SHA256 = (Get-FileHash -LiteralPath $bridgeExecutable -Algorithm SHA256).Hash
    }
    if (!$LLMConfig) {
        $LLMConfig = Join-Path $repositoryRoot 'llm.ini'
        if (!(Test-Path -LiteralPath $LLMConfig -PathType Leaf)) {
            $LLMConfig = Join-Path $repositoryRoot 'llm.example.ini'
        }
    }
    $LLMConfig = [System.IO.Path]::GetFullPath($LLMConfig)
    $targetConfig = Join-Path $targetData 'llm.ini'
    if ($ReplaceLLMConfig -or !(Test-Path -LiteralPath $targetConfig -PathType Leaf)) {
        if (!(Test-Path -LiteralPath $LLMConfig -PathType Leaf)) { throw 'The source LLM configuration does not exist.' }
        $copies += [pscustomobject]@{
            Name = 'llm.ini'
            Source = $LLMConfig
            Target = $targetConfig
            SHA256 = (Get-FileHash -LiteralPath $LLMConfig -Algorithm SHA256).Hash
        }
    }
} else {
    Write-Warning 'LLMBridge.exe was not built; automatic startup requires the packaged EXE beside RedAlert.dll.'
}
if (!$MetadataFile) {
    $MetadataFile = Join-Path (Split-Path -Parent $SourceDirectory) 'ccmod.json'
    if (!(Test-Path -LiteralPath $MetadataFile -PathType Leaf)) {
        $MetadataFile = Join-Path $repositoryRoot 'WorkshopContent\AIBoost\ccmod.json'
    }
}
$MetadataFile = [System.IO.Path]::GetFullPath($MetadataFile)
if (!(Test-Path -LiteralPath $MetadataFile -PathType Leaf)) { throw ('Missing mod metadata: ' + $MetadataFile) }
if ((Get-Content -LiteralPath $MetadataFile -Raw | ConvertFrom-Json).game_type -ne 'RA') {
    throw 'The source metadata is not for Red Alert.'
}
$copies += [pscustomobject]@{
    Name = 'ccmod.json'
    Source = $MetadataFile
    Target = $targetMetadata
    SHA256 = (Get-FileHash -LiteralPath $MetadataFile -Algorithm SHA256).Hash
}
foreach ($copy in $copies) {
    if ($copy.Source -ieq $copy.Target) { throw ('Source and destination must differ: ' + $copy.Target) }
}

# Never interrupt a match or replace a DLL while the game is using it.
$runningGame = @(Get-Process -Name ClientG,InstanceServerG -ErrorAction SilentlyContinue)
if ($runningGame.Count -gt 0) {
    throw 'Close Red Alert (ClientG and InstanceServerG) before installing. No mod files were changed.'
}
if (!$PSCmdlet.ShouldProcess($ModDirectory, 'Back up and install RedAlert.dll, symbols, mod metadata and available portable LLM files')) { return }

$backupName = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$backupDirectory = Join-Path $repositoryRoot ('review\installed-backups\' + $backupName)
New-Item -ItemType Directory -Path $backupDirectory | Out-Null
$originalFiles = @{}
foreach ($copy in $copies) {
    $backupFile = Join-Path $backupDirectory $copy.Name
    if (Test-Path -LiteralPath $copy.Target -PathType Leaf) {
        Copy-Item -LiteralPath $copy.Target -Destination $backupFile
        $previousHash = (Get-FileHash -LiteralPath $copy.Target -Algorithm SHA256).Hash
        if ((Get-FileHash -LiteralPath $backupFile -Algorithm SHA256).Hash -ne $previousHash) {
            throw ('Backup verification failed: ' + $backupFile)
        }
        $originalFiles[$copy.Name] = $backupFile
    } else {
        $originalFiles[$copy.Name] = $null
    }
}
$record = [ordered]@{
    installed = $false
    utc = [DateTime]::UtcNow.ToString('o')
    mod_directory = $ModDirectory
    backup_directory = $backupDirectory
    files = @($copies | ForEach-Object {
        if ($_.Name -eq 'llm.ini') { $_ | Select-Object Name,Target }
        else { $_ | Select-Object Name,Target,SHA256 }
    })
}
$recordPath = Join-Path $backupDirectory 'installation.json'
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($recordPath, ($record | ConvertTo-Json -Depth 4), $utf8)
$changedFiles = @()
try {
    # Recheck after making the backups, in case the game has just started.
    if (@(Get-Process -Name ClientG,InstanceServerG -ErrorAction SilentlyContinue).Count -gt 0) {
        throw 'Red Alert started during backup. No mod files were changed.'
    }
    foreach ($copy in $copies) {
        $changedFiles += $copy
        Copy-Item -LiteralPath $copy.Source -Destination $copy.Target -Force
        if ((Get-FileHash -LiteralPath $copy.Target -Algorithm SHA256).Hash -ne $copy.SHA256) {
            throw ('Installed file verification failed: ' + $copy.Target)
        }
    }
} catch {
    $installError = $_
    foreach ($copy in $changedFiles) {
        try {
            if ($originalFiles[$copy.Name]) {
                Copy-Item -LiteralPath $originalFiles[$copy.Name] -Destination $copy.Target -Force
            } elseif (Test-Path -LiteralPath $copy.Target -PathType Leaf) {
                Remove-Item -LiteralPath $copy.Target
            }
        } catch { Write-Warning ('Could not restore ' + $copy.Target + '; backup: ' + $backupDirectory) }
    }
    throw $installError
}
$record['installed'] = $true
[System.IO.File]::WriteAllText($recordPath, ($record | ConvertTo-Json -Depth 4), $utf8)
$record | ConvertTo-Json -Depth 4
