// Copyright 2022 Charles Lohr, you may use this file or any portions herein under any of the BSD, MIT, or CC0 licenses.
// Refactored single-file version.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "default64mbdtc.h"

//////////////////////////////////////////////////////////////////////////////
// Constants
//////////////////////////////////////////////////////////////////////////////

static const uint32_t RAM_IMAGE_OFFSET = 0x80000000;

//////////////////////////////////////////////////////////////////////////////
// Step result codes
//////////////////////////////////////////////////////////////////////////////

typedef enum
{
	RV_STEP_OK       = 0,     // normal execution
	RV_STEP_WFI      = 1,     // processor halted waiting for interrupt
	RV_STEP_FAULT    = 3,     // hard fault, abort requested
	RV_STEP_RESTART  = 0x7777, // guest wrote syscon restart code
	RV_STEP_POWEROFF = 0x5555, // guest wrote syscon poweroff code
} RVStepResult;

//////////////////////////////////////////////////////////////////////////////
// CPU state
//////////////////////////////////////////////////////////////////////////////

struct MiniRV32IMAState
{
	uint32_t regs[32];

	uint32_t pc;
	uint32_t mstatus;
	uint32_t cyclel;
	uint32_t cycleh;

	uint32_t timerl;
	uint32_t timerh;
	uint32_t timermatchl;
	uint32_t timermatchh;

	uint32_t mscratch;
	uint32_t mtvec;
	uint32_t mie;
	uint32_t mip;

	uint32_t mepc;
	uint32_t mtval;
	uint32_t mcause;

	// Bits 0..1 = privilege (Machine = 3, User = 0)
	// Bit 2     = WFI (Wait for interrupt)
	// Bit 3+    = Load/Store reservation LSBs
	uint32_t extraflags;
};

//////////////////////////////////////////////////////////////////////////////
// Emulator state
//////////////////////////////////////////////////////////////////////////////

struct EmulatorState
{
	struct MiniRV32IMAState cpu;
	uint8_t               * ram;
	uint32_t                ram_size;
	int                     fail_on_all_faults;
};

// Only used by the POSIX signal handler (CtrlC), which cannot receive
// a parameter. Do not use this pointer anywhere else.
static struct EmulatorState * g_emu = 0;

static const char * kernel_command_line = 0;

//////////////////////////////////////////////////////////////////////////////
// Platform forward declarations
//////////////////////////////////////////////////////////////////////////////

static uint64_t GetTimeMicroseconds();
static void     CaptureKeyboardInput();
static void     ResetKeyboardInput();
static void     MiniSleep();
static int      IsKBHit();
static int      ReadKBByte();

//////////////////////////////////////////////////////////////////////////////
// Peripheral I/O
//////////////////////////////////////////////////////////////////////////////

static int is_mmio( uint32_t addr )
{
	return addr >= 0x10000000 && addr < 0x12000000;
}

static RVStepResult HandleControlStore( struct EmulatorState * emu, uint32_t addy, uint32_t val )
{
	if( addy == 0x10000000 )       // UART 8250 / 16550 data
	{
		printf( "%c", val );
		fflush( stdout );
	}
	else if( addy == 0x11004004 )  // CLINT timermatchh
		emu->cpu.timermatchh = val;
	else if( addy == 0x11004000 )  // CLINT timermatchl
		emu->cpu.timermatchl = val;
	else if( addy == 0x11100000 )  // SYSCON (reboot, poweroff, etc.)
	{
		emu->cpu.pc += 4;
		if( val == 0x7777 ) return RV_STEP_RESTART;
		if( val == 0x5555 ) return RV_STEP_POWEROFF;
		return RV_STEP_FAULT; // unknown syscon value
	}
	return RV_STEP_OK;
}

static uint32_t HandleControlLoad( struct EmulatorState * emu, uint32_t addy )
{
	if( addy == 0x10000005 )                       // UART line status
		return 0x60 | IsKBHit();
	else if( addy == 0x10000000 && IsKBHit() )     // UART RX
		return ReadKBByte();
	else if( addy == 0x1100bffc )                  // CLINT timerh
		return emu->cpu.timerh;
	else if( addy == 0x1100bff8 )                  // CLINT timerl
		return emu->cpu.timerl;
	return 0;
}

