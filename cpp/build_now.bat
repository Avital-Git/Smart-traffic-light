@echo off
cd /d "C:\Users\User\Desktop\smart traffic project\cpp"
cmake --build build --config Release > "%TEMP%\stbuild_out.txt" 2> "%TEMP%\stbuild_err.txt"
echo EXIT_CODE=%ERRORLEVEL% >> "%TEMP%\stbuild_out.txt"
