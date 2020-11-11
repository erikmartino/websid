/*
* Emulates MOS Technology 6510 CPU - as far as needed to play RSID files.
*
* Implementation was originally based on the code from the old TinySid, but
* it has meanwhile been rewritten almost completely:
*
*    - added impls for illegal 6510 op codes, fixed errors in V-flag calculation,
*      added handling for 6510 addressing "bugs"
*    - correct total cycle-time calculation for ALL (used) 6510 OP codes
*    - timing of OP internal "bus write" for badline handling
*    - switched from a "per OP" based stepping to a approximated "per clock" mode
*      (as a base for "badline" handling, etc)
*
* Interactions with external components (VIA, CIA, SID) are emulated on a cycle-
* by-cycle level, however there may be imprecisions with regard to intra-OP
* timing (see  "SUMMARY OF SINGLE CYCLE EXECUTION" in "MOS MCS6500 hardware
* manual.TXT"), e.g. if a 4 cycle OP would actually perform some read in its 3rd
* cycle it is emulated here as if it were to do everything at the end in cycle 4.
*
*
* LIMITATIONS:
*  - No real sub-instruction accuracy NOR pipeline (overlapping OPs) handling
*    but some (hopefully) "good enough" approximation. Delays regarding when
*    updated data will be visible on the bus (and to which component) may not
*    be correctly modeled for all special cases.
*  - flag handling in BCD mode is not implemented (see
*    http://www.oxyron.de/html/opcodes02.html)
*
*
* HW clock timing info:
*
* Based on the �0 clock delivered by VIC, the CPU generates the two output
* clocks �1 and �2. The respective phase width and rise/fall times vary slightly
* resulting in something like this:
*
* �0 ������\_______________/���������\___________
*
* �1 __________/���������\_______________/�������
*
* �2 ��������\_______________/���������\_________
*
* It seems that �2 is a "delayed" version of the input �0 whereas �1 is an
* "inverted version" of �0 (and �2). On sub-cycle level �1 and �2 are not
* completely in sync. While the CPU is clocked with �2 all the other
* components are clocked with �1.
*
* From a simplistic software point of view, the above can be thought of as each
* "system clock" having 2 phases: where the 1st phase is always used by "the
* other components" and the CPU (usually) uses phase 2.
*
*
* Code structure & performance:
*
* This file still uses the old switch/case approach from TinySID to implement
* the various CPU operations. The code would certainly benefit a lot from some
* added structuring elements. However, riddiculous as it may seem, the WebAssembly
* implementations of certain browsers (e.g. Google Chrome) are significantly slowed
* down by additional function call nesting (it doesn't seem to affect Firefox that
* much though). It actually seems to be useful to remove certain function calls and
* and instead inline the code via macros. Eventhough high-end mobile phones have
* become fast enough in the past couple of years to run the emulator without a
* hitch, there are quite a few users with somewhat older hardware that is already
* struggling with the CPU load of the current impl. I am therefore reluctant
* to touch the code just now.
*
*
* REMINDER / CAUTION!!!
*
* Something seems to be seriously fucked-up in the C or Emscripten infrastructure
* and maybe it is a problem specific to blocks used in "case" statements. In any
* case this can lead to surprising phantom bugs, e.g.:
*
*     case foo: {
*	  // tmp= _x & _y;
*        someFunc(_x & _y);
*     }
* The argument passed to someFunc will be total garbage unless the unused tmp
* variable is previously assigned with the same expression. WTF?!
*
*
* useful links/docs:
*
*  http://www.oxyron.de/html/opcodes02.html
*  http://6502.org/tutorials/interrupts.html
*  http://www.zimmers.net/anonftp/pub/cbm/documents/chipdata/64doc
*  http://archive.6502.org/datasheets/mos_6500_mpu_preliminary_may_1976.pdf
*  https://wiki.nesdev.com/w/index.php/CPU_interrupts
*
* WebSid (c) 2020 J�rgen Wothke
* version 0.94
*
* Terms of Use: This software is licensed under a CC BY-NC-SA
* (http://creativecommons.org/licenses/by-nc-sa/4.0/).
*/

#include <string.h>
#include <stdio.h>

#include <emscripten.h>

#include "cpu.h"
#include "system.h"

// for interrupt handling
#include "vic.h"
#include "cia.h"

#include "memory.h"
#include "memory_opt.h"

#ifdef TEST
uint8_t test_running = 0;
#endif

// ----------- CPU state -----------

static uint16_t _pc;			// program counter
static uint8_t _a, _x, _y;		// accumulator & x,y registers
static uint8_t _s; 				// stack pointer

#define FLAG_N 128
#define FLAG_V 64
#define FLAG_B1 32
#define FLAG_B0 16
#define FLAG_D 8
#define FLAG_I 4
#define FLAG_Z 2
#define FLAG_C 1

static uint8_t _p;				// status register (see above flags)

#define IRQ_LEAD_DEFAULT 2

// compiler used by EMSCRIPTEN is actually too dumb to even use
// "inline" for the below local function!

#define SETFLAGS(flag, cond) \
    if (cond) _p |= (int32_t)flag;\
    else _p &= ~(int32_t)flag;


// ---- interrupt handling ---

// "Many references will claim that interrupts are polled during the last
// cycle of an instruction, but this is true only when talking about the
// output from the edge and level detectors. As can be deduced from above,
// it's really the status of the interrupt lines at the end of the second-to-last
// cycle that matters." (https://wiki.nesdev.com/w/index.php/CPU_interrupts)


// "When an interrupt occurs 2 or more cycles before the current command
// ends, it is executed immediately after the command. Otherwise, the CPU
// executes the next command first before it calls the interrupt handler.

// The only exception to this rule are "taken branches" to the same page
// which last 3 cycles. Here, the interrupt must have occurred before clock 1
// of the branch command; the normal rule says before clock 2. Branches to a
// different page or branches not taken are behaving normal."


// line "detectors" run in �2 phase and activate the respective internal
// signal in next �1, i.e. the next system clock cycle.
static uint8_t _interrupt_lead_time = IRQ_LEAD_DEFAULT;


// ---- CIA handling ---
static uint8_t _irq_committed = 0;	// CPU is committed to running the IRQ
static uint32_t _irq_line_ts = 0;

// #define TRACE_IRQ_TIMING

#ifdef TRACE_IRQ_TIMING
uint32_t _irq_start;
#endif


// how the 6502's pipline affects the handling of the I-flag:

// "The RTI instruction affects IRQ inhibition immediately. If an IRQ is
// pending and an RTI is executed that clears the I flag, the CPU will
// invoke the IRQ handler immediately after RTI finishes executing. This
// is due to RTI restoring the I flag from the stack before polling
// for interrupts.

// The CLI, SEI, and PLP instructions on the other hand change the I flag
// after polling for interrupts (like all two-cycle instructions they poll
// the interrupt lines at the end of the first cycle), meaning they can
// effectively delay an interrupt until after the next instruction. For
// example, if an interrupt is pending and the I flag is currently set,
// executing CLI will execute the next instruction before the CPU invokes
// the IRQ handler."

// as long as the IRQ line stays active new IRQ will trigger as soon
// as the I-flag is cleared .. let's presume that the I-flag masking
// is done initially and once committed it will no longer matter
// (like in the NMI case)


// test-case: Humphrey_Bogart.sid, Monster_Museum.sid

// IRQ will still trigger during 1st cycle of an SEI but be blocked during 2nd
// (_exe_instr_opcode is the previous - if completed - or the currently executed
// op-code. this SEI_OP check is flawed since FLAG_I might already have been set
// before the sei operation)
#define EXECUTING_SEI_LAST_CYCLE \
	((_exe_instr_opcode == SEI_OP ) && (_exe_instr_cycles_remain == 1))

