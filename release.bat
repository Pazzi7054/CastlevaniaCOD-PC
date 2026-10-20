@echo off
setlocal
rem A release a player can download: Castlevania.exe and the launcher without the game code, and the compiler folder
rem the first start builds CastlevaniaGame.dll with, from the player's own copy (src\common\setup.cpp).
rem   release.bat [version]      writes out\release\CastlevaniaCOD-PC-<version>.zip
rem Needs what build.bat needs, Python, and the game in assets (the build reads it; nothing of it is packed).
cd /d "%~dp0"
set "VERSION=%~1"
if not defined VERSION set "VERSION=dev"
set "PKG=out\release\CastlevaniaCOD-PC"
set "GLUE=out\split-glue"
set "SPLIT=out\build\split"

echo [1/6] Whole build
set CI=1
call "%~dp0build.bat" > release_build_log.txt 2>&1 || (echo       build.bat failed, see release_build_log.txt & goto :fail)

echo [2/6] Toolchain
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" if exist "D:\Microsoft Visual Studio\Installer\vswhere.exe" set "VSWHERE=D:\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "LLVM=%ProgramFiles%\LLVM\bin"
set "PATH=%LLVM%;%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRT=%%c"
for %%t in ("%LLVM%\clang.exe" "%LLVM%\lld-link.exe" "%LLVM%\llvm-dlltool.exe" "%VCRT%\vcruntime140.dll") do if not exist %%t (echo       missing %%~t & goto :fail)
where python >nul 2>nul || (echo       Python is needed for split\glue.py & goto :fail)
rem the SDK the whole build used
set /p SDK=<out\build\release\sdk_dir.txt

echo [3/6] The split between the exe and the game DLL
python split\glue.py out\build\release generated\game %GLUE% || goto :fail
clang++ --target=x86_64-pc-windows-msvc -nostdinc -isystem split\include -I"%SDK%\runtime" -std=gnu++20 -march=x86-64-v2 -O2 -fno-strict-aliasing -c %GLUE%\dll_glue.cpp -o %GLUE%\glue.obj || goto :fail
llvm-dlltool -m i386:x86-64 -d split\crt\vcruntime140.def -l %GLUE%\vcruntime140.lib || goto :fail
llvm-dlltool -m i386:x86-64 -d split\crt\ucrtbase.def -l %GLUE%\ucrtbase.lib || goto :fail

echo [4/6] Castlevania.exe without the game code
cmake --preset split >nul || goto :fail
cmake --build %SPLIT% --target Castlevania CastlevaniaLauncher xbe_recomp || goto :fail

echo [5/6] Package
if exist out\release rd /s /q out\release
mkdir "%PKG%\assets" "%PKG%\compiler\include\cpu" "%PKG%\compiler\xdk\5849"
for %%f in (Castlevania.exe CastlevaniaLauncher.exe) do copy /y "%SPLIT%\%%f" "%PKG%\" >nul
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do (
    copy /y "%VCRT%\%%f.dll" "%PKG%\" >nul
    copy /y "%VCRT%\%%f.dll" "%PKG%\compiler\" >nul
)
copy /y "%SPLIT%\xbe_recomp.exe" "%PKG%\compiler\" >nul
copy /y "%LLVM%\clang.exe" "%PKG%\compiler\" >nul
copy /y "%LLVM%\lld-link.exe" "%PKG%\compiler\" >nul
copy /y split\include\* "%PKG%\compiler\include\" >nul
copy /y "%SDK%\runtime\cpu\x86_ops.h" "%PKG%\compiler\include\cpu\" >nul
copy /y "%SDK%\runtime\cpu\x86_context.h" "%PKG%\compiler\include\cpu\" >nul
copy /y "%SDK%\xdk\5849\layout.toml" "%PKG%\compiler\xdk\5849\" >nul
copy /y config\game.toml "%PKG%\compiler\" >nul
for %%f in (glue.obj game.def vcruntime140.lib ucrtbase.lib) do copy /y "%GLUE%\%%f" "%PKG%\compiler\" >nul
copy /y split\README-release.txt "%PKG%\README.txt" >nul
copy /y LICENSE "%PKG%\LICENSE.txt" >nul
copy /y split\LLVM-LICENSE.txt "%PKG%\compiler\LLVM-LICENSE.txt" >nul
copy /y "%SDK%\THIRD_PARTY.txt" "%PKG%\THIRD_PARTY.txt" >nul
echo Put your game here: its disc image (.iso), or the extracted disc (default.xbe with its folders).> "%PKG%\assets\PUT YOUR GAME HERE.txt"

rem nothing of the game goes out: no generated code, no objects but the glue, no xbe or image
for /r "%PKG%" %%f in (*.xbe *.iso *.cpp x86_recomp*) do (echo       game file in the package: %%f & goto :fail)
for /r "%PKG%" %%f in (*.obj) do if /i not "%%~nxf"=="glue.obj" (echo       object in the package: %%f & goto :fail)

echo [6/6] Zip
tar -a -cf "out\release\CastlevaniaCOD-PC-%VERSION%.zip" -C out\release CastlevaniaCOD-PC || goto :fail
for %%z in ("out\release\CastlevaniaCOD-PC-%VERSION%.zip") do echo       %%~fz  (%%~zz bytes)
exit /b 0

:fail
echo.
echo   RELEASE FAILED
exit /b 1
