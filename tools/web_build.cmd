@echo off
rem Build the WebAssembly version into build-web\ (see docs/WEB_PORT_RESEARCH.md).
rem Needs emsdk at C:\repos\emsdk (emsdk.py install/activate latest) and the
rem lifted code generated with the --mmio-range sections (CLAUDE.md). Then run
rem   .venv\Scripts\python.exe tools\serve.py   and open http://localhost:8080/
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set EMSDK_PYTHON=%~dp0..\.venv\Scripts\python.exe
set EM_CONFIG=C:\repos\emsdk\.emscripten
set PATH=C:\repos\emsdk;C:\repos\emsdk\upstream\emscripten;%PATH%
cd /d %~dp0..
%EMSDK_PYTHON% C:\repos\emsdk\upstream\emscripten\emcmake.py cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release -DRECOMP_RENDERER=gl -DRECOMP_COMPACT_MEM=ON >nul || exit /b 1
cmake --build build-web -j 8 %*
