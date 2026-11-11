@echo off
setlocal
set TCC_VERSION=0.9.27
set TCC_ZIP=tcc-%TCC_VERSION%-win64-bin.zip

REM Skip the download if tcc is already on PATH (CI runners are fresh, local boxes aren't).
where tcc.exe >nul 2>&1 && goto :build

REM Try mirrors in order; first success wins. Keep in sync with .github/workflows/ci.yml.
set "URLS=http://download-mirror.savannah.gnu.org/releases/tinycc/%TCC_ZIP% https://download.savannah.gnu.org/releases/tinycc/%TCC_ZIP% https://download.savannah.nongnu.org/releases/tinycc/%TCC_ZIP%"
set ok=
for %%u in (%URLS%) do (
  if not defined ok (
    echo trying %%u
    curl -fL --retry 3 --retry-all-errors -o tcc.zip "%%u" && set ok=1
  )
)
if not defined ok (
  echo could not download tcc & exit /b 1
)

REM Extract: tar is built into Win10 1803+ and handles zip; fall back to PowerShell.
tar -xf tcc.zip 2>nul || powershell -NoProfile -Command "Expand-Archive -Force tcc.zip ."
del tcc.zip

tcc\tcc.exe -v || exit /b 1
set PATH=%CD%\tcc;%PATH%

:build
tcc.exe -o craft.exe ^
  src\main.c src\util.c src\vars.c src\lex.c src\expand.c ^
  src\parse.c src\rules.c src\build.c src\shell.c

echo.
echo Tip: add %CD% to your PATH so you can run "craft" from anywhere.
