@echo off
rem ===========================================================================
rem  Veyra - installation autonome des dependances de developpement (Windows)
rem
rem  Reproduit en local la chaine de la CI .github/workflows/windows-build.yml :
rem  outils (Git, Python 3.11, CMake, Ninja, VS 2022 Build Tools, Rust),
rem  SDK epingles (scripts/ci/fetch-sdks.ps1), Qt 6.8.3, ONNX Runtime DirectML,
rem  libdovi 3.4.0, vcpkg a la builtin-baseline, FFmpeg 9.0.1 patche et les
rem  bibliotheques du streaming Xbox (libdatachannel, nlohmann-json, Opus).
rem
rem  Idempotent : chaque etape deja satisfaite est sautee. Ne lance pas le build.
rem  Tout est installe sous le dossier deps (defaut : <lecteur>:\Veyra-deps),
rem  sauf les outils systeme (winget) et third_party_local\ du depot (gitignore).
rem
rem  Usage : install-deps.bat [/deps <dossier>] [/dry-run] [/verbose]
rem                          [/nocolor] [/nopause]
rem ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion
set "OLDCP="
for /f "tokens=2 delims=:." %%c in ('chcp') do set /a "OLDCP=%%c" >nul 2>&1
chcp 65001 >nul

rem ---- Versions epinglees (alignees sur la CI) --------------------------------
set "QT_VERSION=6.8.3"
set "QT_ARCH=win64_msvc2022_64"
set "CARGO_C_VERSION=0.10.26"
set "RUST_MIN=1.88"
set "ORT_VERSION=1.24.4"
set "DML_VERSION=1.15.4"
set "VCPKG_COMMIT=30ef65cad98f08e7197c9a1656fbd871bcb72f2d"
set "MIN_FREE_GB=25"

rem ---- Chemins ----------------------------------------------------------------
set "REPO=%~dp0"
set "REPO=%REPO:~0,-1%"
set "DEPS=%~d0\Veyra-deps"
if defined VEYRA_DEPS_ROOT set "DEPS=%VEYRA_DEPS_ROOT%"
set "VERBOSE=0"
set "DRYRUN=0"
set "NOCOLOR=0"
set "NOPAUSE=0"

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="/verbose" (set "VERBOSE=1" & shift & goto :parse_args)
if /i "%~1"=="/dry-run" (set "DRYRUN=1" & shift & goto :parse_args)
if /i "%~1"=="/nocolor" (set "NOCOLOR=1" & shift & goto :parse_args)
if /i "%~1"=="/nopause" (set "NOPAUSE=1" & shift & goto :parse_args)
if /i "%~1"=="/deps" (
    if "%~2"=="" (echo /deps attend un dossier. & exit /b 2)
    set "DEPS=%~f2"
    shift & shift & goto :parse_args
)
if "%~1"=="/?" (call :usage & goto :quit_ok)
if /i "%~1"=="/help" (call :usage & goto :quit_ok)
echo Option inconnue : %~1
goto :usage_error

:args_done
if "%DEPS:~-1%"=="\" set "DEPS=%DEPS:~0,-1%"
if not "%DEPS%"=="%DEPS: =%" (
    echo Le dossier deps "%DEPS%" contient un espace : la compilation de FFmpeg le refuse.
    echo Choisissez un autre dossier avec /deps, par exemple /deps D:\Veyra-deps
    goto :end_error
)
if not exist "%REPO%\CMakePresets.json" (
    echo install-deps.bat doit rester a la racine du depot Veyra.
    goto :end_error
)

set "QT_DIR=%DEPS%\qt\%QT_VERSION%\msvc2022_64"
if defined VEYRA_QT_DIR set "QT_DIR=%VEYRA_QT_DIR%"
set "SDKSRC=%DEPS%\sdk-src"
set "VCPKG=%DEPS%\vcpkg"
set "DOVI=%DEPS%\dovi"
set "ORT_ROOT=%DEPS%\ort-dml"
set "DL=%DEPS%\downloads"
set "TPL=%REPO%\third_party_local"

set "PS=powershell.exe"
where pwsh.exe >nul 2>&1 && set "PS=pwsh.exe"
set "PSRUN=%PS% -NoLogo -NoProfile -ExecutionPolicy Bypass"

