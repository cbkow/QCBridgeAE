param([string]$Step)
# Windows-side checks for the agent hygiene change; run over SSH, one step
# at a time. Desktop launches go through run.ps1 (a one-shot interactive task).
$exe = "$env:USERPROFILE\Documents\GitHub\QCBridge\agent\target\release\qcbridge-agent.exe"
$cfg = "$env:APPDATA\QCBridge"

function Procs {
    $names = "qcbridge-agent", "blender", "qcb-capture-win", "ffmpeg"
    $p = Get-Process -Name $names -ErrorAction SilentlyContinue
    if (-not $p) { "  (none of agent/blender/helper/ffmpeg running)"; return }
    foreach ($x in $p) {
        "  {0,-18} pid {1,-6} window={2} title='{3}'" -f $x.ProcessName, $x.Id, $x.MainWindowHandle, $x.MainWindowTitle
    }
}
function Consoles {
    # conhost.exe windows on the desktop: one per visible console
    $c = Get-Process -Name conhost -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
    "  visible console windows: " + (@($c).Count)
    foreach ($x in $c) { "    conhost pid {0} title='{1}'" -f $x.Id, $x.MainWindowTitle }
}

switch ($Step) {
    "launch" {
        # The exact case that died: the exe started directly by a task, no cmd wrapper.
        & C:\qcb-lab\run.ps1 -Name agent-direct -Command "`"$exe`" --role replica"
        Start-Sleep 5
        "procs:"; Procs; Consoles
        "task result:"; (schtasks /Query /TN "qcb-lab-agent-direct" /FO LIST /V | Select-String "Last Result|Status:")
        "agent.log tail:"; Get-Content "$cfg\agent.log" -Tail 6
    }
    "status" {
        "procs:"; Procs; Consoles
        "files:"; Get-ChildItem $cfg | ForEach-Object { "  {0,-12} {1,8} {2}" -f $_.Name, $_.Length, $_.LastWriteTime }
        "agent.log tail:"; Get-Content "$cfg\agent.log" -Tail 8
        if (Test-Path "$cfg\blender.log") { "blender.log tail:"; Get-Content "$cfg\blender.log" -Tail 4 }
        if (Test-Path "$cfg\capture.log") { "capture.log tail:"; Get-Content "$cfg\capture.log" -Tail 3 }
    }
    "hardkill" {
        $a = Get-Process -Name qcbridge-agent -ErrorAction SilentlyContinue
        "before:"; Procs
        if ($a) { Stop-Process -Id $a.Id -Force; "killed agent pid $($a.Id) with Stop-Process -Force" }
        Start-Sleep 3
        "after 3 s:"; Procs
    }
    "second" {
        # Headless second instance from this SSH session: must refuse, no box.
        & $exe --role replica --no-tray 2>&1 | ForEach-Object { "  stderr: $_" }
        "  exit code: $LASTEXITCODE"
        "agent.log tail:"; Get-Content "$cfg\agent.log" -Tail 3
    }
    "second-box" {
        # With the tray: the refusal must show a message box on the desktop.
        # This step itself runs on the desktop (through run.ps1), so a plain start is on the desktop too.
        Start-Process -FilePath $exe -ArgumentList "--role", "replica"
        Start-Sleep 4
        $boxes = Get-Process -Name qcbridge-agent -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -ne "" }
        foreach ($b in $boxes) { "  dialog: pid {0} title='{1}'" -f $b.Id, $b.MainWindowTitle; Stop-Process -Id $b.Id -Force; "  (closed it)" }
        if (-not $boxes) { "  no dialog window found" }
        "procs:"; Procs
    }
    "version" {
        "--version from this SSH console:"
        & $exe --version 2>&1 | ForEach-Object { "  $_" }
        "  exit code: $LASTEXITCODE"
    }
    "autostart" {
        & powershell -ExecutionPolicy Bypass -File "$env:USERPROFILE\Documents\GitHub\QCBridge\agent\windows\autostart.ps1" -Role replica -AgentPath $exe
        "task after:"; (schtasks /Query /TN "QCBridge Agent" 2>&1 | Select-Object -First 1)
        "run value:"; (Get-ItemProperty HKCU:\Software\Microsoft\Windows\CurrentVersion\Run)."QCBridge Agent"
    }
    "subsystem" {
        # PE optional header: Subsystem 2 = GUI, 3 = console.
        $b = [System.IO.File]::ReadAllBytes($exe)
        $pe = [BitConverter]::ToInt32($b, 0x3C)
        $sub = [BitConverter]::ToInt16($b, $pe + 24 + 68)
        "  PE subsystem: $sub (2 = GUI, 3 = console)"
        (Get-Item $exe | Select-Object Length, LastWriteTime | Format-List | Out-String).Trim()
    }
}
