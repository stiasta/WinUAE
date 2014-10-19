/*
* UAE - The Un*x Amiga Emulator
*
* MC68000 emulation
*
* Copyright 1995 Bernd Schmidt
*/

#include "readcpu.h"
#include "machdep/m68k.h"
#include "events.h"

#ifdef WITH_SOFTFLOAT
#include <softfloat.h>
#endif

#ifndef SET_CFLG

#define SET_CFLG(x) (CFLG() = (x))
#define SET_NFLG(x) (NFLG() = (x))
#define SET_VFLG(x) (VFLG() = (x))
#define SET_ZFLG(x) (ZFLG() = (x))
#define SET_XFLG(x) (XFLG() = (x))

#define GET_CFLG() CFLG()
#define GET_NFLG() NFLG()
#define GET_VFLG() VFLG()
#define GET_ZFLG() ZFLG()
#define GET_XFLG() XFLG()

#define CLEAR_CZNV() do { \
	SET_CFLG (0); \
	SET_ZFLG (0); \
	SET_NFLG (0); \
	SET_VFLG (0); \
} while (0)

#define COPY_CARRY() (SET_XFLG (GET_CFLG ()))
#endif

extern const int areg_byteinc[];
extern const int imm8_table[];

extern int movem_index1[256];
extern int movem_index2[256];
extern int movem_next[256];

#ifdef FPUEMU
extern int fpp_movem_index1[256];
extern int fpp_movem_index2[256];
extern int fpp_movem_next[256];
#endif

typedef uae_u32 REGPARAM3 cpuop_func (uae_u32) REGPARAM;
typedef void REGPARAM3 cpuop_func_ce (uae_u32) REGPARAM;

struct cputbl {
	cpuop_func *handler;
	uae_u16 opcode;
};

#ifdef JIT
typedef uae_u32 REGPARAM3 compop_func (uae_u32) REGPARAM;

struct comptbl {
	compop_func *handler;
	uae_u32 opcode;
	int specific;
};
#endif

extern uae_u32 REGPARAM3 op_illg (uae_u32) REGPARAM;
extern void REGPARAM3 op_unimpl (uae_u16) REGPARAM;

typedef uae_u8 flagtype;

#ifdef FPUEMU

#ifdef USE_LONG_DOUBLE
typedef long double fptype;
#define LDPTR tbyte ptr
#else
typedef double fptype;
#define LDPTR qword ptr
#endif
#endif

#define MAX68020CYCLES 4

#define CPU_PIPELINE_MAX 4
#define CPU000_MEM_CYCLE 4
#define CPU000_CLOCK_MULT 2
#define CPU020_MEM_CYCLE 3
#define CPU020_CLOCK_MULT 4

#define CACHELINES020 64
struct cache020
{
	uae_u32 data;
	uae_u32 tag;
	bool valid;
};

#define CACHELINES030 16
struct cache030
{
	uae_u32 data[4];
	bool valid[4];
	uae_u32 tag;
};

#define CACHESETS040 64
#define CACHELINES040 4
struct cache040
{
	uae_u32 data[CACHELINES040][4];
	bool dirty[CACHELINES040][4];
	bool valid[CACHELINES040];
	uae_u32 tag[CACHELINES040];
};

struct mmufixup
{
    int reg;
    uae_u32 value;
};
extern struct mmufixup mmufixup[2];

typedef struct
{
	fptype fp;
#ifdef WITH_SOFTFLOAT
	floatx80 fpx;
#endif
} fpdata;

struct regstruct
{
	uae_u32 regs[16];

	uae_u32 pc;
	uae_u8 *pc_p;
	uae_u8 *pc_oldp;
	uae_u16 opcode;
	uae_u32 instruction_pc;

	uae_u16 irc, ir, db;
	uae_u32 spcflags;
	uae_u32 last_prefetch;
	uae_u32 chipset_latch_rw;
	uae_u32 chipset_latch_read;
	uae_u32 chipset_latch_write;

