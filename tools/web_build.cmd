@echo off
rem Build the WebAssembly version into build-web\ with the "web" preset
rem (CMakePresets.json; docs/BUILDING.md). Needs emsdk at C:\repos\emsdk
rem (emsdk install/activate latest) and the lifted code generated with the
rem --mmio-range sections. Then run
rem   .venv\Scripts\python.exe tools\serve.py   and open http://localhost:8080/
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0..
cmake --preset web >nul || exit /b 1
cmake --build --preset web %*
