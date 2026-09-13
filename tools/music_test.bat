@echo off
REM Generates music_test\ sine-tone WAVs + music.ini in the repo root, then
REM optionally launches zelda3.exe so you can hear the music player mix.
REM Requires python on PATH (any 3.x).
python "%~dp0music_test.py" %*
if errorlevel 1 (
  echo ERROR: music_test.py failed
  exit /b 1
)
echo.
echo music.ini + music_test\ are ready in %CD%
set /p RUN="Launch zelda3.exe now to test? [y/N] "
if /i "%RUN%"=="y" start "" /wait zelda3.exe
