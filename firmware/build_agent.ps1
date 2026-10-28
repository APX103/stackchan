# StackChan firmware build script (agent/CI helper)
# Usage: powershell -ExecutionPolicy Bypass -File build_agent.ps1 [-Target build|flash|monitor|fullclean] [-Port COM26]
param(
    [string]$Target = "build",
    [string]$Port = ""
)
$ErrorActionPreference = "Continue"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$env:Path = 'C:\Python311;C:\Python311\Scripts;C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Windows\System32\WindowsPowerShell\v1.0'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
& 'D:\Espressif\frameworks\esp-idf-v5.5.5\export.ps1'
if ($?) {
    Set-Location $PSScriptRoot
    $extra = @()
    if ($Port) { $extra = @("-p", $Port) }
    python 'D:\Espressif\frameworks\esp-idf-v5.5.5\tools\idf.py' @extra $Target
    exit $LASTEXITCODE
}
exit 1