	uaecptr usp, isp, msp;
	uae_u16 sr;
	flagtype t1;
	flagtype t0;
	flagtype s;
	flagtype m;
	flagtype x;
	flagtype stopped;
	int halted;
	int exception;
	int intmask;
	int ipl, ipl_pin;

	uae_u32 vbr, sfc, dfc;

#ifdef FPUEMU
	fpdata fp[8];
	fpdata fp_result;
	uae_u32 fp_result_status;
	uae_u32 fpcr, fpsr, fpiar;
	uae_u32 fpu_state;
	uae_u32 fpu_exp_state;
	fpdata exp_src1, exp_src2;
	uae_u32 exp_pack[3];
	uae_u16 exp_opcode, exp_extra, exp_type;
	uae_u16 exp_size;
	bool fp_exception;
#endif
#ifndef CPUEMU_68000_ONLY
	uae_u32 cacr, caar;
	uae_u32 itt0, itt1, dtt0, dtt1;
	uae_u32 tcr, mmusr, urp, srp, buscr;
	uae_u32 mmu_fslw;
	uae_u32 mmu_fault_addr, mmu_effective_addr;
	uae_u16 mmu_ssw;
	uae_u32 wb2_address;
	uae_u32 wb3_data;
	uae_u8 wb3_status, wb2_status;
	int mmu_enabled;
	int mmu_page_size;
#endif

	uae_u32 pcr;
	uae_u32 address_space_mask;

	uae_u32 prefetch020[CPU_PIPELINE_MAX];
	uae_u32 prefetch020addr;
	uae_u32 cacheholdingdata020;
	uae_u32 cacheholdingaddr020;
	int ce020memcycles;
	int ce020extracycles;
	bool ce020memcycle_data;
	int ce020_tail;
	frame_time_t ce020_tail_cycles;
	int memory_waitstate_cycles;
};

extern struct regstruct regs;

#define MAX_CPUTRACESIZE 128
struct cputracememory
{
	uae_u32 addr;
	uae_u32 data;
	int mode;
};

struct cputracestruct
{
	uae_u32 regs[16];
	uae_u32 usp, isp, pc;
	uae_u16 ir, irc, sr, opcode;
	int intmask, stopped, state;

	uae_u32 msp, vbr;
	uae_u32 cacr, caar;
	uae_u32 prefetch020[CPU_PIPELINE_MAX];
	uae_u32 prefetch020addr;
	uae_u32 cacheholdingdata020;
	uae_u32 cacheholdingaddr020;
	struct cache020 caches020[CACHELINES020];

	uae_u32 startcycles;
	int needendcycles;
	int memoryoffset;
	int cyclecounter, cyclecounter_pre, cyclecounter_post;
	int readcounter, writecounter;
	struct cputracememory ctm[MAX_CPUTRACESIZE];
};

STATIC_INLINE uae_u32 munge24 (uae_u32 x)
{
	return x & regs.address_space_mask;
}

extern int mmu_enabled, mmu_triggered;
extern int cpu_cycles;
extern int cpucycleunit;
extern int m68k_pc_indirect;
STATIC_INLINE void set_special (uae_u32 x)
{
	regs.spcflags |= x;
	cycles_do_special ();
}

STATIC_INLINE void unset_special (uae_u32 x)
{
	regs.spcflags &= ~x;
}

#define m68k_dreg(r,num) ((r).regs[(num)])
#define m68k_areg(r,num) (((r).regs + 8)[(num)])

extern uae_u32(*x_prefetch)(int);
extern uae_u32(*x_get_byte)(uaecptr addr);
extern uae_u32(*x_get_word)(uaecptr addr);
extern uae_u32(*x_get_long)(uaecptr addr);
extern void(*x_put_byte)(uaecptr addr, uae_u32 v);
extern void(*x_put_word)(uaecptr addr, uae_u32 v);
extern void(*x_put_long)(uaecptr addr, uae_u32 v);
extern uae_u32(*x_next_iword)(void);
extern uae_u32(*x_next_ilong)(void);
extern uae_u32(*x_get_ilong)(int);
extern uae_u32(*x_get_iword)(int);
extern uae_u32(*x_get_ibyte)(int);

