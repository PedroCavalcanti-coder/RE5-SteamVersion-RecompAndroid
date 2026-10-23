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
set "RE5_GUEST_LOCK=global"
rem Diagnosticos (desligados: custam desempenho). Tire o "rem" para ligar:
rem   perfil no log a cada 10 s (amostra as threads a cada 1 ms):
rem set "RE5_PROFILE=10"
rem   gravacao do som em work\snd (mistura, buffers e events.txt):
rem set "RE5_DSOUND_DUMP=%~dp0work\snd"
rem set "RE5_DSOUND_DUMP_SECS=300"
rem Ferramentas de depuracao da colisao (nao custam nada ate apertar a tecla):
rem   F10 = despeja a memoria do jogo em work\dump\guest_N.bin (para o hk_near.py)
rem   F8 zera e F9 grava as funcoes executadas desde o F8 em work\cov\cov_N.txt
rem   F6 = linhas de colisao por cima do quadro (experimental)
if not exist "%~dp0work\dump" mkdir "%~dp0work\dump"
if not exist "%~dp0work\cov" mkdir "%~dp0work\cov"
set "RE5_DUMP=1"
set "RE5_COV=1"
set "RE5_COLDEBUG=1"
rem   F7 = Deterministic TAS & Trace (grava work\trace_recomp.bin e aplica snapshot_s115.json)
set "RE5_DETERMINISTIC=1"
set "RE5_TAS_KEY=7"
set "RE5_TRACE_FILE=%~dp0work\trace_recomp.bin"
rem   F11 = captura o movimento dos dois personagens jogaveis (0x00B72FA0, uma chamada de cada)
rem         com a memoria antes e depois, em work\dump\capture_N_* (para o snap_diff.py).
rem         Teste da escada: segure "para frente" encostado no degrau e aperte F11.
set "RE5_CAPTURE=00B72FA0"
set "RE5_CAPTURE_N=2"
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