// open issue: there should be special handling for RTI as well (i.e. RTI clears the FLAG_I
// early enough so that new IRQ triggered during RTI will be executed directly after the RTI.)
// however.. the test suite does not complain and an attempt to add respective logic has
// made things worse.. I must be overlooking something..
// used rule: FLAG_I must not be set - except if it's been set by a SEI no longer than 1 cycle ago..
#define CHECK_FOR_IRQ() \
	if ( ( (!(_p & FLAG_I)) && !EXECUTING_SEI_LAST_CYCLE ) \
			&& (vicIRQ() || ciaIRQ()) ) { \
		/* test-case: Vicious_SID_2-Escos (needs FLAG_I check)*/ \
	 \
		if (!_irq_line_ts) { \
			_irq_committed = 1;			/* there is no way back now */ \
			_irq_line_ts = sysCycles();	/* ts when line was activated */ \
		} \
	} else { \
		if (!_irq_committed) _irq_line_ts = 0;	/* IRQ flag really relevant here? (see mandatory check in IS_IRQ_PENDING()) */ \
	}


// relevant test suites: "irq", "imr"
// test-cases: Vaakataso.sid, Vicious_SID_2-Carmina_Burana.sid
//            depend on the FLAG_I re-test!

// default: require regular lead-time except in special case "sei"
// this check is done at beginning of new cycle after previous op has
// been completed (i.e. _exe_instr_opcode has been reset)
#define AFTER_SEI \
	(_opc == SEI_OP)

#define IS_IRQ_PENDING() \
	(_irq_committed ?  \
		( AFTER_SEI && ((sysCycles() - _irq_line_ts) >= 1) ) || \
			( !(_p & FLAG_I) && ((sysCycles() - _irq_line_ts) >= _interrupt_lead_time) )  \
		: 0) // test-case: "IMR"


	// ---- NMI handling ---

static uint8_t _nmi_committed = 0;		// CPU is committed to running the NMI
static uint8_t _nmi_line = 0;			// state change detection
static uint32_t _nmi_line_ts = 0;		// for scheduling


// when the CPU detects the "NMI line" activation it "commits" to
// running that NMI handler and no later state change will stop
// that NMI from being executed (see above for scheduling)

// test-case: "ICR01" ("READING ICR=81 MUST PASS NMI"): eventhough
// the NMI line is immediately acknowledged/cleared in the same
// cycle that the CIA sets it, the NMI handler should still be called.
#define CHECK_FOR_NMI() \
	if (ciaNMI()) {				/* NMI line is active now */\
	\
		/* NMI is different from IRQ in that only the transition from \
		   high to low signal triggers an NMI, and the line has to be \
		   restored to high (meaning "false" here) before the next NMI \
		   can trigger.*/ \
		\
		if (!_nmi_line) { \
			_nmi_committed = 1;			/* there is no way back now */ \
			_nmi_line = 1;				/* using 1 to model HW line "low" signal */ \
			_nmi_line_ts = sysCycles(); \
			 \
			/* "If both an NMI and an IRQ are pending at the end of an \
			   instruction, the NMI will be handled and the pending \
			   status of the IRQ forgotten (though it's likely to be \
			   detected again during later polling)." \
			 \
			_irq_committed= 0; \
			_irq_line_ts= 0; */ \
		} else { \
			/* line already/still activated... cannot trigger new  \
			   NMI before previous one has been acknowledged */ \
		} \
	} else { \
		_nmi_line = 0;			/* NMI has been acknowledged */ \
		if (!_nmi_committed) { \
			/* still needed until the committed NMI has been scheduled */ \
			_nmi_line_ts = 0; \
		} \
	}

#define IS_NMI_PENDING()\
	(_nmi_committed ? (sysCycles() - _nmi_line_ts) >= _interrupt_lead_time : 0)


uint8_t cpuIsValidPcPSID() {
	// only used to to run PSID INIT separately.. everything else
	// runs without this limitation

	// for RSIDs there is not really "any" invalid PC
	// test-case: Boot_Zak_v2.sid (uses $0000 for IRQ handler).
	return _pc > 1;
}

// MOS6510 instruction modes
#define imp 0
#define imm 1
#define abs 2
#define abx 3
#define aby 4
#define zpg 6
#define zpx 7
#define zpy 8
#define ind 9
#define idx 10
#define idy 11
#define acc 12
#define rel 13

// enum of all mnemonic codes of the MOS6510 operations
enum {
	adc, alr, anc, and, ane, arr, asl, bcc, bcs, beq, bit, bmi, bne, bpl, brk, bvc,
	bvs, clc, cld, cli, clv, cmp, cpx, cpy, dcp, dec, dex, dey, eor, inc, inx, iny,
	isb, jam, jmp, jsr, lae, lax, lda, ldx, ldy, lsr, lxa, nop, ora, pha, php, pla,
	plp, rla, rol, ror, rra, rti, rts, sax, sbc, sbx, sec, sed, sei, sha, shs, shx,
	shy, slo, sre, sta, stx, sty, tax, tay, tsx, txa, txs, tya,
	// added pseudo OPs (replacing unusable JAM OPs):
	sti, stn, nul
};

// artificial OPs patched in the positions of unsable JAM ops to ease impl:

const static uint8_t START_IRQ_OP = 0x02;	// "sti" pseudo OP for interrupt handling
const static uint8_t START_NMI_OP = 0x12;	// "stn"             "
const static uint8_t NULL_OP = 0x22;		// "nul" means "empty main"

const static uint8_t SEI_OP = 0x78;			// regular opcode

static uint8_t _opc;						// last executed opcode


static const int32_t _mnemonics[256] = {
	brk,ora,sti,slo,nop,ora,asl,slo,php,ora,asl,anc,nop,ora,asl,slo,
	bpl,ora,stn,slo,nop,ora,asl,slo,clc,ora,nop,slo,nop,ora,asl,slo,
	jsr,and,nul,rla,bit,and,rol,rla,plp,and,rol,anc,bit,and,rol,rla,
	bmi,and,jam,rla,nop,and,rol,rla,sec,and,nop,rla,nop,and,rol,rla,
	rti,eor,jam,sre,nop,eor,lsr,sre,pha,eor,lsr,alr,jmp,eor,lsr,sre,
	bvc,eor,jam,sre,nop,eor,lsr,sre,cli,eor,nop,sre,nop,eor,lsr,sre,
	rts,adc,jam,rra,nop,adc,ror,rra,pla,adc,ror,arr,jmp,adc,ror,rra,
	bvs,adc,jam,rra,nop,adc,ror,rra,sei,adc,nop,rra,nop,adc,ror,rra,
	nop,sta,nop,sax,sty,sta,stx,sax,dey,nop,txa,ane,sty,sta,stx,sax,
	bcc,sta,jam,sha,sty,sta,stx,sax,tya,sta,txs,shs,shy,sta,shx,sha,
	ldy,lda,ldx,lax,ldy,lda,ldx,lax,tay,lda,tax,lxa,ldy,lda,ldx,lax,
	bcs,lda,jam,lax,ldy,lda,ldx,lax,clv,lda,tsx,lae,ldy,lda,ldx,lax,
	cpy,cmp,nop,dcp,cpy,cmp,dec,dcp,iny,cmp,dex,sbx,cpy,cmp,dec,dcp,
	bne,cmp,jam,dcp,nop,cmp,dec,dcp,cld,cmp,nop,dcp,nop,cmp,dec,dcp,
	cpx,sbc,nop,isb,cpx,sbc,inc,isb,inx,sbc,nop,sbc,cpx,sbc,inc,isb,
	beq,sbc,jam,isb,nop,sbc,inc,isb,sed,sbc,nop,isb,nop,sbc,inc,isb
};