extern uae_u32(*x_cp_get_byte)(uaecptr addr);
extern uae_u32(*x_cp_get_word)(uaecptr addr);
extern uae_u32(*x_cp_get_long)(uaecptr addr);
extern void(*x_cp_put_byte)(uaecptr addr, uae_u32 v);
extern void(*x_cp_put_word)(uaecptr addr, uae_u32 v);
extern void(*x_cp_put_long)(uaecptr addr, uae_u32 v);
extern uae_u32(*x_cp_next_iword)(void);
extern uae_u32(*x_cp_next_ilong)(void);

extern uae_u32(REGPARAM3 *x_cp_get_disp_ea_020)(uae_u32 base, int idx) REGPARAM;

/* direct (regs.pc_p) access */

STATIC_INLINE void m68k_setpc(uaecptr newpc)
{
	regs.pc_p = regs.pc_oldp = get_real_address(newpc);
	regs.instruction_pc = regs.pc = newpc;
}
STATIC_INLINE uaecptr m68k_getpc(void)
{
	return (uaecptr)(regs.pc + ((uae_u8*)regs.pc_p - (uae_u8*)regs.pc_oldp));
}
#define M68K_GETPC m68k_getpc()
STATIC_INLINE uaecptr m68k_getpc_p(uae_u8 *p)
{
	return (uaecptr)(regs.pc + ((uae_u8*)p - (uae_u8*)regs.pc_oldp));
}
STATIC_INLINE void m68k_incpc(int o)
{
	regs.pc_p += o;
}

STATIC_INLINE uae_u32 get_dibyte(int o)
{
	return do_get_mem_byte((uae_u8 *)((regs).pc_p + (o) + 1));
}
STATIC_INLINE uae_u32 get_diword(int o)
{
	return do_get_mem_word((uae_u16 *)((regs).pc_p + (o)));
}
STATIC_INLINE uae_u32 get_dilong(int o)
{
	return do_get_mem_long((uae_u32 *)((regs).pc_p + (o)));
}
STATIC_INLINE uae_u32 next_diword(void)
{
	uae_u32 r = do_get_mem_word((uae_u16 *)((regs).pc_p));
	m68k_incpc(2);
	return r;
}
STATIC_INLINE uae_u32 next_dilong(void)
{
	uae_u32 r = do_get_mem_long((uae_u32 *)((regs).pc_p));
	m68k_incpc(4);
	return r;
}

STATIC_INLINE void m68k_do_bsr(uaecptr oldpc, uae_s32 offset)
{
	m68k_areg(regs, 7) -= 4;
	put_long(m68k_areg(regs, 7), oldpc);
	m68k_incpc(offset);
}
STATIC_INLINE void m68k_do_rts(void)
{
	uae_u32 newpc = get_long(m68k_areg(regs, 7));
	m68k_setpc(newpc);
	m68k_areg(regs, 7) += 4;
}

/* indirect (regs.pc) access */

STATIC_INLINE void m68k_setpci(uaecptr newpc)
{
	regs.instruction_pc = regs.pc = newpc;
}
STATIC_INLINE uaecptr m68k_getpci(void)
{
	return regs.pc;
}
STATIC_INLINE void m68k_incpci(int o)
{
	regs.pc += o;
}

STATIC_INLINE uae_u32 get_iibyte(int o)
{
	return get_wordi(m68k_getpci() + (o)) & 0xff;
}
STATIC_INLINE uae_u32 get_iiword(int o)
{
	return get_wordi(m68k_getpci() + (o));
}
STATIC_INLINE uae_u32 get_iilong(int o)
{
	return get_longi(m68k_getpci () + (o));
}

