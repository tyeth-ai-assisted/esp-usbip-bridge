param([string]$Action = "build", [string]$Port = "")
$ErrorActionPreference = "Stop"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$env:IDF_PATH = "C:\esp\idf-master"
. "C:\esp\idf-master\export.ps1" | Out-Null
# The checkout this script lives in (tools/bench/../..)
Set-Location (Resolve-Path (Join-Path $PSScriptRoot "..\.."))
$argsList = @("--preview", "-B", "build-s31-function-coreboard-1")
if ($Port) { $argsList += @("-p", $Port) }
$argsList += @("-DIDF_TARGET=esp32s31", "-DSDKCONFIG=sdkconfig.s31-function-coreboard-1",
               "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.s31-function-coreboard-1")
$argsList += $Action.Split(" ")
idf.py @argsList
$rc = $LASTEXITCODE
# Keep the committed full config in step (see scripts/build-board.sh)
if ($rc -eq 0 -and (Test-Path "sdkconfig.s31-function-coreboard-1")) {
    Copy-Item "sdkconfig.s31-function-coreboard-1" "sdkconfig.s31-function-coreboard-1.example" -Force
}
exit $rc
