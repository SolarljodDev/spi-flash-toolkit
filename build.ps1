param([switch]$Clean)
$ErrorActionPreference = 'Continue'
$root     = $PSScriptRoot
$buildDir = "$root\build\cmake"

# Read cmake/ninja paths from cmake/tool_paths.cmake
$cmakeExe = $null; $ninjaExe = $null
foreach ($ln in Get-Content "$root\cmake\tool_paths.cmake" -EA SilentlyContinue) {
    if ($ln -match 'CMAKE_EXE\s+"([^"]+)"')         { $cmakeExe = $Matches[1] }
    if ($ln -match 'CMAKE_MAKE_PROGRAM\s+"([^"]+)"') { $ninjaExe = $Matches[1] }
}
if (-not $cmakeExe) { $c = Get-Command cmake -EA SilentlyContinue; if ($c) { $cmakeExe = $c.Source } }
if (-not $cmakeExe) { Write-Host 'cmake not found - run setup.ps1' -ForegroundColor Red; exit 1 }

# Configure (silent; only warnings/errors printed)
$confArgs = @('-S',$root, '-B',$buildDir, '-G','Ninja',
              "-DCMAKE_TOOLCHAIN_FILE=$root\cmake\toolchain.cmake",
              '-DCMAKE_BUILD_TYPE=Debug', '--log-level=WARNING')
if ($ninjaExe) { $confArgs += "-DCMAKE_MAKE_PROGRAM=$ninjaExe" }
& $cmakeExe @confArgs 2>&1 | Where-Object { "$_" -match 'error|warning|WARN|ERR' } |
    ForEach-Object { Write-Host "$_" -ForegroundColor Yellow }

# Build
$buildArgs = @('--build', $buildDir)
if ($Clean) { $buildArgs += '--clean-first' }
& $cmakeExe @buildArgs
$exitCode = $LASTEXITCODE

# Size table (optional - runs only if scripts/show-size.ps1 exists)
if ($exitCode -eq 0) {
    $helper = Join-Path $root 'scripts\show-size.ps1'
    if (Test-Path $helper) { & $helper -Root $root -BuildDir $buildDir }
}
exit $exitCode