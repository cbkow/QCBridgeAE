@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cd /d %USERPROFILE%\Documents\GitHub\QCBridge\agent && %USERPROFILE%\.cargo\bin\cargo build --release > C:\qcb-lab\agent-build.log 2>&1
