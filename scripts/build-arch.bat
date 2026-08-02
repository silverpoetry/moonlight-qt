@echo off
setlocal enableDelayedExpansion

rem Run from Qt command prompt with working directory set to root of repo

set BUILD_CONFIG=%1

rem Convert to lower case for windeployqt
if /I "%BUILD_CONFIG%"=="debug" (
    set BUILD_CONFIG=debug
    set WIX_MUMS=10
) else (
    if /I "%BUILD_CONFIG%"=="release" (
        set BUILD_CONFIG=release
        set WIX_MUMS=10
    ) else (
        if /I "%BUILD_CONFIG%"=="signed-release" (
            set BUILD_CONFIG=release
            set SIGN=1
            set MUST_DEPLOY_SYMBOLS=1

            rem Fail if there are unstaged changes
            git diff-index --quiet HEAD --
            if !ERRORLEVEL! NEQ 0 (
                echo Signed release builds must not have unstaged changes!
                exit /b 1
            )

            echo Updating dependencies
            powershell %cd%\setup-deps.ps1
            if !ERRORLEVEL! NEQ 0 (
                exit /b 1
            )
        ) else (
            echo Invalid build configuration - expected 'debug' or 'release'
            echo Usage: scripts\build-arch.bat ^(release^|debug^)
            exit /b 1
        )
    )
)

rem Locate qmake and determine if we're using qmake.exe or (host-)qmake.bat.
rem The batch wrappers are used by some cross-compiled Qt distributions.
where qmake.bat >nul 2>&1
if !ERRORLEVEL! EQU 0 (
    set QMAKE_CMD=call qmake.bat
) else (
    where host-qmake.bat >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set QMAKE_CMD=call host-qmake.bat
    ) else (
        where qmake.exe >nul 2>&1
        if !ERRORLEVEL! EQU 0 (
            set QMAKE_CMD=qmake.exe
        ) else (
            echo Unable to find QMake. Did you add Qt bins to your PATH?
            goto Error
        )
    )
)

rem Read target metadata from qmake instead of guessing it from installation
rem directory names. Package managers do not necessarily encode the target
rem architecture or Qt major version in their paths.
for /F "usebackq delims=" %%i in (`%QMAKE_CMD% -query QT_INSTALL_BINS`) do set QT_PATH=%%i
for /F "usebackq delims=" %%i in (`%QMAKE_CMD% -query QT_HOST_BINS`) do set HOSTBIN_PATH=%%i
for /F "usebackq delims=" %%i in (`%QMAKE_CMD% -query QT_INSTALL_ARCHDATA`) do set QT_ARCHDATA=%%i
for /F "usebackq delims=" %%i in (`%QMAKE_CMD% -query QT_VERSION`) do set QT_VERSION=%%i

if not exist "%QT_ARCHDATA%\mkspecs\qconfig.pri" (
    echo Unable to locate Qt architecture metadata in %QT_ARCHDATA%
    goto Error
)

for /F "tokens=3" %%i in ('findstr /B /C:"QT_ARCH =" "%QT_ARCHDATA%\mkspecs\qconfig.pri"') do set QT_ARCH=%%i
for /F "tokens=1 delims=." %%i in ("%QT_VERSION%") do set QT_MAJOR_VERSION=%%i

if /I "%QT_ARCH%"=="arm64" (
    set ARCH=arm64
) else if /I "%QT_ARCH%"=="aarch64" (
    set ARCH=arm64
) else if /I "%QT_ARCH%"=="x86_64" (
    set ARCH=x64
) else if /I "%QT_ARCH%"=="amd64" (
    set ARCH=x64
) else if /I "%QT_ARCH%"=="i386" (
    set ARCH=x86
) else if /I "%QT_ARCH%"=="x86" (
    set ARCH=x86
) else (
    echo Unsupported Qt target architecture: %QT_ARCH%
    goto Error
)

echo QT_PATH=%QT_PATH%
echo QT_HOST_PATH=%HOSTBIN_PATH%
echo QT_VERSION=%QT_VERSION%
echo QT_ARCH=%QT_ARCH%

if exist "%QT_PATH%\host-qtpaths.bat" (
    echo Using windeployqt.exe from QT_HOST_PATH
    set WINDEPLOYQT_CMD="%HOSTBIN_PATH%\windeployqt.exe" --qtpaths "%QT_PATH%\host-qtpaths.bat"
) else (
    if exist "%QT_PATH%\windeployqt.exe" (
        echo Using windeployqt.exe from QT_PATH
        set WINDEPLOYQT_CMD="%QT_PATH%\windeployqt.exe"
    ) else (
        if exist "%QT_PATH%\qtpaths.bat" (
            echo Using windeployqt.exe from QT_HOST_PATH
            set WINDEPLOYQT_CMD="%HOSTBIN_PATH%\windeployqt.exe" --qtpaths "%QT_PATH%\qtpaths.bat"
        ) else (
            echo Unable to locate windeployqt for this Qt installation
            goto Error
        )
    )
)

