@echo off
setlocal
title re5-recompiled - Executavel Nativo 64-bit
cd /d "%~dp0"

rem Dados do jogo (nativePC_MT), fora do git.
set "GAME_DIR=%~dp0bin_input\game"
set "RECOMP_EXE=build\test-build\re5.exe"

rem Log completo (stderr + arquivo) e enderecos que o catalogo nao achou (etapa 2.6).
rem Para ver cada import chamado: set RE5_LOG=trace
set "RE5_LOG_FILE=%~dp0work\re5_run.log"
set "RE5_SEED_LOG=%~dp0work\runtime_seeds.txt"
if not exist "%~dp0work" mkdir "%~dp0work"
rem Teste de colisao: uma thread do jogo por vez (mais lento). Se a colisao
rem ficar certa assim, o defeito e corrida entre threads. Tire o "rem" para ligar:
rem set "RE5_GUEST_LOCK=global"
rem Diagnosticos (desligados: custam desempenho). Tire o "rem" para ligar:
rem   perfil no log a cada 10 s (amostra as threads a cada 1 ms):
rem set "RE5_PROFILE=10"
rem   gravacao do som em work\snd (mistura, buffers e events.txt):
rem set "RE5_DSOUND_DUMP=%~dp0work\snd"
rem set "RE5_DSOUND_DUMP_SECS=300"
rem Video: Vulkan pela DXVK mais nova que a GPU aceita (build\test-build\dxvk\3.1.1
rem ou 2.6.2; o log diz qual). Forcar uma: set "RE5_DXVK=2.6.2"
rem Sem Vulkan (Direct3D do sistema): set "RE5_D3D9_DLL=d3d9.dll"
rem HUD da DXVK: fps, GPU/driver/versao do Vulkan e versao da DXVK (tire a linha para esconder)
set "DXVK_HUD=fps,devinfo,version"
set "DXVK_LOG_PATH=%~dp0work"

echo ======================================================================
echo   re5-recompiled: Executando o Resident Evil 5 Nativo de 64-bits
echo ======================================================================
echo.
echo Executavel: %RECOMP_EXE%
echo Pasta de Dados: %GAME_DIR%
echo Log: %RE5_LOG_FILE%
echo.
echo Funcao ou import sem traducao: o programa para (de proposito) com o
echo endereco ou o nome no log.
echo.

if not exist "%RECOMP_EXE%" (
    echo [ERRO] Executavel '%RECOMP_EXE%' nao encontrado!
    pause
    exit /b 1
)

"%RECOMP_EXE%" --game-dir "%GAME_DIR%" --exe bin_input/re5dx9.clean.exe
echo.
echo Codigo de saida: %ERRORLEVEL%

echo.
echo ======================================================================
echo   Execucao finalizada. Ultimas linhas do log:
echo ======================================================================
powershell -NoProfile -Command "Get-Content -Tail 25 '%RE5_LOG_FILE%'"
pause
