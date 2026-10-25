# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
<#
.SYNOPSIS
    Build and flash RadioSonic firmware from a plain PowerShell terminal
    (no VS Code / ESP-IDF extension required).

.DESCRIPTION
    Finds and activates ESP-IDF v5.5.x, builds with idf.py, then flashes with
    esptool directly. Run it through flash.cmd from PowerShell or Command
    Prompt (ESP-IDF does NOT support MSys/Mingw bash), e.g.:

        .\flash.cmd
        .\flash.cmd -Monitor
        .\flash.cmd -Port COM7 -Full -Monitor

    flash.cmd bypasses PowerShell's script execution policy for that one run.
    Without it, Windows refuses .ps1 scripts by default and also refuses ones
    extracted from a downloaded zip. .\flash.ps1 works directly wherever the
    policy allows it.

    ESP-IDF does not need to be activated first: the script uses an already
    activated shell, $env:IDF_EXPORT_PS1, an EIM install (what the VS Code
    ESP-IDF extension installs), or the older ESP-IDF Tools Installer, and
    stops if the version is not v5.5.x.

    The script handles the board's three USB identities automatically:

      - TinyUSB CDC console (VID_303A PID_4001, the normal running firmware):
        esptool cannot auto-reset this port, so the script sends the
        'bootloader' console command first, waits for the board to
        re-enumerate in ROM download mode, and flashes that port. The ROM's
        native-USB download mode cannot reliably self-reset after flashing
        (a hard_reset attempt wedges its USB port), so the script flashes
        with --after no_reset, then tries a watchdog reset; if that fails,
        it asks you to tap RESET or power cycle.

      - ROM download mode (PID_0009, e.g. after holding BOOT): flashed as-is.

      - USB-Serial-JTAG (PID_1001, legacy pre-TinyUSB firmware or early
        boot): esptool auto-resets it before and after flashing; fully
        hands-free.

.PARAMETER Port
    Serial port of the board. If omitted, the script auto-detects the ESP32-S3
    by its Espressif USB vendor ID (VID_303A). Pass this to override.

.PARAMETER Monitor
    Open the serial monitor after flashing (quit with Ctrl+]). Only works on
    the USB-Serial-JTAG port: idf.py monitor cannot drive the TinyUSB CDC
    console (use PuTTY/RealTerm on that port instead).

.PARAMETER Full
    Flash everything (bootloader + partition table + app) instead of the
    faster app-only flash. Use after changing sdkconfig, partitions.csv, or
    the board revision.

.PARAMETER Clean
    Run 'idf.py fullclean' before building. Needed once if the build dir was
    last configured by VS Code (different Python env).

.PARAMETER ResetConfig
    Delete the generated sdkconfig before building so it is regenerated from
    sdkconfig.defaults + sdkconfig.defaults.esp32s3. Use after a git pull that
    changed the defaults (the config-drift guard fails the build otherwise).
    WARNING: this discards menuconfig-only choices not stored in the defaults
    files - most importantly the board revision. Re-select it in menuconfig
    afterward if you are not on the default.
#>
param(
    [string]$Port,
    [switch]$Monitor,
    [switch]$Full,
    [switch]$Clean,
    [switch]$ResetConfig
)

$ErrorActionPreference = "Stop"

# The ESP-IDF release this firmware builds against (any patch level).
$IdfRelease = [version]"5.5"

# --- Helpers ---------------------------------------------------------------

function Get-BoardDevices {
    # All present Espressif (VID_303A) serial devices, as Port + DevicePid.
    Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.PNPDeviceID -match "VID_303A" -and $_.Name -match "\(COM\d+\)" } |
        ForEach-Object {
            $comPort = $null
            $devicePid = $null
            if ($_.Name -match "\((COM\d+)\)") { $comPort = $Matches[1] }
            if ($_.PNPDeviceID -match "PID_([0-9A-Fa-f]{4})") { $devicePid = $Matches[1].ToUpper() }
            if ($comPort) {
                [pscustomobject]@{ Port = $comPort; DevicePid = $devicePid }
            }
        }
}

function Enter-DownloadMode {
    # Send the 'bootloader' console command to the TinyUSB CDC console.
    # The port dies as the board resets into ROM download mode; that is
    # expected and treated as success.
    param([string]$ConsolePort)
    Write-Host "Sending 'bootloader' command to console on $ConsolePort..." -ForegroundColor Cyan
    $sp = New-Object System.IO.Ports.SerialPort $ConsolePort, 115200
    $sp.DtrEnable = $true   # firmware answers the DTR rising edge with its banner
    try {
        try {
            $sp.Open()
        } catch {
            throw "Cannot open $ConsolePort - another program is using it. " +
                  "Disconnect the lab notebook / web lab page / PuTTY from the board and rerun."
        }
        Start-Sleep -Milliseconds 1200
        try { $sp.DiscardInBuffer() } catch {}
        $sp.Write("bootloader`r`n")
        Start-Sleep -Milliseconds 500
    } finally {
        try { $sp.Close() } catch {}
        try { $sp.Dispose() } catch {}
    }
}