echo Detected target architecture: %ARCH%

set SIGNTOOL_PARAMS=sign /tr http://timestamp.digicert.com /td sha256 /fd sha256 /sha1 8b9d0d682ad9459e54f05a79694bc10f9876e297 /v

set BUILD_ROOT=%cd%\build
set SOURCE_ROOT=%cd%
set BUILD_FOLDER=%BUILD_ROOT%\build-%ARCH%-%BUILD_CONFIG%
set DEPLOY_FOLDER=%BUILD_ROOT%\deploy-%ARCH%-%BUILD_CONFIG%
set INSTALLER_FOLDER=%BUILD_ROOT%\installer-%ARCH%-%BUILD_CONFIG%
set SYMBOLS_FOLDER=%BUILD_ROOT%\symbols-%ARCH%-%BUILD_CONFIG%

rem Allow CI to override the version.txt with an environment variable
if defined CI_VERSION (
    set VERSION=%CI_VERSION%
) else (
    set /p VERSION=<%SOURCE_ROOT%\app\version.txt
)

rem Use the correct VC tools for the specified architecture
if /I "%ARCH%" EQU "x64" (
    rem x64 is a special case that doesn't match %PROCESSOR_ARCHITECTURE%
    set VC_ARCH=AMD64
) else (
    set VC_ARCH=%ARCH%
)

rem If we're not building for the current platform, use the cross compiling toolchain
if /I "%VC_ARCH%" NEQ "%PROCESSOR_ARCHITECTURE%" (
    set VC_ARCH=%PROCESSOR_ARCHITECTURE%_%VC_ARCH%
)

rem Find Visual Studio and run vcvarsall.bat
set VSWHERE="%SOURCE_ROOT%\scripts\vswhere.exe"
set VS_INSTALL_PATH=
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS_INSTALL_PATH=%%i

if not defined VS_INSTALL_PATH (
    echo Unable to locate Visual Studio C++ build tools
    goto Error
)
if not exist "%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo Visual Studio installation is missing vcvarsall.bat: %VS_INSTALL_PATH%
    goto Error
)

call "%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvarsall.bat" %VC_ARCH%
if !ERRORLEVEL! NEQ 0 goto Error
where cl.exe >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo Visual Studio environment did not provide cl.exe
    goto Error
)

