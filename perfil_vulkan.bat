@echo off
setlocal
title re5-recompiled - Perfil da thread principal (re5vk)
cd /d "%~dp0"

rem Abre o jogo com o backend Vulkan proprio e o perfil da thread principal.
rem Rode o benchmark variavel normalmente (pelo menos a area 1) e feche o jogo.
rem O relatorio sai a cada 10 s em work\perf\vk_main.log.
rem Para comparar com a DXVK: perfil_vulkan.bat dxvk

set "GAME_DIR=%~dp0bin_input\game"
set "RECOMP_EXE=build\windows-x64\re5.exe"
if not exist "%~dp0work\perf" mkdir "%~dp0work\perf"

set "RE5_GFX=re5vk"
set "RE5_LOG_FILE=%~dp0work\perf\vk_main.log"
if /i "%~1"=="dxvk" (
    set "RE5_GFX=dxvk"
    set "RE5_LOG_FILE=%~dp0work\perf\dxvk_main.log"
)
set "RE5_PROFILE=10"
set "RE5_PROFILE_THREAD=main"
set "RE5_FPS_LOG=1"
set "RE5_SEED_LOG=%~dp0work\runtime_seeds.txt"
set "DXVK_LOG_PATH=%~dp0work"

echo Backend: %RE5_GFX%   Log: %RE5_LOG_FILE%
"%RECOMP_EXE%" --game-dir "%GAME_DIR%" --exe bin_input/re5dx9.clean.exe
echo Codigo de saida: %ERRORLEVEL%
pause
