@echo off
rem Reinstalle le mod dans Discord (a lancer apres une mise a jour de Discord).
rem N'utilise JAMAIS -repair : ca retelechargerait le Vencord officiel.
setlocal
set "BASE=%~dp0vencord"

echo Fermeture de Discord...
taskkill /F /IM DiscordTaskbarTyping.exe /T >nul 2>&1
taskkill /F /IM Discord.exe /T >nul 2>&1
timeout /t 2 /nobreak >nul

set "VENCORD_USER_DATA_DIR=%BASE%"
set "VENCORD_DEV_INSTALL=1"

echo Reparation en cours...
"%BASE%\VencordInstallerCli.exe" -install -branch stable
if errorlevel 1 (
    echo.
    echo La reparation a echoue. Ferme Discord completement et reessaie.
    pause
    exit /b 1
)

echo Relance de Discord...
start "" "%LOCALAPPDATA%\Discord\Update.exe" --processStart Discord.exe
exit /b 0
