@echo off
rem wcinput: cmd の内部コマンドが、語の途中の / をどう読むかを見る (Windows と Wine で同じものを流す)
rem 使い方: slash.cmd <作業のフォルダー>   (中に t を作り直しながら試す)
setlocal
set ROOT=%~1
if "%ROOT%"=="" set ROOT=%TEMP%\wcslash
if not exist "%ROOT%" mkdir "%ROOT%"
cd /d "%ROOT%"

call :case a "copy b/1 c/"
call :case a "copy b/1 c/" again
call :case . "copy a/b c"
call :case a "dir/w"
call :case a "dir b/1"
call :case a "dir b/w"
call :case a "dir b/"
call :case a "type b/1"
call :case a "type b\1"
call :case a "del b/1"
call :case a "cd b/c"
call :case a "cd d/e"
call :case a "md x/y"
call :case a "rd d/e"
call :case a "ren b/1 3"
call :case a "move b/2 c/"
call :case a "copy b\1 c\"
call :case a "copy b/1 c/1x"
call :case a "copy b /b c\"
call :case a "echo a/b"
call :case a "copy b/1+b/2 c/12"
call :case a "dir /b b/2"
call :case a "copy b/a c/"
call :case a "xcopy b/1 c/"
call :case a "dir b/a"
call :case a "copy b/* c/"
call :case a "copy b/1 c/b"
call :case a "copy b/x.cmd/a c/"
call :case a "if exist b/1 echo yes"
call :case a "call b/x.cmd"
call :case a "pushd d/e"
call :case a "del b/q"
call :case a "type b/1 b/2"
call :case a "dir b/2/b"
rem Windows では b を命令として読み、スクリプトがここで止まる (最後に置く)
call :case a "b/x.cmd"
goto :eof

:reset
cd /d "%ROOT%"
if exist t rmdir /s /q t
mkdir t\a\b t\a\c t\a\d\e
echo one> t\a\b\1
echo two> t\a\b\2
echo aaa> t\a\b\a
echo @echo xcmd %%CD%%> t\a\b\x.cmd
goto :eof

:case
call :reset
if not "%~3"=="" (
  rem the second run: the same command on the result of the first
  cd /d "%ROOT%\t\%~1"
  %~2 < nul > nul 2>&1
)
cd /d "%ROOT%\t\%~1"
echo ===== [%~1] %~2 %~3
echo n| %~2
echo ----- errorlevel %errorlevel%, cd %CD%
cd /d "%ROOT%\t"
for /f "delims=" %%f in ('dir /s /b /a') do (
  set "P=%%f"
  call echo   %%P:%ROOT%\t\=%%
)
goto :eof
