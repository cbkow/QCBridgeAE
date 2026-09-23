<#
.SYNOPSIS
  Copy the QCBridgeAE Transmit device into MediaCore, where After Effects
  and Premiere Pro load it from.

.DESCRIPTION
  The Windows twin of copying the .bundle into
  /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/ on macOS:
  %PROGRAMFILES%\Adobe\Common\Plug-ins\7.0\MediaCore\ is shared by both hosts
  and is not writable without elevation, so this script re-launches itself
  through UAC when it has to. Hand-copy for now; A7 is the installer.

  Restart the host after installing: neither AE nor Premiere rescans
  MediaCore while running, and neither unloads a Transmit device on quit
  (lab/results/2026-09-21-a4-transmit-probe), so an upgrade needs a quit
  first or the copy is refused as in use.

.PARAMETER BuildDir
  Where QCBridgeAE-Transmit.prm is. Default: build-release beside this repo.

.PARAMETER Probe
  Also install the A4 probe device (QCBridgeAE-Transmit-Probe.prm). Off by
  default: the probe is an instrument and shows up as a second device.

.PARAMETER Remove
  Delete the installed device(s) instead.
#>
param(
    [string] $BuildDir = "",
    [switch] $Probe,
    [switch] $Remove
)

$ErrorActionPreference = "Stop"
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$($MyInvocation.MyCommand.Path)`"")
    if ($BuildDir) { $args += @("-BuildDir", "`"$BuildDir`"") }
    if ($Probe)    { $args += "-Probe" }
    if ($Remove)   { $args += "-Remove" }
    Write-Host "MediaCore needs elevation; asking (UAC)…"
    $p = Start-Process -FilePath "powershell.exe" -ArgumentList $args -Verb RunAs -Wait -PassThru
    exit $p.ExitCode
}

$dest = Join-Path ${env:ProgramFiles} "Adobe\Common\Plug-ins\7.0\MediaCore"
if (-not (Test-Path $dest)) { throw "MediaCore folder not found: $dest (is an Adobe host installed?)" }
if (-not $BuildDir) { $BuildDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "build-release" }

$files = @("QCBridgeAE-Transmit.prm")
if ($Probe) { $files += "QCBridgeAE-Transmit-Probe.prm" }

foreach ($f in $files) {
    $target = Join-Path $dest $f
    if ($Remove) {
        if (Test-Path $target) { Remove-Item -Force $target; Write-Host "removed $target" }
        else { Write-Host "not installed: $f" }
        continue
    }
    $src = Join-Path $BuildDir $f
    if (-not (Test-Path $src)) { throw "not built: $src" }
    Copy-Item -Force $src $target
    Write-Host "installed $target ($((Get-Item $target).Length) bytes)"
}
if (-not $Remove) { Write-Host "restart After Effects / Premiere Pro, then enable the device under Video Preview (Mercury Transmit)" }