rem La baseline vcpkg suit vcpkg.json quand il est lisible.
for /f "usebackq delims=" %%B in (`%PSRUN% -Command "(Get-Content -Raw -LiteralPath '%REPO%\vcpkg.json' | ConvertFrom-Json).'builtin-baseline'" 2^>nul`) do set "VCPKG_COMMIT=%%B"

for /f "usebackq delims=" %%T in (`%PSRUN% -Command "Get-Date -Format yyyyMMdd-HHmmss"`) do set "STAMP=%%T"
set "LOGDIR=%DEPS%\logs\install-%STAMP%"
if not exist "%LOGDIR%" mkdir "%LOGDIR%" || (echo Impossible de creer %LOGDIR% & goto :end_error)

rem ---- Couleurs ANSI et barre -------------------------------------------------
set "C_0=" & set "C_G=" & set "C_R=" & set "C_C=" & set "C_D=" & set "C_B=" & set "C_Y="
if "%NOCOLOR%"=="0" (
    for /f %%e in ('echo prompt $E^| cmd') do set "ESC=%%e"
    set "C_0=!ESC![0m" & set "C_G=!ESC![92m" & set "C_R=!ESC![91m" & set "C_C=!ESC![96m"
    set "C_D=!ESC![90m" & set "C_B=!ESC![1m" & set "C_Y=!ESC![93m"
)
set "BARW=40"
set "BARFULL=████████████████████████████████████████"
set "BAREMPTY=░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░"

rem ---- Etapes : fonction, poids (duree relative), libelle --------------------
set "STEP_COUNT=0"
set "TOTALW=0"
set "DONEW=0"
call :def prereq     1  "Prérequis : winget, dossier, espace disque"
call :def git        2  "Git"
call :def python     2  "Python 3.11"
call :def buildtools 2  "CMake + Ninja"
call :def vs        15  "Visual Studio 2022 Build Tools : C++, Windows SDK, anglais"
call :def rust       4  "Rust stable ≥ %RUST_MIN% via rustup"
call :def cargoc     6  "cargo-c %CARGO_C_VERSION%"
call :def sdks       5  "SDK épinglés : DLSS, XeSS, FidelityFX, AMF, NVENC, NVOF, dovi_tool"
call :def qt         8  "Qt %QT_VERSION% msvc2022_64 via aqtinstall"
call :def ort        2  "ONNX Runtime DirectML %ORT_VERSION% + DirectML %DML_VERSION% (RIFE)"
call :def dovi       4  "libdovi 3.4.0 compilé depuis les sources"
call :def vcpkg      3  "vcpkg à la baseline + port FFmpeg patché"
call :def ffmpeg    40  "FFmpeg 9.0.1 patché, dav1d, zlib, libass, OpenSSL (30 à 90 min)"
call :def xbox      10  "Streaming Xbox : libdatachannel, nlohmann-json, Opus, SDL3 (10 à 30 min)"
call :def envfiles   1  "Fichiers d'environnement et de configuration CMake"

set "CUR=Démarrage"
set "T_START=%TIME:~0,8%"
for /l %%i in (1,1,%STEP_COUNT%) do (
    call :run_step %%i
    if errorlevel 1 goto :failed
)

set "DONEW=%TOTALW%"
set "CUR=Terminé"
call :draw
if "%DRYRUN%"=="1" (
    echo   %C_Y%Simulation terminée : rien n'a été installé ni téléchargé.%C_0%
) else (
    echo   %C_G%%C_B%Toutes les dépendances sont prêtes.%C_0%  Début %T_START%, fin %TIME:~0,8%
    echo.
    echo   Configurer :  "%DEPS%\veyra-configure.cmd"
    echo   Compiler   :  cmake --build "%REPO%\out\build\x64-release" --parallel 2 --target veyra_qml_ui
    echo   Variables  :  "%DEPS%\veyra-deps-env.cmd"
)
echo   Journaux   :  %LOGDIR%
echo.
title [100%%] Veyra - dépendances prêtes
if "%NOPAUSE%"=="0" pause
goto :quit_ok

:failed
call :draw
echo   %C_R%%C_B%Échec à l'étape : !CUR!%C_0%
if exist "%STEP_LOG%" (
    echo   Dernières lignes de %STEP_LOG% :
    echo   %C_D%------------------------------------------------------------------%C_0%
    %PSRUN% -Command "Get-Content -LiteralPath '%STEP_LOG%' -Tail 40 | ForEach-Object { '    ' + $_ }"
    echo   %C_D%------------------------------------------------------------------%C_0%
)
echo   Corrigez la cause puis relancez : les étapes déjà faites seront sautées.
echo.
title [ÉCHEC] Veyra - !CUR!
goto :end_error

