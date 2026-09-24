@echo off
rem Rebuild QCView's Release tree from an SSH session. Run through a
rem scheduled task (see ONE-SEAT.md): a process started directly from SSH
rem dies with the session. The viewer must not be running: the linker
rem cannot overwrite a running qcview.exe.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cd /d %USERPROFILE%\Documents\GitHub\QCView-Player && cmake --build build-release > C:\qcb-lab\qcview-build.log 2>&1
