@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 failed & exit /b 1 )
cd /d "%~dp0"
cl /nologo /O2 /EHsc /W3 /std:c++17 /utf-8 /DUNICODE /D_UNICODE src\main.cpp /Fe:space.exe /Fo:main.obj /Fm:space.map
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
cl /nologo /O2 /EHsc /W3 /std:c++17 /utf-8 src\server.cpp /Fe:server.exe /Fo:server.obj
if errorlevel 1 ( echo SERVER BUILD FAILED & exit /b 1 )
echo BUILD OK