:usage
echo.
echo  install-deps.bat [/deps ^<dossier^>] [/dry-run] [/verbose] [/nocolor] [/nopause]
echo.
echo    /deps     dossier d'installation, sans espace. Défaut : %~d0\Veyra-deps
echo              (ou variable VEYRA_DEPS_ROOT)
echo    /dry-run  vérifie seulement ce qui est présent ou manquant
echo    /verbose  affiche la sortie des outils au lieu de l'écrire dans les journaux
echo    /nocolor  désactive les couleurs
echo    /nopause  ne pas attendre une touche à la fin
echo.
echo  VEYRA_QT_DIR peut pointer vers un Qt %QT_VERSION% msvc2022_64 déjà installé.
echo  Des fenêtres UAC peuvent apparaître pour Git, CMake et Visual Studio.
exit /b 0

:usage_error
call :usage
:end_error
if "%NOPAUSE%"=="0" pause
if defined OLDCP chcp %OLDCP% >nul
exit /b 1

:quit_ok
if defined OLDCP chcp %OLDCP% >nul
exit /b 0

rem ===========================================================================
rem  Moteur : definition, execution et affichage des etapes
rem ===========================================================================
:def
set /a STEP_COUNT+=1
set "FN%STEP_COUNT%=%~1"
set "W%STEP_COUNT%=%~2"
set "NAME%STEP_COUNT%=%~3"
set "MARK%STEP_COUNT%=%C_D%[    ]%C_0%"
set "NOTE%STEP_COUNT%="
set /a TOTALW+=%~2
exit /b 0

:run_step
set "IDX=%~1"
set "FN=!FN%IDX%!"
set "CUR=!NAME%IDX%!"
set "MARK%IDX%=%C_C%[ >> ]%C_0%"
set "NOTE%IDX%=  %C_D%— en cours depuis %TIME:~0,8%%C_0%"
call :draw
set "N=0%IDX%"
set "STEP_LOG=%LOGDIR%\%N:~-2%-%FN%.log"
if "%VERBOSE%"=="0" echo   %C_D%Journal : %STEP_LOG%%C_0%
set "STEP_NOTE="
set "STEP_TODO="
if "%VERBOSE%"=="1" goto :run_step_live
call :step_%FN% >"%STEP_LOG%" 2>&1
goto :run_step_after
:run_step_live
call :step_%FN%
:run_step_after
set "RC=%errorlevel%"
if not "%RC%"=="0" (
    set "MARK%IDX%=%C_R%[FAIL]%C_0%"
    set "NOTE%IDX%=  %C_R%— code %RC%%C_0%"
    exit /b 1
)
set /a DONEW+=W%IDX%
set "MARK%IDX%=%C_G%[ OK ]%C_0%"
if defined STEP_TODO set "MARK%IDX%=%C_Y%[ ?? ]%C_0%"
set "NOTE%IDX%="
if defined STEP_NOTE set "NOTE%IDX%=  %C_D%— !STEP_NOTE!%C_0%"
exit /b 0

:draw
set /a PCT=DONEW*100/TOTALW
set /a FILL=DONEW*BARW/TOTALW
set /a EMPTY=BARW-FILL
set "BAR=!BARFULL:~0,%FILL%!!BAREMPTY:~0,%EMPTY%!"
if "%VERBOSE%"=="0" cls
echo.
echo   %C_B%Veyra — installation autonome des dépendances%C_0%
if "%DRYRUN%"=="1" echo   %C_Y%Mode simulation : aucune installation%C_0%
echo   %C_D%Dépôt : %REPO%%C_0%
echo   %C_D%Deps  : %DEPS%%C_0%
echo.
echo   %C_G%!BAR!%C_0%  %C_B%!PCT!%%%C_0%
echo.
for /l %%i in (1,1,%STEP_COUNT%) do echo   !MARK%%i!  !NAME%%i!!NOTE%%i!
echo.
title [!PCT!%%] Veyra deps - !CUR!
exit /b 0