static const int32_t _modes[256] = {
	imp,idx,abs,idx,zpg,zpg,zpg,zpg,imp,imm,acc,imm,abs,abs,abs,abs,
	rel,idy,abs,idy,zpx,zpx,zpx,zpx,imp,aby,imp,aby,abx,abx,abx,abx,
	abs,idx,imp,idx,zpg,zpg,zpg,zpg,imp,imm,acc,imm,abs,abs,abs,abs,
	rel,idy,imp,idy,zpx,zpx,zpx,zpx,imp,aby,imp,aby,abx,abx,abx,abx,
	imp,idx,imp,idx,zpg,zpg,zpg,zpg,imp,imm,acc,imm,abs,abs,abs,abs,
	rel,idy,imp,idy,zpx,zpx,zpx,zpx,imp,aby,imp,aby,abx,abx,abx,abx,
	imp,idx,imp,idx,zpg,zpg,zpg,zpg,imp,imm,acc,imm,ind,abs,abs,abs,
	rel,idy,imp,idy,zpx,zpx,zpx,zpx,imp,aby,imp,aby,abx,abx,abx,abx,
	imm,idx,imm,idx,zpg,zpg,zpg,zpg,imp,imm,imp,imm,abs,abs,abs,abs,
	rel,idy,imp,idy,zpx,zpx,zpy,zpy,imp,aby,imp,aby,abx,abx,aby,aby,
	imm,idx,imm,idx,zpg,zpg,zpg,zpg,imp,imm,imp,imm,abs,abs,abs,abs,
	rel,idy,imp,idy,zpx,zpx,zpy,zpy,imp,aby,imp,aby,abx,abx,aby,aby,
	imm,idx,imm,idx,zpg,zpg,zpg,zpg,imp,imm,imp,imm,abs,abs,abs,abs,
	rel,idy,imp,idy,zpx,zpx,zpx,zpx,imp,aby,imp,aby,abx,abx,abx,abx,
	imm,idx,imm,idx,zpg,zpg,zpg,zpg,imp,imm,imp,imm,abs,abs,abs,abs,
	rel,idy,imp,idy,zpx,zpx,zpx,zpx,imp,aby,imp,aby,abx,abx,abx,abx
};


// Cycles per operation (adjustments apply) - note: these timings
// only consider the time to the next OP but not the additional
// cycles at the OPs end that overlap with the fetching of the
// next OP (due to pipelining)!

static const int32_t _opbase_frame_cycles[256] = {
	7,6,7,8,3,3,5,5,3,2,2,2,4,4,6,6,
	2,5,7,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
	2,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,0,8,3,3,5,5,3,2,2,2,3,4,6,6,
	2,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,0,8,3,3,5,5,4,2,2,2,5,4,6,6,
	2,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7,
	2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
	2,6,0,6,4,4,4,4,2,5,2,5,5,5,5,5,
	2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
	2,5,0,5,4,4,4,4,2,4,2,4,4,4,4,4,
	2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
	2,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7,
	2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
	2,5,0,8,4,4,6,6,2,4,2,7,4,4,7,7
};

// Used to emulate "CPU stun" (by VIC): 0 for OPs that don't
// use "bus writes", else cycle (starting at 1) in which "write"
// is started (with exception of BRK/JSR these are then all the
// remaining steps of the OP)
// note: the relevant OPs are not affected by page boundary
// crossing, i.e. no further adjustments needed.

static const int32_t _opbase_write_cycle[256] = {
	3,0,3,7,0,0,4,4,3,0,0,0,0,0,5,5,
	0,0,3,7,0,0,5,5,0,0,0,6,0,0,6,6,
	4,0,0,7,0,0,4,4,0,0,0,0,0,0,5,5,
	0,0,0,7,0,0,5,5,0,0,0,6,0,0,6,6,
	0,0,0,7,0,0,4,4,3,0,0,0,0,0,5,5,
	0,0,0,7,0,0,5,5,0,0,0,6,0,0,6,6,
	0,0,0,7,0,0,4,4,0,0,0,0,0,0,5,5,
	0,0,0,7,0,0,5,5,0,0,0,6,0,0,6,6,
	0,6,0,6,3,3,3,3,0,0,0,0,4,4,4,4,
	0,6,0,0,4,4,4,4,0,5,0,0,0,5,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,7,0,0,4,4,0,0,0,0,0,0,5,5,
	0,0,0,7,0,0,5,5,0,0,0,6,0,0,6,6,
	0,0,0,7,0,0,4,4,0,0,0,0,0,0,5,5,
	0,0,0,7,0,0,5,5,0,0,0,6,0,0,6,6
};


// gets hi-byte of addr; used for some obscure illegal ops
static uint8_t getH1(const int32_t *mode) {
    static uint16_t ad;  // avoid reallocation
    switch(*mode) {
        case abs:
            return memGet(_pc + 1) + 1;
        case abx:
            ad = (memGet(_pc + 0) | (memGet(_pc + 1) << 8)) + _x;
            return (ad >> 8) + 1;
        case aby:
            ad = (memGet(_pc + 0) | (memGet(_pc + 1) << 8)) + _y;
            return (ad >> 8) + 1;
        case zpg:
			ad = memGet(_pc + 0);
            return (ad >> 8) + 1;
        case idx:
			// indexed indirect, e.g. LDA ($10,X)
            ad = memGet(_pc + 0) + _x;
            ad = memGet(ad & 0xff) | (memGet((ad + 1) & 0xff) << 8);
            return (ad >> 8) + 1;
        case idy:
			// indirect indexed, e.g. LDA ($20),Y
            ad = memGet(_pc + 0);
            ad = (memGet(ad) | (memGet((ad + 1) & 0xff) << 8)) + _y;
            return (ad >> 8) + 1;
    }
    return 0;
}

static uint8_t getInput(const int32_t *mode) {
	// reads all the bytes belonging to the operation and
	// advances the _pc accordingly

    static uint16_t ad;  // avoid reallocation

    switch(*mode) {
        case acc:
            return _a;
        case imp:
            return 0;
        case imm:
            return memGet(_pc++);
        case abs:
            ad = memGet(_pc++);
            ad |= memGet(_pc++) << 8;
            return memGet(ad);
        case abx:
            ad = memGet(_pc++);
            ad |= memGet(_pc++) << 8;
            return memGet(ad + _x);
       case aby:
            ad = memGet(_pc++);
            ad |= memGet(_pc++) << 8;
            return memGet(ad + _y);
        case zpg:
			ad = memGet(_pc++);
            return memGet(ad);
        case zpx:
            ad = memGet(_pc++) + _x;
			ad &=  0xff;
            return memGet(ad);
        case zpy:
            ad = memGet(_pc++) + _y;
			ad &= 0xff;
            return memGet(ad);
        case idx:
			// indexed indirect, e.g. LDA ($10,X)
            ad = memGet(_pc++) + _x;
            ad = memGet(ad & 0xff) | (memGet((ad + 1) & 0xff) << 8);
            return memGet(ad);
        case idy:
			// indirect indexed, e.g. LDA ($20),Y
            ad = memGet(_pc++);
            ad = (memGet(ad) | (memGet((ad + 1) & 0xff) << 8)) + _y;
            return memGet(ad);
    }
    return 0;
}

static void setOutput(const int32_t *mode, uint8_t val) {
	// note: only used after "getInput", i.e. the _pc
	// is already pointing to the next command

    static uint16_t ad;  // avoid reallocation
    switch(*mode) {
        case acc:
            _a = val;
            return;
        case abs:
            ad = memGet(_pc - 2) | (memGet(_pc - 1) << 8);
            memSet(ad, val);
            return;
        case abx:
            ad = (memGet(_pc - 2) | (memGet(_pc - 1) << 8)) + _x;
            memSet(ad, val);
            return;
        case aby:
            ad = (memGet(_pc - 2) | (memGet(_pc - 1) << 8)) + _y;
            memSet(ad, val);
            return;
        case zpg:
            ad = memGet(_pc - 1);
            memSet(ad, val);
            return;
        case zpx:
            ad = memGet(_pc - 1) + _x;
			ad &= 0xff;
            memSet(ad, val);
            return;
        case zpy:
            ad = memGet(_pc - 1) + _y;
			ad &= 0xff;
            memSet(ad, val);
            return;
        case idx:
			// indexed indirect, e.g. LDA ($10,X)
            ad = memGet(_pc - 1) + _x;
            ad = memGet(ad & 0xff) | (memGet((ad + 1) & 0xff) << 8);
			memSet(ad, val);
            return;
        case idy:
			// indirect indexed, e.g. LDA ($20),Y
            ad = memGet(_pc - 1);
            ad = (memGet(ad) | (memGet((ad + 1) & 0xff) << 8)) + _y;
			memSet(ad, val);
            return;
    }
}


#define ABS_INDEXED_ADDR(ad, ad2, reg) \
	ad = memGet((*pc)++); \
	ad |= memGet((*pc)++) <<8; \
	ad2 = ad + reg

#define CHECK_BOUNDARY_CROSSED(ad, ad2) \
	if ((ad2 & 0xff00) != (ad & 0xff00)) { \
		adjustment++; \
	}

#define INDIRECT_INDEXED_ADDR(ad, ad2) \
	ad = memGet((*pc)++); \
	ad2 = memGet(ad); \
	ad2 |= memGet((ad + 1) & 0xff) << 8; \
	ad = ad2 + _y;


