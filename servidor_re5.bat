@echo off
setlocal
title RE5 - servidor do multiplayer
cd /d "%~dp0"
rem Servidor de retransmissao do multiplayer LAN (tools/re5_server). Fechar esta janela derruba o servidor.
rem Uso: servidor_re5.bat [porta]      (padrao 27040)
set "SRV="
for %%P in ("build\windows-x64\re5_server.exe" "build\host-rel-o2\re5_server.exe" "re5_server.exe") do if exist %%P if not defined SRV set "SRV=%%~P"
if not defined SRV (
    echo re5_server.exe nao encontrado. Compile com: cmake --build ^<pasta do build^> --target re5_server
    pause
    exit /b 1
)
set "PORTA=%~1"
if "%PORTA%"=="" set "PORTA=27040"
echo Enderecos deste PC ^(use um deles no RE5_LAN_SERVER dos jogadores^):
for /f "tokens=2 delims=:" %%I in ('ipconfig ^| findstr /C:"IPv4"') do echo   %%I:%PORTA%
echo.
"%SRV%" --port %PORTA%
pause