rem ===========================================================================
rem  Utilitaires
rem ===========================================================================
:winget
rem %1 = identifiant winget, %2.. = arguments supplementaires (deja quotes)
echo ^> winget install --id %~1 %2 %3 %4
winget install --id %~1 -e --source winget --silent --accept-package-agreements --accept-source-agreements --disable-interactivity %2 %3 %4
set "WRC=%errorlevel%"
if "%WRC%"=="0" exit /b 0
rem 3010 / 0x8A150109 : redemarrage conseille ; 0x8A15002B / 0x8A150061 : deja installe
if "%WRC%"=="3010" exit /b 0
if "%WRC%"=="-1978334967" exit /b 0
if "%WRC%"=="-1978335189" exit /b 0
if "%WRC%"=="-1978335135" exit /b 0
echo winget a echoue pour %~1, code %WRC%
exit /b 1

:refresh_path
for /f "usebackq delims=" %%P in (`%PSRUN% -Command "[Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')"`) do set "PATH=%%P"
set "PATH=%PATH%;%USERPROFILE%\.cargo\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links"
exit /b 0

:find_vs
set "VSROOT="
set "VCVARS="
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" exit /b 1
pushd "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%V in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%V"
popd
if not defined VSROOT exit /b 1
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" exit /b 1
exit /b 0

:vcenv
if defined VEYRA_VCENV_DONE exit /b 0
call :find_vs || (echo Visual Studio C++ introuvable & exit /b 1)
echo ^> call "%VCVARS%"
call "%VCVARS%" >nul || exit /b 1
set "VEYRA_VCENV_DONE=1"
exit /b 0

rem ===========================================================================
rem  Etapes
rem ===========================================================================
:step_prereq
echo Depot : %REPO%
echo Deps  : %DEPS%
where winget >nul 2>&1 || (
    echo winget introuvable : installez "App Installer" depuis le Microsoft Store.
    exit /b 1
)
winget --version
set "FREE_GB="
for /f "usebackq delims=" %%F in (`%PSRUN% -Command "[math]::Floor((Get-PSDrive -Name '%DEPS:~0,1%').Free/1GB)"`) do set "FREE_GB=%%F"
echo Espace libre sur %DEPS:~0,1%: : %FREE_GB% Go, minimum %MIN_FREE_GB% Go
set "STEP_NOTE=%FREE_GB% Go libres sur %DEPS:~0,1%:"
if not defined FREE_GB exit /b 0
if %FREE_GB% LSS %MIN_FREE_GB% (
    if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=espace insuffisant : %FREE_GB% Go, %MIN_FREE_GB% Go requis" & exit /b 0)
    echo Espace insuffisant.
    exit /b 1
)
exit /b 0

