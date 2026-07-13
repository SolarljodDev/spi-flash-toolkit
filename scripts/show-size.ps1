param(
    [string]$Root     = (Split-Path $PSScriptRoot -Parent),
    [string]$BuildDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\cmake')
)

$toolPaths = Join-Path $Root 'cmake\tool_paths.cmake'
$gcBin = $null; $gcPfx = $null
foreach ($ln in Get-Content $toolPaths -EA SilentlyContinue) {
    if ($ln -match 'TOOLCHAIN_GCC_BIN\s+"([^"]+)"')    { $gcBin = $Matches[1] }
    if ($ln -match 'TOOLCHAIN_GCC_PREFIX\s+"([^"]+)"') { $gcPfx = $Matches[1] }
}
$sizeExe = if ($gcBin -and $gcPfx) { "$gcBin\${gcPfx}size.exe" } else { $null }
if (-not $sizeExe -or -not (Test-Path $sizeExe)) { return }

$elf = Get-ChildItem "$BuildDir\*.elf" -EA SilentlyContinue | Select-Object -First 1
if (-not $elf) { return }

$sizeOut = & $sizeExe $elf.FullName 2>$null | Select-Object -Skip 1 -First 1
if ($sizeOut -notmatch '^\s*(\d+)\s+(\d+)\s+(\d+)') { return }
$flashUsed = [int]$Matches[1] + [int]$Matches[2]
$ramUsed   = [int]$Matches[2] + [int]$Matches[3]

$ldScript = "mcu.ld"  # fallback for old projects
foreach ($ln in Get-Content "$Root\cmake\mcu_config.cmake" -EA SilentlyContinue) {
    if ($ln -match 'MCU_LD_SCRIPT\s+"([^"]+)"') { $ldScript = $Matches[1]; break }
}
$flashTotal = 0; $ramTotal = 0
foreach ($ln in Get-Content (Join-Path $Root $ldScript) -EA SilentlyContinue) {
    if ($ln -match '^\s+FLASH\s.*LENGTH\s*=\s*(\d+)K')  { $flashTotal = [int]$Matches[1] * 1024 }
    if ($ln -match '\bRAM\b.*LENGTH\s*=\s*(\d+)K')     { $ramTotal   = [int]$Matches[1] * 1024 }
}

Write-Host ''
if ($flashTotal -gt 0) {
    $hr = '  ' + [string]::new([char]0x2500, 59)
    $fp = [int]($flashUsed * 100 / $flashTotal)
    $rp = [int]($ramUsed   * 100 / $ramTotal)
    $fc = if ($fp -lt 70) { 'Green' } elseif ($fp -lt 90) { 'Yellow' } else { 'Red' }
    $rc = if ($rp -lt 70) { 'Green' } elseif ($rp -lt 90) { 'Yellow' } else { 'Red' }
    $fb = [string]::new([char]0x2588, [int]($fp/5))
    $fe = [string]::new([char]0x2591, 20 - [int]($fp/5))
    $rb = [string]::new([char]0x2588, [int]($rp/5))
    $re = [string]::new([char]0x2591, 20 - [int]($rp/5))
    Write-Host $hr -ForegroundColor DarkGray
    Write-Host '   Flash  [' -NoNewline -ForegroundColor White
    if ($fb) { Write-Host $fb -NoNewline -ForegroundColor $fc }
    Write-Host $fe -NoNewline -ForegroundColor DarkGray
    Write-Host (']  {0,7:N0} / {1,7:N0} B  ({2,3}%)' -f $flashUsed,$flashTotal,$fp) -ForegroundColor White
    Write-Host '   RAM    [' -NoNewline -ForegroundColor White
    if ($rb) { Write-Host $rb -NoNewline -ForegroundColor $rc }
    Write-Host $re -NoNewline -ForegroundColor DarkGray
    Write-Host (']  {0,7:N0} / {1,7:N0} B  ({2,3}%)' -f $ramUsed,$ramTotal,$rp) -ForegroundColor White
    Write-Host $hr -ForegroundColor DarkGray
} else {
    $hr = '  ' + [string]::new([char]0x2500, 19)
    Write-Host $hr -ForegroundColor DarkGray
    Write-Host ('   Flash  {0,9:N0} B' -f $flashUsed) -ForegroundColor White
    Write-Host ('   RAM    {0,9:N0} B' -f $ramUsed)   -ForegroundColor White
    Write-Host $hr -ForegroundColor DarkGray
}
Write-Host ''