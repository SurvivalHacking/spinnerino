#include "gyruss.h"
#include "esp_task_wdt.h"

// Geometria nativa Gyruss per la trasposizione SPINNERINO (224 largo x 256 alto).
#define GYR_NW 224   // larghezza nativa (gfb_x = colonne) = stride gyr_full
#define GYR_NH 256   // altezza nativa  (gfb_y = righe, 32 tile-row x 8)

// ============================================================
// M6809 callback bridge (static functions → function pointers)
// ============================================================

gyruss *g_gyruss_instance = nullptr;

// --- Controllo EC11 a manopola rotante (SPINNERINO) ---
// Gyruss e' direzionale a 8 vie: la nave va verso la direzione premuta. Mappiamo
// la posizione ASSOLUTA dell'encoder (ec11_dial_counter, free-running 0..255) a
// una direzione bussola a 8 settori -> ruotando sempre nello stesso verso la nave
// gira tutto l'anello senza fermarsi a ore 9/3.
extern volatile uint8_t ec11_dial_counter;   // definito in input.cpp
#define GYR_DIAL_DIR     (+1)    // verso rotazione: metti (-1) se la nave gira al contrario
#define GYR_SENS         (2)     // sensibilita': PIU' ALTO = PIU' LENTA (serve piu' rotazione)
#define GYR_SPAWN_POS    (128)   // settore di partenza (128 = DOWN/ore 6, dove nasce la nave)
#define GYR_DIAL_OFFSET  (0)     // micro-allineamento 0..255 (display ruotato 90), opzionale
#define GYR_IDLE_MS      (90)    // controllo FLUIDO: invia direzione solo mentre ruoti;
                                 // dopo GYR_IDLE_MS senza rotazione la nave si ferma dov'e'.
                                 // Piu' alto = piu' "morbido/inerziale"; piu' basso = stop piu' secco.

static uint8_t gyruss_m6809_read(uint16_t addr) {
    return g_gyruss_instance->sub_read(addr);
}
static void gyruss_m6809_write(uint16_t addr, uint8_t val) {
    g_gyruss_instance->sub_write(addr, val);
}
static uint8_t gyruss_m6809_read_opcode(uint16_t addr) {
    return g_gyruss_instance->sub_read_opcode(addr);
}

// ============================================================
// i8039 sample MCU (drums) — callback i8048
// port 0 = BUS (legge il comando = soundlatch2), 1 = P1 (->DAC), 2 = P2 (irq clear)
// Vedi MAME konami/gyruss.cpp: p1_out_cb->dac_w, io_map 0x00-0xff->soundlatch2.
// ============================================================
void gyruss::wrI8048_port(struct i8048_state_S *state, unsigned char port, unsigned char val) {
    (void)state;
    if (port == 1) {
        i8039_dac = val;            // P1 -> DAC: campione corrente dei drums
    }
    // port 2 (P2) = irq_clear: l'IRQ e' edge-trigger one-shot nel core -> no-op
}

unsigned char gyruss::rdI8048_port(struct i8048_state_S *state, unsigned char port) {
    (void)state;
    if (port == 0) return soundlatch2;   // INS A,BUS -> comando latchato
    return 0xFF;
}

unsigned char gyruss::rdI8048_xdm(struct i8048_state_S *state, unsigned char addr) {
    (void)state; (void)addr;
    return soundlatch2;                  // MOVX A,@R -> comando latchato (alternativa al BUS)
}

unsigned char gyruss::rdI8048_rom(struct i8048_state_S *state, unsigned short addr) {
    (void)state;
    return gyruss_rom_i8039[addr & 0x0FFF];
}

// ============================================================
// M6809 sub-CPU memory map
// ============================================================

uint8_t gyruss::sub_read(uint16_t addr) {
    if (addr == 0x0000) {
        return scanline_counter;
    }
    if (addr >= 0x4000 && addr <= 0x47FF) {
        return sub_ram[addr - 0x4000];
    }
    if (addr >= 0x6000 && addr <= 0x67FF) {
        return shared_ram[addr - 0x6000];
    }
    // ROM at 0xE000-0xFFFF only (no mirror, matching MAME)
    if (addr >= 0xE000) {
        return pgm_read_byte(&gyruss_rom_sub_raw[addr - 0xE000]);
    }
    return 0xFF;
}