static void HandleOtherCSRWrite( struct EmulatorState * emu, uint16_t csrno, uint32_t value )
{
	if( csrno == 0x136 )
	{
		printf( "%d", value ); fflush( stdout );
	}
	else if( csrno == 0x137 )
	{
		printf( "%08x", value ); fflush( stdout );
	}
	else if( csrno == 0x138 )
	{
		uint32_t ptrstart = value - RAM_IMAGE_OFFSET;
		uint32_t ptrend   = ptrstart;
		if( ptrstart >= emu->ram_size )
		{
			printf( "DEBUG PASSED INVALID PTR (%08x)\n", value );
			return;
		}
		while( ptrend < emu->ram_size && emu->ram[ptrend] != 0 )
			ptrend++;
		if( ptrend != ptrstart )
			fwrite( emu->ram + ptrstart, ptrend - ptrstart, 1, stdout );
	}
	else if( csrno == 0x139 )
	{
		putchar( value ); fflush( stdout );
	}
}

static int32_t HandleOtherCSRRead( struct EmulatorState * emu, uint16_t csrno )
{
	(void)emu;
	if( csrno == 0x140 )
	{
		if( !IsKBHit() ) return -1;
		return ReadKBByte();
	}
	return 0;
}

//////////////////////////////////////////////////////////////////////////////
// Memory helpers
//////////////////////////////////////////////////////////////////////////////

static inline uint8_t  mem_load1 ( uint8_t * image, uint32_t ofs ) { return *(uint8_t  *)(image + ofs); }
static inline uint16_t mem_load2 ( uint8_t * image, uint32_t ofs ) { return *(uint16_t *)(image + ofs); }
static inline uint32_t mem_load4 ( uint8_t * image, uint32_t ofs ) { return *(uint32_t *)(image + ofs); }
static inline int8_t   mem_load1s( uint8_t * image, uint32_t ofs ) { return *(int8_t   *)(image + ofs); }
static inline int16_t  mem_load2s( uint8_t * image, uint32_t ofs ) { return *(int16_t  *)(image + ofs); }
static inline void mem_store1( uint8_t * image, uint32_t ofs, uint32_t val ) { *(uint8_t  *)(image + ofs) = val; }
static inline void mem_store2( uint8_t * image, uint32_t ofs, uint32_t val ) { *(uint16_t *)(image + ofs) = val; }
static inline void mem_store4( uint8_t * image, uint32_t ofs, uint32_t val ) { *(uint32_t *)(image + ofs) = val; }

//////////////////////////////////////////////////////////////////////////////
// Immediate decode — one function per RISC-V instruction format
//
// Every RV32I instruction encodes its immediate in one of these five layouts.
// Centralizing the bit manipulation here means:
//   - execute logic reads as intent, not as bit twiddling
//   - adding new instructions only requires picking the right format
//   - porting to another language means translating 5 functions, not N cases
//
// All functions return a sign-extended int32_t matching the spec.
//////////////////////////////////////////////////////////////////////////////

// Format I: loads, JALR, OP-IMM   [31:20]
static inline int32_t decode_imm_I( uint32_t ir )
{
	uint32_t imm = ir >> 20;
	return (int32_t)(imm | ((imm & 0x800) ? 0xfffff000 : 0));
}

// Format S: stores   [31:25|11:7]
static inline int32_t decode_imm_S( uint32_t ir )
{
	uint32_t imm = ((ir >> 7) & 0x1f) | ((ir & 0xfe000000) >> 20);
	return (int32_t)(imm | ((imm & 0x800) ? 0xfffff000 : 0));
}

// Format B: branches   [31|7|30:25|11:8] (multiples of 2, bit 0 always 0)
static inline int32_t decode_imm_B( uint32_t ir )
{
	uint32_t imm = ((ir & 0xf00)      >>  7)
	             | ((ir & 0x7e000000)  >> 20)
	             | ((ir & 0x80)        <<  4)
	             | ((ir >> 31)         << 12);
	return (int32_t)(imm | ((imm & 0x1000) ? 0xffffe000 : 0));
}

// Format J: JAL   [31|19:12|20|30:21] (multiples of 2, bit 0 always 0)
static inline int32_t decode_imm_J( uint32_t ir )
{
	uint32_t imm = ((ir & 0x80000000) >> 11)
	             | ((ir & 0x7fe00000) >> 20)
	             | ((ir & 0x00100000) >>  9)
	             |  (ir & 0x000ff000);
	return (int32_t)(imm | ((imm & 0x00100000) ? 0xffe00000 : 0));
}

// Format U: LUI, AUIPC   [31:12] (lower 12 bits always zero)
static inline int32_t decode_imm_U( uint32_t ir )
{
	return (int32_t)(ir & 0xfffff000);
}