rem Find VC redistributable DLLs
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Redist\MSVC\*\%ARCH%\Microsoft.VC*.CRT`) do set VC_REDIST_DLL_PATH=%%i
if not defined VC_REDIST_DLL_PATH (
    echo Unable to locate the Visual C++ redistributable for %ARCH%
    goto Error
)

echo Cleaning output directories
rmdir /s /q %DEPLOY_FOLDER%
rmdir /s /q %BUILD_FOLDER%
rmdir /s /q %INSTALLER_FOLDER%
rmdir /s /q %SYMBOLS_FOLDER%
mkdir %BUILD_ROOT%
mkdir %DEPLOY_FOLDER%
mkdir %BUILD_FOLDER%
mkdir %INSTALLER_FOLDER%
mkdir %SYMBOLS_FOLDER%

rem Enable LTCG for official builds
set CFLAGS=/GL
set CXXFLAGS=/GL
set LDFLAGS=/LTCG

echo Configuring the project
pushd %BUILD_FOLDER%
%QMAKE_CMD% %SOURCE_ROOT%\moonlight-qt.pro
if !ERRORLEVEL! NEQ 0 goto Error
popd

echo Compiling Moonlight in %BUILD_CONFIG% configuration
pushd %BUILD_FOLDER%
if "%QT_MAJOR_VERSION%"=="5" (
    rem Qt 5 qmake does not emit object-level dependencies for .moc files
    rem included by source files. Serialize this legacy generator path so its
    rem aggregate moc target completes before compilation begins.
    %SOURCE_ROOT%\scripts\jom.exe /J 1 %BUILD_CONFIG%
) else (
    %SOURCE_ROOT%\scripts\jom.exe %BUILD_CONFIG%
)
if !ERRORLEVEL! NEQ 0 goto Error
popd

if /I "%ARCH%" EQU "x64" (
    echo Running protocol conformance tests
    "%BUILD_FOLDER%\tests\clipboardmanifest\%BUILD_CONFIG%\clipboard-manifest-test.exe"
    if !ERRORLEVEL! NEQ 0 goto Error
)

echo Saving PDBs
for /r "%BUILD_FOLDER%" %%f in (*.pdb) do (
    copy "%%f" %SYMBOLS_FOLDER%
    if !ERRORLEVEL! NEQ 0 goto Error
)
copy %SOURCE_ROOT%\libs\windows\lib\%ARCH%\*.pdb %SYMBOLS_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error
powershell -NoProfile -Command "Compress-Archive -Path '%SYMBOLS_FOLDER%\*.pdb' -DestinationPath '%SYMBOLS_FOLDER%\MoonlightDebuggingSymbols-%ARCH%-%VERSION%.zip' -CompressionLevel Optimal -Force"
if !ERRORLEVEL! NEQ 0 goto Error

if "%ML_SYMBOL_STORE%" NEQ "" (
    echo Publishing PDBs to symbol store: %ML_SYMBOL_STORE%
    symstore add /f %SYMBOLS_FOLDER%\*.pdb /s %ML_SYMBOL_STORE% /t Moonlight
    if !ERRORLEVEL! NEQ 0 goto Error
) else (
    if "%MUST_DEPLOY_SYMBOLS%"=="1" (
        echo "A symbol server must be specified in ML_SYMBOL_STORE for signed release builds"
        exit /b 1
    )
)

if "%ML_SYMBOL_ARCHIVE%" NEQ "" (
    echo Copying PDB ZIP to symbol archive: %ML_SYMBOL_ARCHIVE%
    copy %SYMBOLS_FOLDER%\MoonlightDebuggingSymbols-%ARCH%-%VERSION%.zip %ML_SYMBOL_ARCHIVE%
    if !ERRORLEVEL! NEQ 0 goto Error
) else (
    if "%MUST_DEPLOY_SYMBOLS%"=="1" (
        echo "A symbol archive directory must be specified in ML_SYMBOL_ARCHIVE for signed release builds"
        exit /b 1
    )
)

echo Copying DLL dependencies
copy %SOURCE_ROOT%\libs\windows\lib\%ARCH%\*.dll %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

echo Copying AntiHooking.dll
copy %BUILD_FOLDER%\AntiHooking\%BUILD_CONFIG%\AntiHooking.dll %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

echo Copying GC mapping list
copy %SOURCE_ROOT%\app\SDL_GameControllerDB\gamecontrollerdb.txt %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

if "%QT_MAJOR_VERSION%"=="5" (
    echo Copying qt.conf for Qt 5
    copy %SOURCE_ROOT%\app\qt_qt5.conf %DEPLOY_FOLDER%\qt.conf
    if !ERRORLEVEL! NEQ 0 goto Error

    rem Qt 5.15
    set WINDEPLOYQT_ARGS=--no-qmltooling --no-virtualkeyboard
) else (
    rem Qt 6.8+
    set WINDEPLOYQT_ARGS=--no-system-d3d-compiler --no-system-dxc-compiler --skip-plugin-types qmltooling,generic --no-ffmpeg
    set WINDEPLOYQT_ARGS=!WINDEPLOYQT_ARGS! --no-quickcontrols2fusion --no-quickcontrols2imagine --no-quickcontrols2universal
    set WINDEPLOYQT_ARGS=!WINDEPLOYQT_ARGS! --no-quickcontrols2fusionstyleimpl --no-quickcontrols2imaginestyleimpl --no-quickcontrols2universalstyleimpl --no-quickcontrols2windowsstyleimpl --no-quickcontrols2fluentwinui3styleimpl
)

echo Deploying Qt dependencies
%WINDEPLOYQT_CMD% --dir %DEPLOY_FOLDER% --%BUILD_CONFIG% --qmldir %SOURCE_ROOT%\app\gui --no-opengl-sw --no-compiler-runtime --no-sql %WINDEPLOYQT_ARGS% %BUILD_FOLDER%\app\%BUILD_CONFIG%\Moonlight.exe
if !ERRORLEVEL! NEQ 0 goto Error

echo Deleting unused files
rem Qt 5.x directories
if exist "%DEPLOY_FOLDER%\QtQuick\Controls.2\Fusion" rmdir /s /q "%DEPLOY_FOLDER%\QtQuick\Controls.2\Fusion"
if exist "%DEPLOY_FOLDER%\QtQuick\Controls.2\Imagine" rmdir /s /q "%DEPLOY_FOLDER%\QtQuick\Controls.2\Imagine"
if exist "%DEPLOY_FOLDER%\QtQuick\Controls.2\Universal" rmdir /s /q "%DEPLOY_FOLDER%\QtQuick\Controls.2\Universal"
rem Qt 6.8+ directories
if exist "%DEPLOY_FOLDER%\qml\QtQuick\Controls\Fusion" rmdir /s /q "%DEPLOY_FOLDER%\qml\QtQuick\Controls\Fusion"
if exist "%DEPLOY_FOLDER%\qml\QtQuick\Controls\Imagine" rmdir /s /q "%DEPLOY_FOLDER%\qml\QtQuick\Controls\Imagine"
if exist "%DEPLOY_FOLDER%\qml\QtQuick\Controls\Universal" rmdir /s /q "%DEPLOY_FOLDER%\qml\QtQuick\Controls\Universal"
if exist "%DEPLOY_FOLDER%\qml\QtQuick\Controls\Windows" rmdir /s /q "%DEPLOY_FOLDER%\qml\QtQuick\Controls\Windows"
if exist "%DEPLOY_FOLDER%\qml\QtQuick\Controls\FluentWinUI3" rmdir /s /q "%DEPLOY_FOLDER%\qml\QtQuick\Controls\FluentWinUI3"
if exist "%DEPLOY_FOLDER%\qml\QtQuick\NativeStyle" rmdir /s /q "%DEPLOY_FOLDER%\qml\QtQuick\NativeStyle"
rem icuuc.dll ships with all supported OSes (and Qt incorrectly deploys the x64 version on ARM64)
if exist "%DEPLOY_FOLDER%\icuuc.dll" del "%DEPLOY_FOLDER%\icuuc.dll"

echo Copying third-party notices
copy "%SOURCE_ROOT%\THIRD_PARTY_NOTICES.txt" "%DEPLOY_FOLDER%\THIRD_PARTY_NOTICES.txt"
if !ERRORLEVEL! NEQ 0 goto Error

if "%SIGN%"=="1" (
    echo Signing deployed binaries
    set FILES_TO_SIGN=%BUILD_FOLDER%\app\%BUILD_CONFIG%\Moonlight.exe
    for /r "%DEPLOY_FOLDER%" %%f in (*.dll *.exe) do (
        set FILES_TO_SIGN=!FILES_TO_SIGN! %%f
    )
    signtool %SIGNTOOL_PARAMS% !FILES_TO_SIGN!
    if !ERRORLEVEL! NEQ 0 goto Error
)

if "%ML_SYMBOL_STORE%" NEQ "" (
    echo Publishing binaries to symbol store: %ML_SYMBOL_STORE%
    symstore add /r /f %DEPLOY_FOLDER%\*.* /s %ML_SYMBOL_STORE% /t Moonlight
    if !ERRORLEVEL! NEQ 0 goto Error
    symstore add /r /f %BUILD_FOLDER%\app\%BUILD_CONFIG%\Moonlight.exe /s %ML_SYMBOL_STORE% /t Moonlight
    if !ERRORLEVEL! NEQ 0 goto Error
)

echo Building MSI
cmd /c "set VERSION= && msbuild -Restore %SOURCE_ROOT%\wix\Moonlight\Moonlight.wixproj /p:Configuration=%BUILD_CONFIG% /p:Platform=%ARCH% /p:MSBuildProjectExtensionsPath=%BUILD_FOLDER%\"
if !ERRORLEVEL! NEQ 0 goto Error

echo Copying application binary to deployment directory
copy %BUILD_FOLDER%\app\%BUILD_CONFIG%\Moonlight.exe %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

echo Building portable package
rem This must be done after WiX harvesting and signing, since the VCRT dlls are MS signed
rem and should not be harvested for inclusion in the full installer
copy "%VC_REDIST_DLL_PATH%\*.dll" %DEPLOY_FOLDER%
if !ERRORLEVEL! NEQ 0 goto Error

rem Since we don't publish Windows installers for CI builds, let's use the user profile
rem location of the regular non-portable version by default. We'll place a file in the
rem the package to allow the user to rename if they want portable behavior.
if defined CI_VERSION (
    echo. > %DEPLOY_FOLDER%\portable.dat.inactive
    if !ERRORLEVEL! NEQ 0 goto Error
) else (
    rem This file tells Moonlight that it's a portable installation
    echo. > %DEPLOY_FOLDER%\portable.dat
    if !ERRORLEVEL! NEQ 0 goto Error
)

powershell -NoProfile -Command "Compress-Archive -Path '%DEPLOY_FOLDER%\*' -DestinationPath '%INSTALLER_FOLDER%\MoonlightPortable-%ARCH%-%VERSION%.zip' -CompressionLevel Optimal -Force"
if !ERRORLEVEL! NEQ 0 goto Error

echo Build successful for Moonlight v%VERSION% %ARCH% binaries!
exit /b 0

:Error
echo Build failed!
exit /b !ERRORLEVEL!
