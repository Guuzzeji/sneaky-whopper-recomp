@echo off
rem Builds ANGLE (GL ES 3 on D3D11, Vulkan or desktop GL) for the gl renderer
rem and copies bin\, lib\ and include\ to third_party\angle\. The recipe is
rem mmozeiko/build-angle's, with the Vulkan and GL back ends turned on (its
rem prebuilt zips are D3D11 only). Needs git and VS 2022; takes about 3 GB and
rem half an hour the first time. The source goes to %ANGLE_SRC%, default
rem ..\angle next to the repo (like emsdk). ANGLE_COMMIT pins a revision.
setlocal enabledelayedexpansion

set ROOT=%~dp0..
if "%ANGLE_SRC%" equ "" set ANGLE_SRC=%ROOT%\..\angle
set OUT=%ROOT%\third_party\angle
set DEPOT=%ANGLE_SRC%\..\depot_tools

where /q git.exe || (echo ERROR: git.exe not found & exit /b 1)

set PATH=%DEPOT%;%PATH%
set DEPOT_TOOLS_WIN_TOOLCHAIN=0
if not exist "%DEPOT%" (
  call git clone --depth=1 --no-tags --single-branch https://chromium.googlesource.com/chromium/tools/depot_tools.git "%DEPOT%" || exit /b 1
)
rem The first gclient run bootstraps depot_tools' own python3.
call gclient --version >nul 2>&1

if "%ANGLE_COMMIT%" equ "" (
  for /f "tokens=1 usebackq" %%F in (`git ls-remote https://chromium.googlesource.com/angle/angle HEAD`) do set ANGLE_COMMIT=%%F
)
if not exist "%ANGLE_SRC%" (
  call git init "%ANGLE_SRC%" || exit /b 1
  call git -C "%ANGLE_SRC%" remote add origin https://chromium.googlesource.com/angle/angle || exit /b 1
)
call git -C "%ANGLE_SRC%" fetch --no-recurse-submodules origin %ANGLE_COMMIT% || exit /b 1
call git -C "%ANGLE_SRC%" reset --hard FETCH_HEAD || exit /b 1

pushd "%ANGLE_SRC%"
call python3 scripts\bootstrap.py || exit /b 1
rem Skip the big test-only dependencies.
"C:\Program Files\Git\usr\bin\sed.exe" -i.bak -e "/'third_party\/catapult'\: /,+3d" -e "/'third_party\/dawn'\: /,+3d" -e "/'third_party\/llvm\/src'\: /,+3d" -e "/'third_party\/SwiftShader'\: /,+3d" -e "/'third_party\/VK-GL-CTS\/src'\: /,+3d" DEPS || exit /b 1
call gclient sync -f -D -R || exit /b 1
rem Chromium's toolchain pins a Windows SDK (10.0.28000 in late 2026); use the
rem newest one installed instead.
for /f %%V in ('dir /b /o:n "%ProgramFiles(x86)%\Windows Kits\10\Include"') do set WINSDK=%%V
"C:\Program Files\Git\usr\bin\sed.exe" -i -e "s/^SDK_VERSION = '.*'/SDK_VERSION = '%WINSDK%'/" build\vs_toolchain.py build\toolchain\win\setup_toolchain.py || exit /b 1
rem ...and the newest Windows version that SDK's headers know.
"C:\Program Files\Git\usr\bin\sed.exe" -i -e "s/\"NTDDI_VERSION=NTDDI_WIN11_[A-Z]*\"/\"NTDDI_VERSION=WDK_NTDDI_VERSION\"/" build\config\win\BUILD.gn || exit /b 1

call gn gen out/x64 --args="target_cpu=""x64"" angle_build_all=false is_debug=false angle_has_frame_capture=false angle_enable_d3d11=true angle_enable_vulkan=true angle_enable_gl=true angle_enable_d3d9=false angle_enable_wgpu=false angle_enable_null=false angle_enable_swiftshader=false use_siso=false" || exit /b 1
"C:\Program Files\Git\usr\bin\sed.exe" -i.bak -e "s/\/MD/\/MT/" build\config\win\BUILD.gn || exit /b 1
call autoninja --offline -C out/x64 libEGL libGLESv2 || exit /b 1
popd

for %%D in (bin lib include) do if not exist "%OUT%\%%D" mkdir "%OUT%\%%D"
echo %ANGLE_COMMIT%> "%OUT%\commit.txt"
for %%F in (libEGL.dll libGLESv2.dll d3dcompiler_47.dll vulkan-1.dll) do copy /y "%ANGLE_SRC%\out\x64\%%F" "%OUT%\bin" >nul || exit /b 1
for %%F in (libEGL.dll.lib libGLESv2.dll.lib) do copy /y "%ANGLE_SRC%\out\x64\%%F" "%OUT%\lib" >nul || exit /b 1
for %%D in (KHR EGL GLES2 GLES3) do xcopy /S /I /Q /Y "%ANGLE_SRC%\include\%%D" "%OUT%\include\%%D" >nul || exit /b 1
echo ANGLE %ANGLE_COMMIT% is in %OUT%
