// ============================================================================
// goindol_snd.cpp — TU SEPARATA per il core SSG (emu2149) di Goindol.
//
// emu2149.c (PSG/SSG dello YM2203) e' compilato QUI, in una translation unit
// distinta dal .ino, perche' fm.c (YM2203 FM, incluso nel .ino) e i core di
// lignaggio Burczynski collidono se finiscono nella stessa TU (vedi
// boblbobl_snd.cpp). goindol.cpp (nel .ino) chiama PSG_* tramite emu2149.h: il
// linker risolve i simboli da questa TU.
//
// Se ANCHE Bubble Bobble e' attivo, emu2149.c e' gia' compilato da
// boblbobl_snd.cpp -> qui lo saltiamo per non duplicare i simboli (PSG_* sono
// identici e condivisi). Riusiamo lo stesso file in machines/boblbobl/snd/.
// ============================================================================
#include "config.h"

#if defined(ENABLE_GOINDOL) && !defined(ENABLE_BOBLBOBL)
// I core C audio inizializzano tabelle da double verso interi: in C++ (gnu++2b)
// e' "narrowing" = errore. Sopprimiamo solo per questo include.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
#include "machines/boblbobl/snd/emu2149.c"
#pragma GCC diagnostic pop
#endif