//////////////////////////////////////////////////////////////////////////////
// CPU step
//////////////////////////////////////////////////////////////////////////////

static RVStepResult MiniRV32IMAStep( struct EmulatorState * emu, uint32_t elapsedUs, int count )
{
	struct MiniRV32IMAState * state = &emu->cpu;
	uint8_t  * image    = emu->ram;
	uint32_t   ram_size = emu->ram_size;

	// Advance timer
	uint32_t new_timer = state->timerl + elapsedUs;
	if( new_timer < state->timerl ) state->timerh++;
	state->timerl = new_timer;

	// Fire or clear timer interrupt (MTIP)
	if( ( state->timerh > state->timermatchh ||
	    ( state->timerh == state->timermatchh && state->timerl > state->timermatchl ) )
	    && ( state->timermatchh || state->timermatchl ) )
	{
		state->extraflags &= ~4;
		state->mip |= 1 << 7;
	}
	else
		state->mip &= ~(1 << 7);

	// WFI: processor sleeping
	if( state->extraflags & 4 )
		return RV_STEP_WFI;

	uint32_t trap  = 0;
	uint32_t rval  = 0;
	uint32_t pc    = state->pc;
	uint32_t cycle = state->cyclel;

	// Pending timer interrupt?
	if( (state->mip & (1<<7)) && (state->mie & (1<<7)) && (state->mstatus & 0x8) )
	{
		trap = 0x80000007;
		pc  -= 4;
	}
	else
	for( int icount = 0; icount < count; icount++ )
	{
		uint32_t ir   = 0;
		rval  = 0;
		cycle++;
		uint32_t ofs_pc = pc - RAM_IMAGE_OFFSET;

		if( ofs_pc >= ram_size )
		{
			trap = 2; // instruction access fault
			break;
		}
		else if( ofs_pc & 3 )
		{
			trap = 1; // instruction address misaligned
			break;
		}

		ir = mem_load4( image, ofs_pc );
		uint32_t rdid = (ir >> 7) & 0x1f;

		switch( ir & 0x7f )
		{
			case 0x37: // LUI
				rval = decode_imm_U( ir );
				break;

			case 0x17: // AUIPC
				rval = pc + decode_imm_U( ir );
				break;

			case 0x6f: // JAL
			{
				int32_t offset = decode_imm_J( ir );
				rval = pc + 4;
				pc   = pc + offset - 4;
				break;
			}

			case 0x67: // JALR
			{
				int32_t imm_se = decode_imm_I( ir );
				rval = pc + 4;
				pc   = ((state->regs[(ir >> 15) & 0x1f] + imm_se) & ~1) - 4;
				break;
			}

			case 0x63: // Branch
			{
				int32_t  offset = decode_imm_B( ir );
				int32_t  rs1    = state->regs[(ir >> 15) & 0x1f];
				int32_t  rs2    = state->regs[(ir >> 20) & 0x1f];
				uint32_t target = pc + offset - 4;
				rdid = 0;
				switch( (ir >> 12) & 0x7 )
				{
					case 0: if(  rs1 ==  rs2 )                    pc = target; break; // BEQ
					case 1: if(  rs1 !=  rs2 )                    pc = target; break; // BNE
					case 4: if(  rs1  <  rs2 )                    pc = target; break; // BLT
					case 5: if(  rs1 >=  rs2 )                    pc = target; break; // BGE
					case 6: if( (uint32_t)rs1  < (uint32_t)rs2 )  pc = target; break; // BLTU
					case 7: if( (uint32_t)rs1 >= (uint32_t)rs2 )  pc = target; break; // BGEU
					default: trap = 2;
				}
				break;
			}

			case 0x03: // Load
			{
				int32_t  imm_se = decode_imm_I( ir );
				uint32_t addr   = state->regs[(ir >> 15) & 0x1f] + imm_se;
				uint32_t ofs    = addr - RAM_IMAGE_OFFSET;

				if( ofs >= ram_size - 3 )
				{
					if( is_mmio(addr) )
						rval = HandleControlLoad( emu, addr );
					else { trap = 5; rval = addr; } // load access fault
				}
				else
				{
					switch( (ir >> 12) & 0x7 )
					{
						case 0: rval = mem_load1s( image, ofs ); break; // LB
						case 1: rval = mem_load2s( image, ofs ); break; // LH
						case 2: rval = mem_load4 ( image, ofs ); break; // LW
						case 4: rval = mem_load1 ( image, ofs ); break; // LBU
						case 5: rval = mem_load2 ( image, ofs ); break; // LHU
						default: trap = 2;
					}
				}
				break;
			}

			case 0x23: // Store
			{
				int32_t  imm  = decode_imm_S( ir );
				uint32_t rs1  = state->regs[(ir >> 15) & 0x1f];
				uint32_t rs2  = state->regs[(ir >> 20) & 0x1f];
				uint32_t addr = rs1 + imm;
				uint32_t ofs  = addr - RAM_IMAGE_OFFSET;
				rdid = 0;

				if( ofs >= ram_size - 3 )
				{
					if( is_mmio(addr) )
					{
						RVStepResult sr = HandleControlStore( emu, addr, rs2 );
						if( sr != RV_STEP_OK ) return sr;
					}
					else { trap = 7; rval = addr; } // store access fault
				}
				else
				{
					switch( (ir >> 12) & 0x7 )
					{
						case 0: mem_store1( image, ofs, rs2 ); break; // SB
						case 1: mem_store2( image, ofs, rs2 ); break; // SH
						case 2: mem_store4( image, ofs, rs2 ); break; // SW
						default: trap = 2;
					}
				}
				break;
			}

			case 0x13: // OP-IMM
			case 0x33: // OP
			{
				int32_t  imm    = decode_imm_I( ir );
				uint32_t rs1    = state->regs[(ir >> 15) & 0x1f];
				int      is_reg = !!(ir & 0x20);
				uint32_t rs2    = is_reg ? state->regs[imm & 0x1f] : (uint32_t)imm;

				if( is_reg && (ir & 0x02000000) ) // RV32M
				{
					switch( (ir >> 12) & 0x7 )
					{
						case 0: rval = rs1 * rs2; break; // MUL
						case 1: rval = ((int64_t)(int32_t)rs1 * (int64_t)(int32_t)rs2) >> 32; break; // MULH
						case 2: rval = ((int64_t)(int32_t)rs1 * (uint64_t)rs2)         >> 32; break; // MULHSU
						case 3: rval = ((uint64_t)rs1 * (uint64_t)rs2)                 >> 32; break; // MULHU
						case 4: rval = (rs2 == 0) ? (uint32_t)-1   : ((int32_t)rs1 == INT32_MIN && (int32_t)rs2 == -1) ? rs1 : (uint32_t)((int32_t)rs1 / (int32_t)rs2); break; // DIV
						case 5: rval = (rs2 == 0) ? 0xffffffff     : rs1 / rs2; break; // DIVU
						case 6: rval = (rs2 == 0) ? rs1            : ((int32_t)rs1 == INT32_MIN && (int32_t)rs2 == -1) ? 0 : (uint32_t)((int32_t)rs1 % (int32_t)rs2); break; // REM
						case 7: rval = (rs2 == 0) ? rs1            : rs1 % rs2; break; // REMU
					}
				}
				else
				{
					switch( (ir >> 12) & 0x7 )
					{
						case 0: rval = (is_reg && (ir & 0x40000000)) ? (rs1 - rs2) : (rs1 + rs2); break; // ADD/SUB
						case 1: rval = rs1 << (rs2 & 0x1f); break; // SLL
						case 2: rval = (int32_t)rs1 < (int32_t)rs2; break; // SLT
						case 3: rval = rs1 < rs2; break; // SLTU
						case 4: rval = rs1 ^ rs2; break; // XOR
						case 5: rval = (ir & 0x40000000) ? ((int32_t)rs1 >> (rs2 & 0x1f)) : (rs1 >> (rs2 & 0x1f)); break; // SRL/SRA
						case 6: rval = rs1 | rs2; break; // OR
						case 7: rval = rs1 & rs2; break; // AND
					}
				}
				break;
			}

			case 0x0f: // FENCE — no-op in this impl
				rdid = 0;
				break;

			case 0x73: // SYSTEM (Zicsr + privileged)
			{
				uint32_t csrno   = ir >> 20;
				uint32_t microop = (ir >> 12) & 0x7;

				if( microop & 3 ) // Zicsr
				{
					int      rs1imm   = (ir >> 15) & 0x1f;
					uint32_t rs1      = state->regs[rs1imm];
					uint32_t writeval = rs1;

					switch( csrno )
					{
						case 0x340: rval = state->mscratch; break;
						case 0x305: rval = state->mtvec;    break;
						case 0x304: rval = state->mie;      break;
						case 0xc00: rval = cycle;           break;
						case 0x344: rval = state->mip;      break;
						case 0x341: rval = state->mepc;     break;
						case 0x300: rval = state->mstatus;  break;
						case 0x342: rval = state->mcause;   break;
						case 0x343: rval = state->mtval;    break;
						case 0xf11: rval = 0xff0ff0ff;      break; // mvendorid
						case 0x301: rval = 0x40401101;      break; // misa: RV32IMA
						default:    rval = HandleOtherCSRRead( emu, csrno ); break;
					}

					switch( microop )
					{
						case 1: writeval = rs1;            break; // CSRRW
						case 2: writeval = rval |  rs1;    break; // CSRRS
						case 3: writeval = rval & ~rs1;    break; // CSRRC
						case 5: writeval = rs1imm;         break; // CSRRWI
						case 6: writeval = rval |  rs1imm; break; // CSRRSI
						case 7: writeval = rval & ~rs1imm; break; // CSRRCI
					}

					switch( csrno )
					{
						case 0x340: state->mscratch = writeval; break;
						case 0x305: state->mtvec    = writeval; break;
						case 0x304: state->mie      = writeval; break;
						case 0x344: state->mip      = writeval; break;
						case 0x341: state->mepc     = writeval; break;
						case 0x300: state->mstatus  = writeval; break;
						case 0x342: state->mcause   = writeval; break;
						case 0x343: state->mtval    = writeval; break;
						default:    HandleOtherCSRWrite( emu, csrno, writeval ); break;
					}
				}
				else if( microop == 0 ) // SYSTEM
				{
					rdid = 0;
					if( (csrno & 0xff) == 0x02 ) // MRET
					{
						uint32_t ms = state->mstatus;
						uint32_t ef = state->extraflags;
						state->mstatus    = ((ms & 0x80) >> 4) | ((ef & 3) << 11) | 0x80;
						state->extraflags = (ef & ~3) | ((ms >> 11) & 3);
						pc = state->mepc - 4;
					}
					else
					{
						switch( csrno )
						{
							case 0x000: trap = (state->extraflags & 3) ? 12 : 9; break; // ECALL
							case 0x001: trap = 4; break; // EBREAK
							case 0x105: // WFI
								state->mstatus    |= 8;
								state->extraflags |= 4;
								if( state->cyclel > cycle ) state->cycleh++;
								state->cyclel = cycle;
								state->pc     = pc + 4;
								return RV_STEP_WFI;
							default: trap = 2; // illegal
						}
					}
				}
				else
					trap = 2; // illegal
				break;
			}

			case 0x2f: // RV32A
			{
				uint32_t rs1   = state->regs[(ir >> 15) & 0x1f];
				uint32_t rs2   = state->regs[(ir >> 20) & 0x1f];
				uint32_t irmid = (ir >> 27) & 0x1f;
				uint32_t ofs   = rs1 - RAM_IMAGE_OFFSET;

				if( ofs >= ram_size - 3 )
				{
					trap = 7; // store/AMO access fault
					rval = rs1;
				}
				else
				{
					rval = mem_load4( image, ofs );
					int dowrite = 1;
					switch( irmid )
					{
						case 2:  dowrite = 0; state->extraflags = (state->extraflags & 0x07) | (ofs << 3); break; // LR.W
						case 3:  rval = (state->extraflags >> 3) != (ofs & 0x1fffffff); dowrite = !rval;    break; // SC.W
						case 1:  break;                                                                            // AMOSWAP.W
						case 0:  rs2 += rval; break;                                                               // AMOADD.W
						case 4:  rs2 ^= rval; break;                                                               // AMOXOR.W
						case 12: rs2 &= rval; break;                                                               // AMOAND.W
						case 8:  rs2 |= rval; break;                                                               // AMOOR.W
						case 16: rs2 = ((int32_t)rs2 < (int32_t)rval) ? rs2 : rval; break;                        // AMOMIN.W
						case 20: rs2 = ((int32_t)rs2 > (int32_t)rval) ? rs2 : rval; break;                        // AMOMAX.W
						case 24: rs2 = (rs2 < rval) ? rs2 : rval; break;                                          // AMOMINU.W
						case 28: rs2 = (rs2 > rval) ? rs2 : rval; break;                                          // AMOMAXU.W
						default: trap = 2; dowrite = 0; break;
					}
					if( dowrite ) mem_store4( image, ofs, rs2 );
				}
				break;
			}

			default:
				trap = 2; // illegal opcode
		}

		if( trap )
		{
			state->pc = pc;
			if( emu->fail_on_all_faults ) { printf( "FAULT\n" ); return RV_STEP_FAULT; }
			break;
		}

		if( rdid )
			state->regs[rdid] = rval;

		pc += 4;
	}

	// Handle traps and interrupts
	if( trap )
	{
		if( trap & 0x80000000 ) // interrupt
		{
			state->mcause = trap;
			state->mtval  = 0;
			pc += 4;
		}
		else // exception
		{
			state->mcause = trap - 1;
			state->mtval  = (trap > 5 && trap <= 8) ? rval : pc;
		}
		state->mepc    = pc;
		state->mstatus = ((state->mstatus & 0x08) << 4) | ((state->extraflags & 3) << 11);
		pc = state->mtvec - 4;
		state->extraflags |= 3; // enter machine mode
		pc += 4;
	}

	if( state->cyclel > cycle ) state->cycleh++;
	state->cyclel = cycle;
	state->pc     = pc;
	return RV_STEP_OK;
}

