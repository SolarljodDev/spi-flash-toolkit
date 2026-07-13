<#
.SYNOPSIS
  Writes a raw EEPROM dump file to the chip over the board's serial protocol
  (see the 'W'/'S' commands documented at the top of user/src/main.c), then
  reads the chip back and verifies every byte matches.

.PARAMETER Port
  COM port to use (e.g. COM5). If omitted, lists available ports and prompts.

.PARAMETER DumpPath
  Path to the dump file to write. Defaults to Downloads\eeprom_dump.bin.

.PARAMETER Baud
  Must match UART_BAUD in main.c (2000000).

.PARAMETER AddrBytes / PageSize / BatchSize
  Defaults match the M95128 profile (CHIP_PROFILES.m95128 in web/index.html):
  2 address bytes, 64-byte pages, 64-byte batches. Dump size must be an exact
  multiple of BatchSize -- the firmware's eeprom_program() does integer
  division on this and silently drops any remainder.

.EXAMPLE
  ./scripts/flash-dump.ps1
  ./scripts/flash-dump.ps1 -Port COM5 -DumpPath C:\Users\me\Downloads\eeprom_dump.bin
#>
param(
    [string]$Port,
    [string]$DumpPath,
    [int]$Baud = 2000000,
    [int]$AddrBytes = 2,
    [int]$PageSize = 64,
    [int]$BatchSize = 64
)

$ErrorActionPreference = 'Stop'

# Double-clicking (or "Run with PowerShell") closes the window the instant the
# script exits, so any error would flash by unseen -- every exit path in this
# script goes through here so there's always a chance to read what happened.
function Fail([string]$msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    Write-Host ""
    Read-Host "Press Enter to close"
    exit 1
}

if (-not $DumpPath) {
    $DumpPath = Join-Path $env:USERPROFILE 'Downloads\eeprom_dump.bin'
}
if (-not (Test-Path $DumpPath)) {
    Fail "Dump file not found: $DumpPath"
}

$dumpBytes = [System.IO.File]::ReadAllBytes($DumpPath)
$size = $dumpBytes.Length
Write-Host "Loaded dump: $DumpPath ($size bytes)"

if ($size % $BatchSize -ne 0) {
    Fail "Dump size ($size) is not an exact multiple of BatchSize ($BatchSize) -- the firmware would silently drop the remainder. Aborting."
}

# --- Port selection ---------------------------------------------------------
if (-not $Port) {
    $available = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique
    if ($available.Count -eq 0) {
        Fail "No COM ports found. Is the board plugged in?"
    }
    $pnp = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '\(COM\d+\)' }
    Write-Host "Available COM ports:"
    for ($i = 0; $i -lt $available.Count; $i++) {
        $portName = $available[$i]
        $match = $pnp | Where-Object { $_.Name -match [regex]::Escape("($portName)") } | Select-Object -First 1
        if ($match) {
            Write-Host "  [$i] $portName - $($match.Name)"
        } else {
            Write-Host "  [$i] $portName"
        }
    }
    $choice = Read-Host "Select port number"
    if ($choice -notmatch '^\d+$' -or [int]$choice -ge $available.Count) {
        Fail "Invalid selection."
    }
    $Port = $available[[int]$choice]
}

function Read-ExactBytes {
    param([System.IO.Ports.SerialPort]$SerialPort, [int]$Count, [int]$TimeoutMs = 5000)
    $buf = New-Object byte[] $Count
    $offset = 0
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ($offset -lt $Count) {
        if ((Get-Date) -gt $deadline) {
            throw "Timeout waiting for $Count byte(s) from board (got $offset)."
        }
        try {
            $n = $SerialPort.Read($buf, $offset, $Count - $offset)
            if ($n -gt 0) { $offset += $n }
        } catch [System.TimeoutException] {
            # keep looping until the outer deadline trips
        }
    }
    return $buf
}

function ToLE16([int]$v) {
    return [byte[]]@( ($v -band 0xFF), (($v -shr 8) -band 0xFF) )
}
function ToLE32([int]$v) {
    return [byte[]]@( ($v -band 0xFF), (($v -shr 8) -band 0xFF), (($v -shr 16) -band 0xFF), (($v -shr 24) -band 0xFF) )
}