function Wait-DownloadPort {
    # Wait for the board to re-enumerate in ROM download mode (PID_0009) or
    # as USB-Serial-JTAG (PID_1001) after the bootloader command.
    param([int]$TimeoutSec = 15)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $dev = Get-BoardDevices | Where-Object { $_.DevicePid -in @("0009", "1001") } | Select-Object -First 1
        if ($dev) { return $dev }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Get-IdfVersion {
    # Version of the ESP-IDF tree at $IdfPath from its version.cmake, or $null
    # if there is no ESP-IDF tree there.
    param([string]$IdfPath)
    if (-not $IdfPath) { return $null }
    $file = Join-Path $IdfPath "tools\cmake\version.cmake"
    if (-not (Test-Path $file)) { return $null }
    $text = Get-Content $file -Raw
    $parts = @()
    foreach ($name in "MAJOR", "MINOR", "PATCH") {
        if ($text -notmatch "IDF_VERSION_$name\s+(\d+)") { return $null }
        $parts += [int]$Matches[1]
    }
    return [version]::new($parts[0], $parts[1], $parts[2])
}

function Select-IdfInstall {
    # From an installer's list (objects with Id, Path, Version), pick the one
    # this firmware builds with: the installer's own selection if it is the
    # right release, else the newest install of that release. The selection
    # alone can't be trusted: EIM has been seen pointing at an id that is no
    # longer installed. Every version seen is kept for the not-found message.
    param([object[]]$Installs, [string]$SelectedId)
    $Installs = @($Installs | Where-Object { $_ })
    $script:IdfSeen += @($Installs | ForEach-Object { "v$($_.Version) at $($_.Path)" })
    $usable = @($Installs | Where-Object {
        $_.Version.Major -eq $IdfRelease.Major -and $_.Version.Minor -eq $IdfRelease.Minor })
    if ($usable.Count -eq 0) { return $null }
    $selected = $usable | Where-Object { $_.Id -eq $SelectedId } | Select-Object -First 1
    if ($selected) { return $selected }
    return $usable | Sort-Object Version -Descending | Select-Object -First 1
}

function Find-EimInstall {
    # EIM (ESP-IDF Installation Manager), Espressif's current installer and
    # the one the VS Code ESP-IDF extension installs through, lists its IDFs
    # in eim_idf.json in its tools folder. Each entry names an activation
    # script; the IDF's own export.ps1 cannot find EIM's tools or Python.
    $jsonPaths = @("C:\Espressif\tools\eim_idf.json", (Join-Path $HOME ".espressif\tools\eim_idf.json"))
    if ($env:IDF_TOOLS_PATH) { $jsonPaths = @(Join-Path $env:IDF_TOOLS_PATH "eim_idf.json") + $jsonPaths }
    foreach ($json in $jsonPaths) {
        if (-not (Test-Path $json)) { continue }
        try { $cfg = Get-Content $json -Raw | ConvertFrom-Json } catch { continue }
        $installs = @(foreach ($entry in @($cfg.idfInstalled)) {
            if (-not $entry -or -not $entry.activationScript -or -not (Test-Path $entry.activationScript)) { continue }
            $version = Get-IdfVersion $entry.path
            if (-not $version) { continue }
            [pscustomobject]@{
                Id = $entry.id; Path = $entry.path; Version = $version
                ActivationScript = $entry.activationScript; Python = $entry.python
            }
        })
        $pick = Select-IdfInstall $installs $cfg.idfSelectedId
        if ($pick) { return $pick }
    }
    return $null
}

function Find-ClassicInstall {
    # The older ESP-IDF Tools Installer lists its IDFs in esp_idf.json, keyed
    # by id, and Initialize-Idf.ps1 activates one by id (export.ps1 alone does
    # not put the Xtensa toolchain on PATH for that layout).
    $json = "C:\Espressif\esp_idf.json"
    if (-not (Test-Path "C:\Espressif\Initialize-Idf.ps1") -or -not (Test-Path $json)) { return $null }
    try { $cfg = Get-Content $json -Raw | ConvertFrom-Json } catch { return $null }
    $installs = @(foreach ($prop in @($cfg.idfInstalled.PSObject.Properties)) {
        $version = Get-IdfVersion $prop.Value.path
        if ($version) { [pscustomobject]@{ Id = $prop.Name; Path = $prop.Value.path; Version = $version } }
    })
    return Select-IdfInstall $installs $cfg.idfSelectedId
}

function Invoke-IdfPy {
    # Run idf.py on the IDF's Python. Activation scripts define idf.py as a
    # function or alias in their own scope, which does not reach this script.
    & $script:IdfPython (Join-Path $env:IDF_PATH "tools\idf.py") @args
}

# --- Find the board ---------------------------------------------------------

$device = $null
if ($Port) {
    $device = Get-BoardDevices | Where-Object { $_.Port -eq $Port } | Select-Object -First 1
    if (-not $device) {
        # Port given but not a recognized Espressif device; flash it blind
        # with the classic auto-reset behavior.
        $device = [pscustomobject]@{ Port = $Port; DevicePid = $null }
    }
} else {
    $found = @(Get-BoardDevices)
    if (-not $found) {
        throw "No ESP32-S3 board found (no USB device with VID_303A). " +
              "Plug the board in via USB, or pass -Port COMx explicitly."
    }
    if ($found.Count -gt 1) {
        Write-Host "Multiple ESP32 boards found: $(($found | ForEach-Object Port) -join ', '). Using $($found[0].Port). Pass -Port to choose." -ForegroundColor Yellow
    }
    $device = $found[0]
    Write-Host "Auto-detected board on $($device.Port) (PID_$($device.DevicePid))" -ForegroundColor Green
}

# --- Activate ESP-IDF environment -------------------------------------------
# Uses the first of these that applies (no personal paths):
#   1. Already activated (IDF_PATH set): use the shell as-is.
#   2. $env:IDF_EXPORT_PS1: an export.ps1 or EIM activation script to run.
#   3. EIM, which is what the VS Code ESP-IDF extension installs.
#   4. The older ESP-IDF Tools Installer.
# 3 and 4 can hold several IDF versions side by side; both pick a v5.5.x one.
# Whatever is chosen, the version is checked before building.
$script:IdfSeen = @()
$IdfPython = $null
if ($env:IDF_PATH) {
    Write-Host "ESP-IDF already activated (IDF_PATH=$env:IDF_PATH)" -ForegroundColor Cyan
} else {
    $hint = ""
    if ($env:IDF_EXPORT_PS1 -and -not (Test-Path $env:IDF_EXPORT_PS1)) {
        Write-Host "Ignoring IDF_EXPORT_PS1: $env:IDF_EXPORT_PS1 does not exist." -ForegroundColor Yellow
    }
    if ($env:IDF_EXPORT_PS1 -and (Test-Path $env:IDF_EXPORT_PS1)) {
        $source = "via IDF_EXPORT_PS1 ($env:IDF_EXPORT_PS1)"
        $activate = { & $env:IDF_EXPORT_PS1 }
        $hint = " (Clear IDF_EXPORT_PS1 to let this script find ESP-IDF itself.)"
    } elseif ($eim = Find-EimInstall) {
        $source = "v$($eim.Version) from EIM ($($eim.Path))"
        $activate = { & $eim.ActivationScript }
        $IdfPython = $eim.Python   # used if activation leaves IDF_PYTHON_ENV_PATH unset
    } elseif ($classic = Find-ClassicInstall) {
        $source = "v$($classic.Version) from the ESP-IDF Tools Installer ($($classic.Path))"
        $activate = { & "C:\Espressif\Initialize-Idf.ps1" -IdfId $classic.Id }
    } else {
        $seen = if ($script:IdfSeen) { " Installed versions found: $($script:IdfSeen -join '; ')." } else { "" }
        throw ("ESP-IDF v$IdfRelease.x not found.$seen Install ESP-IDF v$IdfRelease.x with the " +
               "ESP-IDF extension for VS Code or Espressif's installer (EIM), then rerun. " +
               "See docs/GETTING_STARTED.md.")
    }
    Write-Host "Activating ESP-IDF $source..." -ForegroundColor Cyan
    try {
        & $activate | Out-Null
    } catch {
        throw "ESP-IDF activation failed ($source): $($_.Exception.Message)$hint"
    }
    if (-not $env:IDF_PATH) { throw "ESP-IDF activation did not set IDF_PATH ($source).$hint" }
}

$idfVersion = Get-IdfVersion $env:IDF_PATH
if (-not $idfVersion -or $idfVersion.Major -ne $IdfRelease.Major -or $idfVersion.Minor -ne $IdfRelease.Minor) {
    $have = if ($idfVersion) { "v$idfVersion" } else { "unreadable" }
    throw ("This firmware needs ESP-IDF v$IdfRelease.x, but the active ESP-IDF is $have " +
           "($env:IDF_PATH). Install v$IdfRelease.x (VS Code ESP-IDF extension or EIM). If it is " +
           "already installed, rerun from a new terminal where ESP-IDF is not activated and " +
           "IDF_EXPORT_PS1 is not set.")
}

# idf.py, esptool and the config-drift check all run on the IDF's own Python.
if ($env:IDF_PYTHON_ENV_PATH -and (Test-Path "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe")) {
    $IdfPython = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
} elseif (-not ($IdfPython -and (Test-Path $IdfPython))) {
    $IdfPython = (Get-Command python -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1).Source
    if (-not $IdfPython) {
        throw "ESP-IDF is active but its Python was not found (IDF_PYTHON_ENV_PATH is unset and python is not on PATH)."
    }
}
Write-Host "Using ESP-IDF v$idfVersion, Python $IdfPython" -ForegroundColor Cyan

# Pin the chip target. Nothing else names it once both the build dir and
# sdkconfig are gone (-Clean -ResetConfig, or a fresh clone): the defaults
# files don't set CONFIG_IDF_TARGET, so idf.py falls back to plain esp32 and
# the configure step fails resolving esp_tinyusb ("Is your current target
# esp32 set correctly?"). That failure also leaves a build dir that
# 'idf.py set-target' and 'fullclean' then refuse to touch.
if (-not $env:IDF_TARGET) { $env:IDF_TARGET = "esp32s3" }

# --- Move to the project (this script's own directory) ----------------------

Set-Location $PSScriptRoot

# --- Build -------------------------------------------------------------------

if ($Clean) {
    Write-Host "Running fullclean..." -ForegroundColor Cyan
    Invoke-IdfPy fullclean
    if ($LASTEXITCODE -ne 0) { throw "idf.py fullclean failed" }
}

if ($ResetConfig) {
    $sdkconfig = "$PSScriptRoot\sdkconfig"
    if (Test-Path $sdkconfig) {
        Write-Host "Deleting sdkconfig to regenerate from defaults (-ResetConfig)..." -ForegroundColor Cyan
        Remove-Item $sdkconfig -Force
    }
}

# Reconfigure when what the last configure baked in has changed: the version
# (a new commit or tag) or the out-of-tree component settings. ninja only
# watches files, so neither would otherwise reach the build. The record is
# written by CMakeLists.txt.
$record = "$PSScriptRoot\build\rs_configured.txt"
if (Test-Path $record) {
    $version = (git -C $PSScriptRoot describe --tags --always --dirty 2>$null)
    if (-not $version) {
        $versionFile = "$PSScriptRoot\VERSION"
        $version = if (Test-Path $versionFile) { (Get-Content $versionFile -TotalCount 1).Trim() } else { "dev" }
    }
    $want = "$version|$env:RS_EXTRA_COMPONENT_DIRS|$env:RS_DEPENDENCIES_LOCK"
    if ((Get-Content $record -Raw) -ne $want) {
        Write-Host "Version or out-of-tree components changed since the last configure; reconfiguring..." -ForegroundColor Cyan
        Invoke-IdfPy reconfigure
        if ($LASTEXITCODE -ne 0) { throw "idf.py reconfigure failed" }
    }
}

Write-Host "Building..." -ForegroundColor Cyan
Invoke-IdfPy build
if ($LASTEXITCODE -ne 0) {
    # Was the failure the config-drift guard? Re-run it directly (fast) to tell
    # a stale sdkconfig apart from a real compile error, and point the user at
    # the one-flag fix instead of the raw menuconfig/delete instructions.
    $drift = "$PSScriptRoot\scripts\check_config_drift.py"
    if (-not $ResetConfig -and (Test-Path $drift)) {
        & $IdfPython $drift $PSScriptRoot | Out-Null
        if ($LASTEXITCODE -ne 0) {
            # Re-echo the flags the user actually passed, plus -ResetConfig.
            $flags = foreach ($kv in $PSBoundParameters.GetEnumerator()) {
                if ($kv.Value -is [switch]) { "-$($kv.Key)" } else { "-$($kv.Key) $($kv.Value)" }
            }
            $suggest = (@(".\flash.cmd") + $flags + "-ResetConfig") -join ' '
            Write-Host ""
            Write-Host "Build failed because sdkconfig has drifted from the committed defaults" -ForegroundColor Yellow
            Write-Host "(usually after a git pull that changed sdkconfig.defaults)." -ForegroundColor Yellow
            Write-Host "Re-run with -ResetConfig to regenerate it from the defaults:" -ForegroundColor Yellow
            Write-Host "    $suggest" -ForegroundColor Cyan
            Write-Host "(discards menuconfig-only settings such as a non-default board revision)." -ForegroundColor Yellow
        }
    }
    throw "Build failed"
}

# --- Get the board into a flashable state ------------------------------------

$flashPort = $device.Port
$inRomDownloadMode = ($device.DevicePid -eq "0009")

if ($device.DevicePid -eq "4001") {
    # TinyUSB CDC console: esptool cannot reset it; use the console command.
    Enter-DownloadMode $device.Port
    Write-Host "Waiting for the board to re-enumerate in download mode..." -ForegroundColor Cyan
    $dl = Wait-DownloadPort
    if (-not $dl) {
        throw "Board did not re-enumerate in download mode. " +
              "Enter it manually (hold BOOT/SW2, tap RESET) and rerun."
    }
    $flashPort = $dl.Port
    $inRomDownloadMode = ($dl.DevicePid -eq "0009")
    Write-Host "Board in download mode on $flashPort (PID_$($dl.DevicePid))" -ForegroundColor Green
}

# --- Flash -------------------------------------------------------------------

# In ROM native-USB download mode (PID_0009) the usual RTS hard reset after
# flashing is a no-op that wedges the ROM's USB port; a watchdog reset boots
# the new firmware cleanly instead.
$afterFlash = if ($inRomDownloadMode) { "watchdog_reset" } else { "hard_reset" }
$argsFile = if ($Full) { "flash_args" } else { "flash_app_args" }

Write-Host "Flashing $(if ($Full) { 'bootloader + partition table + app' } else { 'app' }) on $flashPort..." -ForegroundColor Cyan
Push-Location "$PSScriptRoot\build"
try {
    # stdout is captured (and still shown) so a failed exit can be checked
    # against what esptool actually wrote.
    & $IdfPython -m esptool --chip esp32s3 -p $flashPort -b 460800 `
        --before default_reset --after $afterFlash write_flash "@$argsFile" |
        Tee-Object -Variable esptoolOut
    if ($LASTEXITCODE -ne 0) {
        # After a watchdog reset the board leaves download mode and its port
        # disappears; esptool 4.12+ reports that as a serial error and exits
        # non-zero even though every segment was written and verified.
        $text = $esptoolOut -join "`n"
        $written = ([regex]::Matches($text, "Wrote \d+ bytes")).Count
        $verified = ([regex]::Matches($text, "Hash of data verified")).Count
        if ($inRomDownloadMode -and $written -gt 0 -and $verified -eq $written -and
            $text -match "Hard resetting with a watchdog") {
            Write-Host "All $written segment(s) verified; the serial error above came from the board rebooting." -ForegroundColor Yellow
        } else {
            throw "esptool flash failed on $flashPort"
        }
    }
} finally {
    Pop-Location
}

