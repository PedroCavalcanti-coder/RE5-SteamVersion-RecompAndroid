@echo off
setlocal
title re5-recompiled - Perfil de uma thread do jogo
cd /d "%~dp0"

rem Abre o jogo com o perfil de uma thread. Rode o benchmark variavel normalmente
rem (pelo menos a area 1) e feche o jogo. Relatorio a cada 10 s em work\perf\<backend>_<thread>.log.
rem
rem   perfil_vulkan.bat                    re5vk, thread principal
rem   perfil_vulkan.bat re5vk render       re5vk, thread de render (a que chama o D3D9)
rem   perfil_vulkan.bat dxvk main          DXVK, thread principal
rem   perfil_vulkan.bat re5vk all          todas as threads

set "GAME_DIR=%~dp0bin_input\game"
set "RECOMP_EXE=build\windows-x64\re5.exe"
if not exist "%~dp0work\perf" mkdir "%~dp0work\perf"

set "RE5_GFX=re5vk"
if /i "%~1"=="dxvk" set "RE5_GFX=dxvk"
set "WHICH=main"
if not "%~2"=="" set "WHICH=%~2"
set "RE5_PROFILE_THREAD=main"
if /i "%WHICH%"=="render" set "RE5_PROFILE_THREAD=00457C60"
if /i "%WHICH%"=="all" set "RE5_PROFILE_THREAD="

set "RE5_LOG_FILE=%~dp0work\perf\%RE5_GFX%_%WHICH%.log"
set "RE5_PROFILE=10"
set "RE5_FPS_LOG=1"
set "RE5_SEED_LOG=%~dp0work\runtime_seeds.txt"
set "DXVK_LOG_PATH=%~dp0work"

echo Backend: %RE5_GFX%   Thread: %WHICH%   Log: %RE5_LOG_FILE%
"%RECOMP_EXE%" --game-dir "%GAME_DIR%" --exe bin_input/re5dx9.clean.exe
echo Codigo de saida: %ERRORLEVEL%
pause