void gyruss::sub_write(uint16_t addr, uint8_t val) {
    if (addr == 0x2000) {
        // MAME: gyruss_irq_clear_w — acknowledge IRQ (CLEAR_LINE)
        // IRQ mask is controlled by main CPU LS259 Q1 (0xC181), not here
        return;
    }
    if (addr >= 0x4000 && addr <= 0x47FF) {
        sub_ram[addr - 0x4000] = val;
        return;
    }
    if (addr >= 0x6000 && addr <= 0x67FF) {
        shared_ram[addr - 0x6000] = val;
        return;
    }
}

uint8_t gyruss::sub_read_opcode(uint16_t addr) {
    // Konami-1 decrypted opcodes at 0xE000-0xFFFF only (no mirror)
    if (addr >= 0xE000) {
        return pgm_read_byte(&gyruss_rom_sub_decrypt[addr - 0xE000]);
    }
    return sub_read(addr);
}

// ============================================================
// Audio Z80 dual-core task
// ============================================================

static void gyruss_audio_task_fn(void *param) {
    gyruss *g = (gyruss *)param;
    esp_task_wdt_delete(NULL);
    // PACING a tempo REALE, fine (come l'originale a taskYIELD ma a rate giusto).
    // Audio Z80 Gyruss @ 3.579 MHz; rapportato al main Z80 (3.072 MHz, 10000
    // step/frame = 600k step/s) -> ~699k step/s = 0.699 step/us.
    // (Il vTaskDelay(1) a 10ms precedente desincronizzava gli AY; ora batch ~250us.)
    uint32_t last = micros();
    while (g->audio_running) {
        uint32_t now = micros();
        uint32_t elapsed = now - last;
        if (elapsed >= 250) {                           // batch fine ~250us
            last = now;
            if (elapsed > 50000) elapsed = 50000;       // cap dopo stalli
            int steps = (int)((elapsed * 699UL) / 1000UL);  // 0.699 step/us (pacing reale; la VELOCITA' AY si regola con AY_INC in audio.cpp)
            if (steps > 0) g->run_audio_batch(steps);
        }
        taskYIELD();
    }
    vTaskDelete(NULL);
}

void gyruss::run_audio_batch(int steps) {
    for (int i = 0; i < steps && audio_running; i++) {
        StepZ80(&cpu[1]);
        if (sound_irq_pending && (cpu[1].IFF & IFF_1)) {
            IntZ80(&cpu[1], INT_RST38);
            sound_irq_pending = 0;
        }
    }
    // audio_cycle_approx = timer port A (AY3 reg14). NON influenza il tempo dei
    // suoni (verificato): il tempo musica/drums/SFX e' dato dall'IRQ che il gioco
    // manda allo Z80 audio una volta per frame. Lasciato a ×8 (baseline).
    audio_cycle_approx += steps * 8;
    // NOTA: l'i8039 (drums) NON e' piu' steppato qui (causava crackle da raffiche).
    // Ora e' steppato per-campione dentro ay_render_buffer (audio.cpp) -> DAC
    // campionato regolarmente a 32kHz, niente aliasing. Comandi/IRQ restano qui
    // via outZ80 (soundlatch2/notINT, volatili, letti dal render).
}

void gyruss::start_audio_task() {
    if (audio_task_handle) return;
    audio_running = 1;
    int audio_core = (emu_core_id == 0) ? 1 : 0;
    xTaskCreatePinnedToCore(gyruss_audio_task_fn, "gyr_audio", 8192, this, 1, &audio_task_handle, audio_core);
}

void gyruss::stop_audio_task() {
    if (!audio_task_handle) return;
    audio_running = 0;
    vTaskDelay(20);  // let audio task exit
    audio_task_handle = NULL;
}

// ============================================================
// Init and Reset
// ============================================================

void gyruss::init(Input *input, unsigned short *framebuffer, sprite_S *spritebuffer, unsigned char *memorybuffer) {
    machineBase::init(input, framebuffer, spritebuffer, memorybuffer);
    g_gyruss_instance = this;
    m6809_read_fn = gyruss_m6809_read;
    m6809_write_fn = gyruss_m6809_write;
    m6809_opcode_fn = gyruss_m6809_read_opcode;
    audio_task_handle = NULL;
    audio_running = 0;
    emu_core_id = -1;

    // Buffer nativo intero per la trasposizione SPINNERINO (PSRAM).
    if (!gyr_full)
        gyr_full = (unsigned short*)ps_malloc(GYR_NW * GYR_NH * sizeof(unsigned short));
}

