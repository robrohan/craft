@echo off
REM Bootstrap craft on Windows with tcc (no Cygwin/MSYS needed).
tcc -o craft.exe ^
  src\main.c src\util.c src\vars.c src\lex.c src\expand.c ^
  src\parse.c src\rules.c src\build.c src\shell.c
