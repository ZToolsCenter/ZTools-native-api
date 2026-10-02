@echo off
rem Run the offline selftest for the translate line-clustering algorithm:
rem compiles the pure algorithm unit together with the selftest into a
rem standalone executable with MSVC and runs it (no node / node-gyp needed).
rem Usage: scripts\run-translate-cluster-selftest.cmd
setlocal
cd /d "%~dp0.."

call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [selftest] vcvars64 not found, MSVC 2022 Community required & exit /b 1
)
if not exist build mkdir build

rem /Fo:build\ 让 cl 的中间 .obj 落在 build/（已 gitignore），避免污染仓库根目录
cl /nologo /EHsc /std:c++17 /W4 /utf-8 ^
  /I src\screenshot\algo ^
  test\translate-cluster-selftest.cpp src\screenshot\algo\translate_cluster.cpp ^
  /Fe:build\translate-cluster-selftest.exe /Fo:build\
if errorlevel 1 exit /b 1

build\translate-cluster-selftest.exe
