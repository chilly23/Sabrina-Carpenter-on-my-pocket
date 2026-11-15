$ErrorActionPreference = 'Stop'
$taskPython = Get-ChildItem -Path "$env:USERPROFILE\.espressif\python_env\idf*_py*_env\Scripts\python.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (!$taskPython) { $taskPython = (Get-Command python -ErrorAction Stop).Source }
& $taskPython (Join-Path $PSScriptRoot 'flash.py') COM10
exit $LASTEXITCODE
