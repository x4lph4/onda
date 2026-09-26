@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "D:\1 - KIKE EL VASCO PROGRAMAS\seratodjpor\build"
ninja %*