:step_git
where git >nul 2>&1 && (set "STEP_NOTE=déjà présent" & git --version & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à installer" & exit /b 0)
call :winget Git.Git || exit /b 1
call :refresh_path
where git >nul 2>&1 || (echo git reste introuvable apres installation & exit /b 1)
set "STEP_NOTE=installé"
exit /b 0

:step_python
py -3.11 -c "import sys; print(sys.version)" 2>nul && (set "STEP_NOTE=déjà présent" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à installer" & exit /b 0)
call :winget Python.Python.3.11 || exit /b 1
call :refresh_path
py -3.11 -c "import sys; print(sys.version)" || (echo py -3.11 reste introuvable & exit /b 1)
set "STEP_NOTE=installé"
exit /b 0

:step_buildtools
set "MISSING="
where cmake >nul 2>&1 || set "MISSING=!MISSING! Kitware.CMake"
where ninja >nul 2>&1 || set "MISSING=!MISSING! Ninja-build.Ninja"
if not defined MISSING (set "STEP_NOTE=déjà présents" & cmake --version & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à installer :!MISSING!" & exit /b 0)
for %%p in (%MISSING%) do (
    call :winget %%p || exit /b 1
)
call :refresh_path
where cmake >nul 2>&1 || (echo cmake reste introuvable & exit /b 1)
where ninja >nul 2>&1 || (echo ninja reste introuvable & exit /b 1)
set "STEP_NOTE=installé :!MISSING!"
exit /b 0

:step_vs
call :find_vs && (set "STEP_NOTE=déjà présent : !VSROOT!" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à installer, environ 7 Go sur C:" & exit /b 0)
echo Installation de Visual Studio 2022 Build Tools, cela peut prendre 15 a 40 minutes.
call :winget Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended --addProductLang en-US" || exit /b 1
call :find_vs || (echo Build Tools installes mais vcvars64.bat introuvable & exit /b 1)
set "STEP_NOTE=installé : !VSROOT!"
exit /b 0

:step_rust
set "PATH=%PATH%;%USERPROFILE%\.cargo\bin"
where rustup >nul 2>&1 || (
    if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=rustup à installer" & exit /b 0)
    call :winget Rustlang.Rustup || exit /b 1
    call :refresh_path
)
where rustup >nul 2>&1 || (echo rustup reste introuvable & exit /b 1)
set "RUSTV="
for /f "tokens=2" %%v in ('rustc +stable --version 2^>nul') do set "RUSTV=%%v"
echo rustc stable actuel : %RUSTV%
%PSRUN% -Command "try { if ([version]'%RUSTV%' -ge [version]'%RUST_MIN%') { exit 0 } } catch {}; exit 1"
if not errorlevel 1 (set "STEP_NOTE=rustc %RUSTV%" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=toolchain stable à installer ou mettre à jour" & exit /b 0)
rustup toolchain install stable --profile minimal --no-self-update || exit /b 1
set "RUSTV="
for /f "tokens=2" %%v in ('rustc +stable --version 2^>nul') do set "RUSTV=%%v"
%PSRUN% -Command "try { if ([version]'%RUSTV%' -ge [version]'%RUST_MIN%') { exit 0 } } catch {}; exit 1" || (echo rustc %RUSTV% trop ancien & exit /b 1)
set "STEP_NOTE=rustc %RUSTV%"
exit /b 0

:step_cargoc
set "PATH=%PATH%;%USERPROFILE%\.cargo\bin"
where cargo-cinstall >nul 2>&1 && (set "STEP_NOTE=déjà présent" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à compiler" & exit /b 0)
call :vcenv || exit /b 1
cargo +stable install cargo-c --version %CARGO_C_VERSION% --locked || exit /b 1
set "STEP_NOTE=installé"
exit /b 0

:step_sdks
set "SDK_OK=1"
if not exist "%TPL%\nvidia\DLSS_SDK_310.7.0\include\nvsdk_ngx.h" set "SDK_OK=0"
if not exist "%TPL%\intel\xess-3.0.2\inc\" set "SDK_OK=0"
if not exist "%TPL%\amd\FidelityFX-SDK-2.3.0\Kits\FidelityFX\api\include\" set "SDK_OK=0"
if not exist "%TPL%\amd\AMF-SDK-1.5.3\include\" set "SDK_OK=0"
if not exist "%TPL%\nvidia\nv-codec-headers-13.0\include\ffnvcodec\nvEncodeAPI.h" set "SDK_OK=0"
if not exist "%TPL%\nvidia\Optical_Flow_SDK_5.0.7\NvOFInterface\nvOpticalFlowCommon.h" set "SDK_OK=0"
if not exist "%SDKSRC%\dovi_tool\dolby_vision\Cargo.toml" set "SDK_OK=0"
if "%SDK_OK%"=="1" (set "STEP_NOTE=déjà présents" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à télécharger" & exit /b 0)
rem fetch-sdks.ps1 ecrit DOVI_SOURCE dans $env:GITHUB_ENV : fichier local au lieu de la CI.
set "GITHUB_ENV=%LOGDIR%\fetch-sdks.env"
echo ^> %PS% -File scripts\ci\fetch-sdks.ps1
%PSRUN% -File "%REPO%\scripts\ci\fetch-sdks.ps1" -VeyraRoot "%REPO%" -WorkRoot "%SDKSRC%" -ManifestOut "%DEPS%\sdk-manifest.json" || exit /b 1
set "GITHUB_ENV="
if not exist "%SDKSRC%\dovi_tool\dolby_vision\Cargo.toml" (echo source dovi_tool absente & exit /b 1)
set "STEP_NOTE=téléchargés et vérifiés"
exit /b 0

:step_qt
if exist "%QT_DIR%\lib\cmake\Qt6\Qt6Config.cmake" (set "STEP_NOTE=déjà présent" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à télécharger, environ 3 Go" & exit /b 0)
set "VENV=%DEPS%\tools\aqt-venv"
if not exist "%VENV%\Scripts\python.exe" (
    py -3.11 -m venv "%VENV%" || exit /b 1
)
"%VENV%\Scripts\python.exe" -m pip install --disable-pip-version-check --upgrade "aqtinstall>=3.2,<4" || exit /b 1
rem aqt ecrit aqtinstall.log dans le dossier courant : on le garde dans les journaux.
pushd "%LOGDIR%" || exit /b 1
"%VENV%\Scripts\python.exe" -m aqt install-qt windows desktop %QT_VERSION% %QT_ARCH% --outputdir "%DEPS%\qt"
set "RC_AQT=%errorlevel%"
popd
if not "%RC_AQT%"=="0" exit /b 1
if not exist "%QT_DIR%\lib\cmake\Qt6\Qt6Config.cmake" (echo Qt6Config.cmake absent de %QT_DIR% & exit /b 1)
set "STEP_NOTE=installé"
exit /b 0

:step_ort
if exist "%ORT_ROOT%\ort\build\native\include\dml_provider_factory.h" if exist "%ORT_ROOT%\dml\include\DirectML.h" (set "STEP_NOTE=déjà présents" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à télécharger depuis nuget.org" & exit /b 0)
if not exist "%DL%" mkdir "%DL%"
call :nuget Microsoft.ML.OnnxRuntime.DirectML %ORT_VERSION% ort || exit /b 1
call :nuget Microsoft.AI.DirectML %DML_VERSION% dml || exit /b 1
if not exist "%ORT_ROOT%\ort\build\native\include\dml_provider_factory.h" (echo en-tete ONNX Runtime absent & exit /b 1)
if not exist "%ORT_ROOT%\dml\include\DirectML.h" (echo en-tete DirectML absent & exit /b 1)
set "STEP_NOTE=installés"
exit /b 0

:nuget
rem %1 = paquet, %2 = version, %3 = sous-dossier de ORT_ROOT
set "PKG_FILE=%DL%\%~1.%~2.nupkg"
echo ^> https://www.nuget.org/api/v2/package/%~1/%~2
"%SystemRoot%\System32\curl.exe" -fL -sS --retry 3 -o "%PKG_FILE%" "https://www.nuget.org/api/v2/package/%~1/%~2" || exit /b 1
if exist "%ORT_ROOT%\%~3" rd /s /q "%ORT_ROOT%\%~3"
mkdir "%ORT_ROOT%\%~3" || exit /b 1
"%SystemRoot%\System32\tar.exe" -xf "%PKG_FILE%" -C "%ORT_ROOT%\%~3" || exit /b 1
exit /b 0

:step_dovi
if exist "%DOVI%\include\libdovi\rpu_parser.h" if exist "%DOVI%\lib\dovi.lib" (set "STEP_NOTE=déjà présent" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à compiler" & exit /b 0)
set "PATH=%PATH%;%USERPROFILE%\.cargo\bin"
call :vcenv || exit /b 1
pushd "%SDKSRC%\dovi_tool\dolby_vision" || exit /b 1
cargo +stable cinstall --release --locked --prefix "%DOVI%" --library-type cdylib
set "RC_DOVI=%errorlevel%"
popd
if not "%RC_DOVI%"=="0" exit /b 1
rem find_library() cherche dovi.lib ; cargo-c nomme la bibliotheque d'import dovi.dll.lib.
if not exist "%DOVI%\lib\dovi.lib" if exist "%DOVI%\lib\dovi.dll.lib" copy /y "%DOVI%\lib\dovi.dll.lib" "%DOVI%\lib\dovi.lib" >nul
if not exist "%DOVI%\include\libdovi\rpu_parser.h" (echo en-tete libdovi absent & exit /b 1)
if not exist "%DOVI%\lib\dovi.lib" (echo dovi.lib absent & exit /b 1)
set "STEP_NOTE=compilé"
exit /b 0

:step_vcpkg
set "VCPKG_HEAD="
if exist "%VCPKG%\.git" for /f %%H in ('git -C "%VCPKG%" rev-parse HEAD 2^>nul') do set "VCPKG_HEAD=%%H"
set "VCPKG_READY=0"
if /i "%VCPKG_HEAD%"=="%VCPKG_COMMIT%" if exist "%VCPKG%\vcpkg.exe" set "VCPKG_READY=1"
if "%DRYRUN%"=="1" (
    if "%VCPKG_READY%"=="1" (set "STEP_NOTE=déjà au commit %VCPKG_COMMIT:~0,10%") else (set "STEP_TODO=1" & set "STEP_NOTE=à cloner au commit %VCPKG_COMMIT:~0,10%")
    exit /b 0
)
if "%VCPKG_READY%"=="1" goto :vcpkg_overlay
if exist "%VCPKG%\" if not exist "%VCPKG%\.git" (
    echo %VCPKG% existe mais n'est pas un depot git : deplacez-le ou utilisez /deps.
    exit /b 1
)
if not exist "%VCPKG%\.git" (
    git init -q "%VCPKG%" || exit /b 1
    git -C "%VCPKG%" remote add origin https://github.com/microsoft/vcpkg.git || exit /b 1
)
git -C "%VCPKG%" fetch -q --depth 1 origin %VCPKG_COMMIT% || exit /b 1
git -C "%VCPKG%" -c advice.detachedHead=false checkout -q FETCH_HEAD || exit /b 1
call "%VCPKG%\bootstrap-vcpkg.bat" -disableMetrics || exit /b 1
:vcpkg_overlay
py -3.11 -I "%REPO%\scripts\ci\make-ffmpeg-overlay.py" "%VCPKG%" "%REPO%" "%DEPS%\vcpkg-overlay" || exit /b 1
set "STEP_NOTE=commit %VCPKG_COMMIT:~0,10%, port FFmpeg patché"
exit /b 0

:step_ffmpeg
set "FF_OK=1"
if not exist "%VCPKG%\installed\x64-windows\include\libavfilter\avfilter.h" set "FF_OK=0"
if not exist "%VCPKG%\installed\x64-windows\bin\dav1d.dll" set "FF_OK=0"
if not exist "%VCPKG%\installed\x64-windows\include\openssl\evp.h" set "FF_OK=0"
if not exist "%VCPKG%\installed\x64-windows-static\include\ass\ass.h" set "FF_OK=0"
if "%FF_OK%"=="1" (set "STEP_NOTE=déjà présents" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à compiler" & exit /b 0)
if not exist "%DEPS%\vcpkg-cache" mkdir "%DEPS%\vcpkg-cache"
set "VCPKG_BINARY_SOURCES=clear;files,%DEPS%\vcpkg-cache,readwrite"
set "VCPKG_DISABLE_METRICS=1"
rem --classic : sinon vcpkg.json a la racine du depot forcerait le mode manifest.
"%VCPKG%\vcpkg.exe" install --classic --overlay-ports="%DEPS%\vcpkg-overlay" --clean-after-build "ffmpeg[core,avcodec,avformat,avfilter,swresample,swscale,dav1d,zlib]:x64-windows" "libass:x64-windows-static" "openssl:x64-windows" || exit /b 1
"%VCPKG%\vcpkg.exe" list --classic > "%DEPS%\vcpkg-list.txt"
type "%DEPS%\vcpkg-list.txt"
set "STEP_NOTE=compilés"
exit /b 0

:step_xbox
rem Versions et correctif libjuice : scripts\xbox\dependency-lock.json.
set "XB=%VCPKG%\installed\x64-windows-static\share"
set "XB_OK=1"
if not exist "%XB%\libdatachannel\LibDataChannelConfig.cmake" set "XB_OK=0"
if not exist "%XB%\nlohmann_json\nlohmann_jsonConfig.cmake" set "XB_OK=0"
if not exist "%XB%\opus\OpusConfig.cmake" set "XB_OK=0"
if not exist "%XB%\sdl3\SDL3Config.cmake" set "XB_OK=0"
if "%XB_OK%"=="1" (set "STEP_NOTE=déjà présents" & exit /b 0)
if "%DRYRUN%"=="1" (set "STEP_TODO=1" & set "STEP_NOTE=à compiler" & exit /b 0)
if not exist "%DEPS%\vcpkg-cache" mkdir "%DEPS%\vcpkg-cache"
set "VCPKG_BINARY_SOURCES=clear;files,%DEPS%\vcpkg-cache,readwrite"
set "VCPKG_DISABLE_METRICS=1"
"%VCPKG%\vcpkg.exe" install --classic --overlay-ports="%REPO%\scripts\xbox\vcpkg-overlay" --clean-after-build "libdatachannel[srtp,ws]:x64-windows-static" "nlohmann-json:x64-windows-static" "opus:x64-windows-static" "sdl3:x64-windows-static" || exit /b 1
if not exist "%XB%\libdatachannel\LibDataChannelConfig.cmake" (echo LibDataChannelConfig.cmake absent & exit /b 1)
"%VCPKG%\vcpkg.exe" list --classic > "%DEPS%\vcpkg-list.txt"
set "STEP_NOTE=compilés"
exit /b 0

:step_envfiles
if "%DRYRUN%"=="1" (set "STEP_NOTE=sauté en simulation" & exit /b 0)
call :find_vs || (echo Visual Studio C++ introuvable & exit /b 1)
set "BUILD_DIR=%REPO%\out\build\x64-release"
set "F_DLSS=%TPL:\=/%/nvidia/DLSS_SDK_310.7.0"
set "F_FF=%VCPKG:\=/%/installed/x64-windows"
set "F_ASS=%VCPKG:\=/%/installed/x64-windows-static"
set "F_DOVI=%DOVI:\=/%"
set "F_ORT=%ORT_ROOT:\=/%"
set "F_QT=%QT_DIR:\=/%"
rem Xbox active seulement si ses bibliotheques sont la (CMake les trouve via CMAKE_PREFIX_PATH).
set "F_XBOX="
set "F_PREFIX=%F_QT%"
if exist "%VCPKG%\installed\x64-windows-static\share\libdatachannel\LibDataChannelConfig.cmake" (
    set "F_XBOX=-DVEYRA_ENABLE_XBOX=ON"
    set "F_PREFIX=%F_QT%;%F_ASS%"
)

set "F=%DEPS%\veyra-deps-env.cmd"
> "%F%" echo @echo off
>>"%F%" echo rem Genere par install-deps.bat le %STAMP%
>>"%F%" echo set "VEYRA_DEPS_ROOT=%DEPS%"
>>"%F%" echo set "VEYRA_QT_DIR=%QT_DIR%"
>>"%F%" echo set "VEYRA_DLSS_SDK_ROOT=%F_DLSS%"
>>"%F%" echo set "VEYRA_FFMPEG_ROOT=%F_FF%"
>>"%F%" echo set "VEYRA_LIBASS_ROOT=%F_ASS%"
>>"%F%" echo set "VEYRA_LIBDOVI_ROOT=%F_DOVI%"
>>"%F%" echo set "VEYRA_ORT_DML_ROOT=%F_ORT%"
>>"%F%" echo set "OPENSSL_ROOT_DIR=%F_FF%"
>>"%F%" echo set "VEYRA_VCVARS=%VCVARS%"
>>"%F%" echo set "PATH=%QT_DIR%\bin;%%PATH%%"

set "F=%DEPS%\veyra-configure.cmd"
> "%F%" echo @echo off
>>"%F%" echo rem Genere par install-deps.bat le %STAMP% : configure Veyra comme la CI Windows.
>>"%F%" echo rem Arguments supplementaires transmis a cmake, par exemple -DVEYRA_ENABLE_REMOTEPLAY=ON
>>"%F%" echo setlocal
>>"%F%" echo call "%VCVARS%" ^>nul ^|^| exit /b 1
>>"%F%" echo cd /d "%REPO%" ^|^| exit /b 1
>>"%F%" echo cmake --preset x64-release -B "%BUILD_DIR%" -DVEYRA_BUILD_QML_UI=ON -DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON "-DVEYRA_DLSS_SDK_ROOT=%F_DLSS%" "-DVEYRA_FFMPEG_ROOT=%F_FF%" "-DVEYRA_LIBASS_ROOT=%F_ASS%" "-DVEYRA_LIBDOVI_ROOT=%F_DOVI%" "-DVEYRA_ORT_DML_ROOT=%F_ORT%" "-DOPENSSL_ROOT_DIR=%F_FF%" "-DCMAKE_PREFIX_PATH=%F_PREFIX%" %F_XBOX% %%*
>>"%F%" echo if errorlevel 1 exit /b 1
>>"%F%" echo echo.
>>"%F%" echo echo Configuration OK. Compiler avec :
>>"%F%" echo echo   cmake --build "%BUILD_DIR%" --parallel 2 --target veyra_qml_ui veyra_virtualcam_components
type "%DEPS%\veyra-deps-env.cmd"
type "%DEPS%\veyra-configure.cmd"
set "STEP_NOTE=veyra-deps-env.cmd, veyra-configure.cmd"
if defined F_XBOX set "STEP_NOTE=!STEP_NOTE!, Xbox activé"
exit /b 0
