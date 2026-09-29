# Discord Taskbar Typing

Affiche, juste au-dessus de la barre des tâches Windows, la photo de profil des personnes
(3 maximum) qui sont en train de t'écrire en **message privé** sur Discord, avec les 3 petits
points animés.

## Installer (utilisateurs)

1. Télécharge `DiscordTaskbarTyping-Setup.exe` dans l'onglet **Releases**.
2. Lance-le et suis l'assistant (aucun droit administrateur requis).
3. Relance Discord.

L'overlay démarre avec Discord et se ferme quand Discord est quitté. Après une mise à jour de
Discord, le mod peut disparaître : lance **Réparer** depuis le menu Démarrer.

> Windows SmartScreen peut afficher un avertissement : l'exécutable n'est pas signé.
> "Informations complémentaires", puis "Exécuter quand même".

## Comment ça marche

```
Discord (Vencord + plugin TaskbarTyping)
        |  événement TYPING_START (messages privés uniquement)
        |  POST http://127.0.0.1:27345/typing   (local, rien ne sort de ton PC)
        v
Overlay C++ (Win32 + GDI+)  -> affiche les pp au-dessus de la barre des tâches
```

- `plugin/` : plugin Vencord. `index.ts` détecte qui écrit, `native.ts` envoie les données à
  l'overlay depuis le processus principal d'Electron (la CSP de Discord bloquerait un `fetch`)
  et lance / arrête l'overlay.
- `overlay/` : l'application C++ (serveur HTTP local, téléchargement des avatars via WinHTTP,
  dessin GDI+). Elle surveille le processus de Discord et se ferme avec lui.
- `installer/` : script Inno Setup + script PowerShell qui construit l'installateur complet.

## Compiler depuis les sources

Prérequis : Visual Studio (outils C++), Git, Node.js 22+, pnpm, Inno Setup 6.

**1. Overlay** (dans "x64 Native Tools Command Prompt for VS") :

```
cd overlay\src
cl /std:c++17 /O2 /MT /EHsc /utf-8 /DUNICODE /D_UNICODE main.cpp OverlayWindow.cpp /Fe:DiscordTaskbarTyping.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib ws2_32.lib gdiplus.lib winhttp.lib ole32.lib
```

Test rapide sans Discord : `DiscordTaskbarTyping.exe --test` affiche 3 pastilles fixes.

**2. Installateur complet** (clone Vencord, ajoute le plugin, build, compile le Setup) :

```
cd installer
powershell -ExecutionPolicy Bypass -File .\build-release.ps1 -OverlayExe "..\overlay\src\DiscordTaskbarTyping.exe"
```

Le Setup est créé dans `installer\output\`. Le commit de Vencord embarqué est noté dans
`BUILD-INFO.txt` (utilise `-VencordRef <commit>` pour figer une version).

## Avertissements

- Les mods du client Discord ne sont pas autorisés par les conditions d'utilisation de Discord.
  Le risque est faible pour un plugin qui ne fait que lire des événements locaux, mais il existe.
- Ce projet n'est affilié ni à Discord ni à Vencord.

## Licence

GPL-3.0 (voir `LICENSE`). Le plugin s'appuie sur les API internes de
[Vencord](https://github.com/Vendicated/Vencord) (GPL-3.0), et l'installateur distribue un build
de Vencord : le tout est donc publié sous la même licence, avec ces sources.