//////////////////////////////////////////////////////////////////////////////
// Debug
//////////////////////////////////////////////////////////////////////////////

static void DumpState( struct EmulatorState * emu )
{
	struct MiniRV32IMAState * core = &emu->cpu;
	uint32_t pc        = core->pc;
	uint32_t pc_offset = pc - RAM_IMAGE_OFFSET;
	uint32_t ir        = 0;

	printf( "PC: %08x ", pc );
	if( pc_offset < emu->ram_size - 3 )
	{
		ir = mem_load4( emu->ram, pc_offset );
		printf( "[0x%08x] ", ir );
	}
	else
		printf( "[xxxxxxxxxx] " );

	uint32_t * r = core->regs;
	printf( "Z:%08x ra:%08x sp:%08x gp:%08x tp:%08x t0:%08x t1:%08x t2:%08x "
	        "s0:%08x s1:%08x a0:%08x a1:%08x a2:%08x a3:%08x a4:%08x a5:%08x ",
	        r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],r[8],r[9],r[10],r[11],r[12],r[13],r[14],r[15] );
	printf( "a6:%08x a7:%08x s2:%08x s3:%08x s4:%08x s5:%08x s6:%08x s7:%08x "
	        "s8:%08x s9:%08x s10:%08x s11:%08x t3:%08x t4:%08x t5:%08x t6:%08x\n",
	        r[16],r[17],r[18],r[19],r[20],r[21],r[22],r[23],r[24],r[25],r[26],r[27],r[28],r[29],r[30],r[31] );
}