void gyruss::reset() {
    // Stop audio task if running (before resetting state)
    stop_audio_task();

    machineBase::reset();
    g_gyruss_instance = this;
    m6809_read_fn = gyruss_m6809_read;
    m6809_write_fn = gyruss_m6809_write;
    m6809_opcode_fn = gyruss_m6809_read_opcode;

    memset(sub_ram, 0, sizeof(sub_ram));
    memset(shared_ram, 0, sizeof(shared_ram));
    sub_irq_mask = 1;  // default enabled; main CPU can control via LS259 Q1 (0xC181)
    sound_latch = 0;
    sound_latch_pending = 0;
    sound_irq_pending = 0;
    flip_screen = 0;
    scanline_counter = 0;
    frame_odd = 0;

    // Controllo EC11: azzera l'accumulatore relativo e parti "disarmato"
    // -> la nave non si muove finche' non ruoti l'encoder.
    dial_prev    = ec11_dial_counter;
    dial_acc     = 0;
    dial_armed   = 0;
    dial_last_ms = 0;
    memset(ay_address, 0, sizeof(ay_address));
    audio_cycle_approx = 0;
    emu_core_id = -1;

    // i8039 sample MCU (drums)
    i8048_reset(&i8039_cpu);
    soundlatch2 = 0;
    i8039_dac = 128;       // DAC a riposo = centro (silenzio)
    i8039_irq_pending = 0;

    // Reset M6809 sub-CPU
    m6809_reset(&sub_cpu);

    // Reset audio Z80 (still single-core at this point, audio task not started yet)
    current_cpu = 1;
    ResetZ80(&cpu[1]);
    current_cpu = 0;
}

// ============================================================
// Main Z80 memory map
// ============================================================

// Determine if current context is audio Z80 (works for both single-core and dual-core)
inline bool gyruss::is_audio_cpu() {
    if (audio_task_handle != NULL)
        return (xPortGetCoreID() != emu_core_id);  // dual-core: check which core
    return (current_cpu == 1);                       // single-core: use flag
}

unsigned char gyruss::opZ80(unsigned short Addr) {
    if (!is_audio_cpu()) {
        if (Addr < 0x6000)
            return pgm_read_byte(&gyruss_rom_main[Addr]);
        return rdZ80(Addr);
    } else {
        if (Addr < 0x4000)
            return pgm_read_byte(&gyruss_rom_audio[Addr]);
        return rdZ80(Addr);
    }
}