static uint8_t adjustPageBoundaryCrossing(uint16_t* pc, int32_t mode) {

	// only relevant/called in abx/aby/idy mode and depending
	// on the operation some of these modes may not exist, e.g.
	// NOP: only exists in "abx" variant
	// LDX: only "aby"
	// LDY: only "abx"

    static uint16_t ad, ad2;  // avoid reallocation

	uint8_t adjustment = 0;

    switch(mode) {
        case abx:
			ABS_INDEXED_ADDR(ad, ad2, _x);
			CHECK_BOUNDARY_CROSSED(ad, ad2)
			break;
        case aby:
			ABS_INDEXED_ADDR(ad, ad2, _y);
			CHECK_BOUNDARY_CROSSED(ad, ad2)
			break;
        case idy:
			// indirect indexed, e.g. LDA ($20),Y
			INDIRECT_INDEXED_ADDR(ad, ad2);
			CHECK_BOUNDARY_CROSSED(ad, ad2)
			break;
    }
	return adjustment;
}

static uint8_t adjustBranchTaken(uint16_t* pc, uint8_t opc, uint8_t* lead_time) {
	static uint16_t wval;	// avoid reallocation

    int8_t dist = (int8_t)memGet((*pc)++);	// like getInput(opc, imm)
    wval = (*pc) + dist;

	// + 1 cycle if branches to same page
	// + 2 cycles if branches to different page
	uint8_t adjustment = (((*pc) & 0x100) != (wval & 0x100)) ? 2 : 1;

	// special case IRQ lead time.. (applies only when on same page)
	if (adjustment == 1) {
		(*lead_time) += 1;
	}
	return adjustment;
}

// determine next operation with its duration and interrupt lead time
static void prefetchOperation( int16_t* opcode, int8_t* cycles, uint8_t* lead_time) {

	uint16_t pc = _pc;
    uint8_t opc = memGet(pc++);
    int32_t mode = _modes[opc];

	(*opcode) = opc;
	(*cycles) = _opbase_frame_cycles[opc];	// get base cycles
	(*lead_time) = IRQ_LEAD_DEFAULT;		// default

	// calc adjustments
	switch (_mnemonics[opc]) {

		// ops that are subject to +1 cycle on page crossing - according to:
		// 1) synertek_programming_manual
		// 2) MOS6510UnintendedOpcodes
		// 3) "Extra Instructions Of The 65XX Series CPU"

		// both 2&3 claim that for "and", "ora" and "lae" the "iny"
		// correction does not apply - however the "irq" test from the
		// test suite shows that this claim is incorrect

		case adc:
        case and:
		case cmp:
		case eor:
		case lae:	// see 2,3: only aby exists
		case lax:	// see 2,3: only aby,idy exist
		case lda:
		case ldx:
		case ldy:
		case nop:	// see 2: only abx exits
        case ora:
		case sbc:
			(*cycles) += adjustPageBoundaryCrossing(&pc, mode);
            break;

        case bcc:
            if (!(_p & FLAG_C)) (*cycles) += adjustBranchTaken(&pc, opc, lead_time);
            break;
        case bcs:
            if (_p & FLAG_C) (*cycles) += adjustBranchTaken(&pc, opc, lead_time);
            break;
        case bne:
            if (!(_p & FLAG_Z)) (*cycles) += adjustBranchTaken(&pc, opc, lead_time);
            break;
        case beq:
            if (_p & FLAG_Z) (*cycles) += adjustBranchTaken(&pc, opc, lead_time);
            break;
        case bpl:
            if (!(_p & FLAG_N)) (*cycles) += adjustBranchTaken(&pc, opc, lead_time);
            break;
        case bmi:
            if (_p & FLAG_N) (*cycles) += adjustBranchTaken(&pc, opc, lead_time);
            break;
        case bvc:
            if (!(_p & FLAG_V)) (*cycles) += adjustBranchTaken(&pc, opc, lead_time);
            break;
        case bvs:
            if (_p & FLAG_V) (*cycles) += adjustBranchTaken(&pc, opc, lead_time);
            break;

		default:
			break;
    }
}

#ifdef DEBUG
uint16_t cpuGetPC() {
	return _pc;
}

uint8_t cpuGetSP() {
	return _s;
}
#endif

#ifdef PSID_DEBUG_ADSR
uint16_t _play_addr = 0;

void cpuPsidDebug(uint16_t play_addr) {
	_play_addr = play_addr;
}
#endif

// CAUTION: advances the _pc as a side-effect
static void operationSTx(const int32_t* mode, uint8_t val) {
    static uint16_t ad;  // avoid reallocation

    switch(*mode) {
        case acc:
            _a = val;
            return;
        case abs:
            ad = memGet(_pc++);
            ad |= memGet(_pc++) << 8;
            memSet(ad, val);
            return;
        case abx:
            ad = memGet(_pc++);
            ad |= memGet(_pc++) << 8;
            ad += _x;
            memSet(ad, val);
            return;
        case aby:
            ad = memGet(_pc++);
            ad |= memGet(_pc++) << 8;
            ad += _y;
            memSet(ad, val);
            return;
        case zpg:
            ad = memGet(_pc++);
            memSet(ad, val);
            return;
        case zpx:
            ad = memGet(_pc++) + _x;
			ad &= 0xff;
            memSet(ad, val);
            return;
        case zpy:
            ad = memGet(_pc++) + _y;
			ad &= 0xff;
            memSet(ad, val);
            return;
        case idx:
            ad = memGet(_pc++) + _x;
            ad = memGet(ad & 0xff) | (memGet((ad + 1) & 0xff) << 8);
            memSet(ad, val);
            return;
        case idy:
            ad = memGet(_pc++);
            ad = ((memGet(ad) | (memGet((ad + 1) & 0xff) << 8))) + _y;
            memSet(ad, val);
            return;
    }
}

static void push(uint8_t val) {
    MEM_WRITE_RAM(0x100 + _s, val);
	_s = (_s - 1) & 0xff;				// real stack just wraps around...
}

static uint8_t pop() {
	_s = (_s + 1) & 0xff;				// real stack just wraps around...
    return MEM_READ_RAM(0x100 + _s);	// pos is now the new first free element..
}

static void branch(uint8_t is_taken) {
    if (is_taken) {
		int8_t dist = (int8_t)memGet(_pc++);	// like getInput() in "imm" mode
		_pc += dist;
	} else {
		_pc++;	// just skip the byte
	}
}

// instruction that is executing in a "cycle-by-cycle manner"
static int16_t _exe_instr_opcode;
static int8_t _exe_instr_cycles;
static int8_t _exe_instr_cycles_remain;

#ifdef PSID_DEBUG_ADSR
static uint16_t _frame_count;
extern void sidDebug(int16_t frame_count);
#endif

static void cpuRegReset() {
    _a =_x =_y = 0;
    _p = 0;
    _s = 0xff;
    _pc = 0;
}

void cpuSetProgramCounter(uint16_t pc, uint8_t a) {
	cpuRegReset();
	_a = a;
	_pc = pc;

	push(0);	// marker used to detect when "init" returns to non existing "main"
	push(0);
}

void cpuIrqFlagPSID(uint8_t on) {
	SETFLAGS(FLAG_I, on);
}

