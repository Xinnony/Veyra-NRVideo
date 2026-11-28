$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$exe=Join-Path $root 'out/build/x64-release/veyra_qml_ui.exe'
if(-not (Test-Path -LiteralPath $exe)){throw 'Build Veyra first using scripts/build.ps1 -Preset x64-release'}
# The build directory carries no Qt DLLs; borrow the Qt this build was configured with.
$cache=Get-Content -LiteralPath (Join-Path (Split-Path $exe) 'CMakeCache.txt') -Raw
$qtDir=[regex]::Match($cache,'(?m)^Qt6_DIR:[^=]*=(.+)$').Groups[1].Value.Trim()
if(-not $qtDir){throw 'Qt6_DIR missing from CMakeCache.txt; configure the QML build first'}
$env:PATH=[IO.Path]::GetFullPath((Join-Path $qtDir '../../../bin'))+';'+$env:PATH
# Same for FFmpeg (avcodec/avformat/...): its DLLs live in the bin folder next to the configured lib folder.
$ffLibDir=[regex]::Match($cache,'(?m)^FFMPEG_LIBRARY_DIRS:[^=]*=([^;\r\n]+)').Groups[1].Value.Trim()
if($ffLibDir){
  $ffBin=[IO.Path]::GetFullPath((Join-Path $ffLibDir '../bin'))
  if(Test-Path -LiteralPath $ffBin){$env:PATH=$ffBin+';'+$env:PATH}
}
Start-Process -FilePath $exe -WorkingDirectory $root -WindowStyle Normal