STATIC_INLINE uae_u32 next_iibyte (void)
{
	uae_u32 r = get_iibyte (0);
	m68k_incpci (2);
	return r;
}
STATIC_INLINE uae_u32 next_iiword (void)
{
	uae_u32 r = get_iiword (0);
	m68k_incpci (2);
	return r;
}
STATIC_INLINE uae_u32 next_iiwordi (void)
{
	uae_u32 r = get_wordi(m68k_getpci());
	m68k_incpci (2);
	return r;
}
STATIC_INLINE uae_u32 next_iilong (void)
{
	uae_u32 r = get_iilong(0);
	m68k_incpci (4);
	return r;
}
STATIC_INLINE uae_u32 next_iilongi (void)
{
	uae_u32 r = get_longi (m68k_getpci ());
	m68k_incpci (4);
	return r;
}

STATIC_INLINE void m68k_do_bsri(uaecptr oldpc, uae_s32 offset)
{
	m68k_areg(regs, 7) -= 4;
	put_long(m68k_areg(regs, 7), oldpc);
	m68k_incpci(offset);
}
STATIC_INLINE void m68k_do_rtsi(void)
{
	uae_u32 newpc = get_long(m68k_areg(regs, 7));
	m68k_setpci(newpc);
	m68k_areg(regs, 7) += 4;
}

/* indirect jit friendly versions */

STATIC_INLINE uae_u32 get_iibyte_jit(int o)
{
	return get_wordi(m68k_getpc() + (o)) & 0xff;
}
STATIC_INLINE uae_u32 get_iiword_jit(int o)
{
	return get_wordi(m68k_getpc() + (o));
}
STATIC_INLINE uae_u32 get_iilong_jit(int o)
{
	return get_longi(m68k_getpc() + (o));
}
STATIC_INLINE uae_u32 next_iiword_jit(void)
{
	uae_u32 r = get_wordi(m68k_getpc());
	m68k_incpc(2);
	return r;
}
STATIC_INLINE uae_u32 next_iilong_jit(void)
{
	uae_u32 r = get_longi(m68k_getpc());
	m68k_incpc(4);
	return r;
}
STATIC_INLINE void m68k_do_bsri_jit(uaecptr oldpc, uae_s32 offset)
{
	m68k_areg(regs, 7) -= 4;
	x_put_long(m68k_areg(regs, 7), oldpc);
	m68k_incpc(offset);
}
STATIC_INLINE void m68k_do_rtsi_jit(void)
{
	uae_u32 newpc = x_get_long(m68k_areg(regs, 7));
	m68k_setpc(newpc);
	m68k_areg(regs, 7) += 4;
}

/* common access */

STATIC_INLINE void m68k_incpc_normal(int o)
{
	if (m68k_pc_indirect > 0)
		m68k_incpci(o);
	else
		m68k_incpc(o);
}

STATIC_INLINE void m68k_setpc_normal(uaecptr pc)
{
	if (m68k_pc_indirect > 0) {
		regs.pc_p = regs.pc_oldp = 0;
		m68k_setpci(pc);
	} else {
		m68k_setpc(pc);
	}
}

extern void write_dcache030(uaecptr, uae_u32, int);
extern uae_u32 read_dcache030(uaecptr, int);
extern uae_u32 get_word_icache030(uaecptr addr);
extern uae_u32 get_long_icache030(uaecptr addr);

uae_u32 fill_icache040(uae_u32 addr);
extern void put_long_cache_040(uaecptr, uae_u32);
extern void put_word_cache_040(uaecptr, uae_u32);
extern void put_byte_cache_040(uaecptr, uae_u32);
extern uae_u32 get_ilong_cache_040(int);
extern uae_u32 get_iword_cache_040(int);
extern uae_u32 get_long_cache_040(uaecptr);
extern uae_u32 get_word_cache_040(uaecptr);
extern uae_u32 get_byte_cache_040(uaecptr);
extern uae_u32 next_iword_cache040(void);
extern uae_u32 next_ilong_cache040(void);
extern uae_u32 get_word_icache040(uaecptr addr);
extern uae_u32 get_long_icache040(uaecptr addr);