//////////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////////

static int64_t SimpleReadNumberInt( const char * number, int64_t defaultNumber )
{
	if( !number || !number[0] ) return defaultNumber;
	int radix = 10;
	if( number[0] == '0' )
	{
		char nc = number[1];
		number += 2;
		if( nc == 0 )        return 0;
		else if( nc == 'x' ) radix = 16;
		else if( nc == 'b' ) radix = 2;
		else { number--; radix = 8; }
	}
	char * endptr;
	uint64_t ret = strtoll( number, &endptr, radix );
	return (endptr == number) ? defaultNumber : (int64_t)ret;
}

//////////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////////

int main( int argc, char ** argv )
{
	int          i;
	long long    instct           = -1;
	int          show_help        = 0;
	int          time_divisor     = 1;
	int          fixed_update     = 0;
	int          do_sleep         = 1;
	int          single_step      = 0;
	int          dtb_ptr          = 0;
	uint32_t     ram_size         = 64 * 1024 * 1024;
	const char * image_file_name  = 0;
	const char * dtb_file_name    = 0;

	struct EmulatorState emu;
	memset( &emu, 0, sizeof(emu) );
	emu.fail_on_all_faults = 0;

	for( i = 1; i < argc; i++ )
	{
		const char * param = argv[i];
		int param_continue = 0;
		do
		{
			if( param[0] == '-' || param_continue )
			{
				switch( param[1] )
				{
					case 'm': if( ++i < argc ) ram_size           = SimpleReadNumberInt( argv[i], ram_size ); break;
					case 'c': if( ++i < argc ) instct             = SimpleReadNumberInt( argv[i], -1 );       break;
					case 'k': if( ++i < argc ) kernel_command_line = argv[i];                                 break;
					case 'f': image_file_name  = (++i < argc) ? argv[i] : 0;                                 break;
					case 'b': dtb_file_name    = (++i < argc) ? argv[i] : 0;                                 break;
					case 'l': param_continue = 1; fixed_update           = 1; break;
					case 'p': param_continue = 1; do_sleep                = 0; break;
					case 's': param_continue = 1; single_step             = 1; break;
					case 'd': param_continue = 1; emu.fail_on_all_faults  = 1; break;
					case 't': if( ++i < argc ) time_divisor = SimpleReadNumberInt( argv[i], 1 ); break;
					default:
						if( param_continue ) param_continue = 0;
						else show_help = 1;
						break;
				}
			}
			else { show_help = 1; break; }
			param++;
		} while( param_continue );
	}

	if( show_help || image_file_name == 0 || time_divisor <= 0 )
	{
		fprintf( stderr,
			"./mini-rv32ima [parameters]\n"
			"\t-m [ram amount]\n"
			"\t-f [running image]\n"
			"\t-k [kernel command line]\n"
			"\t-b [dtb file, or 'disable']\n"
			"\t-c [instruction count]\n"
			"\t-s  single step with full processor state\n"
			"\t-t [time division base]\n"
			"\t-l  lock time base to instruction count\n"
			"\t-p  disable sleep when wfi\n"
			"\t-d  fail immediately on all faults\n" );
		return 1;
	}

	emu.ram_size = ram_size;
	emu.ram      = malloc( ram_size );
	if( !emu.ram )
	{
		fprintf( stderr, "Error: could not allocate system image.\n" );
		return -4;
	}

	g_emu = &emu;

restart:
	{
		FILE * f = fopen( image_file_name, "rb" );
		if( !f || ferror(f) )
		{
			fprintf( stderr, "Error: \"%s\" not found\n", image_file_name );
			return -5;
		}
		fseek( f, 0, SEEK_END );
		long flen = ftell( f );
		fseek( f, 0, SEEK_SET );
		if( (uint32_t)flen > ram_size )
		{
			fprintf( stderr, "Error: image (%ld bytes) does not fit in %u bytes of RAM\n", flen, ram_size );
			return -6;
		}
		memset( emu.ram, 0, ram_size );
		if( fread( emu.ram, flen, 1, f ) != 1 )
		{
			fprintf( stderr, "Error: could not load image.\n" );
			return -7;
		}
		fclose( f );

		if( dtb_file_name )
		{
			if( strcmp( dtb_file_name, "disable" ) != 0 )
			{
				f = fopen( dtb_file_name, "rb" );
				if( !f || ferror(f) )
				{
					fprintf( stderr, "Error: \"%s\" not found\n", dtb_file_name );
					return -5;
				}
				fseek( f, 0, SEEK_END );
				long dtblen = ftell( f );
				fseek( f, 0, SEEK_SET );
				dtb_ptr = ram_size - dtblen - sizeof( struct MiniRV32IMAState );
				if( fread( emu.ram + dtb_ptr, dtblen, 1, f ) != 1 )
				{
					fprintf( stderr, "Error: could not load dtb \"%s\"\n", dtb_file_name );
					return -9;
				}
				fclose( f );
			}
		}
		else
		{
			dtb_ptr = ram_size - sizeof(default64mbdtb) - sizeof( struct MiniRV32IMAState );
			memcpy( emu.ram + dtb_ptr, default64mbdtb, sizeof(default64mbdtb) );
			if( kernel_command_line )
				strncpy( (char*)(emu.ram + dtb_ptr + 0xc0), kernel_command_line, 54 );
		}
	}

	CaptureKeyboardInput();

	// CPU lives at the very end of RAM
	memset( &emu.cpu, 0, sizeof(emu.cpu) );
	emu.cpu.pc          = RAM_IMAGE_OFFSET;
	emu.cpu.regs[10]    = 0x00;                                          // hart ID
	emu.cpu.regs[11]    = dtb_ptr ? (dtb_ptr + RAM_IMAGE_OFFSET) : 0;   // dtb pointer
	emu.cpu.extraflags |= 3;                                             // machine mode

	if( dtb_file_name == 0 )
	{
		// Patch default DTB with actual usable RAM size
		uint32_t * dtb = (uint32_t *)(emu.ram + dtb_ptr);
		if( dtb[0x13c/4] == 0x00c0ff03 )
		{
			uint32_t v = dtb_ptr;
			dtb[0x13c/4] = (v>>24) | (((v>>16)&0xff)<<8) | (((v>>8)&0xff)<<16) | ((v&0xff)<<24);
		}
	}

	uint64_t lastTime        = fixed_update ? 0 : (GetTimeMicroseconds() / time_divisor);
	int      instrs_per_flip = single_step ? 1 : 1024;

	for( uint64_t rt = 0; rt < (uint64_t)(instct + 1) || instct < 0; rt += instrs_per_flip )
	{
		uint64_t * this_ccount = (uint64_t *)&emu.cpu.cyclel;
		uint32_t   elapsedUs   = fixed_update
		                       ? (uint32_t)(*this_ccount / time_divisor - lastTime)
		                       : (uint32_t)(GetTimeMicroseconds() / time_divisor - lastTime);
		lastTime += elapsedUs;

		if( single_step ) DumpState( &emu );

		RVStepResult ret = MiniRV32IMAStep( &emu, elapsedUs, instrs_per_flip );
		switch( ret )
		{
			case RV_STEP_OK:       break;
			case RV_STEP_WFI:      if( do_sleep ) MiniSleep(); *this_ccount += instrs_per_flip; break;
			case RV_STEP_FAULT:    instct = 0; break;
			case RV_STEP_RESTART:  goto restart;
			case RV_STEP_POWEROFF: printf( "POWEROFF@0x%08x%08x\n", emu.cpu.cycleh, emu.cpu.cyclel ); return 0;
			default:               printf( "Unknown failure\n" ); break;
		}
	}

	DumpState( &emu );
	return 0;
}

