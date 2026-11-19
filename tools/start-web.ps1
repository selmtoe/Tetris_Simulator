param([int]$Port = 8765, [switch]$NoBrowser)
$ErrorActionPreference = 'Stop'
$webRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$webPython = Get-Command python, python3, py -ErrorAction SilentlyContinue | Select-Object -First 1
$webPythonPath = if ($webPython) { $webPython.Source } else { $null }
if (-not $webPythonPath) {
    $webPythonPath = Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'Programs\Python') -Filter python.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $webPythonPath) { throw 'Python 3 is required to build the web tools. The published GitHub Pages version works without Python.' }
& $webPythonPath (Join-Path $PSScriptRoot 'build-web.py')
if ($LASTEXITCODE -ne 0) { throw 'The public web build failed.' }
& (Join-Path $PSScriptRoot 'serve.ps1') -Port $Port -OpenPath 'dist/pages/hub/' -NoBrowser:$NoBrowser
