@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /EHsc /std:c++17 /utf-8 /W1 /D_CRT_SECURE_NO_WARNINGS /I shim /I "C:\Users\Mitch\OneDrive\Projects\libraries\WebPanel" host_main.cpp "C:\Users\Mitch\OneDrive\Projects\libraries\WebPanel\WebPanel.cpp" /Fe:wphost.exe /Fo:obj\ 2>&1
if errorlevel 1 exit /b 1
wphost.exe