//////////////////////////////////////////////////////////////////////////////
// Platform: Windows
//////////////////////////////////////////////////////////////////////////////

#if defined(WINDOWS) || defined(WIN32) || defined(_WIN32)

#include <windows.h>
#include <conio.h>

#define strtoll _strtoi64

static void CaptureKeyboardInput() { system(""); }
static void ResetKeyboardInput()   {}
static void MiniSleep()            { Sleep(1); }

static uint64_t GetTimeMicroseconds()
{
	static LARGE_INTEGER lpf;
	LARGE_INTEGER li;
	if( !lpf.QuadPart ) QueryPerformanceFrequency( &lpf );
	QueryPerformanceCounter( &li );
	return ((uint64_t)li.QuadPart * 1000000ULL) / (uint64_t)lpf.QuadPart;
}

static int IsKBHit() { return _kbhit(); }

static int ReadKBByte()
{
	static int is_escape_sequence = 0;
	if( is_escape_sequence == 1 ) { is_escape_sequence++; return '['; }
	int r = _getch();
	if( is_escape_sequence )
	{
		is_escape_sequence = 0;
		switch( r )
		{
			case 'H': return 'A'; case 'P': return 'B';
			case 'K': return 'D'; case 'M': return 'C';
			case 'G': return 'H'; case 'O': return 'F';
			default:  return r;
		}
	}
	switch( r )
	{
		case 13:  return 10;
		case 224: is_escape_sequence = 1; return 27;
		default:  return r;
	}
}