#ifdef TEST
char _load_filename[32];
#endif
static void runPrefetchedOp() {
	// avoid reallocation of these tmp vars; todo: perftest and quantify benefit of this ugly hack
	static uint8_t bval;
	static uint16_t wval;
	static int32_t c;  		// temp for "carry"

	// note:  Read-Modify-Write instructions (ASL, LSR, ROL, ROR, INC,
	// DEC, SLO, SRE, RLA, RRA, ISB, DCP) write the originally read
	// value before they then write the updated value. This is used
	// by some programs to acknowledge/clear interrupts - since the
	// 1st write will clear all the originally set bits.

//	if (_pc == 0xEA79) fprintf(stderr, "%x %x\n", memGet(0xEA79), memGet(0xEA7a));	// warmstart

	// The operation MUST BE fetched in the 1st cycle (i.e. when
	// prefetching is none - or the wrong command could be used
	// later .. see "cia1tb123" test - where the command byte is
	// updated by the timer - changing the OP while the instruction
	// is already executed)

#ifdef PSID_DEBUG_ADSR
	if (_play_addr == _pc) {	// PSID play routine is about to be invoked..
		sidDebug(_frame_count - 1);
		_frame_count++;
	}
#endif

    _pc++;						// skip the opcode byte
	_opc = _exe_instr_opcode;	// use what was actually valid at the 1st cycle of the op

    switch (_mnemonics[_opc]) {
		// ideally the most often used OPs should be retriveable
		// most quickly.. but is is unclear what strategy the
		// optimizer will actually be using to implement this (and
		// the optimizer cannot know what programs will be emulated
		// here so it has no clue what might be the most used OPs)..
		// let's just hope it is using some constant time access scheme.

        case adc: {
			uint8_t in1 = _a;
			uint8_t in2 = getInput(&(_modes[_opc]));

			// note: The carry flag is used as the carry-in (bit 0)
			// for the operation, and the resulting carry-out (bit 8)
			// value is stored in the carry flag.

            wval = (uint16_t)in1 + in2 + ((_p & FLAG_C) ? 1 : 0);	// "carry-in"
            SETFLAGS(FLAG_C, wval & 0x100);

			_a = (uint8_t)wval;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);

			// calc overflow flag (http://www.righto.com/2012/12/the-6502-overflow-flag-explained.html)
			// also see http://www.6502.org/tutorials/vflag.html
			SETFLAGS(FLAG_V, (~(in1 ^ in2)) & (in1 ^ _a) & 0x80);
			}
            break;
		case alr: 		// aka ASR - Kukle.sid, Raveloop14_xm.sid (that song has other issues though)
			//	ALR #{imm} = AND #{imm} + LSR
            bval = getInput(&(_modes[_opc]));
			_a = _a & bval;

            SETFLAGS(FLAG_C, _a & 1);
            _a >>= 1;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
			break;
        case anc:	// Kukle.sid, Axelf.sid (Crowther), Whats_Your_Lame_Excuse.sid, Probing_the_Crack_with_a_Hook.sid
            bval = getInput(&(_modes[_opc]));
			_a = _a & bval;

			// http://codebase64.org/doku.php?id=base:some_words_about_the_anc_opcode
            SETFLAGS(FLAG_C, _a & 0x80);
			// supposedly also sets these (http://www.oxyron.de/html/opcodes02.html)
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
        case and:
            bval = getInput(&(_modes[_opc]));
            _a &= bval;

			SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
        case ane: {// aka XAA; another useless op that is only used in the tests
	        bval = getInput(&(_modes[_opc]));
			const uint8_t con = 0x0; 	// this is HW dependent.. i.e. this OP is bloody useless
			_a = (_a | con) & _x & bval;

            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
			}
			break;
        case arr: {		// Whats_Your_Lame_Excurse.sid uses this. sigh "the crappier the song...." &
						// Probing_the_Crack_with_a_Hook.sid
			// AND
            bval = getInput(&(_modes[_opc]));
            _a &= bval;

			// set C+V based on this intermediate state of bits 6+7 (before ROR)
			uint8_t bit7 = !!(_a & 0x80);
			uint8_t bit6 = !!(_a & 0x40);

            c = !!(_p & FLAG_C);

			SETFLAGS(FLAG_V, bit7 ^ bit6);
            SETFLAGS(FLAG_C, bit7);

			// ROR - C+V not affected here
            _a >>= 1;

			if (c) {
				_a |= 0x80;	// exchange bit 7 with carry
			}
            SETFLAGS(FLAG_N, _a & 0x80);
            SETFLAGS(FLAG_Z, !_a);
			}
            break;
        case asl: {
			const int32_t *mode = &(_modes[_opc]);
            wval = getInput(mode);

            setOutput(mode, (uint8_t)wval);	// "read-modify-write"
            wval <<= 1;
            setOutput(mode, (uint8_t)wval);

            SETFLAGS(FLAG_Z, !(wval & 0xff));
            SETFLAGS(FLAG_N, wval & 0x80);
            SETFLAGS(FLAG_C, wval & 0x100);
			} break;
        case bcc:
            branch(!(_p & FLAG_C));
            break;
        case bcs:
            branch(_p & FLAG_C);
            break;
        case bne:
            branch(!(_p & FLAG_Z));
            break;
        case beq:
            branch(_p & FLAG_Z);
            break;
        case bpl:
            branch(!(_p & FLAG_N));
            break;
        case bmi:
            branch(_p & FLAG_N);
            break;
        case bvc:
            branch(!(_p & FLAG_V));
            break;
        case bvs:
            branch(_p & FLAG_V);
            break;
        case bit:
            bval = getInput(&(_modes[_opc]));

            SETFLAGS(FLAG_Z, !(_a & bval));
            SETFLAGS(FLAG_N, bval & 0x80);
            SETFLAGS(FLAG_V, bval & 0x40);	// bit 6
            break;
        case brk:
#ifdef TEST
			// tests use various ROM routines which would normally
			// produce screen output (etc)..
	/*		if (_pc == 0xFFE5) {	// get a char
				// tests only call this to wait AFTER an error

				EM_ASM_({ console.log("test failed?");});

				wval = pop();
				wval |= pop() << 8;	// RTS to where it came from
				_pc = wval + 1;
				break;
			} else */
			if (_pc == 0xFFD3) {	// print char via FFD2

				// BASIC start of a single test would init this to 0 whereas direct
				// start from 0801 will set this to 1. It controls if the tests are
				// chained, and chaining is here deliberately activated so that
				// LOAD trigger (E16F) can be used to detect when a test has completed
				memWriteRAM(0x030C, 0);

				EM_ASM_({ window['outputPETSCII'](($0));}, _a);	// easier to deal with this on JavaScript side (pervent optimizer renaming the func)

				wval = pop();
				wval |= pop() << 8;	// RTS to where it came from
				_pc = wval + 1;
				break;
			} else if (_pc == 0xBDCE) {	// print AX as number via BDCD
				// just another way of printing PETSCII
				EM_ASM_({ window['outputPETSCII'](($0));}, _x);	// easier to deal with this on JavaScript side (pervent optimizer renaming the func)

				wval = pop();
				wval |= pop() << 8;	// RTS to where it came from
				_pc = wval + 1;
				break;
			} else if (_pc == 0xE170) {	// load E16F
				// report the next test file (this means that this test was successful)
				uint16_t addr = memReadRAM(0x00bb) | (memReadRAM(0x00bc) << 8);
				uint8_t len = memReadRAM(0x00b7);
				if (len > 31) len = 31;
				for (int i= 0; i<len; i++) {
					_load_filename[i] = memReadRAM(addr++);
				}
				_load_filename[len] = 0;

				EM_ASM_({ window['loadFileError'](Pointer_stringify($0));}, _load_filename);	// easier to deal with this on JavaScript side (pervent optimizer renaming the func)

				test_running = 0;
				break;
			} else if (_pc == 0xFFE5) {	// scan keyboard
				_a = 3;			// always return this "key press"
				wval = pop();
				wval |= pop() << 8;
				_pc = wval + 1;
				break;
			} else if ((_pc == 0x8001) || (_pc == 0xA475)) {	// exit
				test_running = 0;
				break;
			}
#endif
			EM_ASM_({ console.log('BRK from:        $' + ($0).toString(16));}, _pc-1);	// less mem than inclusion of fprintf

			// _pc has already been incremented by 1 (see above)
			// (return address to be stored on the stack is original _pc+2 )
			push((_pc + 1) >> 8);
			push((_pc + 1));
			push(_p | FLAG_B0 | FLAG_B1);	// only in the stack copy

			_pc = memGet(0xfffe);
			_pc |= memGet(0xffff) << 8;		// somebody might finger the IRQ vector or the BRK vector at 0316/0317 to use this?

			SETFLAGS(FLAG_I, 1);
            break;
        case clc:
            SETFLAGS(FLAG_C, 0);
            break;
        case cld:
            SETFLAGS(FLAG_D, 0);
            break;
        case cli:
            SETFLAGS(FLAG_I, 0);

			// known limitation: this op should have a similar delay as "sei"
			// just in the other direction.. not implemented since it
			// seems to be irrelevant in WebSid context (and test suite doesn't care)

            break;
        case clv:
            SETFLAGS(FLAG_V, 0);
            break;
        case cmp:
            bval = getInput(&(_modes[_opc]));
            wval = (uint16_t)_a - bval;

			SETFLAGS(FLAG_Z, !wval);		// _a == bval
            SETFLAGS(FLAG_N, wval & 0x80);	// _a < bval
            SETFLAGS(FLAG_C, _a >= bval);
            break;
        case cpx:
            bval = getInput(&(_modes[_opc]));
            wval = (uint16_t)_x - bval;

			SETFLAGS(FLAG_Z, !wval);
            SETFLAGS(FLAG_N, wval & 0x80);
            SETFLAGS(FLAG_C, _x >= bval);
            break;
        case cpy:
            bval = getInput(&(_modes[_opc]));
            wval = (uint16_t)_y - bval;

			SETFLAGS(FLAG_Z, !wval);
            SETFLAGS(FLAG_N, wval & 0x80);
            SETFLAGS(FLAG_C, _y >= bval);
            break;
        case dcp: {		// used by: Clique_Baby.sid, Musik_Run_Stop.sid
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
			// dec
            bval--;
            setOutput(mode, bval);
			// cmp
            wval = (uint16_t)_a - bval;
            SETFLAGS(FLAG_Z, !wval);
            SETFLAGS(FLAG_N, wval & 0x80);
            SETFLAGS(FLAG_C, _a >= bval);
			} break;
        case dec: {
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
            bval--;
            setOutput(mode, bval);

			SETFLAGS(FLAG_Z, !bval);
            SETFLAGS(FLAG_N, bval & 0x80);
			} break;
        case dex:
            _x--;
            SETFLAGS(FLAG_Z, !_x);
            SETFLAGS(FLAG_N, _x & 0x80);
            break;
        case dey:
            _y--;
            SETFLAGS(FLAG_Z, !_y);
            SETFLAGS(FLAG_N, _y & 0x80);
            break;
        case eor:
            bval = getInput(&(_modes[_opc]));
            _a ^= bval;

			SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
        case inc: {
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

			setOutput(mode, bval);	// "read-modify-write"
            bval++;
            setOutput(mode, bval);

			SETFLAGS(FLAG_Z, !bval);
            SETFLAGS(FLAG_N, bval & 0x80);
			} break;
        case inx:
            _x++;

			SETFLAGS(FLAG_Z, !_x);
            SETFLAGS(FLAG_N, _x & 0x80);
            break;
        case iny:
            _y++;

			SETFLAGS(FLAG_Z, !_y);
            SETFLAGS(FLAG_N, _y & 0x80);
            break;
        case isb: {	// aka ISC; see 'insz' tests
			// inc
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

			setOutput(mode, bval);	// "read-modify-write"
            bval++;
            setOutput(mode, bval);

			SETFLAGS(FLAG_Z, !bval);
            SETFLAGS(FLAG_N, bval & 0x80);

			// + sbc
			uint8_t in1 = _a;
			uint8_t in2 = (bval ^ 0xff);	// substract

            wval = (uint16_t)in1 + in2 + ((_p & FLAG_C) ? 1 : 0);
            SETFLAGS(FLAG_C, wval & 0x100);

            _a = (uint8_t)wval;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);

			SETFLAGS(FLAG_V, (~(in1 ^ in2)) & (in1 ^ _a) & 0x80);
            }
			break;
		case jam:	// this op would have crashed the C64
		    _pc = 0;           // just quit the emulation
            break;
        case jmp: {
            bval = memGet(_pc++);		// low-byte
            wval = memGet(_pc++) << 8;	// high-byte
			int32_t mode = _modes[_opc];
            switch (mode) {
                case abs:
					_pc = wval | bval;
                    break;
                case ind:
					// 6502 bug: JMP ($12FF) will fetch the low-byte
					// from $12FF and the high-byte from $1200, i.e.
					// there is never an overflow into the high-byte

                    _pc = memGet(wval | bval);
                    _pc |= memGet(wval | ((bval + 1) & 0xff)) << 8;
                    break;
            }
			} break;
        case jsr:
			// _pc has already been incremented by 1 (see above)
			// (return address to be stored on the stack is original _pc+2 )
            push((_pc + 1) >> 8);
            push((_pc + 1));
            wval = memGet(_pc++);
            wval |= memGet(_pc++) << 8;
            _pc = wval;

            break;
		case lae:	// aka LAS, aka LAR .. just for the tests
            bval = getInput(&(_modes[_opc]));

			_a = _x = _s = (bval & _s);

            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
		case lax:
			// e.g. Vicious_SID_2-15638Hz.sid, Kukle.sid
            _a = getInput(&(_modes[_opc]));

			_x = _a;

            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
		case lxa: {	// Whats_Your_Lame_Excuse.sid - LOL only real dumbshit player uses this op..
            bval = getInput(&(_modes[_opc]));

			const uint8_t con = 0xff;
			_a |= con;	// roulette what the specific CPU uses here
			_a &= bval;
			_x = _a;

            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
		} break;
        case lda:
            _a = getInput(&(_modes[_opc]));

            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
        case ldx:
            _x = getInput(&(_modes[_opc]));

            SETFLAGS(FLAG_Z, !_x);
            SETFLAGS(FLAG_N, _x & 0x80);
            break;
        case ldy:
            _y = getInput(&(_modes[_opc]));

            SETFLAGS(FLAG_Z, !_y);
            SETFLAGS(FLAG_N, _y & 0x80);
            break;
        case lsr: {
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
			wval = (uint8_t)bval;
            wval >>= 1;
            setOutput(mode, (uint8_t)wval);

            SETFLAGS(FLAG_Z, !wval);
            SETFLAGS(FLAG_N, wval & 0x80);	// always clear?
            SETFLAGS(FLAG_C, bval & 1);
			} break;
        case nop:
			getInput(&(_modes[_opc]));	 // make sure the PC is advanced correctly
            break;
        case ora:
            bval = getInput(&(_modes[_opc]));

            _a |= bval;

			SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
        case pha:
            push(_a);
            break;
        case php:
            push(_p | FLAG_B0 | FLAG_B1);	// only in the stack copy
            break;
        case pla:
            _a = pop();

            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
        case plp: {
			bval = pop();
            _p = bval & ~(FLAG_B0 | FLAG_B1);

			// known limitation: see sei/cli delay regarding FLAG_I (applies here too)

			// XXX CAUTION: any cached flags must also be refreshed here...
			}
			break;
        case rla: {				// see Spasmolytic_part_6.sid
			// rol
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
            c = !!(_p & FLAG_C);
            SETFLAGS(FLAG_C, bval & 0x80);
			bval <<= 1;
            bval |= c;
            setOutput(mode, bval);

			// + and
            _a &= bval;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
			} break;
        case rol:  {
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
            c = !!(_p & FLAG_C);
            SETFLAGS(FLAG_C, bval & 0x80);
			bval <<= 1;
            bval |= c;
            setOutput(mode, bval);

            SETFLAGS(FLAG_N, bval & 0x80);
            SETFLAGS(FLAG_Z, !bval);
			} break;
        case ror: {
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
            c = !!(_p & FLAG_C);
            SETFLAGS(FLAG_C, bval & 1);
            bval >>= 1;
            bval |= 0x80 * c;
            setOutput(mode, bval);

			SETFLAGS(FLAG_N, bval & 0x80);
            SETFLAGS(FLAG_Z, !bval);
			} break;
        case rra: {
			// ror
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
            c = !!(_p & FLAG_C);
            SETFLAGS(FLAG_C, bval & 1);
            bval >>= 1;
            bval |= 0x80 * c;
            setOutput(mode, bval);

			// + adc
			uint8_t in1 = _a;
			uint8_t in2 = bval;

            wval = (uint16_t)in1 + in2 + ((_p & FLAG_C) ? 1 : 0);
            SETFLAGS(FLAG_C, wval & 0x100);

            _a = (uint8_t)wval;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);

			SETFLAGS(FLAG_V, (~(in1 ^ in2)) & (in1 ^ _a) & 0x80);
			} break;
        case rti:
			// timing hack: some optimized progs use JMP to an RTI
			// that is placed such that the nearby interrupt status
			// register is implicitly read - automatically acknowledging
			// the interrupt without having to explicitly read the register.
			switch(_pc) {
				case 0xdc0d:
				case 0xdd0d:	// e.g. LMan - Vortex.sid
					memGet(_pc);
					break;
			}

			bval = pop();
            _p = bval & ~(FLAG_B0 | FLAG_B1);

            wval = pop();
            wval |= pop() << 8;
            _pc = wval;	// not like 'rts'! correct address is expected here!

			sysSetNMIMarker(0);	// hack to improve digi output

