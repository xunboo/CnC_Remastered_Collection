param(
    [string]$LogPath = '',
    [string]$PythonExecutable = ''
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$dependencyDirectory = Join-Path $repositoryRoot 'build\log-viewer-deps'
$pythonPrefix = @()

if (!$PythonExecutable) {
    # Reuse the local Codex runtime when no standalone Python has been installed.
    $bundledPython = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
    if (Test-Path -LiteralPath $bundledPython) {
        $PythonExecutable = $bundledPython
    } elseif (Get-Command py -ErrorAction SilentlyContinue) {
        $PythonExecutable = (Get-Command py).Source
        $pythonPrefix = @('-3')
    } elseif (Get-Command python -ErrorAction SilentlyContinue) {
        $PythonExecutable = (Get-Command python).Source
    } else {
        throw 'Python 3.10+ with Tkinter is required. Supply -PythonExecutable with its path.'
    }
}

& $PythonExecutable @pythonPrefix -c 'import sys, tkinter; sys.exit(0 if sys.version_info >= (3, 10) else 1)'
if ($LASTEXITCODE -ne 0) { throw 'Python 3.10+ with Tkinter is required.' }

& $PythonExecutable @pythonPrefix -c 'import importlib.util, sys; sys.path.insert(0, sys.argv[1]); sys.exit(0 if importlib.util.find_spec(sys.argv[2]) else 1)' $dependencyDirectory 'matplotlib'
if ($LASTEXITCODE -ne 0) {
    Write-Output 'Preparing Matplotlib in build/log-viewer-deps (first run only)...'
    & $PythonExecutable @pythonPrefix -m pip install --disable-pip-version-check --target $dependencyDirectory -r (Join-Path $PSScriptRoot 'requirements-log-viewer.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Could not install Matplotlib. Check the network and Python pip installation.' }
}

$viewerArguments = @((Join-Path $PSScriptRoot 'plot_ai_log.py'))
if ($LogPath) { $viewerArguments += @('--input', $LogPath) }
& $PythonExecutable @pythonPrefix @viewerArguments
if ($LASTEXITCODE -ne 0) { throw 'The log viewer exited with an error.' }
