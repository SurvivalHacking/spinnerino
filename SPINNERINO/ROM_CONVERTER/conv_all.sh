#!/bin/bash
set -e
export PYTHONUTF8=1   # stdout UTF-8 (innocuo su Mac, fix print non-ASCII su Windows)

# Esegui sempre dalla cartella di questo script (ROM_CONVERTER): i converter
# usano path relativi a ../ROMS e ../machines, quindi il cwd deve essere qui.
cd "$(dirname "$0")"

echo "========================================"
echo " Conversione ROM - tutti i giochi"
echo "========================================"

python3 romconv_gigas2.py
python3 romconv_arkangc.py
python3 romconv_motorace.py
python3 romconv_arkanoid2.py
python3 romconv_phoenix.py
python3 romconv_galaga.py
python3 romconv_sbrkout.py
python3 romconv_bombbee.py
python3 romconv_roadfighter.py
python3 romconv_galaxian.py
python3 romconv_spaceinvaders.py
python3 romconv_gigas.py
python3 romconv_goindol.py
python3 romconv_gyruss.py
python3 romconv_pang.py

echo ""
echo "========================================"
echo " Tutti i convertitori completati!"
echo "========================================"
