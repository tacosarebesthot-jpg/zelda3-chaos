@echo off
REM Build zelda3 with MSVC BuildTools + bundled SDL2 (third_party/SDL2-2.26.3)
if not exist third_party\SDL2-2.26.3\lib\x64\SDL2.lib (
  echo ERROR: SDL2 not found in third_party\SDL2-2.26.3
  exit /b 1
)
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found in either BuildTools location
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 (
  echo ERROR: vcvars64 failed
  exit /b 1
)
if not exist build\msvc mkdir build\msvc
set INC=-I. -Ithird_party\SDL2-2.26.3\include
REM MUSIC_PLAYER_HAVE_VORBIS=1 enables OGG decode (third_party\stb\stb_vorbis.c).
REM Makefile users: add -DMUSIC_PLAYER_HAVE_VORBIS=1 to the CFLAGS in Makefile.
REM (MP3 needs no define - dr_mp3 is on by default; -DMUSIC_PLAYER_NO_MP3 opts out.)
set DEF=/DWIN32 /DNDEBUG /D_CONSOLE /D_CRT_SECURE_NO_WARNINGS /DSYSTEM_VOLUME_MIXER_AVAILABLE=0 /DMUSIC_PLAYER_HAVE_VORBIS=1
set CFLAGS=/nologo /O2 /MD /std:c11 /W0 /c %DEF% %INC%
(for %%f in (src\*.c snes\*.c) do @echo %%f) > build\msvc\sources.rsp
echo third_party\gl_core\gl_core_3_1.c>> build\msvc\sources.rsp
echo third_party\opus-1.3.1-stripped\opus_decoder_amalgam.c>> build\msvc\sources.rsp
cl %CFLAGS% /Fobuild\msvc\ @build\msvc\sources.rsp
if not %errorlevel%==0 (
  echo ERROR: compile failed
  exit /b 1
)
(echo /OUT:zelda3.exe) > build\msvc\link.rsp
echo /SUBSYSTEM:CONSOLE>> build\msvc\link.rsp
echo build\msvc\*.obj>> build\msvc\link.rsp
echo third_party\SDL2-2.26.3\lib\x64\SDL2.lib>> build\msvc\link.rsp
echo third_party\SDL2-2.26.3\lib\x64\SDL2main.lib>> build\msvc\link.rsp
echo ws2_32.lib winhttp.lib opengl32.lib user32.lib gdi32.lib winmm.lib imm32.lib ole32.lib oleaut32.lib version.lib setupapi.lib shell32.lib advapi32.lib>> build\msvc\link.rsp
link @build\msvc\link.rsp
if not %errorlevel%==0 (
  echo ERROR: link failed
  exit /b 1
)
echo BUILD OK: zelda3.exe
