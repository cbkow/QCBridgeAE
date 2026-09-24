# Open QCView on the stream URL in C:\qcb-lab\stream-url.txt (interactive session).
$url = (Get-Content C:\qcb-lab\stream-url.txt -Raw).Trim()
$exe = "$env:USERPROFILE\Documents\GitHub\QCView-Player\build-release\qcview.exe"
Start-Process -FilePath $exe -ArgumentList @($url)