unsigned char gyruss::rdZ80(unsigned short Addr) {
    if (!is_audio_cpu()) {
        if (Addr < 0x6000)
            return pgm_read_byte(&gyruss_rom_main[Addr]);

        if (Addr >= 0x8000 && Addr <= 0x83FF)
            return memory[GYR_CRAM_OFF + (Addr & 0x3FF)];

        if (Addr >= 0x8400 && Addr <= 0x87FF)
            return memory[GYR_VRAM_OFF + (Addr & 0x3FF)];

        if (Addr >= 0x9000 && Addr <= 0x9FFF)
            return memory[GYR_WRAM_OFF + (Addr - 0x9000)];

        if (Addr >= 0xA000 && Addr <= 0xA7FF)
            return shared_ram[Addr - 0xA000];

        if (Addr == 0xC000) return GYRUSS_DSW1;

        if (Addr == 0xC080) {
            unsigned char keymask = input->buttons_get();
            unsigned char retval = 0xFF;
            if (keymask & BUTTON_COIN)  retval &= ~0x01;
            if (keymask & BUTTON_START) retval &= ~0x08;
            return retval;
        }

        if (Addr == 0xC0A0) {
            unsigned char keymask = input->buttons_get();
            unsigned char retval = 0xFF;
            // --- Controllo FLUIDO (a velocita') ---
            // Accumula la rotazione (delta con segno, gestisce il wrap 255<->0).
            // La nave riceve una direzione SOLO mentre stai ruotando (o nei
            // GYR_IDLE_MS appena dopo l'ultimo scatto); quando ti fermi NON inviamo
            // alcuna direzione -> la nave si congela all'angolo corrente (qualsiasi,
            // non solo gli 8 ottanti). Movimento continuo e proporzionale.
            uint32_t now = millis();
            int8_t   d   = (int8_t)(ec11_dial_counter - dial_prev);
            dial_prev = ec11_dial_counter;
            if (d != 0) { dial_acc += (GYR_DIAL_DIR * (int)d); dial_armed = 1; dial_last_ms = now; }

            if (dial_armed && (uint32_t)(now - dial_last_ms) <= GYR_IDLE_MS) {
                uint8_t pos    = (uint8_t)(dial_acc / GYR_SENS + GYR_SPAWN_POS + GYR_DIAL_OFFSET);
                uint8_t sector = (uint8_t)((pos + 16) >> 5) & 7;   // +16 = centra i settori
                switch (sector) {
                    case 0: retval &= ~0x04;        break; // UP
                    case 1: retval &= ~(0x04|0x02); break; // UP+RIGHT
                    case 2: retval &= ~0x02;        break; // RIGHT
                    case 3: retval &= ~(0x08|0x02); break; // DOWN+RIGHT
                    case 4: retval &= ~0x08;        break; // DOWN
                    case 5: retval &= ~(0x08|0x01); break; // DOWN+LEFT
                    case 6: retval &= ~0x01;        break; // LEFT
                    case 7: retval &= ~(0x04|0x01); break; // UP+LEFT
                }
            }
            if ((keymask & BUTTON_FIRE) && game_started) retval &= ~0x10;
            return retval;
        }

        if (Addr == 0xC0C0) return 0xFF;
        if (Addr == 0xC0E0) return GYRUSS_DSW0;
        if (Addr == 0xC100) return GYRUSS_DSW2;

        return 0xFF;
    }
    else {
        if (Addr < 0x4000)
            return pgm_read_byte(&gyruss_rom_audio[Addr]);

        if (Addr >= 0x6000 && Addr <= 0x63FF)
            return memory[GYR_ARAM_OFF + (Addr - 0x6000)];

        if (Addr == 0x8000) {
            return sound_latch;
        }

        return 0xFF;
    }
}

void gyruss::wrZ80(unsigned short Addr, unsigned char Value) {
    if (!is_audio_cpu()) {
        if (Addr >= 0x8000 && Addr <= 0x83FF) {
            memory[GYR_CRAM_OFF + (Addr & 0x3FF)] = Value;
            return;
        }

        if (Addr >= 0x8400 && Addr <= 0x87FF) {
            memory[GYR_VRAM_OFF + (Addr & 0x3FF)] = Value;
            if (!game_started && Value != 0) game_started = 1;
            return;
        }

        if (Addr >= 0x9000 && Addr <= 0x9FFF) {
            memory[GYR_WRAM_OFF + (Addr - 0x9000)] = Value;
            return;
        }

        if (Addr >= 0xA000 && Addr <= 0xA7FF) {
            shared_ram[Addr - 0xA000] = Value;
            return;
        }

        if (Addr == 0xC000) return;

        if (Addr == 0xC080) {
            // MAME: sh_irqtrigger_w — latch IRQ for audio Z80 (HOLD_LINE)
            sound_irq_pending = 1;
            return;
        }

        if (Addr == 0xC100) {
            sound_latch = Value;
            return;
        }

        // MAME: LS259 mainlatch at 0xC180-0xC187, each address sets one Q bit
        // Q0=NMI mask, Q2/Q3=coin counters, Q5=flip screen
        // Q1=sub CPU IRQ mask (kept always enabled, game init clears it otherwise)
        if (Addr >= 0xC180 && Addr <= 0xC187) {
            int bit = Addr & 7;
            int bval = Value & 1;
            switch (bit) {
                case 0: irq_enable[0] = bval; break;
                case 5: flip_screen = bval; break;
            }
            return;
        }

        return;
    }
    else {
        if (Addr >= 0x6000 && Addr <= 0x63FF) {
            memory[GYR_ARAM_OFF + (Addr - 0x6000)] = Value;
            return;
        }
    }
}