#ifdef TRACE_IRQ_TIMING
			if (_irq_start) {
				_irq_start = sysCycles() - _irq_start;
				EM_ASM_({ console.log('irq t: ' + ($0).toString(16));}, _irq_start);
				_irq_start = 0;
			}
#endif
            break;
        case rts:
            wval = pop();
            wval |= pop() << 8;
			_pc = wval + 1;
            break;
        case sbc:    {
            bval = getInput(&(_modes[_opc])) ^ 0xff;

			uint8_t in1 = _a;
			uint8_t in2 = bval;

            wval =(uint16_t)in1 + in2 + ((_p & FLAG_C) ? 1 : 0);
            SETFLAGS(FLAG_C, wval & 0x100);

            _a = (uint8_t)wval;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
			SETFLAGS(FLAG_V, (~(in1 ^ in2)) & (in1 ^ _a) & 0x80);
			}
            break;
        case sha:    {	// aka AHX; for the benefit of the 'SHAAY' test (etc).. have yet to find a song that uses this
			const int32_t *mode = &(_modes[_opc]);
			uint8_t h = getH1(mode);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
			bval= _a & _x & h;
			setOutput(mode, bval);
			}
            break;
        case shx:    {	// for the benefit of the 'SHXAY' test (etc).. have yet to find a song that uses this
			const int32_t *mode = &(_modes[_opc]);
			uint8_t h = getH1(mode);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
			bval= _x & h;
			setOutput(mode, bval);
			}
            break;
        case shy:    {	// for the benefit of the 'SHYAY' test (etc).. have yet to find a song that uses this
			const int32_t *mode = &(_modes[_opc]);
			uint8_t h = getH1(mode);		// who cares about this OP
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
			bval= _y & h;
			setOutput(mode, bval);
			}
            break;
        case sax: {				// aka AXS; e.g. Vicious_SID_2-15638Hz.sid, Kukle.sid
			const int32_t *mode = &(_modes[_opc]);
			getInput(mode);	 // make sure the PC is advanced correctly

            setOutput(mode, bval);	// "read-modify-write"
			bval = _a & _x;
			setOutput(mode, bval);
			
			// no flags are affected; registers unchanged
			} break;
		case sbx: // somtimes called SAX; used in Kukle.sid, Artefacts.sid, Whats_Your_Lame_Excuse.sid, Probing_the_Crack_with_a_Hook.sid
			// affects N Z and C (like CMP)
			bval = getInput(&(_modes[_opc]));

            SETFLAGS(FLAG_C, (_x & _a) >= bval);	// affects the carry but NOT the overflow

			_x = ((_x & _a) - bval) & 0xff;	// _a unchanged (calc not affected by input carry)

            SETFLAGS(FLAG_Z,!_x);			// _a == bval
            SETFLAGS(FLAG_N, _x & 0x80);	// _a < bval
			break;
        case sec:
            SETFLAGS(FLAG_C, 1);
            break;
        case sed:
            SETFLAGS(FLAG_D, 1);
            break;
        case sei:
			// This operation (like CLI and PLP) is timing critical since it affects IRQ
			// execution via the FLAG_I. It would NOT prevent an IRQ to still be triggered
			// during its 1st cycle - effectively reducing the IRQs "lead time"
			// to 1 cycle, i.e. the IRQ would be executed immediately after the SEI.

			// WebSid's approach of setting all the results of an operation at the end the op's last cycle
			// (i.e. cycle 2 here) in this case is somewhat off with regards to timing of the flag
			// change. Since IRQ polling in this emulation is always handled at the start of a new cycle,
			// the set flag will only be observed in the cycle after SEI has completed (which must be
			// compensated for in respective IRQ timing calculation).

            SETFLAGS(FLAG_I, 1);
            break;
		case shs: {	// 	aka TAS
			// instable op; hard to think of a good reason why
			// anybody would ever use this..
			_s = _a & _x;

			const int32_t *mode = &(_modes[_opc]);
			uint8_t h = getH1(mode);
			bval = getInput(mode); 	// make sure the PC is advanced correctly

			setOutput(mode, bval);
			bval = _s&h;
			setOutput(mode, bval);

			} break;
        case slo: {			// see Spasmolytic_part_6.sid
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
			wval = (uint8_t)bval;
            wval <<= 1;
            setOutput(mode, (uint8_t)wval);

            SETFLAGS(FLAG_C, wval & 0x100);
			// + ora
            bval = wval & 0xff;
            _a |= bval;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
			} break;
        case sre: {      		// aka LSE; see Spasmolytic_part_6.sid, Halv_2_2.sid
			// like SLO but shifting right and with eor

			// copied section from 'lsr'
			const int32_t *mode = &(_modes[_opc]);
            bval = getInput(mode);

            setOutput(mode, bval);	// "read-modify-write"
			wval = (uint8_t)bval;
            wval >>= 1;
            setOutput(mode, (uint8_t)wval);

            SETFLAGS(FLAG_Z, !wval);
            SETFLAGS(FLAG_N, wval & 0x80);
            SETFLAGS(FLAG_C, bval & 1);
			// + copied section from 'eor'
            bval = wval & 0xff;

            _a ^= bval;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);

			} break;
        case sta:
            operationSTx(&(_modes[_opc]), _a);
            break;
        case stx:
            operationSTx(&(_modes[_opc]), _x);
            break;
        case sty:
            operationSTx(&(_modes[_opc]), _y);
            break;
        case tax:
            _x = _a;
            SETFLAGS(FLAG_Z, !_x);
            SETFLAGS(FLAG_N, _x & 0x80);
            break;
        case tay:
            _y = _a;
            SETFLAGS(FLAG_Z, !_y);
            SETFLAGS(FLAG_N, _y & 0x80);
            break;
        case tsx:
			_x = _s;
            SETFLAGS(FLAG_Z, !_x);
            SETFLAGS(FLAG_N, _x & 0x80);
            break;
        case txa:
            _a = _x;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
        case txs:
            _s = _x;
            break;
        case tya:
            _a = _y;
            SETFLAGS(FLAG_Z, !_a);
            SETFLAGS(FLAG_N, _a & 0x80);
            break;
		default:
