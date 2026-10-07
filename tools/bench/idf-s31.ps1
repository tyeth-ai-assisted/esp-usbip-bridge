param([string]$Action = "build", [string]$Port = "")
$ErrorActionPreference = "Stop"
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$env:IDF_PATH = "C:\esp\idf-master"
. "C:\esp\idf-master\export.ps1" | Out-Null
Set-Location "C:\dev\arduino\esp-usbip-bridge\.claude\worktrees\esp32-s31-ethernet-support-bb9b94"
$argsList = @("--preview", "-B", "build-s31-function-coreboard-1")
if ($Port) { $argsList += @("-p", $Port) }
$argsList += @("-DIDF_TARGET=esp32s31", "-DSDKCONFIG=sdkconfig.s31-function-coreboard-1",
               "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.s31-function-coreboard-1")
$argsList += $Action.Split(" ")
idf.py @argsList
exit $LASTEXITCODE
