$logDir = Join-Path $env:LOCALAPPDATA 'JC3VR'
Get-ChildItem -LiteralPath $logDir -Filter 'jc3vr*.log' -ErrorAction SilentlyContinue | Select-Object FullName,Length,LastWriteTime