#ifdef DEBUG
			EM_ASM_({ console.log('op code not implemented: ' + ($0).toString(16) + ' at ' + ($1).toString(16));}, _opc, _pc);	// less mem than inclusion of fprintf
#endif
			getInput(&(_modes[_opc]));	 // at least make sure the PC is advanced correctly (potentially used in timing)
    }
}

/*
* Notes on VIC "bad lines": VIC may "stun" the CPU for 40 (+up to 3) cycles
* (or more if sprites are involved - which isn't supported here) and this
* may occur right in the middle of some OP. The "stun" starts whenever the
* current OP does its next "bus read" - only consecutive "writes" of the
* current OP are allowed to complete. (Within a 7 cycle BRK OP the code would
* be allowed to complete the 3 push stack operations - but ONLY if the "OP"
* was already past the 2 initial cycles..)
* => see vic.c for more information
*
* To handle this correctly OPs would need to be simulated at a sub-cycle level.
* But in the context of SID music players this is probably unnecessary overkill
* (except for the most hardcore RSID files - but those most likely avoid the
* badlines problem altogether since they cannot afford the 40 cycles gap when
* they want to play hi-fi digi-samples..).
*
* => CPU may run any cycles that DO NOT use the bus.. and there might be
*    special cases that I overlooked
*/
#define CHECK_FOR_VIC_STUN(is_stunned) \
	/* VIC badline handling (i.e. CPU may be paused/stunned) */ \
	uint8_t stun_mode = vicStunCPU();	/* it won't hurt to also STUN the crappy PSID songs */ \
	if (stun_mode) { \
		if ((stun_mode == 2) || (_exe_instr_opcode < 0)) { \
			is_stunned = 1; \
		} else { \
			uint8_t bus_write = _opbase_write_cycle[_exe_instr_opcode]; \
			if (bus_write) { \
				/* this OP may be allowed to still perform "bus write" (if that's the current step): */ \
				int8_t p = _exe_instr_cycles -_exe_instr_cycles_remain; \
				if (p >= bus_write) { \
					/* allow this "write to complete".. see below */ \
					is_stunned = 0; \
				} else { \
					is_stunned = 1; \
				} \
			} else { \
				is_stunned = 1; \
			} \
		} \
	} else { \
		is_stunned = 0; \
	}

