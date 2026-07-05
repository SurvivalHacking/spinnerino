@echo off
REM stdout UTF-8 anche su Windows (cp1252 non digerisce -> frecce/box nei print)
set PYTHONUTF8=1
echo ========================================
echo  Conversione ROM - tutti i giochi
echo ========================================

python romconv_gigas2.py
if errorlevel 1 goto :error

python romconv_arkangc.py
if errorlevel 1 goto :error

python romconv_motorace.py
if errorlevel 1 goto :error

python romconv_arkanoid2.py
if errorlevel 1 goto :error

python romconv_phoenix.py
if errorlevel 1 goto :error

python romconv_galaga.py
if errorlevel 1 goto :error

python romconv_sbrkout.py
if errorlevel 1 goto :error

python romconv_bombbee.py
if errorlevel 1 goto :error

python romconv_roadfighter.py
if errorlevel 1 goto :error

python romconv_galaxian.py
if errorlevel 1 goto :error

python romconv_spaceinvaders.py
if errorlevel 1 goto :error

python romconv_gigas.py
if errorlevel 1 goto :error

python romconv_goindol.py
if errorlevel 1 goto :error

python romconv_gyruss.py
if errorlevel 1 goto :error

python romconv_pang.py
if errorlevel 1 goto :error

echo.
echo ========================================
echo  Tutti i convertitori completati!
echo ========================================
goto :end

:error
echo.
echo !! ERRORE (codice %errorlevel%) — conversione interrotta.
pause
exit /b %errorlevel%

:end
pause
