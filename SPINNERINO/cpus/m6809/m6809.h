/*
 * M6809 CPU Emulator for Galagino
 *
 * Lightweight Motorola 6809 emulator designed for ESP32.
 * Supports all standard M6809 instructions including:
 * - 8/16-bit registers: A, B, D(=A:B), X, Y, U, S, PC, DP, CC
 * - Addressing modes: inherent, immediate, direct, indexed, extended
 * - Interrupts: IRQ, FIRQ, NMI
 * - Page 2 (0x10xx) and Page 3 (0x11xx) extended opcodes
 *
 * Interface uses function pointer callbacks for memory access,
 * set by the active machine (gyruss, tutankhm, etc.).
 */

#ifndef M6809_H
#define M6809_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Condition Code register bits */
#define M6809_CC_C  0x01  /* Carry */
#define M6809_CC_V  0x02  /* Overflow */
#define M6809_CC_Z  0x04  /* Zero */
#define M6809_CC_N  0x08  /* Negative */
#define M6809_CC_I  0x10  /* IRQ mask */
#define M6809_CC_H  0x20  /* Half carry */
#define M6809_CC_F  0x40  /* FIRQ mask */
#define M6809_CC_E  0x80  /* Entire state saved */

typedef struct m6809_state_S {
    uint8_t  A, B;       /* Accumulators */
    uint16_t X, Y;       /* Index registers */
    uint16_t U, S;       /* User and System stack pointers */
    uint16_t PC;         /* Program counter */
    uint8_t  DP;         /* Direct page register */
    uint8_t  CC;         /* Condition code register */

    int      cycles;     /* Cycles consumed in current step */
    int      total_cycles; /* Total cycles consumed */

    uint8_t  irq_pending;   /* IRQ request pending */
    uint8_t  firq_pending;  /* FIRQ request pending */
    uint8_t  nmi_pending;   /* NMI request pending */
    uint8_t  nmi_armed;     /* NMI armed (requires S to be written first) */

    uint8_t  halted;     /* CPU halted (SYNC/CWAI) */
} m6809_state;

/* Reset the CPU - sets PC from reset vector at 0xFFFE */
void m6809_reset(m6809_state *s);

/* Execute one instruction, returns cycles consumed */
int m6809_step(m6809_state *s);

/* Signal an IRQ (active while pending flag set) */
void m6809_irq(m6809_state *s);

/* Signal a FIRQ */
void m6809_firq(m6809_state *s);

/* Signal NMI */
void m6809_nmi(m6809_state *s);

/*
 * Function pointer callbacks for memory access.
 * Each machine sets these before using the m6809.
 */
typedef uint8_t (*m6809_read_fn_t)(uint16_t addr);
typedef void (*m6809_write_fn_t)(uint16_t addr, uint8_t val);

extern m6809_read_fn_t m6809_read_fn;
extern m6809_write_fn_t m6809_write_fn;
extern m6809_read_fn_t m6809_opcode_fn;

/* Bus access functions (implemented in m6809.c, dispatch via function pointers) */
uint8_t m6809_read(m6809_state *s, uint16_t addr);
void m6809_write(m6809_state *s, uint16_t addr, uint8_t val);
uint8_t m6809_read_opcode(m6809_state *s, uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif /* M6809_H */