$sp = $null
$exitCode = 0
try {
    Write-Host "Opening $Port at $Baud baud..."
    $sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.ReadTimeout = 500   # short internal timeout; Read-ExactBytes below layers its own deadline on top
    $sp.WriteTimeout = 5000
    $sp.Open()
    Start-Sleep -Milliseconds 500
    $sp.DiscardInBuffer()
    $sp.DiscardOutBuffer()

    # --- Confirm the EEPROM is actually connected before touching anything ---
    # PB14 (MISO) has an internal pull-up in the firmware, so a genuinely
    # disconnected chip reads back as all-1s -- RDSR would come back 0xFF,
    # which isn't a plausible real status (WIP stuck busy forever etc).
    Write-Host "Checking EEPROM status register..."
    $sp.Write([byte[]]@(0x53), 0, 1)  # 'S'
    $ack = Read-ExactBytes -SerialPort $sp -Count 1 -TimeoutMs 5000
    if ($ack[0] -ne 0x41) {
        throw "No link ACK from the board (got 0x$($ack[0].ToString('X2'))). Check the board is powered and the COM port is correct."
    }
    $status = Read-ExactBytes -SerialPort $sp -Count 1 -TimeoutMs 5000
    Write-Host ("Status register: 0x{0:X2}" -f $status[0])
    if ($status[0] -eq 0xFF) {
        throw "Status register reads 0xFF -- the EEPROM does not appear to be connected. Check soldering/clip contact and try again."
    }

    # --- 'W' command ---------------------------------------------------------
    Write-Host "Writing $size bytes (addrBytes=$AddrBytes pageSize=$PageSize batchSize=$BatchSize)..."
    $sp.Write([byte[]]@(0x57), 0, 1)  # 'W'
    $ack2 = Read-ExactBytes -SerialPort $sp -Count 1 -TimeoutMs 5000
    if ($ack2[0] -ne 0x41) {
        throw "No link ACK for 'W' (got 0x$($ack2[0].ToString('X2')))."
    }
    $sp.Write([byte[]]@($AddrBytes), 0, 1)
    $sp.Write([byte[]]@($PageSize), 0, 1)
    $sp.Write((ToLE16 $BatchSize), 0, 2)
    $sp.Write((ToLE32 $size), 0, 4)

    $numBatches = $size / $BatchSize
    for ($b = 0; $b -lt $numBatches; $b++) {
        $offset = $b * $BatchSize
        $sp.Write($dumpBytes, $offset, $BatchSize)
        $batchAck = Read-ExactBytes -SerialPort $sp -Count 1 -TimeoutMs 25000
        if ($batchAck[0] -eq 0x15) {
            throw "NAK on batch $b (offset 0x$($offset.ToString('X'))) -- the chip stopped responding mid-write."
        } elseif ($batchAck[0] -ne 0x06) {
            throw "Unexpected response 0x$($batchAck[0].ToString('X2')) on batch $b."
        }
        Write-Progress -Activity "Writing" -Status "$($offset + $BatchSize) / $size bytes" -PercentComplete ((($offset + $BatchSize) / $size) * 100)
    }
    Write-Progress -Activity "Writing" -Completed

    # --- Firmware streams the whole chip back automatically after the write --
    Write-Host "Reading back for verification..."
    $readback = Read-ExactBytes -SerialPort $sp -Count $size -TimeoutMs 30000

    $mismatches = 0
    for ($i = 0; $i -lt $size; $i++) {
        if ($readback[$i] -ne $dumpBytes[$i]) { $mismatches++ }
    }

    if ($mismatches -eq 0) {
        Write-Host "Verified OK: all $size bytes match." -ForegroundColor Green
    } else {
        Write-Host "MISMATCH: $mismatches byte(s) differ between what was written and what was read back!" -ForegroundColor Red
        $exitCode = 1
    }
}
catch {
    Write-Host ""
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
finally {
    if ($sp -and $sp.IsOpen) { $sp.Close() }
}

Write-Host ""
Read-Host "Press Enter to close"
exit $exitCode
