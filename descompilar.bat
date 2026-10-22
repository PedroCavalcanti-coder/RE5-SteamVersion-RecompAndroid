@echo off
setlocal
cd /d "%~dp0"
title re5-recompiled - Descompilacao e Lifting
echo ======================================================================
echo   re5-recompiled: Lifting do Resident Evil 5 (x86 -^> C)
echo ======================================================================
echo.
echo Executando lifting das funcoes catalogadas...
echo Voce vera cada bloco de funcoes sendo traduzido para C em tempo real.
echo.

python -u scripts\run_lift.py lift

echo.
echo ======================================================================
echo   Processo concluido! Os arquivos C estao em 'generated\src\'.
echo ======================================================================
pause
