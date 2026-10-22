@echo off
setlocal
cd /d "%~dp0"
title re5-recompiled - Progresso da Compilacao
python scripts\monitor_build.py
pause