//////////////////////////////////////////////////////////////////////////////
// Platform: POSIX
//////////////////////////////////////////////////////////////////////////////

#else

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

static void CtrlC( int sig ) { (void)sig; if( g_emu ) DumpState( g_emu ); exit(0); }

static void CaptureKeyboardInput()
{
	atexit( ResetKeyboardInput );
	signal( SIGINT, CtrlC );
	struct termios term;
	tcgetattr( 0, &term );
	term.c_lflag &= ~(ICANON | ECHO);
	tcsetattr( 0, TCSANOW, &term );
}

static void ResetKeyboardInput()
{
	struct termios term;
	tcgetattr( 0, &term );
	term.c_lflag |= ICANON | ECHO;
	tcsetattr( 0, TCSANOW, &term );
}

static void MiniSleep() { usleep(500); }

static uint64_t GetTimeMicroseconds()
{
	struct timeval tv;
	gettimeofday( &tv, 0 );
	return tv.tv_usec + (uint64_t)tv.tv_sec * 1000000ULL;
}

static int is_eofd = 0;

static int ReadKBByte()
{
	if( is_eofd ) return 0xffffffff;
	char c = 0;
	return (read( fileno(stdin), &c, 1 ) > 0) ? (int)c : -1;
}

static int IsKBHit()
{
	if( is_eofd ) return -1;
	int n = 0;
	ioctl( 0, FIONREAD, &n );
	if( !n && write( fileno(stdin), 0, 0 ) != 0 ) { is_eofd = 1; return -1; }
	return !!n;
}

#endif