if ($inRomDownloadMode) {
    # Confirm the watchdog reset actually booted the firmware (the console
    # re-enumerates as PID_4001 or, on legacy firmware, PID_1001).
    Write-Host "Waiting for the board to reboot into the new firmware..." -ForegroundColor Cyan
    # The USB-Serial-JTAG port (PID_1001) appears briefly during early boot
    # before the firmware hands the PHY to the TinyUSB console (PID_4001),
    # so keep polling until the console itself shows up.
    $rebooted = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $dev = Get-BoardDevices | Where-Object { $_.DevicePid -in @("4001", "1001") } | Select-Object -First 1
        if ($dev) { $rebooted = $dev }
        if ($rebooted -and $rebooted.DevicePid -eq "4001") { break }
        Start-Sleep -Milliseconds 500
    }
    if ($rebooted) {
        Write-Host "Board is up on $($rebooted.Port) (console)." -ForegroundColor Green
    } else {
        Write-Host "Board did not re-enumerate: tap RESET or power cycle it to boot the new firmware." -ForegroundColor Yellow
    }
}

# --- Monitor -----------------------------------------------------------------

if ($Monitor) {
    if ($device.DevicePid -eq "4001" -or $inRomDownloadMode) {
        Write-Host "Skipping monitor: idf.py monitor cannot drive the TinyUSB CDC console. Use PuTTY/RealTerm on the console port instead." -ForegroundColor Yellow
    } else {
        Invoke-IdfPy -p $flashPort monitor
    }
}
