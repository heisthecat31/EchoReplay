@echo off
rem Builds EchoReplayViewer into build\. Needs Visual Studio (MSVC), CMake and Ninja, EchoConstruct beside this repo
rem (built: its build\ folder), and this repo built (its build\ has bin\echoreplay_reader.dll).
rem   build.bat            the viewer
rem   build.bat deps       first rebuild the EchoConstruct libraries and EchoReplay's reader it links (long)
rem Set VCVARS to your vcvars64.bat if Visual Studio is not found (e.g. set VCVARS=J:\vs2026\VC\Auxiliary\Build\vcvars64.bat).
setlocal
if "%VCVARS%"=="" (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2^>nul`) do set VSDIR=%%i
)
if "%VCVARS%"=="" if exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" set VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat
if "%VCVARS%"=="" if exist "J:\vs2026\VC\Auxiliary\Build\vcvars64.bat" set VCVARS=J:\vs2026\VC\Auxiliary\Build\vcvars64.bat
if "%VCVARS%"=="" ( echo Visual Studio not found: set VCVARS & exit /b 1 )
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if /i "%1"=="deps" (
  cmake --build ..\..\EchoConstruct\build --target shell-ui renderer imgui imguizmo imgui_node_editor baker_fingerprint echo_xatlas echo_meshoptimizer echo_directxtex echo_zydis echo_zstd_decompress echo_miniz || exit /b 1
  cmake --build ..\build --target echoreplay_reader echodemo || exit /b 1
)
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build build --target EchoReplayViewer || exit /b 1
echo Built build\EchoReplayViewer.exe
