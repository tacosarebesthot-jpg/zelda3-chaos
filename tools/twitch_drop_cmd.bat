@echo off
setlocal
title zelda3 twitch dropper
rem Manual single-command dropper - equivalent of ZALiA's twitch_test.bat.
rem Usage:  twitch_drop_cmd.bat heal 4        (verb + optional numeric arg)
rem         twitch_drop_cmd.bat confuse      (timed effects use default length)
rem Requires the game running with drop=1 in twitch_config.txt.

set "DROP=%~dp0..\twitch_drop"
if not exist "%DROP%" mkdir "%DROP%"

set "VERB=%~1"
if "%VERB%"=="" set "VERB=heal"
set "ARG=%~2"
if "%ARG%"=="" set "ARG=0"

> "%DROP%\manual_%RANDOM%%RANDOM%.txt" echo %VERB%^|%ARG%^|manual|0
echo dropped: %VERB% ^| %ARG% ^| manual ^| default dur
echo (game polls twitch_drop\ every frame; files must have .txt extension)
pause
endlocal