void gyruss::outZ80(unsigned short Port, unsigned char Value) {
    if (!is_audio_cpu()) return;

    uint8_t port_lo = Port & 0xFF;

    // MAME Gyruss audio IO: 5 AY chips, stride-4 ports
    // AY1: addr=0x00, read=0x01, write=0x02
    // AY2: addr=0x04, read=0x05, write=0x06
    // AY3: addr=0x08, read=0x09, write=0x0a
    // AY4: addr=0x0c, read=0x0d, write=0x0e
    // AY5: addr=0x10, read=0x11, write=0x12
    // 0x14: i8039 IRQ trigger -> asserisce l'IRQ dell'8039 (drums)
    // 0x18: soundlatch2 -> comando campione per l'8039
    if (port_lo == 0x18) { soundlatch2 = Value; return; }       // comando drums
    if (port_lo == 0x14) { i8039_irq_pending = 1; return; }     // pulsa IRQ 8039 (flag cross-core)

    if (port_lo > 0x12) return;

    int ay_chip = port_lo / 4;    // 0-4 = AY1-AY5
    int func = port_lo % 4;       // 0=addr, 1=read(ignored), 2=write

    if (func == 0) {
        // Address latch
        ay_address[ay_chip] = Value & 0x0F;
    } else if (func == 2) {
        // Data write — map TUTTI e 5 gli AY a soundregs (5*16=80 = sizeof soundregs)
        if (ay_chip < 5) {
            soundregs[(ay_chip * 16) + ay_address[ay_chip]] = Value;
        }
    }
}

unsigned char gyruss::inZ80(unsigned short Port) {
    if (!is_audio_cpu()) return 0xFF;

    uint8_t port_lo = Port & 0xFF;

    // AY data read ports: 0x01(AY1), 0x05(AY2), 0x09(AY3), 0x0d(AY4), 0x11(AY5)
    if (port_lo > 0x12) return 0xFF;
    int ay_chip = port_lo / 4;
    int func = port_lo % 4;

    if (func == 1 && ay_chip < 5) {
        int reg = ay_address[ay_chip];
        // AY3 register 14 (port A) returns a timer value (MAME: porta_r)
        if (ay_chip == 2 && reg == 14) {
            static const uint8_t timer_table[10] = {
                0x00, 0x01, 0x02, 0x03, 0x04, 0x09, 0x0a, 0x0b, 0x0a, 0x0d
            };
            return timer_table[(audio_cycle_approx / 1024) % 10];
        }
        if (ay_chip < 5) {
            return soundregs[(ay_chip * 16) + reg];
        }
    }

    return 0xFF;
}

// ============================================================
// Frame execution
// ============================================================

// ===== MANOPOLA VELOCITA' GIOCO (frazionaria, fine) =====
// Velocita' = GYR_SPEED_NUM / GYR_SPEED_DEN frame di gioco per frame video.
// Il gioco e' frame-locked (1 NMI/vblank = 1 frame). DEN=10 fisso (passi 0.1x):
//   10 = 1.0x   13 = 1.3x   14 = 1.4x (attuale)   15 = 1.5x   20 = 2.0x
#define GYR_SPEED_NUM 15
#define GYR_SPEED_DEN 10

void gyruss::run_frame(void) {
    // Start audio task on first frame (now we know which core we're on)
    if (emu_core_id < 0) {
        emu_core_id = xPortGetCoreID();
        start_audio_task();
    }

    // Accumulatore: esegue in media NUM/DEN frame di gioco per chiamata.
    static unsigned int speed_acc = 0;
    speed_acc += GYR_SPEED_NUM;
    int nframes = 0;
    while (speed_acc >= GYR_SPEED_DEN) { speed_acc -= GYR_SPEED_DEN; nframes++; }

    for (int f = 0; f < nframes; f++) {
        current_cpu = 0;
        scanline_counter = 0;
        int sc_acc = 0;
        for (int i = 0; i < 500; i++) {
            StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
            StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
            StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);
            StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]); StepZ80(&cpu[0]);

            m6809_step(&sub_cpu); m6809_step(&sub_cpu); m6809_step(&sub_cpu); m6809_step(&sub_cpu); m6809_step(&sub_cpu);
            m6809_step(&sub_cpu); m6809_step(&sub_cpu); m6809_step(&sub_cpu); m6809_step(&sub_cpu); m6809_step(&sub_cpu);

            sc_acc += 256;
            if (sc_acc >= 500) { sc_acc -= 500; scanline_counter++; }
        }

        // Interrupt di fine frame (vblank): 1 frame di gioco eseguito
        if (irq_enable[0]) IntZ80(&cpu[0], INT_NMI);
        if (sub_irq_mask)  m6809_irq(&sub_cpu);
        frame_odd ^= 1;
    }
    // Audio Z80 + i8039 girano nel render/task separati
}

// ============================================================
// Sprite preparation
// ============================================================