extern void (*x_do_cycles)(unsigned long);
extern void (*x_do_cycles_pre)(unsigned long);
extern void (*x_do_cycles_post)(unsigned long, uae_u32);

extern uae_u32 REGPARAM3 x_get_disp_ea_020 (uae_u32 base, int idx) REGPARAM;
extern uae_u32 REGPARAM3 x_get_disp_ea_ce020 (uae_u32 base, int idx) REGPARAM;
extern uae_u32 REGPARAM3 x_get_disp_ea_ce030 (uae_u32 base, int idx) REGPARAM;
extern uae_u32 REGPARAM3 x_get_disp_ea_040(uae_u32 base, int idx) REGPARAM;
extern uae_u32 REGPARAM3 x_get_bitfield (uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width) REGPARAM;
extern void REGPARAM3 x_put_bitfield (uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width) REGPARAM;

extern void m68k_setstopped (void);
extern void m68k_resumestopped (void);

extern uae_u32 REGPARAM3 get_disp_ea_020 (uae_u32 base, int idx) REGPARAM;
extern uae_u32 REGPARAM3 get_bitfield (uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width) REGPARAM;
extern void REGPARAM3 put_bitfield (uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width) REGPARAM;

extern void m68k_disasm_ea (uaecptr addr, uaecptr *nextpc, int cnt, uae_u32 *seaddr, uae_u32 *deaddr);
extern void m68k_disasm (uaecptr addr, uaecptr *nextpc, int cnt);
extern void m68k_disasm_2 (TCHAR *buf, int bufsize, uaecptr addr, uaecptr *nextpc, int cnt, uae_u32 *seaddr, uae_u32 *deaddr, int safemode);
extern void sm68k_disasm (TCHAR*, TCHAR*, uaecptr addr, uaecptr *nextpc);
extern int get_cpu_model (void);

extern void set_cpu_caches (bool flush);
extern void flush_cpu_caches(bool flush);
extern void flush_cpu_caches_040(uae_u16 opcode);
extern void REGPARAM3 MakeSR (void) REGPARAM;
extern void REGPARAM3 MakeFromSR (void) REGPARAM;
extern void REGPARAM3 Exception (int) REGPARAM;
extern void REGPARAM3 ExceptionL (int, uaecptr) REGPARAM;
extern void NMI (void);
extern void NMI_delayed (void);
extern void prepare_interrupt (uae_u32);
extern void doint (void);
extern void dump_counts (void);
extern int m68k_move2c (int, uae_u32 *);
extern int m68k_movec2 (int, uae_u32 *);
extern bool m68k_divl (uae_u32, uae_u32, uae_u16);
extern bool m68k_mull (uae_u32, uae_u32, uae_u16);
extern void init_m68k (void);
extern void init_m68k_full (void);
extern void m68k_go (int);
extern void m68k_dumpstate (uaecptr *);
extern void m68k_dumpstate (uaecptr, uaecptr *);
extern void m68k_dumpcache (void);
extern int getDivu68kCycles (uae_u32 dividend, uae_u16 divisor);
extern int getDivs68kCycles (uae_s32 dividend, uae_s16 divisor);
extern void divbyzero_special (bool issigned, uae_s32 dst);
extern void m68k_do_rte (void);
extern void protect_roms (bool);
extern void unprotect_maprom (void);
extern bool is_hardreset(void);
extern bool is_keyboardreset(void);

extern void mmu_op (uae_u32, uae_u32);
extern void mmu_op30 (uaecptr, uae_u32, uae_u16, uaecptr);

