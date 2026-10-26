@echo off
set ADB="C:\Users\pedro\AppData\Local\Android\Sdk\platform-tools\adb.exe"
echo ========================================================
echo   Instalador Resident Evil 5 para Android (APK)
echo ========================================================
echo.
echo Conecte seu celular Android via cabo USB com a opcao
echo "Depuracao USB" ativada nas Opcoes do Desenvolvedor.
echo.
%ADB% devices
echo.
echo Instalando ResidentEvil5.apk no celular...
%ADB% install -r ResidentEvil5.apk
if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo  [SUCESSO] O jogo foi instalado com sucesso no aparelho!
    echo ========================================================
) else (
    echo.
    echo ========================================================
    echo  [AVISO] Nao foi possivel instalar automaticamente via USB.
    echo  Voce tambem pode copiar o arquivo "ResidentEvil5.apk"
    echo  diretamente para a memoria do celular e tocar para instalar.
    echo ========================================================
)
echo.
pause