void gyruss::prepare_frame(void) {
    // Snapshot CRAM+VRAM to prevent tearing in half-rate rendering
    // (emulation may modify memory[] between the two screen halves)
    memcpy(tile_snapshot, &memory[GYR_CRAM_OFF], 0x800);

    active_sprites = 0;

    // Sprite RAM at sub CPU address 0x4040-0x40FF (confirmed from MAME)
    unsigned char *sr = &sub_ram[0x40];

    for (int offs = 0xBC; offs >= 0 && active_sprites < 64; offs -= 4) {
        // MAME bitmap: drawgfx at (sr[offs], 241-sr[offs+3]), 8w×16h
        // ROT90 tile-derived mapping: frame_x = bitmap_y - 16, frame_y = bitmap_x + 16
        // Transposed: ROM row r → frame_x (16 wide), ROM col c → frame_y (8 tall)
        int bx = sr[offs];             // MAME bitmap_x
        int by = 241 - sr[offs + 3];   // MAME bitmap_y
        int gfx_bank = sr[offs + 1] & 1;
        int code = ((sr[offs + 2] & 0x20) << 2) | (sr[offs + 1] >> 1);
        int color = sr[offs + 2] & 0x0f;
        int hw_flip_x = (~sr[offs + 2] >> 6) & 1;  // MAME: ~bit6
        int hw_flip_y = (sr[offs + 2] >> 7) & 1;    // MAME: bit7

        sprite[active_sprites].x = by - 16;      // frame_x start (16 wide)
        sprite[active_sprites].y = bx + 16;      // frame_y start (8 tall)
        sprite[active_sprites].code = code;
        sprite[active_sprites].color = color;
        // ROT90 swaps flip axes: bitmap flip_y → frame flip_x (variant bit 0)
        //                        bitmap flip_x → frame flip_y (variant bit 1)
        sprite[active_sprites].flip_y = hw_flip_y;  // → variant bit 0 (row reversal = frame_x flip)
        sprite[active_sprites].flip_x = hw_flip_x;  // → variant bit 1 (pixel reversal = frame_y flip)
        sprite[active_sprites].color_block = gfx_bank;

        active_sprites++;
    }

    // Render nativo a frame INTERO (224x256) in gyr_full. Le blit_* scrivono via
    // `rt` = base della strip. render_row() trasporra' poi nel framebuffer 256.
    if (gyr_full) {
        memset(gyr_full, 0, GYR_NW * GYR_NH * sizeof(unsigned short));
        for (short row = 2; row < 34; row++) {       // tile-row 2..33 (32 righe)
            rt = gyr_full + (row - 2) * 8 * GYR_NW;
            for (char col = 0; col < 28; col++)
                blit_tile(row, col);
            for (int s = 0; s < active_sprites; s++)
                blit_sprite(row, (unsigned char)s);
        }
    }
}

// ============================================================
// Tile rendering
// ============================================================

void gyruss::blit_tile(short row, char col) {
    // Gyruss uses column-major VRAM with 90-degree CRT rotation
    // tile_reverse_col=1, tile_col_offset=0, tile_row_offset=0
    int vram_row = row - 2;
    int vram_col = 29 - col;  // reversed column order

    if (vram_row < 0 || vram_row >= 32 || vram_col < 0 || vram_col >= 32) return;

    unsigned short vram_addr = (31 - vram_col) * 32 + vram_row;
    unsigned char tile_code_raw = tile_snapshot[GYR_VRAM_OFF + vram_addr];
    unsigned char color_attr = tile_snapshot[GYR_CRAM_OFF + vram_addr];

    unsigned short tile_code = ((color_attr & 0x20) ? 256 : 0) + tile_code_raw;
    if (tile_code >= 512) tile_code = 0;

    unsigned char color_group = color_attr & 0x0F;
    unsigned char flip_x = (color_attr >> 6) & 1;
    unsigned char flip_y = (color_attr >> 7) & 1;

    const unsigned short *tile_data = gyruss_tilemap[tile_code];
    const unsigned short *colors = gyruss_char_colormap[color_group];

    // Preload 4 palette colors from flash to local RAM (4 reads vs 64)
    unsigned short lc[4];
    lc[0] = pgm_read_word(&colors[0]);
    lc[1] = pgm_read_word(&colors[1]);
    lc[2] = pgm_read_word(&colors[2]);
    lc[3] = pgm_read_word(&colors[3]);

    unsigned short *ptr = rt + 8 * col;

    for (int r = 0; r < 8; r++) {
        int src_r = flip_y ? (7 - r) : r;
        unsigned short pix = pgm_read_word(&tile_data[src_r]);

        if (flip_x) {
            for (int c = 0; c < 8; c++)
                ptr[c] = lc[(pix >> ((7 - c) * 2)) & 3];
        } else {
            for (int c = 0; c < 8; c++)
                ptr[c] = lc[(pix >> (c * 2)) & 3];
        }
        ptr += 224;
    }
}