extern void fpuop_arithmetic(uae_u32, uae_u16);
extern void fpuop_dbcc(uae_u32, uae_u16);
extern void fpuop_scc(uae_u32, uae_u16);
extern void fpuop_trapcc(uae_u32, uaecptr, uae_u16);
extern void fpuop_bcc(uae_u32, uaecptr, uae_u32);
extern void fpuop_save(uae_u32);
extern void fpuop_restore(uae_u32);
extern uae_u32 fpp_get_fpsr (void);
extern void fpu_reset (void);
extern void fpux_save (int*);
extern void fpux_restore (int*);
extern bool fpu_get_constant(fpdata *fp, int cr);
extern int fpp_cond(int condition);

extern void exception3 (uae_u32 opcode, uaecptr addr);
extern void exception3i (uae_u32 opcode, uaecptr addr);
extern void exception3b (uae_u32 opcode, uaecptr addr, bool w, bool i, uaecptr pc);
extern void exception2 (uaecptr addr, bool read, int size, uae_u32 fc);
extern void exception2_fake (uaecptr addr);
extern void m68k_reset (void);
extern void cpureset (void);
extern void cpu_halt (int id);
extern void cpu_sleep_millis(int ms);

extern void fill_prefetch (void);
extern void fill_prefetch_020 (void);
extern void fill_prefetch_030 (void);

#define CPU_OP_NAME(a) op ## a

/* 68060 */
extern const struct cputbl op_smalltbl_0_ff[];
extern const struct cputbl op_smalltbl_40_ff[];
extern const struct cputbl op_smalltbl_23_ff[]; // CE
extern const struct cputbl op_smalltbl_33_ff[]; // MMU
/* 68040 */
extern const struct cputbl op_smalltbl_1_ff[];
extern const struct cputbl op_smalltbl_41_ff[];
extern const struct cputbl op_smalltbl_24_ff[]; // CE
extern const struct cputbl op_smalltbl_31_ff[]; // MMU
/* 68030 */
extern const struct cputbl op_smalltbl_2_ff[];
extern const struct cputbl op_smalltbl_42_ff[];
extern const struct cputbl op_smalltbl_22_ff[]; // CE
extern const struct cputbl op_smalltbl_32_ff[]; // MMU
/* 68020 */
extern const struct cputbl op_smalltbl_3_ff[];
extern const struct cputbl op_smalltbl_43_ff[];
extern const struct cputbl op_smalltbl_20_ff[]; // prefetch
extern const struct cputbl op_smalltbl_21_ff[]; // CE
/* 68010 */
extern const struct cputbl op_smalltbl_4_ff[];
extern const struct cputbl op_smalltbl_44_ff[];
extern const struct cputbl op_smalltbl_11_ff[]; // prefetch
extern const struct cputbl op_smalltbl_13_ff[]; // CE
/* 68000 */
extern const struct cputbl op_smalltbl_5_ff[];
extern const struct cputbl op_smalltbl_45_ff[];
extern const struct cputbl op_smalltbl_12_ff[]; // prefetch
extern const struct cputbl op_smalltbl_14_ff[]; // CE

extern cpuop_func *cpufunctbl[65536] ASM_SYM_FOR_FUNC ("cpufunctbl");

#ifdef JIT
extern void flush_icache(uaecptr, int);
extern void flush_icache_hard(uaecptr, int);
extern void compemu_reset(void);
extern bool check_prefs_changed_comp (void);
#else
#define flush_icache(uaecptr, int) do {} while (0)
#define flush_icache_hard(uaecptr, int) do {} while (0)
#endif
extern void flush_dcache (uaecptr, int);
extern void flush_mmu (uaecptr, int);

extern int movec_illg (int regno);
extern uae_u32 val_move2c (int regno);
extern void val_move2c2 (int regno, uae_u32 val);
struct cpum2c {
	int regno;
	const TCHAR *regname;
};
extern struct cpum2c m2cregs[];

extern bool is_cpu_tracer (void);
extern bool set_cpu_tracer (bool force);
extern bool can_cpu_tracer (void);