// cpuClock function pointer
void (*cpuClock)();

/*
* Simulate what the CPU does within the next system clock cycle.
*
* Note on interrupt handling (see http://6502.org/tutorials/interrupts.html):
* an interrupt triggers a 7-clock cycles "virtual OP". An interrupt allows
* the previous OP to complete (it seems reasonable to presume that BRK uses
* the exact same sequence - and JSR does the same just without pushing the
* processor status):
*					2 cycles internal
*					1 cycle push stack: return addr-hi
*					1 cycle push stack: return addr-lo
*					1 cycle push stack: processor status regigster
*				=> BTW: these are the max. 3 consecutive writes that may
*                  occur on a 6502 (see badline handling) - whereas for
*                  the other OPs (except JSR) the writes seem to be at the
*                  end of the OP they are in the middle here..
*					1 cycle pc-lo: get vector
*					1 cycle pc-hi: get vector
*
* special cases:
*
* unhandled: "When an IRQ occurs while SEIn is executing, the IFlag on
*            the stack will be set." => WTF is this supposed to mean?
* unhandled: "When an NMI occurs before clock 4 of a BRKn command, the
*            BRK is finished as a NMI. In this case, BFlag on the stack
*            will not be cleared."
*/
static void cpuClockRSID() {
	// note: on the real HW the respective check happends in �2 phase
	// of the previous CPU cycle and the respective internal interrupt
	// signal then goes high in the �1 phase after (the potential
	// problem of the below impl is that by performing the test here,
	// it might incorrectly pick up some CIA change that has just
	// happend in the �1 phase)

	CHECK_FOR_IRQ();	// check 1st (so NMI can overrule if needed)
	CHECK_FOR_NMI();

	uint8_t is_stunned;
	CHECK_FOR_VIC_STUN(is_stunned);
	if (is_stunned) return;

	if (_exe_instr_opcode < 0) {	// get next instruction

		if(IS_NMI_PENDING()) {				// has higher prio than IRQ

			// some old PlaySID files (with recorded digis) files actually
			// use NMI settings that must not be used here
			sysSetNMIMarker(1);

			_nmi_committed = 0;

			// make that same trigger unusable (interrupt must be
			// acknowledged before a new one can be triggered)
			_nmi_line_ts = 0;
			_exe_instr_opcode = START_NMI_OP;
			_exe_instr_cycles = _opbase_frame_cycles[_exe_instr_opcode];

		} else if (IS_IRQ_PENDING()) {	// interrupts are like a BRK command

			_irq_committed = 0;
			_exe_instr_opcode = START_IRQ_OP;

			_exe_instr_cycles = _opbase_frame_cycles[_exe_instr_opcode];

		} else {
			// default: start execution of next instruction (determine exact timing)
			prefetchOperation( &_exe_instr_opcode, &_exe_instr_cycles, &_interrupt_lead_time);
		}
		// since there are no 1-cycle ops nothing else needs to be done right now
		_exe_instr_cycles_remain =  _exe_instr_cycles - 1;	// we already are in 1st cycle here
	} else {

		// handle "current" instruction
		_exe_instr_cycles_remain--;

		if(_exe_instr_cycles_remain == 0) {
			// complete current instruction

			if (_exe_instr_opcode == START_IRQ_OP) {
				push(_pc >> 8);		// where to resume after the interrupt
				push(_pc & 0xff);

				// known limitation: special case not handled yet: if IRQ occurs
				// while SEI is executing then the Flag_I is here also pushed to the stack
				//

				push(_p | FLAG_B1);	// only in the stack copy ( will clear Flag_I upon RTI...)

				SETFLAGS(FLAG_I, 1);

				_pc = memGet(0xfffe);			// IRQ vector
				_pc |= memGet(0xffff) << 8;
#ifdef TRACE_IRQ_TIMING
				_irq_start = sysCycles();
#endif
			} else if (_exe_instr_opcode == START_NMI_OP) {
				push(_pc >> 8);	// where to resume after the interrupt
				push(_pc & 0xff);
				push(_p | FLAG_B1);	// only in the stack copy

				// "The 6510 will set the IFlag on NMI, too. 6502 untested." (docs of test suite)
				// seems the kernal developers did not know thst... see SEI in FE43 handler..
				SETFLAGS(FLAG_I, 1);

				_pc = memGet(0xfffa);			// NMI vector
				_pc |= memGet(0xfffb) << 8;

			} else if (_exe_instr_opcode == NULL_OP) {
				// just burn cycles
			} else {
				runPrefetchedOp();	// use old impl that runs a complete instruction
			}
			_exe_instr_opcode = -1;	// completed current OP
			_exe_instr_cycles_remain = _exe_instr_cycles = 0;
		}
	}
}

static void cpuClockPSID() {
	// optimization: this is a 1:1 copy of the regular cpuClock() with all the
	// NMI handling thrown out (tested songs ran about 5% faster with this optimization)

	CHECK_FOR_IRQ();	// check 1st (so NMI can overrule if needed)

	if (_exe_instr_opcode < 0) {	// get next instruction

		if (IS_IRQ_PENDING()) {	// interrupts are like a BRK command
			_irq_committed = 0;
			_exe_instr_opcode = START_IRQ_OP;
			_exe_instr_cycles = _opbase_frame_cycles[_exe_instr_opcode];

		} else {
			// default: start execution of next instruction (determine exact timing)
			prefetchOperation( &_exe_instr_opcode, &_exe_instr_cycles, &_interrupt_lead_time);
		}
		// since there are no 1-cycle ops nothing else needs to be done right now
		_exe_instr_cycles_remain =  _exe_instr_cycles - 1;	// we already are in 1st cycle here
	} else {

		// handle "current" instruction
		_exe_instr_cycles_remain--;

		if(_exe_instr_cycles_remain == 0) {
			// complete current instruction

			if (_exe_instr_opcode == START_IRQ_OP) {
				push(_pc >> 8);		// where to resume after the interrupt
				push(_pc & 0xff);
				push(_p | FLAG_B1);	// only in the stack copy ( will clear I-flag upon RTI...)

				SETFLAGS(FLAG_I, 1);

				_pc = memGet(0xfffe);			// IRQ vector
				_pc |= memGet(0xffff) << 8;
#ifdef TRACE_IRQ_TIMING
				_irq_start = sysCycles();
#endif
			} else if (_exe_instr_opcode == NULL_OP) {
				// just burn cycles
			} else {
				runPrefetchedOp();	// use old impl that runs a complete instruction
			}
			_exe_instr_opcode = -1;	// completed current OP
			_exe_instr_cycles_remain = _exe_instr_cycles = 0;
		}
	}
}

void cpuInit(uint8_t is_rsid) {
	cpuClock = is_rsid ? &cpuClockRSID : &cpuClockPSID;

	// cpu status
	_pc = _a = _x = _y = _s = _p = 0;

	_exe_instr_cycles = _exe_instr_cycles_remain = 0;
	_exe_instr_opcode = -1;

	_irq_line_ts = _irq_committed = 0;
	_nmi_line = _nmi_line_ts = _nmi_committed = 0;

	sysSetNMIMarker(0);

#ifdef TEST
	test_running = 1;

	_s = 0x0; 	// this should be equivalent to the 0xfd that the tests expect:
	push(0);	// use as marker to know when to return
	push(0);
	push(0);

	_p = 0x00;	// idiotic advice to set I-flag! (see "irq" tests)
	_pc = 0x0801;

#endif

#ifdef PSID_DEBUG_ADSR
	_frame_count = 0;
#endif
}


void cpuSetProgramCounterPSID(uint16_t pc) {
	_pc= pc;

   SETFLAGS(FLAG_I, 0);		// make sure the IRQ isn't blocked
}
