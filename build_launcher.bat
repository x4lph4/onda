@echo off
rem Compiles the root Onda.exe launcher stub (tools\onda_launcher.c).
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /W3 /Fe:"Onda.exe" tools\onda_launcher.c /link user32.lib shell32.lib
if errorlevel 1 (echo. & echo FALLO & exit /b 1)
echo Onda.exe lanzador creado en la raiz.
