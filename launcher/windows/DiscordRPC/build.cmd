@echo off
setlocal

if not exist json.hpp (
    echo ERROR: json.hpp is missing.
    echo Download it from https://github.com/nlohmann/json/releases/latest/download/json.hpp
    echo and place it in this folder.
    exit /b 1
)

where cl >nul 2>nul
if not errorlevel 1 (
    echo Building with MSVC...
    cl /nologo /std:c++17 /O2 /EHsc /MT /utf-8 /DUNICODE /D_UNICODE ^
        main.cpp /Fe:BloodborneRPC.exe ^
        /link /SUBSYSTEM:WINDOWS kernel32.lib user32.lib
    if errorlevel 1 exit /b 1
    echo Built: BloodborneRPC.exe
    exit /b 0
)

where clang++ >nul 2>nul
if not errorlevel 1 (
    echo Building with Clang...
    clang++ -std=c++17 -O2 -DUNICODE -D_UNICODE -mwindows main.cpp -o BloodborneRPC.exe -static -lkernel32 -luser32
    if errorlevel 1 exit /b 1
    echo Built: BloodborneRPC.exe
    exit /b 0
)

echo No C++ compiler found. Install Visual Studio Build Tools or LLVM.
exit /b 1