// ============================================================
// Sprite rendering
// ============================================================

void gyruss::blit_sprite(short row, unsigned char s_idx) {
    struct sprite_S *s = &sprite[s_idx];

    int spr_start_y = s->y;       // frame_y start (8 tall after ROT90)
    int row_pixel_start = row * 8;

    // Transposed: 16 wide (frame_x) × 8 tall (frame_y) after ROT90
    if (spr_start_y >= row_pixel_start + 8 || spr_start_y + 8 <= row_pixel_start)
        return;

    int code = s->code + (s->color_block ? 256 : 0);
    if (code >= 512) code = 0;

    // ROT90 swaps flip axes: flip_y → variant bit 0, flip_x → variant bit 1
    int variant = 0;
    if (s->flip_y) variant |= 1;   // row reversal = frame_x flip
    if (s->flip_x) variant |= 2;   // pixel reversal = frame_y flip

    unsigned char color_group = s->color;
    const unsigned short *colors = gyruss_sprite_colormap[color_group];

    // Preload 16 sprite palette colors from flash to local RAM
    unsigned short lc[16];
    for (int i = 0; i < 16; i++) lc[i] = pgm_read_word(&colors[i]);

    // Pre-compute visible frame_y range within this row strip
    int c_start = (row_pixel_start > spr_start_y) ? (row_pixel_start - spr_start_y) : 0;
    int c_end = ((row_pixel_start + 8) < (spr_start_y + 8)) ? (row_pixel_start + 8 - spr_start_y) : 8;
    int y_base = spr_start_y - row_pixel_start;

    // ROM row r → frame_x offset (0..15), ROM col c → frame_y offset (0..7)
    for (int r = 0; r < 16; r++) {
        int screen_x = s->x + r;
        if (screen_x < 0 || screen_x >= 224) continue;

        unsigned long row_data = pgm_read_dword(&gyruss_sprites[variant][code][r]);

        for (int c = c_start; c < c_end; c++) {
            unsigned char px = (row_data >> (c * 4)) & 0x0F;
            if (px) {
                rt[(y_base + c) * 224 + screen_x] = lc[px];
            }
        }
    }
}

// ============================================================
// Row rendering
// ============================================================

// Trasposizione del frame nativo (gyr_full 224x256) nel framebuffer 256 di
// SPINNERINO, una strip alla volta (stessa ricetta di Xevious/galaga):
//   fb_x = gfb_y               (riga nativa -> asse lungo 256, gw=256)
//   fb_y = gfb_x               (colonna nativa, NON reversed: Gyruss specchiato vs Xevious)
void gyruss::render_row(short strip_r) {
    if (!gyr_full) return;
    for (int fb_y_strip = 0; fb_y_strip < 8; fb_y_strip++) {
        int fb_y  = strip_r * 8 + fb_y_strip;
        int gfb_x = fb_y;                        // NO reversal (Gyruss e' speculato vs Xevious)
        if (gfb_x < 0 || gfb_x >= GYR_NW) continue;
        unsigned short *dst       = frame_buffer + fb_y_strip * 256;
        const unsigned short *src = gyr_full + gfb_x;   // colonna gfb_x, passo GYR_NW
        for (int gfb_y = 0; gfb_y < GYR_NH; gfb_y++)
            dst[gfb_y] = src[gfb_y * GYR_NW];
    }
}

// ============================================================
// Logo
// ============================================================

const unsigned short *gyruss::logo(void) {
    return gyruss_logo;
}

#ifdef LED_PIN
void gyruss::menuLeds(CRGB *leds) {
    memcpy(leds, menu_leds, NUM_LEDS * sizeof(CRGB));
}
void gyruss::gameLeds(CRGB *leds) {
    memcpy(leds, menu_leds, NUM_LEDS * sizeof(CRGB));
}
#endif
