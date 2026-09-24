param([string]$Name, [string]$Command)
# Run a command on the logged-on desktop from an SSH session: a one-shot
# scheduled task with /it runs in the interactive console session.
schtasks /delete /tn "qcb-lab-$Name" /f 2>$null | Out-Null
schtasks /create /tn "qcb-lab-$Name" /tr "$Command" /sc once /st 00:00 /it /f 2>$null | Out-Null
schtasks /run /tn "qcb-lab-$Name" | Out-Null
