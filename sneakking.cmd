@echo off
rem Sneak King: one command from your disc image to a playable Windows build.
rem   sneakking build path\to\disc.iso     (more: sneakking --help)
rem This part sets up Python (3.11 or newer) and the packages in
rem requirements.txt in .venv, then hands over to tools\sneakking.py.
setlocal
set "ROOT=%~dp0"
set "VENV=%ROOT%.venv\Scripts\python.exe"
if exist "%VENV%" goto deps

call :findpy
if defined PY goto mkvenv
where /q winget || goto nopy
choice /m "Python 3.12 is needed. Install it with winget now"
if errorlevel 2 goto nopy
winget install -e --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements
call :findpy
if not defined PY goto nopy

:mkvenv
echo Setting up Python in .venv ...
%PY% -m venv "%ROOT%.venv" || exit /b 1

:deps
"%VENV%" -c "import capstone, numpy" >nul 2>&1 && goto run
echo Installing Python packages ...
"%VENV%" -m pip install -q --disable-pip-version-check -r "%ROOT%requirements.txt" || exit /b 1

:run
"%VENV%" "%ROOT%tools\sneakking.py" %*
exit /b %errorlevel%

:nopy
echo ERROR: Python 3.11 or newer is needed. Get it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH"), then run this again in a new window.
exit /b 1

rem PY = a Python 3.11+ command, or undefined. The python.exe that only opens
rem the Microsoft Store fails the version test, so it is skipped.
:findpy
set PY=
py -3 -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>&1 && (set "PY=py -3" & exit /b)
python -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>&1 && (set "PY=python" & exit /b)
rem A winget install from this window is not on its PATH yet.
if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" set PY="%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
exit /b
