$ErrorActionPreference = 'Stop'
# 1) ESP-IDF python env if present, 2) otherwise any system Python that has (or can install) esptool + pyserial
$python = Get-ChildItem -Path "$env:USERPROFILE\.espressif\python_env\idf*_py*_env\Scripts\python.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (!$python) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (!$cmd) { $cmd = Get-Command py -ErrorAction SilentlyContinue }
    if (!$cmd) { throw 'Python not found. Install Python 3 from python.org (tick "Add to PATH") and run again.' }
    $python = $cmd.Source
    & $python -c "import esptool, serial" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'Installing esptool and pyserial (one time)...'
        & $python -m pip install --quiet esptool pyserial
        if ($LASTEXITCODE -ne 0) { throw 'pip install esptool pyserial failed.' }
    }
}
& $python (Join-Path $PSScriptRoot 'flash.py')
exit $LASTEXITCODE
