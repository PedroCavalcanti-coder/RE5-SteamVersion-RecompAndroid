@echo off
setlocal
cd /d "%~dp0"
title re5-recompiled - Compilacao completa (lift + build + testes)

rem Lifting (incremental: so as janelas que mudaram, p.ex. pelos tracepoints),
rem compilacao do build\test-build e testes. O resultado de cada etapa vai para
rem work\compilar_tudo.status (lido por quem esperava a compilacao terminar).
if not exist "%~dp0work" mkdir "%~dp0work"
set "STATUS=%~dp0work\compilar_tudo.status"
echo rodando> "%STATUS%"

echo ======================================================================
echo   1/3  Lifting (x86 -^> C)
echo ======================================================================
rem Sai com 1 se alguma funcao nao traduziu (hoje: 0x0094818B, MMX, ja conhecida);
rem os .c saem assim mesmo e o dispatch aborta com o endereco se ela for chamada (D8).
python -u scripts\run_lift.py lift
if errorlevel 1 echo [aviso] ha falhas de lift: ver generated\src\lift_stats.json
if not exist "generated\src\re5_dispatch_table.c" (
    echo falhou: lift> "%STATUS%"
    goto fim
)

echo.
echo ======================================================================
echo   2/3  Compilacao (build\test-build)
echo ======================================================================
cmake --build build\test-build -j 16
if errorlevel 1 (
    echo falhou: build> "%STATUS%"
    goto fim
)

echo.
echo ======================================================================
echo   3/3  Testes (ctest; o difftest_cpu longo fica de fora)
echo ======================================================================
ctest --test-dir build\test-build --output-on-failure -E difftest_cpu
if errorlevel 1 (
    echo falhou: ctest> "%STATUS%"
    goto fim
)
echo ok> "%STATUS%"

:fim
echo.
echo ======================================================================
type "%STATUS%"
echo   Pronto. Para jogar: executar_recompilado.bat
echo ======================================================================
pause
