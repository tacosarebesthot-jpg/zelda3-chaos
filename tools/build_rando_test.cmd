@echo off
REM Build the standalone phase-1 rando logic test binary (NOT part of zelda3.exe).
REM Run from the repo root: tools\build_rando_test.cmd
if not exist tools\rando_test_build mkdir tools\rando_test_build
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo ERROR: vcvars64 failed
  exit /b 1
)
cl /nologo /std:c11 /W0 /O2 /I. /DRANDO_TEST src\rando\*.c /Fe:tools\rando_test.exe /Fo:tools\rando_test_build\
if not %errorlevel%==0 (
  echo ERROR: rando_test compile failed
  exit /b 1
)
echo BUILD OK: tools\rando_test.exe
