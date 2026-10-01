/*########################################################################################################*/
// Course project, option 1: probe-mode translator (TC with profiling, then TC2) where TC2 holds
// only the hot routines, split into a hot chain and an outlined cold region, ordered by
// Pettis-Hansen call weights, with de-virtualized hot indirect jumps/calls.
//
// Build and run: see README.txt.
/*########################################################################################################*/
/*BEGIN_LEGAL
Intel Open Source License

Copyright (c) 2002-2011 Intel Corporation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
/* ===================================================================== */

/*! @file
 * Translate the main image into a TC with BBL, fall-through and indirect
 * jmp/call target counters.  After -prof_time seconds a second thread builds
 * TC2 from the profile and switches every TC routine head to it atomically.
 */

#include "pin.H"
extern "C" {
#include "xed-interface.h"
}
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <values.h>
#include <set>
#include <map>
#include <vector>
#include <algorithm>
#include <time.h>

using namespace std;

/*======================================================================*/
/* commandline switches                                                 */
/*======================================================================*/
KNOB<BOOL>   KnobVerbose(KNOB_MODE_WRITEONCE,    "pintool",
    "verbose", "0", "Verbose run");

KNOB<BOOL>   KnobDumpOrigCode(KNOB_MODE_WRITEONCE,    "pintool",
    "dump_orig_code", "0", "Dump Original non-translated Code");

KNOB<BOOL>   KnobDumpTranslatedCode(KNOB_MODE_WRITEONCE,    "pintool",
    "dump_tc", "0", "Dump Translated Code");

KNOB<BOOL>   KnobDumpTranslatedCode2(KNOB_MODE_WRITEONCE,    "pintool",
    "dump_tc2", "0", "Dump 2nd Translated Code");

KNOB<BOOL>   KnobDoNotCommitTranslatedCode(KNOB_MODE_WRITEONCE,    "pintool",
    "no_tc_commit", "0", "Do not commit translated code");

KNOB<UINT> KnobNumSecsDuringProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "prof_time", "2", "Number of seconds for collecting BBL counters");

KNOB<BOOL> KnobDumpProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "dump_prof", "0", "Dump profiling information to bprofile.out");

KNOB<BOOL> KnobNoProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "no_prof", "0", "Do not collect profile information (no TC2)");

KNOB<BOOL> KnobOptDevirt(KNOB_MODE_WRITEONCE,    "pintool",
    "opt_devirt", "1", "De-virtualize hot indirect jmp/call in TC2");

KNOB<BOOL> KnobOptLayout(KNOB_MODE_WRITEONCE,    "pintool",
    "opt_layout", "1", "Hot/cold BBL split, jcc inversion, Pettis-Hansen routine order in TC2");

KNOB<BOOL> KnobHotOnly(KNOB_MODE_WRITEONCE,    "pintool",
    "hot_only", "0", "Only hot routines get a TC2 copy; cold ones run the original code");

KNOB<BOOL> KnobShortBr(KNOB_MODE_WRITEONCE,    "pintool",
    "short_br", "1", "With opt_layout: use rel8 jmp/jcc in TC2 where they fit");

KNOB<UINT> KnobHotRtn(KNOB_MODE_WRITEONCE,    "pintool",
    "hot_rtn", "64", "Routine is hot if its hottest BBL count >= hot_rtn");

KNOB<UINT> KnobColdBbl(KNOB_MODE_WRITEONCE,    "pintool",
    "cold_bbl", "4", "BBL of a hot routine is cold (outlined) if its count < cold_bbl");

KNOB<UINT> KnobHotAbs(KNOB_MODE_WRITEONCE,    "pintool",
    "hot_abs", "64", "Min absolute count for a hot indirect target");

KNOB<UINT> KnobHotFrac(KNOB_MODE_WRITEONCE,    "pintool",
    "hot_frac", "60", "Min hottest-target percent of the BBL execution count");

KNOB<BOOL> KnobTc2Stats(KNOB_MODE_WRITEONCE,    "pintool",
    "tc2_stats", "0", "Print a one-line TC2 summary to stderr");

KNOB<BOOL> KnobLiveWide(KNOB_MODE_WRITEONCE,    "pintool",
    "live_wide", "1", "Stub liveness follows direct jmp and both jcc edges");

KNOB<BOOL> KnobDevirtImm(KNOB_MODE_WRITEONCE,    "pintool",
    "devirt_imm", "1", "Devirt guard as cmp [mem]|reg, imm32 when the hot target fits (no scratch spill)");

KNOB<BOOL> KnobMigrateTc(KNOB_MODE_WRITEONCE,    "pintool",
    "migrate_tc", "1", "At TC2 commit, point each TC stub's skip jmp into TC2 so frames still in TC move over");

KNOB<BOOL> KnobTcShortBr(KNOB_MODE_WRITEONCE,    "pintool",
    "tc_short_br", "1", "Use rel8 jmp/jcc in TC too where they fit (smaller TC during profiling)");

KNOB<BOOL> KnobOptInline(KNOB_MODE_WRITEONCE,    "pintool",
    "opt_inline", "0", "Inline hot small leaf callees at direct call sites in TC2");

KNOB<UINT> KnobInlHot(KNOB_MODE_WRITEONCE,    "pintool",
    "inl_hot", "1000", "Min execution count of the calling BBL for inlining");

KNOB<UINT> KnobInlMaxIns(KNOB_MODE_WRITEONCE,    "pintool",
    "inl_max_ins", "24", "Max instructions (without nops) of an inlined callee");

KNOB<BOOL> KnobCtrDerive(KNOB_MODE_WRITEONCE,    "pintool",
    "ctr_derive", "0", "No counter for a BBL whose count follows from one incoming edge (Knuth-style)");

KNOB<BOOL> KnobCtrDeriveCheck(KNOB_MODE_WRITEONCE,    "pintool",
    "ctr_derive_check", "0", "Analysis: keep counters on derived BBLs and report derived vs measured");

KNOB<BOOL> KnobRunStats(KNOB_MODE_WRITEONCE,    "pintool",
    "run_stats", "0", "Analysis: with a -prof_time longer than the run, write whole-run profile stats to run_stats.txt");

KNOB<BOOL> KnobColdUnprobe(KNOB_MODE_WRITEONCE,    "pintool",
    "cold_unprobe", "1", "With hot_only: restore cold routines' original entry bytes so they run natively with no hops");

KNOB<BOOL> KnobEntryDirect(KNOB_MODE_WRITEONCE,    "pintool",
    "entry_direct", "1", "Original routine entry jumps straight to its TC2 head (or trampoline), skipping the TC head");

KNOB<BOOL> KnobColdHeadDirect(KNOB_MODE_WRITEONCE,    "pintool",
    "cold_head_direct", "1", "TC head of a cold routine jumps straight to its original entry");

KNOB<BOOL> KnobStubNoHead(KNOB_MODE_WRITEONCE,    "pintool",
    "stub_nohead", "1", "No NOP5 head per profiling stub; the skip jmp overwrites its first instruction");

KNOB<BOOL> KnobCheapTarg(KNOB_MODE_WRITEONCE,    "pintool",
    "cheap_targ", "1", "Short indirect-target stub (rip-relative, 2 scratch regs, R11 free at indirect calls)");

KNOB<BOOL> KnobFtElide(KNOB_MODE_WRITEONCE,    "pintool",
    "ft_elide", "1", "No fall-through stub when the next BBL is entered only by fall-through");

KNOB<BOOL> KnobTargReset(KNOB_MODE_WRITEONCE,    "pintool",
    "targ_reset", "1", "Indirect-target slot count restarts when the slot's target changes");
KNOB<BOOL> KnobTargFlags(KNOB_MODE_WRITEONCE,    "pintool",
    "targ_flags", "1", "Keep RFLAGS across the target stub/devirt guard of an indirect jmp (the target may read them)");

KNOB<BOOL> KnobColdDirect(KNOB_MODE_WRITEONCE,    "pintool",
    "cold_direct", "1", "TC2 branches to cold routines go straight to their original entry");

KNOB<BOOL> KnobOptDeadRegs(KNOB_MODE_WRITEONCE,    "pintool",
    "opt_dead_regs", "1", "Cheap TC counter stubs using dead flags / dead registers");


/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */
std::ofstream* out = 0;

#define VLOG(expr) do { if (KnobVerbose.Value()) { cerr expr; } } while (0)

// For XED:
#if defined(TARGET_IA32E)
    xed_state_t dstate = {XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b};
#else
    xed_state_t dstate = { XED_MACHINE_MODE_LEGACY_32, XED_ADDRESS_WIDTH_32b};
#endif

const unsigned max_inst_len = XED_MAX_INSTRUCTION_BYTES;

ADDRINT lowest_sec_addr = 0;
ADDRINT highest_sec_addr = 0;

// TC containing the new code:
char *tc = nullptr;
unsigned tc_size = 0;

// 2nd TC containing the new code after gearing:
char *tc2 = nullptr;
unsigned tc2_size = 0;

unsigned max_tc_size = 0;

// basic instruction types.
typedef enum {
    RegularIns = 0,
    RtnHeadIns,
    ProfilingIns,

} ins_enum_t;

// instructions map with an entry for each new instruction in the code.
typedef struct {
    ADDRINT orig_ins_addr;
    ADDRINT new_ins_addr;
    ADDRINT orig_targ_addr;
    ADDRINT orig_rip_addr;
    ADDRINT app_ins_addr;   // original application address (chaining key)
    ins_enum_t ins_type;
    char encoded_ins[XED_MAX_INSTRUCTION_BYTES];
    unsigned size;
    int targ_map_entry;
    unsigned bbl_num;
    xed_category_enum_t xed_category;
    bool short_br;          // TC2 only: encode with rel8
    bool stub_head;         // TC: first entry of a profiling stub
} instr_map_t;


// Instrs map:
instr_map_t *instr_map = NULL;
static size_t instr_map_bytes = 0;
unsigned num_of_instr_map_entries = 0;
unsigned max_ins_count = 0;

// Next ProfilingIns entry starts a headless stub (see begin_stub).
static bool g_stub_first = false;

// Zero-filled, lazily committed allocation.
static void *lazy_zero_alloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

// Indirect target hash: 16 slots on (addr >> 4) & 15 (addr & 3 put all
// 16-byte aligned targets into one slot).
#define MAX_TARG_ADDRS 0xF
#define TARG_HASH_SHIFT 4

// Target counters, only for BBLs ending in an indirect jmp/call.
typedef struct {
  ADDRINT addr;
  UINT64 count;
} targ_slot_t;
typedef struct {
  targ_slot_t s[MAX_TARG_ADDRS+1];   // slot offset = target & 0xF0
  ADDRINT base;                      // &s[0], read by the stub via [rip+x]
  ADDRINT pad;
} targ_prof_t;

static targ_prof_t *targ_pool = nullptr;   // in the TC mapping
static unsigned targ_num = 0;
static unsigned max_targ = 0;

// Bbl map of all the bbl exec counters to be collected at runtime:
typedef struct {
  UINT64 counter;
  UINT64 fallthru_counter; // for BBLs that terminate with a cond branch.
  unsigned starting_ins_entry;
  unsigned terminating_ins_entry;
  ADDRINT bbl_addr;
  ADDRINT fallthru_app_addr;
  unsigned targ_slot;      // 1-based index into targ_pool, 0 = none
  bool is_cond;
  bool is_indirect;
  bool is_indirect_call;
  bool ft_from_next;       // fall-through count = exec count of BBL b+1
  unsigned char derive;    // -ctr_derive: 0 counted, 1 from branch at derive_src, 2 from b-1
  ADDRINT derive_src;      // app address of the single branch into this BBL
} bbl_map_t;

// -ctr_derive: direct branch/call references per image address (saturating).
static unsigned char *img_refs = nullptr;
static unsigned g_ctr_derived = 0;

bbl_map_t *bbl_map;
unsigned bbl_num = 0;

unsigned max_rtn_count = 0;

struct timespec start_running_time;
struct timespec end_running_time;
static volatile INT32 g_tc_ready = 0;
static volatile INT32 g_start_time_set = 0;
static volatile INT32 g_tc2_committed = 0;
static struct timespec g_app_start;

// Data next to TC2 (within rel32 of it).
static ADDRINT *stub_slots = nullptr;     // cold routine -> original entry
static unsigned stub_num = 0;
static ADDRINT *hot_const_pool = nullptr; // de-virtualization compare values
static unsigned hot_const_num = 0;
static unsigned max_hot_const = 0;
// Spill slots for the de-virtualization scratch register (not push/pop:
// the red zone may be live at an indirect jmp).
static ADDRINT *scratch_slots = nullptr;
#define NUM_SCRATCH_SLOTS 4

// Set by add_indirect_target_stub(): target profiling emitted for this BBL.
static bool g_indirect_prof_emitted = false;

// BBL counters [2b] = exec, [2b+1] = fall-through; inside the TC mapping so
// stubs can use [rip+x].  Copied into bbl_map when profiling stops.
static UINT64 *prof_cnt = nullptr;
static UINT64 g_prof_rax_slot = 0;
static UINT64 *prof_rax_rip = nullptr;   // rip-reachable RAX spill slot
static UINT64 *prof_one_rip = nullptr;   // rip-reachable constant 1
static UINT64 *targ_spill_rip = nullptr; // 3 rip-reachable spill slots
static double g_create_tc_secs = 0;
static unsigned n_stub_inc = 0, n_stub_reg = 0, n_stub_spill = 0;
static unsigned n_ft_elided = 0;
static unsigned g_n_indirect = 0;   // indirect jmp/call count (pre-pass)

// Final fixup pass: rel8 range misses are real (see fix_instructions_displacements).
static bool g_fix_strict = false;
// Strict pass and no size changed yet in it: addresses are exact.
static bool g_fix_exact = false;

// -tc2_stats: TC2 build phase times and fixup pass counts.
enum { PT_SELECT, PT_PH, PT_LAYOUT, PT_EMIT, PT_CHAIN, PT_FIX, PT_SHRINK, PT_FIX2,
       PT_COPY, PT_COMMIT, PT_N };
static const char *g_pt_name[PT_N] = { "select", "ph", "layout", "emit", "chain",
                                       "fix", "shrink", "fix2", "copy", "commit" };
static double g_pt[PT_N];
static unsigned g_fix_rounds = 0, g_fix_passes = 0;
static double now_s()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static unsigned max_bbl_count = 0;
static unsigned cur_max_ins_count = 0;
static unsigned cur_max_tc_size = 0;

// Image address range, per-address flags and app-address -> entry index.
static ADDRINT img_lo = 0;
static ADDRINT img_hi = 0;
static unsigned char *img_flags = nullptr;
#define FLAG_RTN_ENTRY 1
#define FLAG_BR_TARGET 2
static unsigned *addr_idx = nullptr;      // entry index + 1, 0 = none
static size_t addr_idx_bytes = 0;

static inline bool in_img(ADDRINT a) { return a >= img_lo && a < img_hi; }
static inline void set_flag(ADDRINT a, unsigned char f)
{
    if (in_img(a))
        img_flags[a - img_lo] |= f;
}

// One record per translated routine (has a NOP7 head in TC).
struct RtnInfo {
  ADDRINT app_addr;
  ADDRINT app_size;
  unsigned tc_head_entry;   // NOP7 index in the TC instr_map
  unsigned first_bbl;
  unsigned end_bbl;         // exclusive
  ADDRINT orig_entry;       // Pin trampoline to the original code, 0 if not probed
  bool no_layout;           // has loop/jrcxz (rel8 only): keep original order
  bool force_hot;           // _exit: keep the starter's path
  unsigned char orig_bytes[16];  // entry bytes before our probe
  unsigned char probe_len;  // bytes the probe changed (0 = not restorable)
  bool unprobe;             // cold stub targets app_addr; entry restored at commit
  ADDRINT *stub_slot;       // cold stub jmp slot (TC2)
};
static std::vector<RtnInfo> g_rtns;
static ADDRINT g_exit_addr = 0;

// Address 0 is a real target (weak undefined symbols, e.g. _init's
// "call 0" for __gmon_start__ in static gcc); 0 also means "no target".
// Record a genuine 0 as ZERO_ADDR_MARK and map it back when used.
#define ZERO_ADDR_MARK ((ADDRINT)1)
static inline ADDRINT real_addr(ADDRINT a) { return a == ZERO_ADDR_MARK ? 0 : a; }

/* ============================================================= */
/* Service instr routines                                        */
/* ============================================================= */
bool isUncondJump(INS ins)
{
    const xed_decoded_inst_t* xedd = INS_XedDec(ins);
    xed_category_enum_t category_enum = xed_decoded_inst_get_category(xedd);
    if (category_enum == XED_CATEGORY_UNCOND_BR)
      return true;
    return false;
}

/* ---------------------------------------------------------------------- */
/* Probe safety (from ex3)                                                */
/* ---------------------------------------------------------------------- */
static const unsigned PROBE_JUMP_PATCH_BYTES = 14;

static bool any_flag_after(ADDRINT start, unsigned char f)
{
    for (ADDRINT a = start + 1; a < start + PROBE_JUMP_PATCH_BYTES; a++) {
        if (in_img(a) && (img_flags[a - img_lo] & f))
            return true;
    }
    return false;
}

static bool has_no_other_rtn_entry_in_probe_patch(RTN rtn)
{
    return !any_flag_after(RTN_Address(rtn), FLAG_RTN_ENTRY);
}

static bool has_no_branch_target_inside_probe_patch(RTN rtn)
{
    return !any_flag_after(RTN_Address(rtn), FLAG_BR_TARGET);
}

// First PROBE_JUMP_PATCH_BYTES must decode and hold no branch and no
// RIP-relative operand.
static bool has_decodable_probe_header(RTN rtn)
{
    ADDRINT pc = RTN_Address(rtn);
    unsigned decoded_bytes = 0;

    while (decoded_bytes < PROBE_JUMP_PATCH_BYTES) {
        xed_decoded_inst_t xedd;
        xed_decoded_inst_zero_set_mode(&xedd, &dstate);

        xed_error_enum_t xed_code =
            xed_decode(&xedd, reinterpret_cast<UINT8*>(pc + decoded_bytes), max_inst_len);
        if (xed_code != XED_ERROR_NONE)
            return false;

        unsigned len = xed_decoded_inst_get_length(&xedd);
        if (len == 0)
            return false;

        xed_category_enum_t category_enum = xed_decoded_inst_get_category(&xedd);
        if (category_enum == XED_CATEGORY_CALL ||
            category_enum == XED_CATEGORY_COND_BR ||
            category_enum == XED_CATEGORY_UNCOND_BR ||
            category_enum == XED_CATEGORY_RET)
            return false;

        unsigned memops = xed_decoded_inst_number_of_memory_operands(&xedd);
        for (unsigned i = 0; i < memops; i++) {
            if (xed_decoded_inst_get_base_reg(&xedd, i) == XED_REG_RIP)
                return false;
        }

        decoded_bytes += len;
    }

    return true;
}

bool isJumpOrRet(INS ins)
{
   if (!INS_IsCall(ins) &&
       (INS_IsIndirectControlFlow(ins) ||
        INS_IsDirectControlFlow(ins) ||
        INS_IsRet(ins)))
     return true;

   return false;
}

int create_nop7_xedd_instr(xed_decoded_inst_t *xedd)
{
  xed_encoder_instruction_t enc_instr;
  xed_encoder_request_t enc_req;
  char encoded_ins[XED_MAX_INSTRUCTION_BYTES];
  unsigned int ilen = XED_MAX_INSTRUCTION_BYTES;
  unsigned int olen = 0;

  xed_inst0(&enc_instr, dstate, XED_ICLASS_NOP7, 64);

  xed_encoder_request_zero_set_mode(&enc_req, &dstate);
  xed_bool_t convert_ok = xed_convert_to_encoder_request(&enc_req, &enc_instr);
  if (!convert_ok) {
      cerr << "conversion to encode request failed" << endl;
      return -1;
  }
  xed_error_enum_t xed_error = xed_encode(&enc_req,
            reinterpret_cast<UINT8*>(encoded_ins), ilen, &olen);
  if (xed_error != XED_ERROR_NONE) {
      cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
    return -1;
  }
  xed_decoded_inst_zero_set_mode(xedd, &dstate);
  xed_error_enum_t xed_code = xed_decode(xedd, reinterpret_cast<UINT8*>(&encoded_ins), max_inst_len);
  if (xed_code != XED_ERROR_NONE) {
      cerr << "DECODE ERROR: " << xed_error_enum_t2str(xed_code) << endl;
      return -1;
  }
  return 0;
}

// Branches that only have a rel8 form.
static bool is_rel8_only_iclass(xed_iclass_enum_t ic)
{
    return ic == XED_ICLASS_LOOP || ic == XED_ICLASS_LOOPE || ic == XED_ICLASS_LOOPNE ||
           ic == XED_ICLASS_JCXZ || ic == XED_ICLASS_JECXZ || ic == XED_ICLASS_JRCXZ;
}

// Operands of an indirect jmp/call.  The explicit target operand is the
// memory operand that is read (call also has an implicit stack write).
struct IndOp {
  bool mem;
  xed_reg_enum_t reg;
  xed_reg_enum_t base;
  xed_reg_enum_t index;
  xed_reg_enum_t seg;
  xed_int64_t disp;
  xed_uint_t scale;
  xed_uint_t disp_width;
  unsigned addr_width;
};

static bool decode_indirect_op(const xed_decoded_inst_t *xedd, IndOp *op)
{
  op->mem = false;
  op->reg = op->base = op->index = op->seg = XED_REG_INVALID;
  op->disp = 0;
  op->scale = 0;
  op->disp_width = 0;
  op->addr_width = 0;
  unsigned n = xed_decoded_inst_number_of_memory_operands(xedd);
  for (unsigned i = 0; i < n; i++) {
    if (!xed_decoded_inst_mem_read(xedd, i))
      continue;
    op->mem = true;
    op->base = xed_decoded_inst_get_base_reg(xedd, i);
    op->index = xed_decoded_inst_get_index_reg(xedd, i);
    op->seg = xed_decoded_inst_get_seg_reg(xedd, i);
    op->disp = xed_decoded_inst_get_memory_displacement(xedd, i);
    op->scale = xed_decoded_inst_get_scale(xedd, i);
    op->disp_width = xed_decoded_inst_get_memory_displacement_width_bits(xedd, i);
    op->addr_width = xed_decoded_inst_get_memop_address_width(xedd, i);
    return true;
  }
  op->reg = xed_decoded_inst_get_reg(xedd, XED_OPERAND_REG0);
  return op->reg != XED_REG_INVALID;
}


/* ============================================================= */
/* Service dump routines                                         */
/* ============================================================= */

void dump_image_instrs(IMG img)
{
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            RTN_Open( rtn );
            cerr << RTN_Name(rtn) << ":" << endl;
            for( INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins) )
            {
                  cerr << "0x" << hex << INS_Address(ins) << ": " << INS_Disassemble(ins) << endl;
            }
            RTN_Close( rtn );
            cerr << endl;
        }
    }
}

void dump_instr_from_mem (ADDRINT *address, ADDRINT new_addr)
{
  char disasm_buf[2048];
  xed_decoded_inst_t new_xedd;

  xed_decoded_inst_zero_set_mode(&new_xedd,&dstate);

  xed_error_enum_t xed_code = xed_decode(&new_xedd, reinterpret_cast<UINT8*>(address), max_inst_len);

  BOOL xed_ok = (xed_code == XED_ERROR_NONE);
  if (!xed_ok){
      cerr << "invalid opcode" << endl;
  }

  xed_format_context(XED_SYNTAX_INTEL, &new_xedd, disasm_buf, 2048, static_cast<UINT64>(new_addr), 0, 0);

  cerr << "0x" << hex << new_addr << ": " << disasm_buf <<  endl;
}

/*******************/
/*  dump_profile() */
/*******************/
// Called by the TC2 thread right after profiling is disabled.
void dump_profile()
{
    if (!out)
      return;
    for (unsigned r = 0; r < g_rtns.size(); r++) {
      const RtnInfo &ri = g_rtns[r];
      UINT64 mx = 0;
      for (unsigned b = ri.first_bbl; b < ri.end_bbl; b++)
        mx = std::max(mx, bbl_map[b].counter);
      if (!mx)
        continue;
      PIN_LockClient();
      RTN rtn = RTN_FindByAddress(ri.app_addr);
      string name = (rtn == RTN_Invalid()) ? string("Unknown") : RTN_Name(rtn);
      PIN_UnlockClient();
      *out << name << " 0x" << hex << ri.app_addr << dec << " max BBL count: " << mx << endl;
      for (unsigned b = ri.first_bbl; b < ri.end_bbl; b++) {
        const bbl_map_t &bm = bbl_map[b];
        if (!bm.counter && !bm.fallthru_counter)
          continue;
        *out << "  BBL 0x" << hex << bm.bbl_addr << dec
             << " heat: " << bm.counter << " FT heat: " << bm.fallthru_counter;
        if (bm.targ_slot) {
          const targ_prof_t &tp = targ_pool[bm.targ_slot - 1];
          for (unsigned j = 0; j <= MAX_TARG_ADDRS; j++) {
            if (tp.s[j].count)
              *out << " targ 0x" << hex << tp.s[j].addr << dec << ":" << tp.s[j].count;
          }
        }
        *out << endl;
      }
    }
    out->flush();
}

void dump_instr_map_entry(unsigned instr_map_entry)
{
    cerr << dec << instr_map_entry << ": ";
    cerr << " orig_ins_addr: 0x" << hex << instr_map[instr_map_entry].orig_ins_addr;
    cerr << " new_ins_addr: 0x" << hex << instr_map[instr_map_entry].new_ins_addr;

    if (instr_map[instr_map_entry].orig_targ_addr) {
      cerr << " orig_targ_addr: 0x" << hex << instr_map[instr_map_entry].orig_targ_addr;
      ADDRINT new_targ_addr;
      if (instr_map[instr_map_entry].targ_map_entry >= 0)
          new_targ_addr = instr_map[instr_map[instr_map_entry].targ_map_entry].new_ins_addr;
      else
          new_targ_addr = instr_map[instr_map_entry].orig_targ_addr;
      cerr << " new_targ_addr: 0x" << hex << new_targ_addr;
    }

    cerr << "    new instr:";
    dump_instr_from_mem((ADDRINT *)instr_map[instr_map_entry].encoded_ins,
                        instr_map[instr_map_entry].new_ins_addr);
}

void dump_tc(char *tc, unsigned size_tc)
{
  char disasm_buf[2048];
  xed_decoded_inst_t new_xedd;
  ADDRINT address = (ADDRINT)&tc[0];

  while (address < (ADDRINT)&tc[size_tc]) {

      xed_decoded_inst_zero_set_mode(&new_xedd,&dstate);
      xed_error_enum_t xed_code = xed_decode(&new_xedd, reinterpret_cast<UINT8*>(address), max_inst_len);

      BOOL xed_ok = (xed_code == XED_ERROR_NONE);
      if (!xed_ok){
          cerr << "invalid opcode" << endl;
          return;
      }

      xed_format_context(XED_SYNTAX_INTEL, &new_xedd, disasm_buf, 2048, static_cast<UINT64>(address), 0, 0);

      cerr << "0x" << hex << address << ": " << disasm_buf <<  endl;

      address += xed_decoded_inst_get_length (&new_xedd);
  }
}


/* ============================================================= */
/* Translation routines                                         */
/* ============================================================= */

/****************************************************/
/* Encode a direct uncond jump from pc to targ_addr.*/
/****************************************************/
int encode_jump_instr(ADDRINT pc, ADDRINT target_addr, char *encoded_jmp_ins)
{
    xed_encoder_instruction_t enc_instr;
    xed_encoder_request_t enc_req;
    unsigned int ilen = XED_MAX_INSTRUCTION_BYTES;
    unsigned int olen = 0;

    xed_int64_t disp = target_addr - pc - olen;

    if (disp >= -128 && disp <= 127)
      xed_inst1(&enc_instr, dstate, XED_ICLASS_JMP, 64, xed_relbr(disp, 8));
    else
      xed_inst1(&enc_instr, dstate,  XED_ICLASS_JMP, 64, xed_relbr(disp, 32));

    xed_encoder_request_zero_set_mode(&enc_req, &dstate);
    xed_bool_t convert_ok = xed_convert_to_encoder_request(&enc_req, &enc_instr);
    if (!convert_ok) {
        cerr << "conversion to encode request failed" << endl;
        return -1;
    }
    xed_error_enum_t xed_error = xed_encode(&enc_req,
              reinterpret_cast<UINT8*>(encoded_jmp_ins), ilen, &olen);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
      return -1;
    }

    disp = target_addr - pc - olen;

    if (disp >= -128 && disp <= 127)
      xed_inst1(&enc_instr, dstate, XED_ICLASS_JMP, 64, xed_relbr(disp, 8));
    else
      xed_inst1(&enc_instr, dstate,  XED_ICLASS_JMP, 64, xed_relbr(disp, 32));

    xed_encoder_request_zero_set_mode(&enc_req, &dstate);
    convert_ok = xed_convert_to_encoder_request(&enc_req, &enc_instr);
    if (!convert_ok) {
        cerr << "conversion to encode request failed" << endl;
        return -1;
    }
    xed_error = xed_encode(&enc_req,
              reinterpret_cast<UINT8*>(encoded_jmp_ins), ilen, &olen);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
      return -1;
    }
    return olen;
}


/*********************************************************/
/* atomic_patch5: replace 5 code bytes that may be running */
/*********************************************************/
// One locked 8-byte cmpxchg (atomic even across a cache line); the 3 bytes
// after the patch are written back unchanged.
static void atomic_patch5(char *dst, const char *five)
{
    UINT64 expect;
    memcpy(&expect, dst, 8);
    for (;;) {
        UINT64 desired = expect;
        memcpy(&desired, five, 5);
        UINT64 prev = expect;
        __asm__ __volatile__("lock cmpxchgq %2, %1"
                             : "+a"(prev), "+m"(*(volatile UINT64 *)dst)
                             : "r"(desired)
                             : "memory", "cc");
        if (prev == expect)
            return;
        expect = prev;
    }
}

/***************************/
/* disable_profiling_in_tc */
/***************************/
// -migrate_tc: (TC stub head address, app address the stub continues at).
static std::vector<std::pair<ADDRINT, ADDRINT> > g_mig;
static unsigned g_migrated = 0;

int disable_profiling_in_tc(instr_map_t * instr_map, unsigned num_of_instr_map_entries)
{
    for (unsigned i = 0; i < num_of_instr_map_entries; i++) {
        // Overwrite the NOP at the head of each profiling stub by a jmp over it.
        if (instr_map[i].ins_type == ProfilingIns &&
            instr_map[i].stub_head) {
            // Stop at the next stub head (branches may enter there, so each
            // head needs its own skip jmp) and at rel8-trampoline entries
            // (jmp / label), which are live code.
            unsigned j = 1;
            xed_int64_t disp = 0;
            while (i + j < num_of_instr_map_entries &&
                   instr_map[i+j].ins_type == ProfilingIns &&
                   !instr_map[i+j].stub_head &&
                   instr_map[i+j].xed_category != XED_CATEGORY_UNCOND_BR &&
                   instr_map[i+j].xed_category != XED_CATEGORY_NOP) {
                disp += instr_map[i+j].size;
                j++;
            }

          xed_encoder_instruction_t enc_instr;
          xed_encoder_request_t enc_req;
          unsigned int ilen = XED_MAX_INSTRUCTION_BYTES;
          char encoded_jmp_ins[XED_MAX_INSTRUCTION_BYTES];
          unsigned int olen = 5; // skip jump instr is exactly 5 bytes long.

          disp += (instr_map[i].size - olen);
          xed_inst1(&enc_instr, dstate,  XED_ICLASS_JMP, 64, xed_relbr(disp, 32));

          xed_encoder_request_zero_set_mode(&enc_req, &dstate);
          xed_bool_t convert_ok = xed_convert_to_encoder_request(&enc_req, &enc_instr);
          if (!convert_ok) {
              cerr << "conversion to encode request failed" << endl;
              return -1;
          }
          xed_error_enum_t xed_error = xed_encode(&enc_req,
                    reinterpret_cast<UINT8*>(encoded_jmp_ins), ilen, &olen);
          if (xed_error != XED_ERROR_NONE) {
              cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
            return -1;
          }

          if (olen > instr_map[i].size) {
             cerr << " unable to set a relative jump to skip the profiling code stub at: "
                  << hex << "0x" << instr_map[i].new_ins_addr << "\n";
             return -1;
          }

          // The app may be executing this NOP right now.
          atomic_patch5((char *)instr_map[i].new_ins_addr, encoded_jmp_ins);

          // -migrate_tc: remember where this stub continues in the app, so the
          // skip jmp can later point into TC2 (the app state at a stub head is
          // the state at that app instruction).  Skip rel8 trampolines.
          if (KnobMigrateTc.Value()) {
            unsigned k = i + j;
            while (k < num_of_instr_map_entries && instr_map[k].ins_type == ProfilingIns &&
                   instr_map[k].xed_category != XED_CATEGORY_UNCOND_BR &&
                   instr_map[k].xed_category != XED_CATEGORY_NOP)
              k++;
            if (k < num_of_instr_map_entries && instr_map[k].ins_type == RegularIns &&
                instr_map[k].app_ins_addr)
              g_mig.push_back(std::make_pair(instr_map[i].new_ins_addr, instr_map[k].app_ins_addr));
          }
          i += (j - 1);
       }
    }
    return 0;
}

/*************************/
/* add_new_instr_entry() */
/*************************/
int add_new_instr_entry(xed_decoded_inst_t *xedd, ADDRINT pc, ins_enum_t ins_type)
{
    ADDRINT orig_targ_addr = 0x0;

    // Branch displacement -> absolute target.
    xed_uint_t disp_byts = xed_decoded_inst_get_branch_displacement_width(xedd);
    xed_int32_t disp;
    if (disp_byts > 0) {
      disp = xed_decoded_inst_get_branch_displacement(xedd);
      orig_targ_addr = pc + xed_decoded_inst_get_length (xedd) + disp;
      if (orig_targ_addr == 0)
        orig_targ_addr = ZERO_ADDR_MARK;
    }

    // RIP-relative memory operand -> absolute address.
    ADDRINT orig_rip_addr = 0x0;
    unsigned memops = xed_decoded_inst_number_of_memory_operands(xedd);
    for (unsigned m = 0; m < memops; m++) {
      if (xed_decoded_inst_get_base_reg(xedd, m) == XED_REG_RIP) {
         unsigned size = xed_decoded_inst_get_length (xedd);
         xed_int64_t mdisp = xed_decoded_inst_get_memory_displacement(xedd, m);
         orig_rip_addr = (ADDRINT)(pc + mdisp + size);
         if (orig_rip_addr == 0)
           orig_rip_addr = ZERO_ADDR_MARK;
         break;
      }
    }

    xed_encoder_request_init_from_decode (xedd);

    unsigned new_size = 0;

    xed_error_enum_t xed_error =
       xed_encode (xedd, reinterpret_cast<UINT8*>(instr_map[num_of_instr_map_entries].encoded_ins),
                   max_inst_len , &new_size);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
        return -1;
    }

    instr_map_t &e = instr_map[num_of_instr_map_entries];
    e.orig_ins_addr = pc;
    e.new_ins_addr = 0x0;
    e.orig_targ_addr = orig_targ_addr;
    e.orig_rip_addr = orig_rip_addr;
    e.app_ins_addr = pc;
    e.targ_map_entry = -1;
    e.size = new_size;
    e.ins_type = ins_type;
    e.bbl_num = bbl_num;
    e.xed_category = xed_decoded_inst_get_category(xedd);
    e.short_br = false;
    e.stub_head = false;

    num_of_instr_map_entries++;

    // First instruction of a headless stub: it takes the skip jmp (5 bytes).
    if (g_stub_first && ins_type == ProfilingIns) {
        g_stub_first = false;
        if (new_size >= 5) {
            e.stub_head = true;
        } else {
            // Too short: put a NOP5 head in front of it.
            instr_map_t real = e;
            static const char nop5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
            memcpy(e.encoded_ins, nop5, 5);
            e.size = 5;
            e.orig_targ_addr = 0;
            e.orig_rip_addr = 0;
            e.xed_category = XED_CATEGORY_WIDENOP;
            e.stub_head = true;
            instr_map[num_of_instr_map_entries] = real;
            num_of_instr_map_entries++;
        }
    }

    if (num_of_instr_map_entries >= cur_max_ins_count) {
        cerr << "out of memory for map_instr" << endl;
        return -1;
    }

    if (KnobVerbose) {
        cerr << "    new instr:";
        dump_instr_from_mem((ADDRINT *)instr_map[num_of_instr_map_entries-1].encoded_ins,
                            instr_map[num_of_instr_map_entries-1].new_ins_addr);
    }

    return new_size;
}

/***************************/
/* add_new_encoded_instr() */
/***************************/
int add_new_encoded_instr(ADDRINT ins_addr, xed_encoder_instruction_t *enc_instr, ins_enum_t ins_type) {
    char encoded_ins[XED_MAX_INSTRUCTION_BYTES];
    unsigned int ilen = XED_MAX_INSTRUCTION_BYTES;
    unsigned int olen = 0;

    xed_encoder_request_t enc_req;
    xed_encoder_request_zero_set_mode(&enc_req, &dstate);
    xed_bool_t convert_ok = xed_convert_to_encoder_request(&enc_req, enc_instr);
    if (!convert_ok) {
        cerr << "conversion to encode request failed" << endl;
        return -1;
    }

    xed_error_enum_t xed_error = xed_encode(&enc_req,
              reinterpret_cast<UINT8*>(encoded_ins), ilen, &olen);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
      return -1;
    }

    xed_decoded_inst_t xedd;
    xed_decoded_inst_zero_set_mode(&xedd,&dstate);
    xed_error_enum_t xed_code = xed_decode(&xedd, reinterpret_cast<UINT8*>(&encoded_ins), max_inst_len);
    if (xed_code != XED_ERROR_NONE) {
        cerr << "ERROR: xed decode failed for instr at: " << "0x" << hex << ins_addr << endl;
        return -1;
    }
    int rc = add_new_instr_entry(&xedd, ins_addr, ins_type);
    if (rc < 0) {
      cerr << "ERROR: failed during instructon translation." << endl;
      return -1;
    }
    return 0;
}

/*************************************************/
/* indirect_target_profile_is_encodable()        */
/*************************************************/
// The stub rewrites a RIP-relative operand as absolute [disp32] and drops
// segment overrides: skip target profiling where that is wrong.
static bool indirect_target_profile_is_encodable(INS ins, ADDRINT ins_addr)
{
  IndOp op;
  if (!decode_indirect_op(INS_XedDec(ins), &op))
    return false;
  if (!op.mem)
    return op.reg != XED_REG_RSP;
  if (op.seg == XED_REG_FS || op.seg == XED_REG_GS)
    return false;
  if (op.base != XED_REG_RIP)
    return true;

  unsigned orig_size = INS_Size(ins);
  xed_int64_t abs_addr = (xed_int64_t)ins_addr + op.disp + (xed_int64_t)orig_size;
  if (abs_addr > 0x7FFFFFFFLL || abs_addr < -0x7FFFFFFFLL) {
    VLOG(<< " skipping indirect target profiling at 0x" << hex << ins_addr
         << ": absolute address does not fit in 32 bits\n");
    return false;
  }
  return true;
}

/* ---------------------------------------------------------------------- */
/* Liveness for the cheap counter stubs (-opt_dead_regs, from option 3).   */
/* Conservative: dead only if straight-line code after the stub overwrites */
/* it before any read; call/ret/branch end the scan as "live".             */
/* ---------------------------------------------------------------------- */
static bool ends_straight_line(INS ins)
{
    return INS_IsRet(ins) || INS_IsCall(ins) || INS_IsBranch(ins) ||
           INS_IsIndirectControlFlow(ins) || INS_IsDirectControlFlow(ins) ||
           INS_IsSyscall(ins) || INS_IsInterrupt(ins);
}

// Register writes that may keep the old value (no kill).
static bool has_conditional_reg_write(INS ins)
{
    if (INS_IsPredicated(ins) || INS_Category(ins) == XED_CATEGORY_CMOV)
        return true;
    xed_iclass_enum_t ic = xed_decoded_inst_get_iclass(INS_XedDec(ins));
    return ic == XED_ICLASS_BSF || ic == XED_ICLASS_BSR ||
           ic == XED_ICLASS_CMPXCHG || ic == XED_ICLASS_CMPXCHG8B ||
           ic == XED_ICLASS_CMPXCHG16B;
}

// Routine being translated: app address -> INS (RTN is open meanwhile).
static std::map<ADDRINT, INS> g_rtn_ins;
static const unsigned LIVE_SCAN_MAX = 64;   // instructions per query

// Control successors of ins inside the routine.  false: control may leave
// the routine or is not followed (call/ret/indirect/syscall/int).
static bool live_succs(INS ins, INS *succ, unsigned *n)
{
    *n = 0;
    if (INS_IsRet(ins) || INS_IsCall(ins) || INS_IsIndirectControlFlow(ins) ||
        INS_IsSyscall(ins) || INS_IsInterrupt(ins))
        return false;
    if (INS_IsDirectControlFlow(ins)) {
        if (!KnobLiveWide.Value())
            return false;
        std::map<ADDRINT, INS>::const_iterator it =
            g_rtn_ins.find(INS_DirectControlFlowTargetAddress(ins));
        if (it == g_rtn_ins.end())
            return false;
        succ[(*n)++] = it->second;
        if (INS_Category(ins) == XED_CATEGORY_UNCOND_BR)
            return true;            // jmp: target only
    } else if (ends_straight_line(ins)) {
        return false;
    }
    INS nx = INS_Next(ins);
    if (!INS_Valid(nx))
        return false;
    succ[(*n)++] = nx;              // fall-through (also of a jcc)
    return true;
}

// Candidate scratch registers for the counter stub (bit k = kCands[k]).
static const REG kCands[] = {
    LEVEL_BASE::REG_RAX, LEVEL_BASE::REG_RCX, LEVEL_BASE::REG_RDX,
    LEVEL_BASE::REG_RSI, LEVEL_BASE::REG_RDI, LEVEL_BASE::REG_R8,
    LEVEL_BASE::REG_R9,  LEVEL_BASE::REG_R10, LEVEL_BASE::REG_R11,
    LEVEL_BASE::REG_RBX, LEVEL_BASE::REG_RBP, LEVEL_BASE::REG_R12,
    LEVEL_BASE::REG_R13, LEVEL_BASE::REG_R14, LEVEL_BASE::REG_R15
};
static const unsigned kNumCands = sizeof(kCands) / sizeof(kCands[0]);

static int cand_bit(REG r)
{
    REG f = REG_FullRegName(r);
    for (unsigned k = 0; k < kNumCands; k++)
        if (kCands[k] == f)
            return (int)k;
    return -1;
}

static int cand_bit_xed(xed_reg_enum_t x)
{
    if (x == XED_REG_INVALID)
        return -1;
    xed_reg_enum_t f = xed_get_largest_enclosing_register(x);
    for (unsigned k = 0; k < kNumCands; k++)
        if (INS_XedExactMapFromPinReg(kCands[k]) == f)
            return (int)k;
    return -1;
}

// Candidates read by ins (registers, incl. memory base/index).
static UINT32 cand_reads(INS ins)
{
    UINT32 m = 0;
    for (UINT32 k = 0; k < INS_MaxNumRRegs(ins); k++) {
        int b = cand_bit(INS_RegR(ins, k));
        if (b >= 0) m |= 1u << b;
    }
    const xed_decoded_inst_t *xedd = INS_XedDec(ins);
    for (unsigned j = 0; j < xed_decoded_inst_number_of_memory_operands(xedd); j++) {
        int b = cand_bit_xed(xed_decoded_inst_get_base_reg(xedd, j));
        int x = cand_bit_xed(xed_decoded_inst_get_index_reg(xedd, j));
        if (b >= 0) m |= 1u << b;
        if (x >= 0) m |= 1u << x;
    }
    return m;
}

// Candidates killed by ins (unconditional 64/32-bit write).
static UINT32 cand_kills(INS ins)
{
    if (ends_straight_line(ins) || has_conditional_reg_write(ins))
        return 0;
    UINT32 m = 0;
    for (UINT32 k = 0; k < INS_MaxNumWRegs(ins); k++) {
        REG w = INS_RegW(ins, k);
        if (!(REG_is_gr64(w) || REG_is_gr32(w)))
            continue;
        int b = cand_bit(w);
        if (b >= 0) m |= 1u << b;
    }
    return m;
}

// Mask of candidates that are overwritten before any read on every path
// from start (fall-through, direct jmp, both jcc edges; bounded).
static UINT32 gpr_dead_mask(INS start)
{
    const UINT32 all = (1u << kNumCands) - 1;
    UINT32 live = 0;
    std::vector<std::pair<INS, UINT32> > work;
    work.push_back(std::make_pair(start, all));
    std::vector<std::pair<ADDRINT, UINT32> > seen;
    unsigned budget = LIVE_SCAN_MAX;
    while (!work.empty()) {
        INS ins = work.back().first;
        UINT32 pend = work.back().second;
        work.pop_back();
        for (;;) {
            pend &= ~live;
            if (!pend)
                break;
            std::pair<ADDRINT, UINT32> key(INS_Address(ins), pend);
            if (std::find(seen.begin(), seen.end(), key) != seen.end())
                break;
            if (budget == 0)
                return 0;
            budget--;
            seen.push_back(key);
            live |= cand_reads(ins) & pend;
            pend &= ~live;
            pend &= ~cand_kills(ins);
            if (!pend)
                break;
            INS succ[2];
            unsigned n = 0;
            if (!live_succs(ins, succ, &n)) {
                live |= pend;
                break;
            }
            if (n == 2)
                work.push_back(std::make_pair(succ[1], pend));
            ins = succ[0];
        }
    }
    return all & ~live;
}

// Flags clobbered by INC: OF SF ZF AF PF (CF is kept).
static xed_uint32_t inc_clobbered_flags()
{
    xed_flag_set_t fs;
    fs.flat = 0;
    fs.s.of = 1; fs.s.sf = 1; fs.s.zf = 1; fs.s.af = 1; fs.s.pf = 1;
    return fs.flat;
}

// True if OF/SF/ZF/AF/PF are all overwritten before any read on every path
// from start (same path rules as gpr_dead_mask).
static bool inc_flags_dead_at(INS start)
{
    std::vector<std::pair<INS, xed_uint32_t> > work;
    work.push_back(std::make_pair(start, inc_clobbered_flags()));
    std::vector<std::pair<ADDRINT, xed_uint32_t> > seen;
    unsigned budget = LIVE_SCAN_MAX;
    while (!work.empty()) {
        INS ins = work.back().first;
        xed_uint32_t pending = work.back().second;
        work.pop_back();
        for (;;) {
            std::pair<ADDRINT, xed_uint32_t> key(INS_Address(ins), pending);
            if (std::find(seen.begin(), seen.end(), key) != seen.end())
                break;
            if (budget == 0)
                return false;
            budget--;
            seen.push_back(key);
            const xed_decoded_inst_t *xedd = INS_XedDec(ins);
            const xed_simple_flag_t *fi = xed_decoded_inst_get_rflags_info(xedd);
            if (fi) {
                const xed_flag_set_t *rd = xed_simple_flag_get_read_flag_set(fi);
                if (rd && (rd->flat & pending))
                    return false;
            }
            if (!fi && INS_RegRContain(ins, LEVEL_BASE::REG_RFLAGS))
                return false;
            if (!INS_IsDirectControlFlow(ins) && ends_straight_line(ins))
                return false;
            if (fi && xed_simple_flag_get_must_write(fi)) {
                const xed_flag_set_t *wr = xed_simple_flag_get_written_flag_set(fi);
                const xed_flag_set_t *un = xed_simple_flag_get_undefined_flag_set(fi);
                pending &= ~((wr ? wr->flat : 0) | (un ? un->flat : 0));
                if (!pending)
                    break;
            }
            INS succ[2];
            unsigned n = 0;
            if (!live_succs(ins, succ, &n))
                return false;
            if (n == 2)
                work.push_back(std::make_pair(succ[1], pending));
            ins = succ[0];
        }
    }
    return true;
}

// A GPR (not RSP) dead at 'start', or REG_INVALID.
static REG pick_dead_gpr(INS start)
{
    UINT32 m = gpr_dead_mask(start);
    for (unsigned k = 0; k < kNumCands; k++) {
        if (m & (1u << k))
            return kCands[k];
    }
    return REG_INVALID();
}

// Start a profiling stub: a NOP5 head (-stub_nohead 0) or mark the stub's
// first instruction as its head.
static int begin_stub(ADDRINT ins_addr)
{
    if (KnobStubNoHead.Value()) {
        g_stub_first = true;
        return 0;
    }
    xed_encoder_instruction_t enc;
    xed_inst0(&enc, dstate, XED_ICLASS_NOP5, 64);
    if (add_new_encoded_instr(ins_addr, &enc, ProfilingIns) < 0)
        return -1;
    instr_map[num_of_instr_map_entries - 1].stub_head = true;
    return 0;
}

// RIP-relative profiling instruction whose absolute target is 'abs'.
static int add_prof_rip_instr(ADDRINT ins_addr, xed_encoder_instruction_t *enc, ADDRINT abs)
{
    if (add_new_encoded_instr(ins_addr, enc, ProfilingIns) < 0)
        return -1;
    instr_map[num_of_instr_map_entries - 1].orig_rip_addr = abs;
    return 0;
}

/**********************/
/* add_counter_stub() */
/**********************/
// ++*cnt, placed before 'live_from' (the next app instruction).
// opt_dead_regs 0: spill RAX; mov/lea/mov; reload (starter style).
// opt_dead_regs 1: 'inc [rip+x]' if flags are dead, else mov/lea/mov via a
// dead GPR, else the spill form.
int add_counter_stub(INS live_from, ADDRINT ins_addr, UINT64 *cnt)
{
  xed_encoder_instruction_t enc;
  const ADDRINT c = (ADDRINT)cnt;

  // Head, patched into 'jmp over stub' when profiling ends.
  if (begin_stub(ins_addr) < 0)
    return -1;

  if (KnobOptDeadRegs.Value() && INS_Valid(live_from)) {
    if (inc_flags_dead_at(live_from)) {
      xed_inst1(&enc, dstate, XED_ICLASS_INC, 64,
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
      n_stub_inc++;
      return add_prof_rip_instr(ins_addr, &enc, c);
    }
    REG r = pick_dead_gpr(live_from);
    if (r != REG_INVALID()) {
      xed_reg_enum_t xr = INS_XedExactMapFromPinReg(r);
      xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64, xed_reg(xr),
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
      if (add_prof_rip_instr(ins_addr, &enc, c) < 0)
        return -1;
      xed_inst2(&enc, dstate, XED_ICLASS_LEA, 64, xed_reg(xr),
                xed_mem_bd(xr, xed_disp(1, 8), 64));
      if (add_new_encoded_instr(ins_addr, &enc, ProfilingIns) < 0)
        return -1;
      xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64), xed_reg(xr));
      n_stub_reg++;
      return add_prof_rip_instr(ins_addr, &enc, c);
    }
  }

  n_stub_spill++;
  if (KnobOptDeadRegs.Value()) {
    // Spill RAX, rip-relative (slot next to the counters).
    const ADDRINT rs = (ADDRINT)prof_rax_rip;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64), xed_reg(XED_REG_RAX));
    if (add_prof_rip_instr(ins_addr, &enc, rs) < 0)
      return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
    if (add_prof_rip_instr(ins_addr, &enc, c) < 0)
      return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_LEA, 64, xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_RAX, xed_disp(1, 8), 64));
    if (add_new_encoded_instr(ins_addr, &enc, ProfilingIns) < 0)
      return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64), xed_reg(XED_REG_RAX));
    if (add_prof_rip_instr(ins_addr, &enc, c) < 0)
      return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
    return add_prof_rip_instr(ins_addr, &enc, rs);
  }

  // Spill RAX (absolute moffs64 forms, as the starter).
  const ADDRINT slot = (ADDRINT)&g_prof_rax_slot;
  xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
            xed_mem_bd(XED_REG_INVALID, xed_disp(slot, 64), 64), xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc, ProfilingIns) < 0)
    return -1;
  xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
            xed_mem_bd(XED_REG_INVALID, xed_disp(c, 64), 64));
  if (add_new_encoded_instr(ins_addr, &enc, ProfilingIns) < 0)
    return -1;
  xed_inst2(&enc, dstate, XED_ICLASS_LEA, 64, xed_reg(XED_REG_RAX),
            xed_mem_bd(XED_REG_RAX, xed_disp(1, 8), 64));
  if (add_new_encoded_instr(ins_addr, &enc, ProfilingIns) < 0)
    return -1;
  xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
            xed_mem_bd(XED_REG_INVALID, xed_disp(c, 64), 64), xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc, ProfilingIns) < 0)
    return -1;
  xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
            xed_mem_bd(XED_REG_INVALID, xed_disp(slot, 64), 64));
  return add_new_encoded_instr(ins_addr, &enc, ProfilingIns);
}

// RFLAGS may be live after an indirect jmp (a jump-table target can read
// flags set before the jmp). Dead at a call (ABI) and at jmp [rip+x] (PLT/tail call).
static bool jmp_flags_maybe_live(bool is_call, const IndOp &op)
{
  if (!KnobTargFlags.Value() || is_call)
    return false;
  return !(op.mem && op.base == XED_REG_RIP && op.index == XED_REG_INVALID);
}

// save: lea rsp,[rsp-128]; pushfq   restore: popfq; lea rsp,[rsp+128]
// (skip the red zone, lea keeps flags).
static int add_flags_save(ADDRINT ins_addr, bool save, ins_enum_t type)
{
  xed_encoder_instruction_t enc;
  if (!save) {
    xed_inst0(&enc, dstate, XED_ICLASS_POPFQ, 64);
    if (add_new_encoded_instr(ins_addr, &enc, type) < 0) return -1;
  }
  xed_inst2(&enc, dstate, XED_ICLASS_LEA, 64, xed_reg(XED_REG_RSP),
            xed_mem_bd(XED_REG_RSP, xed_disp(save ? -128 : 128, 32), 64));
  if (add_new_encoded_instr(ins_addr, &enc, type) < 0) return -1;
  if (save) {
    xed_inst0(&enc, dstate, XED_ICLASS_PUSHFQ, 64);
    if (add_new_encoded_instr(ins_addr, &enc, type) < 0) return -1;
  }
  return 0;
}

/******************************/
/* add_indirect_target_stub() */
/******************************/
// Record the target of an indirect jmp/call (16-slot hash); placed right
// before it.  The BBL count is done by add_counter_stub().
int add_indirect_target_stub(INS ins, ADDRINT ins_addr, unsigned bbl_num)
{
  xed_encoder_instruction_t enc_instr;

  static uint64_t rax_mem = 0;
  static uint64_t rbx_mem = 0;
  static uint64_t rcx_mem = 0;

  g_indirect_prof_emitted = false;
  if (!INS_IsIndirectControlFlow(ins) || INS_IsRet(ins) ||
      !indirect_target_profile_is_encodable(ins, ins_addr))
    return 0;
  if (!bbl_map[bbl_num].targ_slot) {
    if (targ_num >= max_targ)
      return 0;                       // pool full: no target profile here
    bbl_map[bbl_num].targ_slot = ++targ_num;
  }
  targ_prof_t *tp = &targ_pool[bbl_map[bbl_num].targ_slot - 1];
  tp->base = (ADDRINT)&tp->s[0];
  g_indirect_prof_emitted = true;

  IndOp op;
  decode_indirect_op(INS_XedDec(ins), &op);
  const bool reset = KnobTargReset.Value();

  // Head, patched into 'jmp over stub' when profiling ends.
  if (begin_stub(ins_addr) < 0)
    return -1;

  if (KnobCheapTarg.Value()) {
    // A = target, B = slot pointer.  At an indirect call R11 is dead (not
    // an argument; the unknown callee may clobber it) unless the operand
    // uses it; everything else is spilled to rip-relative slots.
    bool uses_r11 = op.mem ? (op.base == XED_REG_R11 || op.index == XED_REG_R11 ||
                              xed_get_largest_enclosing_register(op.base) == XED_REG_R11 ||
                              xed_get_largest_enclosing_register(op.index) == XED_REG_R11)
                           : (op.reg == XED_REG_R11);
    bool a_free = INS_IsCall(ins) && !uses_r11;
    // Flags live at a jmp: keep them in AL/AH (seto/lahf), so A moves to RDX.
    const bool keep_flags = jmp_flags_maybe_live(INS_IsCall(ins), op);
    xed_reg_enum_t A = a_free ? XED_REG_R11 : (keep_flags ? XED_REG_RDX : XED_REG_RAX);
    xed_reg_enum_t B = XED_REG_RCX;
    const ADDRINT sA = (ADDRINT)&targ_spill_rip[0], sB = (ADDRINT)&targ_spill_rip[1];
    const ADDRINT sF = (ADDRINT)&targ_spill_rip[2];

    if (keep_flags) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64), xed_reg(XED_REG_RAX));
      if (add_prof_rip_instr(ins_addr, &enc_instr, sF) < 0) return -1;
    }
    if (!a_free) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64), xed_reg(A));
      if (add_prof_rip_instr(ins_addr, &enc_instr, sA) < 0) return -1;
    }
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64), xed_reg(B));
    if (add_prof_rip_instr(ins_addr, &enc_instr, sB) < 0) return -1;

    // A = target (operand registers are still intact here).
    if (op.mem && op.base == XED_REG_RIP) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(A),
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
      if (add_prof_rip_instr(ins_addr, &enc_instr,
                             (ADDRINT)(ins_addr + op.disp + INS_Size(ins))) < 0) return -1;
    } else if (op.mem) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(A),
                xed_mem_bisd(op.base, op.index, op.scale,
                             xed_disp(op.disp, op.disp_width ? op.disp_width : 32),
                             op.addr_width ? op.addr_width : 64));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
    } else if (op.reg != A) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(A), xed_reg(op.reg));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
    }
    if (keep_flags) {             // seto al; lahf
      xed_inst1(&enc_instr, dstate, XED_ICLASS_SETO, 8, xed_reg(XED_REG_AL));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
      xed_inst0(&enc_instr, dstate, XED_ICLASS_LAHF, 32);
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
    }

    // B = &slot = base + (A & 0xF0)
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(B), xed_reg(A));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
    xed_inst2(&enc_instr, dstate, XED_ICLASS_AND, 64, xed_reg(B), xed_imm0(0xF0, 32));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
    xed_inst2(&enc_instr, dstate, XED_ICLASS_ADD, 64, xed_reg(B),
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
    if (add_prof_rip_instr(ins_addr, &enc_instr, (ADDRINT)&tp->base) < 0) return -1;

    if (reset) {
      // ZF = same target; store target; count = ZF ? count+1 : 1.
      xed_inst2(&enc_instr, dstate, XED_ICLASS_CMP, 64,
                xed_mem_bd(B, xed_disp(0, 8), 64), xed_reg(A));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_mem_bd(B, xed_disp(0, 8), 64), xed_reg(A));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(A),
                xed_mem_bd(B, xed_disp(8, 8), 64));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_LEA, 64, xed_reg(A),
                xed_mem_bd(A, xed_disp(1, 8), 64));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_CMOVNZ, 64, xed_reg(A),
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
      if (add_prof_rip_instr(ins_addr, &enc_instr, (ADDRINT)prof_one_rip) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_mem_bd(B, xed_disp(8, 8), 64), xed_reg(A));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
    } else {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_mem_bd(B, xed_disp(0, 8), 64), xed_reg(A));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
      xed_inst1(&enc_instr, dstate, XED_ICLASS_INC, 64, xed_mem_bd(B, xed_disp(8, 8), 64));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
    }
    if (keep_flags) {             // add al,0x7f (OF back); sahf; reload RAX
      xed_inst2(&enc_instr, dstate, XED_ICLASS_ADD, 8, xed_reg(XED_REG_AL), xed_imm0(0x7f, 8));
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
      xed_inst0(&enc_instr, dstate, XED_ICLASS_SAHF, 32);
      if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
      if (add_prof_rip_instr(ins_addr, &enc_instr, sF) < 0) return -1;
    }

    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(B),
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
    if (add_prof_rip_instr(ins_addr, &enc_instr, sB) < 0) return -1;
    if (!a_free) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(A),
                xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
      if (add_prof_rip_instr(ins_addr, &enc_instr, sA) < 0) return -1;
    }
    return 0;
  }

  // Starter-style stub: RAX/RBX/RCX saved to absolute slots.
  // RAX = target; RBX = target; RAX = target & 0xF0 (slot offset);
  // record target and count; restore.  (Modifies RFLAGS.)
  xed_reg_enum_t base_reg = op.base;
  xed_reg_enum_t index_reg = op.index;
  xed_reg_enum_t targ_reg = op.mem ? XED_REG_INVALID : op.reg;

  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
            xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rax_mem, 64), 64),
            xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX), xed_reg(XED_REG_RBX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
            xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rbx_mem, 64), 64),
            xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX), xed_reg(XED_REG_RCX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
            xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rcx_mem, 64), 64),
            xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;

  // Operand uses RAX: restore it first.
  if (targ_reg == XED_REG_RAX || base_reg == XED_REG_RAX || index_reg == XED_REG_RAX) {
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rax_mem, 64), 64));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  }
  if (base_reg == XED_REG_RIP) {
    // [RIP+disp] -> [absolute] (range checked above).
    xed_int64_t new_disp = ins_addr + op.disp + INS_Size(ins);
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
              xed_mem_bisd(XED_REG_INVALID, index_reg, op.scale, xed_disp(new_disp, 32),
                           op.addr_width));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  } else if (targ_reg != XED_REG_RAX) {
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
              (targ_reg != XED_REG_INVALID ? xed_reg(targ_reg) :
               xed_mem_bisd(base_reg, index_reg, op.scale, xed_disp(op.disp, op.disp_width),
                            op.addr_width)));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  }
  const bool keep_flags = jmp_flags_maybe_live(INS_IsCall(ins), op);
  if (keep_flags && add_flags_save(ins_addr, true, ProfilingIns) < 0) return -1;

  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RBX), xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_AND, 64, xed_reg(XED_REG_RAX), xed_imm0(0xF0, 32));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
            xed_reg(XED_REG_RCX), xed_imm0((ADDRINT)&tp->s[0], 64));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  if (reset) {
    xed_inst2(&enc_instr, dstate, XED_ICLASS_CMP, 64,
              xed_mem_bisd(XED_REG_RCX, XED_REG_RAX, 1, xed_disp(0, 8), 64),
              xed_reg(XED_REG_RBX));
    if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  }
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
            xed_mem_bisd(XED_REG_RCX, XED_REG_RAX, 1, xed_disp(0, 8), 64),
            xed_reg(XED_REG_RBX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RBX),
            xed_mem_bisd(XED_REG_RCX, XED_REG_RAX, 1, xed_disp(8, 8), 64));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_LEA, 64, xed_reg(XED_REG_RBX),
            xed_mem_bd(XED_REG_RBX, xed_disp(1, 8), 64));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  if (reset) {
    xed_inst2(&enc_instr, dstate, XED_ICLASS_CMOVNZ, 64, xed_reg(XED_REG_RBX),
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
    if (add_prof_rip_instr(ins_addr, &enc_instr, (ADDRINT)prof_one_rip) < 0) return -1;
  }
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
            xed_mem_bisd(XED_REG_RCX, XED_REG_RAX, 1, xed_disp(8, 8), 64),
            xed_reg(XED_REG_RBX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;

  if (keep_flags && add_flags_save(ins_addr, false, ProfilingIns) < 0) return -1;
  // Restore RCX, RBX, RAX.
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
            xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rcx_mem, 64), 64));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RCX), xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
            xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rbx_mem, 64), 64));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RBX), xed_reg(XED_REG_RAX));
  if (add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns) < 0) return -1;
  xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64, xed_reg(XED_REG_RAX),
            xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rax_mem, 64), 64));
  return add_new_encoded_instr(ins_addr, &enc_instr, ProfilingIns);
}

// TC: give rel8-only branch br (loop*, j*cxz) a rel32 leg, since profiling
// stubs can push its target out of rel8 reach:
//   op L_take ; jmp L_ft ; L_take: jmp <target> (rel32) ; L_ft:
// The extra entries are ProfilingIns, so TC2 skips them (it adds its own).
static int add_rel8_trampoline_tc(unsigned br)
{
  xed_encoder_instruction_t enc;
  xed_inst1(&enc, dstate, XED_ICLASS_JMP, 64, xed_relbr(0, 32));
  if (add_new_encoded_instr(0, &enc, ProfilingIns) < 0)
    return -1;
  unsigned j_ft = num_of_instr_map_entries - 1;
  xed_inst1(&enc, dstate, XED_ICLASS_JMP, 64, xed_relbr(0, 32));
  if (add_new_encoded_instr(0, &enc, ProfilingIns) < 0)
    return -1;
  unsigned j_take = num_of_instr_map_entries - 1;
  xed_inst0(&enc, dstate, XED_ICLASS_NOP, 64);
  if (add_new_encoded_instr(0, &enc, ProfilingIns) < 0)
    return -1;
  unsigned l_ft = num_of_instr_map_entries - 1;
  instr_map[l_ft].size = 0;
  instr_map[j_ft].orig_targ_addr = 0;
  instr_map[j_ft].targ_map_entry = (int)l_ft;
  instr_map[j_take].orig_targ_addr = instr_map[br].orig_targ_addr;
  instr_map[br].targ_map_entry = (int)j_take;   // br keeps orig_targ_addr for TC2
  return 0;
}

/**************************************************/
/* chain_all_direct_jmp_and_call_target_entries() */
/**************************************************/
// Resolve direct branch targets to entries by app address (first entry
// for an address wins).  Pre-set targ_map_entry values are kept.
int chain_all_direct_jmp_and_call_target_entries(unsigned from_entry,
                                                 unsigned until_entry)
{
    // Fresh zero pages instead of memset (keeps RSS low).
    if (addr_idx)
        munmap(addr_idx, addr_idx_bytes);
    addr_idx = (unsigned *)lazy_zero_alloc(addr_idx_bytes);
    if (!addr_idx) {
        cerr << "failed to allocate the address index\n";
        return -1;
    }

    for (unsigned i = from_entry; i < until_entry; i++) {
        if (instr_map[i].targ_map_entry < 0)
          instr_map[i].targ_map_entry = -1;
        ADDRINT key = instr_map[i].app_ins_addr;
        if (!in_img(key))
          continue;
        unsigned &slot = addr_idx[key - img_lo];
        if (!slot)
          slot = i + 1;
    }

    for (unsigned i = from_entry; i < until_entry; i++) {
        ADDRINT t = instr_map[i].orig_targ_addr;
        if (t == 0 || instr_map[i].targ_map_entry >= 0 || !instr_map[i].size)
            continue;
        if (!in_img(t))
            continue;
        unsigned s = addr_idx[t - img_lo];
        if (s)
            instr_map[i].targ_map_entry = (int)(s - 1);
    }
    return 0;
}


/***********************************************/
/* set_initial_estimated_new_ins_addrs_in_tc() */
/***********************************************/
int set_initial_estimated_new_ins_addrs_in_tc(char *tc) {
  unsigned tc_cursor = 0;
  for (unsigned i=0; i < num_of_instr_map_entries; i++) {
    instr_map[i].new_ins_addr = (ADDRINT)&tc[tc_cursor];
    tc_cursor += instr_map[i].size;
    if (tc_cursor >= cur_max_tc_size) {
      cerr << "translation cache overflow: need more than " << dec
           << cur_max_tc_size << " bytes\n";
      return -1;
    }
  }
  return 0;
}


/**************************/
/* fix_rip_displacement() */
/**************************/
int fix_rip_displacement(unsigned instr_map_entry)
{
    if (!instr_map[instr_map_entry].size)
        return 0;

    if (!instr_map[instr_map_entry].orig_rip_addr)
      return 0;

    if (instr_map[instr_map_entry].orig_targ_addr != 0)
      return 0;

    xed_decoded_inst_t xedd;
    xed_decoded_inst_zero_set_mode(&xedd, &dstate);

    xed_error_enum_t xed_code =
       xed_decode(&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), max_inst_len);
    if (xed_code != XED_ERROR_NONE) {
        cerr << "ERROR: xed decode failed for instr at: " << "0x"
             << hex << instr_map[instr_map_entry].new_ins_addr << endl;
        return -1;
    }

    if (KnobVerbose) {
      cerr << " Before fixing rip offset\n";
      dump_instr_map_entry(instr_map_entry);
    }

    xed_int64_t new_disp = 0;
    xed_uint_t new_disp_byts = 4;

    new_disp = (xed_int64_t)(real_addr(instr_map[instr_map_entry].orig_rip_addr) - instr_map[instr_map_entry].new_ins_addr -
                               instr_map[instr_map_entry].size);
    if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
        cerr << "Invalid rip displacement larger than 32 bits in fix_rip_displacement\n";
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    xed_encoder_request_set_memory_displacement (&xedd, new_disp, new_disp_byts);

    unsigned max_size = XED_MAX_INSTRUCTION_BYTES;
    unsigned new_size = 0;

    xed_encoder_request_init_from_decode (&xedd);

    xed_error_enum_t xed_error =
       xed_encode (&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins),
                   max_size , &new_size);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    if (KnobVerbose) {
      cerr << " After fixing rip offset\n";
      dump_instr_map_entry(instr_map_entry);
    }

    return new_size;
}


/**************************************/
/* fix_direct_jmp_or_call_to_orig_addr */
/**************************************/
// Branch/call to untranslated code: keep it direct, rel32 to the original
// address (the caches are mapped within +/-2GB of the image).
int fix_direct_jmp_or_call_to_orig_addr(unsigned instr_map_entry)
{
    if (!instr_map[instr_map_entry].size)
      return 0;

    if (instr_map[instr_map_entry].targ_map_entry >= 0) {
        cerr << "ERROR: Invalid jump or call instruction" << endl;
        return -1;
    }

    if (!instr_map[instr_map_entry].orig_targ_addr) {
        cerr << "ERROR: branch to original code with no target address" << endl;
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    xed_decoded_inst_t xedd;
    xed_decoded_inst_zero_set_mode(&xedd,&dstate);

    xed_error_enum_t xed_code =
        xed_decode(&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), max_inst_len);
    if (xed_code != XED_ERROR_NONE) {
        cerr << "ERROR: xed decode failed for instr at: " << "0x"
             << hex << instr_map[instr_map_entry].new_ins_addr << endl;
        return -1;
    }

    xed_category_enum_t category_enum = xed_decoded_inst_get_category(&xedd);

    if (category_enum != XED_CATEGORY_CALL &&
        category_enum != XED_CATEGORY_UNCOND_BR &&
        category_enum != XED_CATEGORY_COND_BR) {
        cerr << "ERROR: Invalid direct branch/call from translated code to original code for:\n";
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    xed_int64_t new_disp =
        (xed_int64_t)real_addr(instr_map[instr_map_entry].orig_targ_addr) -
        (xed_int64_t)instr_map[instr_map_entry].new_ins_addr -
        (xed_int64_t)instr_map[instr_map_entry].size;

    xed_uint_t new_disp_byts = 4;
    // LOOP/J*CXZ only have 8-bit displacements.
    if (is_rel8_only_iclass(xed_decoded_inst_get_iclass(&xedd))) {
        new_disp_byts = 1;
        if (new_disp > 0x7F || new_disp < -0x80) {
            cerr << "Invalid 8-bit loop/JRCXZ displacement in "
                 << "fix_direct_jmp_or_call_to_orig_addr\n";
            dump_instr_map_entry(instr_map_entry);
            return -1;
        }
    } else if (new_disp > 0x7FFFFFFFLL || new_disp < -0x80000000LL) {
        cerr << "Invalid 32-bit branch/call displacement in "
             << "fix_direct_jmp_or_call_to_orig_addr\n";
        cerr << "new displacement: " << dec << new_disp << "\n";
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    xed_encoder_request_init_from_decode(&xedd);
    xed_encoder_request_set_branch_displacement(&xedd, new_disp, new_disp_byts);

    unsigned ilen = XED_MAX_INSTRUCTION_BYTES;
    unsigned olen = 0;
    xed_error_enum_t xed_error =
       xed_encode(&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), ilen, &olen);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    if (KnobVerbose) {
        dump_instr_map_entry(instr_map_entry);
    }

    return olen;
}


/**************************************/
/* fix_direct_jmp_or_call_displacement */
/**************************************/
int fix_direct_jmp_or_call_displacement(unsigned instr_map_entry)
{
    if (!instr_map[instr_map_entry].size)
        return 0;

    // Synthesized TC2 branches may have no target address but a pre-set entry.
    if (instr_map[instr_map_entry].orig_targ_addr == 0 &&
        instr_map[instr_map_entry].targ_map_entry < 0)
      return 0;

    xed_decoded_inst_t xedd;
    xed_decoded_inst_zero_set_mode(&xedd,&dstate);

    xed_error_enum_t xed_code =
        xed_decode(&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), max_inst_len);
    if (xed_code != XED_ERROR_NONE) {
        cerr << "ERROR: xed decode failed for instr at: "
             << "0x" << hex << instr_map[instr_map_entry].new_ins_addr << endl;
        return -1;
    }

    xed_int64_t  new_disp = 0;
    unsigned max_size = XED_MAX_INSTRUCTION_BYTES;
    unsigned new_size = 0;

    xed_category_enum_t category_enum = xed_decoded_inst_get_category(&xedd);

    if (category_enum != XED_CATEGORY_CALL &&
        category_enum != XED_CATEGORY_COND_BR &&
        category_enum != XED_CATEGORY_UNCOND_BR) {
        cerr << "ERROR: unrecognized branch displacement" << endl;
        return -1;
    }

    if (instr_map[instr_map_entry].targ_map_entry < 0) {
       int rc = fix_direct_jmp_or_call_to_orig_addr(instr_map_entry);
       return rc;
    }

    ADDRINT new_targ_addr;
    new_targ_addr = instr_map[instr_map[instr_map_entry].targ_map_entry].new_ins_addr;

    new_disp =
      (new_targ_addr - instr_map[instr_map_entry].new_ins_addr) - instr_map[instr_map_entry].size;
     if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
        cerr << "Invalid rip displacement larger than 32 bits in fix_direct_jmp_or_call_displacement\n";
        return -1;
    }

    bool fits8 = (new_disp <= 127 && new_disp >= -128);

    // A shrunk TC2 branch that no longer fits goes back to rel32 (strict
    // pass only; the size change makes the caller run another pass).
    if (g_fix_exact && instr_map[instr_map_entry].short_br && !fits8)
      instr_map[instr_map_entry].short_br = false;
    xed_uint_t new_disp_byts = instr_map[instr_map_entry].short_br ? 1 : 4;

    // loop*/j*cxz only have rel8 (routed through a rel32 trampoline).
    if (is_rel8_only_iclass(xed_decoded_inst_get_iclass(&xedd)))
      new_disp_byts = 1;

    // Before sizes settle, forward targets are stale: use a placeholder.
    if (new_disp_byts == 1 && !fits8 && !g_fix_exact)
      new_disp = 0;
    else if (new_disp_byts == 1 && !fits8) {
        cerr << "rel8 branch out of range in fix_direct_jmp_or_call_displacement\n";
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    xed_encoder_request_init_from_decode (&xedd);
    xed_encoder_request_set_branch_displacement (&xedd, new_disp, new_disp_byts);

    xed_error_enum_t xed_error =
        xed_encode (&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins), max_size, &new_size);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) <<  endl;
        char buf[2048];
        xed_format_context(XED_SYNTAX_INTEL, &xedd, buf, 2048,
                           static_cast<UINT64>(instr_map[instr_map_entry].orig_ins_addr), 0, 0);
        cerr << " instr: " << "0x" << hex << instr_map[instr_map_entry].orig_ins_addr << " : " << buf <<  endl;
          return -1;
    }

    if (KnobVerbose) {
        dump_instr_map_entry(instr_map_entry);
    }

    return new_size;
}

/************************************/
/* fix_instructions_displacements() */
/************************************/
static int fix_displacements(bool once, bool *changed);

// Iterate until sizes settle, then ONE strict pass on the settled layout:
// only there a rel8 miss is real (earlier passes see stale forward
// addresses).  If the strict pass reverted shrunk branches, sizes changed,
// so settle again.
int fix_instructions_displacements()
{
    for (;;) {
        bool changed = false;
        g_fix_rounds++;
        g_fix_strict = false;
        if (fix_displacements(false, &changed) < 0)
            return -1;
        g_fix_strict = true;
        int rc = fix_displacements(true, &changed);
        g_fix_strict = false;
        if (rc < 0)
            return -1;
        if (!changed)
            return 0;
    }
}

static int fix_displacements(bool once, bool *changed)
{
    int size_diff = 0;
    bool is_diff = false;
    *changed = false;

    do {

        size_diff = 0;
        is_diff = false;
        g_fix_passes++;

        if (KnobVerbose) {
            cerr << "starting a pass of fixing instructions displacements: " << endl;
        }

        for (unsigned i=0; i < num_of_instr_map_entries; i++) {

            instr_map[i].new_ins_addr += size_diff;

            int new_size = fix_rip_displacement(i);
            if (new_size) {
              if (new_size < 0)
                  return -1;
              if (instr_map[i].size != (unsigned)new_size) {
                  if (instr_map[i].size < (unsigned)new_size)
                     size_diff += (new_size - instr_map[i].size);
                  else
                     size_diff -= (instr_map[i].size - new_size);
                  instr_map[i].size = (unsigned)new_size;
                  is_diff = true;
                  continue;
              }
            }

            g_fix_exact = g_fix_strict && size_diff == 0;
            new_size = fix_direct_jmp_or_call_displacement(i);
            g_fix_exact = false;
            if (new_size) {
              if (new_size < 0)
                  return -1;
              if (instr_map[i].size != (unsigned)new_size) {
                if (instr_map[i].size < (unsigned)new_size)
                   size_diff += (new_size - instr_map[i].size);
                else
                   size_diff -= (instr_map[i].size - new_size);
                instr_map[i].size = (unsigned)new_size;
                is_diff = true;
                continue;
              }
            }

        }

        if (num_of_instr_map_entries) {
          unsigned last = num_of_instr_map_entries - 1;
          ADDRINT tc_end = instr_map[last].new_ins_addr + instr_map[last].size;
          ADDRINT tc_base = instr_map[0].new_ins_addr;
          if (tc_end - tc_base >= cur_max_tc_size) {
            cerr << "translation cache overflow during displacement fixup: need "
                 << dec << (tc_end - tc_base) << " bytes, have "
                 << cur_max_tc_size << "\n";
            return -1;
          }
        }
        if (is_diff)
          *changed = true;

    } while (is_diff && !once);

   return 0;
 }


/********************************/
/* find_candidate_rtns_for_tc() */
/********************************/
int find_candidate_rtns_for_tc(IMG img)
{
    int rc = 0;

    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;

        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            RTN_Open( rtn );

            // Direct branch/call targets inside the routine start new BBLs.
            std::set<ADDRINT> is_targ;
            std::map<ADDRINT, ADDRINT> local_src;   // target -> a direct jmp/jcc to it
            bool rel8_only = false;
            g_rtn_ins.clear();
            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
               g_rtn_ins[INS_Address(ins)] = ins;
               if (INS_IsDirectControlFlow(ins)) {
                 ADDRINT targ_addr = INS_DirectControlFlowTargetAddress(ins);
                 is_targ.insert(targ_addr);
                 set_flag(targ_addr, FLAG_BR_TARGET);
                 if (!INS_IsCall(ins))
                   local_src.emplace(targ_addr, INS_Address(ins));
               }
               if (is_rel8_only_iclass(xed_decoded_inst_get_iclass(INS_XedDec(ins))))
                 rel8_only = true;
            }

            unsigned first_bbl = bbl_num;
            INS prev_ins = INS_Invalid();
            bool has_head = false;
            unsigned head_entry = 0;

            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {

                if (KnobVerbose) {
                    cerr << "old instr: ";
                    cerr << "0x" << hex << INS_Address(ins) << ": " << INS_Disassemble(ins) <<  endl;
                }

                ADDRINT ins_addr = INS_Address(ins);

                xed_decoded_inst_t xedd;
                xed_error_enum_t xed_code;

                bool isRtnHeadIns = (RTN_Address(rtn) == ins_addr);
                ins_enum_t ins_type = (isRtnHeadIns ? RtnHeadIns : RegularIns);

                // NOP7 at the routine head: later patched into a jmp to TC2.
                if (!KnobNoProfile && isRtnHeadIns) {
                  rc = create_nop7_xedd_instr(&xedd);
                  if (rc < 0) {
                    cerr << "ERROR: failed to create a NOP7 instr during translation of instr at: "
                         << "0x" << hex << ins_addr << endl;
                    RTN_Close(rtn);
                    return -1;
                  }
                  head_entry = num_of_instr_map_entries;
                  rc = add_new_instr_entry(&xedd, ins_addr, ins_type);
                  if (rc < 0) {
                    cerr << "ERROR: failed during instructon translation." << endl;
                    RTN_Close(rtn);
                    return -1;
                  }
                  has_head = true;
                  ins_type = RegularIns;
                }

                // BBL counter at the BBL start, so the liveness scan sees the block.
                bool at_bbl_start = (bbl_map[bbl_num].bbl_addr == 0);
                if (at_bbl_start)
                  bbl_map[bbl_num].bbl_addr = ins_addr;
                // -ctr_derive: skip the counter when the count follows from the
                // only way in.  TC blocks are entered only by TC branches and
                // fall-through (jump tables and probes go to original code /
                // routine entries), so the image-wide reference count is exact.
                if (!KnobNoProfile && at_bbl_start && KnobCtrDerive.Value() &&
                    !isRtnHeadIns && in_img(ins_addr) &&
                    !(img_flags[ins_addr - img_lo] & FLAG_RTN_ENTRY)) {
                  bool prev_falls = INS_Valid(prev_ins) && !INS_IsRet(prev_ins) &&
                                    INS_Category(prev_ins) != XED_CATEGORY_UNCOND_BR;
                  unsigned refs = img_refs[ins_addr - img_lo];
                  std::map<ADDRINT, ADDRINT>::const_iterator ls = local_src.find(ins_addr);
                  if (!prev_falls && refs == 1 && ls != local_src.end()) {
                    bbl_map[bbl_num].derive = 1;          // single jmp/jcc edge
                    bbl_map[bbl_num].derive_src = ls->second;
                  } else if (prev_falls && refs == 0 && INS_IsCall(prev_ins) &&
                             INS_IsIndirectControlFlow(prev_ins)) {
                    bbl_map[bbl_num].derive = 2;          // return site of b-1
                  }
                  if (bbl_map[bbl_num].derive)
                    g_ctr_derived++;
                }
                if (!KnobNoProfile && at_bbl_start &&
                    (!bbl_map[bbl_num].derive || KnobCtrDeriveCheck.Value())) {
                  rc = add_counter_stub(ins, ins_addr, &prof_cnt[2 * (size_t)bbl_num]);
                  if (rc < 0) {
                    RTN_Close(rtn);
                    return -1;
                  }
                }

                INS next_ins = INS_Next(ins);
                bool isNextInsJumpTarget =
                    INS_Valid(next_ins) && is_targ.count(INS_Address(next_ins));
                bool isIndirectCall = (INS_IsCall(ins) && INS_IsIndirectControlFlow(ins));
                bool isIndirectJump = (INS_IsIndirectControlFlow(ins) &&
                                       !INS_IsRet(ins) && !INS_IsCall(ins));
                // A BBL also ends at the routine's last instruction.
                bool isInsTerminatesBBL = (isJumpOrRet(ins) || isIndirectCall ||
                                           isNextInsJumpTarget || !INS_Valid(next_ins));

                // Indirect jmp/call target profiling, right before the branch.
                g_indirect_prof_emitted = false;
                if (!KnobNoProfile && (isIndirectJump || isIndirectCall)) {
                    rc = add_indirect_target_stub(ins, ins_addr, bbl_num);
                    if (rc < 0) {
                      RTN_Close(rtn);
                      return -1;
                    }
                }

                xed_decoded_inst_zero_set_mode(&xedd,&dstate);
                xed_code = xed_decode(&xedd, reinterpret_cast<UINT8*>(ins_addr), max_inst_len);
                if (xed_code != XED_ERROR_NONE) {
                    cerr << "ERROR: xed decode failed for instr at: " << "0x" << hex << ins_addr << endl;
                    RTN_Close(rtn);
                    return -1;
                }

                rc = add_new_instr_entry(&xedd, INS_Address(ins), ins_type);
                if (rc < 0) {
                    cerr << "ERROR: failed during instructon translation." << endl;
                    RTN_Close(rtn);
                    return -1;
                }

                if (isInsTerminatesBBL) {
                  bbl_map_t &bm = bbl_map[bbl_num];
                  bm.terminating_ins_entry = num_of_instr_map_entries - 1;
                  bm.is_cond = (INS_Category(ins) == XED_CATEGORY_COND_BR);
                  bm.is_indirect = (isIndirectJump || isIndirectCall) && g_indirect_prof_emitted;
                  bm.is_indirect_call = isIndirectCall;
                  if (INS_Valid(next_ins))
                    bm.fallthru_app_addr = INS_Address(next_ins);
                  else if (!INS_IsRet(ins) && !isUncondJump(ins))
                    bm.fallthru_app_addr = ins_addr + INS_Size(ins);  // falls into the next routine
                  g_indirect_prof_emitted = false;
                  bbl_num++;
                  if (bbl_num >= max_bbl_count) {
                    cerr << "exceeded bbl_map capacity (" << dec << max_bbl_count << ")\n";
                    RTN_Close(rtn);
                    return -1;
                  }
                  bbl_map[bbl_num].starting_ins_entry = num_of_instr_map_entries;
                }

                if (is_rel8_only_iclass(xed_decoded_inst_get_iclass(INS_XedDec(ins))) &&
                    instr_map[num_of_instr_map_entries - 1].orig_targ_addr &&
                    add_rel8_trampoline_tc(num_of_instr_map_entries - 1) < 0) {
                  RTN_Close(rtn);
                  return -1;
                }

                // Fall-through counter right after a cond branch; not needed
                // when the next BBL is entered only by this fall-through.
                if (!KnobNoProfile && INS_Category(ins) == XED_CATEGORY_COND_BR &&
                    KnobFtElide.Value() && INS_Valid(next_ins) &&
                    in_img(INS_Address(next_ins)) &&
                    !(img_flags[INS_Address(next_ins) - img_lo] &
                      (FLAG_BR_TARGET | FLAG_RTN_ENTRY))) {
                  bbl_map[bbl_num - 1].ft_from_next = true;
                  n_ft_elided++;
                } else if (!KnobNoProfile && INS_Category(ins) == XED_CATEGORY_COND_BR) {
                  rc = add_counter_stub(next_ins, ins_addr,
                                        &prof_cnt[2 * (size_t)(bbl_num - 1) + 1]);
                  if (rc < 0) {
                    RTN_Close(rtn);
                    return -1;
                  }
                }

                prev_ins = ins;
            } // end for INS...

            if (KnobVerbose) {
                cerr <<   "rtn name: " << RTN_Name(rtn) << endl;
            }

            RTN_Close( rtn );
            g_rtn_ins.clear();

            if (has_head) {
              RtnInfo ri;
              ri.app_addr = RTN_Address(rtn);
              ri.app_size = RTN_Size(rtn);
              ri.tc_head_entry = head_entry;
              ri.first_bbl = first_bbl;
              ri.end_bbl = bbl_num;
              ri.orig_entry = 0;
              ri.probe_len = 0;
              ri.unprobe = false;
              ri.stub_slot = nullptr;
              ri.no_layout = rel8_only;
              ri.force_hot = (g_exit_addr && ri.app_addr == g_exit_addr);
              g_rtns.push_back(ri);
            }

         } // end for RTN..
    } // end for SEC...

    return 0;
}


/***************************/
/* int copy_instrs_to_tc() */
/***************************/
int copy_instrs_to_tc(char *tc)
{
    int cursor = 0;

    for (unsigned i=0; i < num_of_instr_map_entries; i++) {

      if ((ADDRINT)&tc[cursor] != instr_map[i].new_ins_addr) {
          cerr << "ERROR: Non-matching instruction addresses: "
               << hex << (ADDRINT)&tc[cursor]
               << " vs. " << instr_map[i].new_ins_addr << endl;
          return -1;
      }

      memcpy(&tc[cursor], (char *)instr_map[i].encoded_ins, instr_map[i].size);

      cursor += instr_map[i].size;
    }

    return cursor;
}


/***************************************/
/* void commit_translated_rtns_to_tc() */
/***************************************/
// Probe each safe routine to its TC copy; keep Pin's trampoline to the
// original code so TC2 can send cold routines back there.
static void patch_probe_to(ADDRINT app, ADDRINT dest);

inline void commit_translated_rtns_to_tc()
{
    for (size_t k = 0; k < g_rtns.size(); k++) {
        RtnInfo &ri = g_rtns[k];
        ri.orig_entry = 0;

        RTN rtn = RTN_FindByAddress(ri.app_addr);
        if (rtn == RTN_Invalid() || RTN_Address(rtn) != ri.app_addr) {
           VLOG(<< "invalid rtN for commit for addr: 0x" << hex << ri.app_addr << "\n");
           continue;
        }

        if (!RTN_IsSafeForProbedReplacement(rtn) ||
            !has_decodable_probe_header(rtn) ||
            !has_no_other_rtn_entry_in_probe_patch(rtn) ||
            !has_no_branch_target_inside_probe_patch(rtn)) {
           VLOG(<< "skip unsafe probe for addr: 0x" << hex << ri.app_addr << "\n");
           continue;
        }

        memcpy(ri.orig_bytes, (const void *)ri.app_addr, sizeof(ri.orig_bytes));
        AFUNPTR origFptr = RTN_ReplaceProbed(rtn, (AFUNPTR)instr_map[ri.tc_head_entry].new_ins_addr);
        ri.orig_entry = (ADDRINT)origFptr;
        ri.probe_len = 0;
        if (origFptr) {
          // Changed byte range, so the original entry can be restored later
          // (-cold_unprobe).  16 changed bytes: extent unknown, not restorable.
          const unsigned char *now = (const unsigned char *)ri.app_addr;
          unsigned n = 0;
          for (unsigned b = 0; b < sizeof(ri.orig_bytes); b++)
            if (now[b] != ri.orig_bytes[b])
              n = b + 1;
          if (n < sizeof(ri.orig_bytes))
            ri.probe_len = (unsigned char)n;
          // -entry_direct: Pin's probe may go through a bridge; point the
          // entry straight at the TC head (the app has not started yet).
          if (KnobEntryDirect.Value())
            patch_probe_to(ri.app_addr, instr_map[ri.tc_head_entry].new_ins_addr);
        }

        if (origFptr == NULL) {
            VLOG(<< "RTN_ReplaceProbed failed. orig routine addr: 0x" << hex << RTN_Address(rtn)
                 << " translated routine addr: 0x" << hex
                 << instr_map[ri.tc_head_entry].new_ins_addr << endl);
        }
    }
}

/****************************************/
/* void commit_translated_rtns_to_tc2() */
/****************************************/
// Patch each TC routine-head NOP7 into a jmp to its TC2 head (or cold stub).
static std::vector<std::pair<unsigned, ADDRINT> > g_stub_heads;  // head entry -> original entry
static unsigned g_cold_heads_direct = 0;
static unsigned g_entries_direct = 0;

static int rtn_by_head(ADDRINT a);

// mprotect via the syscall instruction (the Pin CRT wrappers refuse app memory).
static long raw_mprotect(ADDRINT addr, ADDRINT len, long prot)
{
  long ret;
  __asm__ __volatile__("syscall"
                       : "=a"(ret)
                       : "a"(10L), "D"(addr), "S"(len), "d"(prot)
                       : "rcx", "r11", "memory");
  return ret;
}

// Write n (<= 8) bytes at original-text address 'app' with one locked 8-byte
// cmpxchg (the 8-n bytes after them are rewritten unchanged).
static bool patch_text8(ADDRINT app, const unsigned char *bytes, unsigned n)
{
  const ADDRINT pg = (ADDRINT)sysconf(_SC_PAGE_SIZE);
  ADDRINT lo = app & ~(pg - 1), hi = (app + 8 + pg - 1) & ~(pg - 1);
  if (raw_mprotect(lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    return false;
  UINT64 expect;
  memcpy(&expect, (const void *)app, 8);
  for (;;) {
    UINT64 desired = expect;
    memcpy(&desired, bytes, n);
    UINT64 prev = expect;
    __asm__ __volatile__("lock cmpxchgq %2, %1"
                         : "+a"(prev), "+m"(*(volatile UINT64 *)app)
                         : "r"(desired)
                         : "memory", "cc");
    if (prev == expect)
      break;
    expect = prev;
  }
  raw_mprotect(lo, hi - lo, PROT_READ | PROT_EXEC);
  return true;
}

// TC phase: replace Pin's probe jmp at 'app' by a direct jmp to 'dest' (the
// TC head) when Pin's jmp goes elsewhere (a bridge).  Counts both cases.
static unsigned g_probe_bridged = 0, g_probe_direct = 0;
static void patch_probe_to(ADDRINT app, ADDRINT dest)
{
  xed_decoded_inst_t xedd;
  xed_decoded_inst_zero_set_mode(&xedd, &dstate);
  if (xed_decode(&xedd, reinterpret_cast<UINT8*>(app), max_inst_len) != XED_ERROR_NONE ||
      xed_decoded_inst_get_iclass(&xedd) != XED_ICLASS_JMP ||
      xed_decoded_inst_get_length(&xedd) < 5)
    return;
  if (xed_decoded_inst_get_branch_displacement_width(&xedd) &&
      app + xed_decoded_inst_get_length(&xedd) +
          (ADDRINT)(INT64)xed_decoded_inst_get_branch_displacement(&xedd) == dest) {
    g_probe_direct++;
    return;
  }
  INT64 d = (INT64)dest - (INT64)(app + 5);
  if (d > 0x7FFF0000LL || d < -0x7FFF0000LL)
    return;
  unsigned char jmp5[5];
  jmp5[0] = 0xE9;
  INT32 d32 = (INT32)d;
  memcpy(&jmp5[1], &d32, 4);
  if (patch_text8(app, jmp5, 5))
    g_probe_bridged++;
}

// Re-point the probe jmp at a probed routine's original entry to 'dest'
// (one hop instead of probe -> TC head -> dest).  Only our own probes.
static void patch_orig_entry(ADDRINT app, ADDRINT dest)
{
  int r = rtn_by_head(app);
  if (r < 0 || !g_rtns[r].orig_entry || app == g_exit_addr || dest == app)
    return;
  xed_decoded_inst_t xedd;
  xed_decoded_inst_zero_set_mode(&xedd, &dstate);
  if (xed_decode(&xedd, reinterpret_cast<UINT8*>(app), max_inst_len) != XED_ERROR_NONE ||
      xed_decoded_inst_get_iclass(&xedd) != XED_ICLASS_JMP ||
      xed_decoded_inst_get_length(&xedd) < 5)
    return;                                   // not a probe jmp we can replace
  INT64 d = (INT64)dest - (INT64)(app + 5);
  if (d > 0x7FFF0000LL || d < -0x7FFF0000LL)
    return;
  unsigned char jmp5[5];
  jmp5[0] = 0xE9;
  INT32 d32 = (INT32)d;
  memcpy(&jmp5[1], &d32, 4);
  if (patch_text8(app, jmp5, 5))
    g_entries_direct++;
}

// Undo our probe on a cold routine: put its original entry bytes back.
// Bytes past the probe jmp are dead while the jmp is intact, so they go
// first; then the first 8 bytes in one atomic write.
static unsigned g_unprobed = 0;
static bool restore_orig_entry(const RtnInfo &ri)
{
  unsigned n = ri.probe_len;
  if (!n)
    return false;
  xed_decoded_inst_t xedd;
  xed_decoded_inst_zero_set_mode(&xedd, &dstate);
  if (xed_decode(&xedd, reinterpret_cast<UINT8*>(ri.app_addr), max_inst_len) != XED_ERROR_NONE ||
      xed_decoded_inst_get_iclass(&xedd) != XED_ICLASS_JMP)
    return false;
  unsigned L = xed_decoded_inst_get_length(&xedd);
  if (n > 8) {
    if (L > 8)
      return false;
    const ADDRINT pg = (ADDRINT)sysconf(_SC_PAGE_SIZE);
    ADDRINT lo = (ri.app_addr + 8) & ~(pg - 1);
    ADDRINT hi = (ri.app_addr + n + pg - 1) & ~(pg - 1);
    if (raw_mprotect(lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
      return false;
    memcpy((void *)(ri.app_addr + 8), ri.orig_bytes + 8, n - 8);
    raw_mprotect(lo, hi - lo, PROT_READ | PROT_EXEC);
  }
  if (!patch_text8(ri.app_addr, ri.orig_bytes, 8))
    return false;
  g_unprobed++;
  return true;
}

int commit_translated_rtns_to_tc2()
{
  size_t sh = 0;   // g_stub_heads is sorted by entry index
  for (unsigned i=0; i < num_of_instr_map_entries; i++) {

       if (instr_map[i].ins_type != RtnHeadIns ||
           instr_map[i].xed_category != XED_CATEGORY_WIDENOP)
         continue;

       // Cold routine: TC head straight to the original entry when rel32
       // reaches (the TC2 stub stays for the other paths).
       ADDRINT dest = instr_map[i].new_ins_addr;
       while (sh < g_stub_heads.size() && g_stub_heads[sh].first < i)
         sh++;
       if (sh < g_stub_heads.size() && g_stub_heads[sh].first == i) {
         ADDRINT cold = g_stub_heads[sh].second;
         int r = rtn_by_head(instr_map[i].app_ins_addr);
         // Unprobed cold routine: restore its entry BEFORE anything points
         // at app_addr (else probe -> TC head -> app_addr loops).  On
         // failure fall back to Pin's trampoline.
         if (r >= 0 && g_rtns[r].unprobe && !restore_orig_entry(g_rtns[r])) {
           cold = g_rtns[r].orig_entry;
           if (g_rtns[r].stub_slot)
             *g_rtns[r].stub_slot = cold;
         }
         INT64 d = (INT64)cold - (INT64)instr_map[i].orig_ins_addr;
         if ((KnobColdHeadDirect.Value() || cold != g_stub_heads[sh].second) &&
             d < 0x7FFF0000LL && d > -0x7FFF0000LL) {
           dest = cold;
           g_cold_heads_direct++;
         }
       }

       int olen = encode_jump_instr(instr_map[i].orig_ins_addr, dest,
                                    instr_map[i].encoded_ins);
       if (olen < 0)
         return -1;

       if (olen == 5) {
         atomic_patch5((char *)instr_map[i].orig_ins_addr,
                       (char *)instr_map[i].encoded_ins);
       } else {
         if (olen > 4)
           memcpy((char *)(instr_map[i].orig_ins_addr + 4),
                  (char *)((ADDRINT)instr_map[i].encoded_ins + 4), olen - 4);
         memcpy((char *)instr_map[i].orig_ins_addr, instr_map[i].encoded_ins, 4);
       }

       if (KnobEntryDirect.Value())
         patch_orig_entry(instr_map[i].app_ins_addr, dest);

       VLOG(<< " committing rtN from: 0x" << hex << instr_map[i].orig_ins_addr
            << " to: 0x" << hex << instr_map[i].new_ins_addr
            << " size: " << olen << endl);
  }

  return 0;
}


/* ============================================================= */
/* TC2: hot-only copy, hot/cold split, Pettis-Hansen, devirt      */
/* ============================================================= */

static const instr_map_t *g_old = nullptr;   // TC instr_map during the rebuild

// Out-of-line miss path of a de-virtualized jmp/call.
struct MissPath {
  unsigned term_idx;        // old-map index of the indirect jmp/call
  unsigned jne_idx;
  int join_idx;             // call: label after the direct call
  unsigned sslot;
  unsigned bbl;
  xed_reg_enum_t scratch;
  bool mem;
  bool is_call;
};
static std::vector<MissPath> g_miss;

struct Tc2Stats {
  unsigned hot_rtns, stub_rtns, cold_bbls, inverted, elided, devirt, short_br, cold_direct;
  unsigned devirt_flags_skip;   // indirect jmps not de-virtualized: flags may be live
  unsigned devirt_noflags;      // indirect jmps with the flag-free (jrcxz) guard
};
static Tc2Stats g_st;

enum { IT_HEAD = 0, IT_BBL = 1, IT_STUB = 2 };
struct Item {
  unsigned kind;
  unsigned id;              // routine index (HEAD/STUB) or BBL number
  bool opt;                 // BBL: layout/devirt allowed
};

static std::vector<std::pair<ADDRINT, unsigned> > g_heads;  // sorted app head -> rtn

static int rtn_by_head(ADDRINT a)
{
  std::vector<std::pair<ADDRINT, unsigned> >::const_iterator it =
      std::lower_bound(g_heads.begin(), g_heads.end(), std::make_pair(a, 0u));
  if (it == g_heads.end() || it->first != a)
    return -1;
  return (int)it->second;
}

static inline bool bbl_is_hot(unsigned b)
{
  return bbl_map[b].counter >= (UINT64)KnobColdBbl.Value();
}

// Hottest indirect target of BBL b, if it passes hot_abs / hot_frac.
static bool pick_hot_indirect(unsigned b, ADDRINT *hot_out, UINT64 *cnt_out)
{
  const bbl_map_t &bm = bbl_map[b];
  if (!bm.is_indirect || !bm.targ_slot)
    return false;
  const targ_prof_t &tp = targ_pool[bm.targ_slot - 1];
  UINT64 best = 0;
  ADDRINT addr = 0;
  for (unsigned j = 0; j <= MAX_TARG_ADDRS; j++) {
    if (tp.s[j].count > best && tp.s[j].addr) {
      best = tp.s[j].count;
      addr = tp.s[j].addr;
    }
  }
  UINT64 exec = bm.counter;
  if (best < KnobHotAbs.Value() || exec == 0 || !addr)
    return false;
  if (best * 100 < exec * (UINT64)KnobHotFrac.Value())
    return false;
  // The direct form must reach the target with rel32.
  INT64 d = (INT64)addr - (INT64)(ADDRINT)tc2;
  if (d > 0x70000000LL || d < -0x70000000LL)
    return false;
  *hot_out = addr;
  *cnt_out = best;
  return true;
}

static ADDRINT *alloc_hot_const(ADDRINT val)
{
  if (!hot_const_pool || hot_const_num >= max_hot_const)
    return nullptr;
  hot_const_pool[hot_const_num] = val;
  return &hot_const_pool[hot_const_num++];
}

static int append_clone(const instr_map_t *src)
{
  if (num_of_instr_map_entries + 1 >= cur_max_ins_count)
    return -1;
  instr_map[num_of_instr_map_entries] = *src;
  instr_map[num_of_instr_map_entries].new_ins_addr = 0;
  instr_map[num_of_instr_map_entries].targ_map_entry = -1;
  instr_map[num_of_instr_map_entries].short_br = false;
  num_of_instr_map_entries++;
  return 0;
}

// Direct jmp/jcc/call with a placeholder rel32 and an app target.
static int append_rel_cf(xed_iclass_enum_t ic, ADDRINT targ, unsigned bbl)
{
  xed_encoder_instruction_t enc;
  xed_inst1(&enc, dstate, ic, 64, xed_relbr(0, 32));
  if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
    return -1;
  unsigned i = num_of_instr_map_entries - 1;
  instr_map[i].orig_targ_addr = targ;
  instr_map[i].app_ins_addr = 0;
  instr_map[i].bbl_num = bbl;
  instr_map[i].targ_map_entry = -1;
  return (int)i;
}

// Zero-size entry used as a branch target.
static int append_label(unsigned bbl)
{
  xed_encoder_instruction_t enc;
  xed_inst0(&enc, dstate, XED_ICLASS_NOP, 64);
  if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
    return -1;
  unsigned i = num_of_instr_map_entries - 1;
  instr_map[i].size = 0;
  instr_map[i].app_ins_addr = 0;
  instr_map[i].bbl_num = bbl;
  return (int)i;
}

// Clone e.  A rel8-only branch (loop*, j*cxz) gets a rel32 leg, since its
// target may move out of rel8 reach in TC2:
//   op L_take ; jmp L_ft ; L_take: jmp <target> (rel32) ; L_ft:
static int append_branch_clone(const instr_map_t *e)
{
  if (!e->orig_targ_addr)
    return append_clone(e);
  xed_decoded_inst_t xedd;
  xed_decoded_inst_zero_set_mode(&xedd, &dstate);
  if (xed_decode(&xedd, reinterpret_cast<UINT8*>(const_cast<char*>(e->encoded_ins)),
                 max_inst_len) != XED_ERROR_NONE)
    return -1;
  if (!is_rel8_only_iclass(xed_decoded_inst_get_iclass(&xedd)))
    return append_clone(e);

  if (append_clone(e) < 0)
    return -1;
  unsigned br = num_of_instr_map_entries - 1;
  int j_ft = append_rel_cf(XED_ICLASS_JMP, 0, e->bbl_num);
  if (j_ft < 0)
    return -1;
  int j_take = append_rel_cf(XED_ICLASS_JMP, e->orig_targ_addr, e->bbl_num);
  if (j_take < 0)
    return -1;
  int l_ft = append_label(e->bbl_num);
  if (l_ft < 0)
    return -1;
  instr_map[br].orig_targ_addr = 0;          // resolved via targ_map_entry
  instr_map[br].targ_map_entry = j_take;
  instr_map[j_ft].targ_map_entry = l_ft;
  return 0;
}

static xed_iclass_enum_t invert_jcc_iclass(xed_iclass_enum_t ic)
{
  switch (ic) {
    case XED_ICLASS_JZ: return XED_ICLASS_JNZ;
    case XED_ICLASS_JNZ: return XED_ICLASS_JZ;
    case XED_ICLASS_JS: return XED_ICLASS_JNS;
    case XED_ICLASS_JNS: return XED_ICLASS_JS;
    case XED_ICLASS_JP: return XED_ICLASS_JNP;
    case XED_ICLASS_JNP: return XED_ICLASS_JP;
    case XED_ICLASS_JO: return XED_ICLASS_JNO;
    case XED_ICLASS_JNO: return XED_ICLASS_JO;
    case XED_ICLASS_JB: return XED_ICLASS_JNB;
    case XED_ICLASS_JNB: return XED_ICLASS_JB;
    case XED_ICLASS_JBE: return XED_ICLASS_JNBE;
    case XED_ICLASS_JNBE: return XED_ICLASS_JBE;
    case XED_ICLASS_JL: return XED_ICLASS_JNL;
    case XED_ICLASS_JNL: return XED_ICLASS_JL;
    case XED_ICLASS_JLE: return XED_ICLASS_JNLE;
    case XED_ICLASS_JNLE: return XED_ICLASS_JLE;
    default: return XED_ICLASS_INVALID;
  }
}

// Inverted copy of jcc src, retargeted to new_targ.  1 = not invertible.
static int append_inverted_jcc(const instr_map_t *src, ADDRINT new_targ)
{
  xed_decoded_inst_t xedd;
  xed_decoded_inst_zero_set_mode(&xedd, &dstate);
  if (xed_decode(&xedd, reinterpret_cast<UINT8*>(const_cast<char*>(src->encoded_ins)),
                 max_inst_len) != XED_ERROR_NONE)
    return -1;
  xed_iclass_enum_t inv = invert_jcc_iclass(xed_decoded_inst_get_iclass(&xedd));
  if (inv == XED_ICLASS_INVALID)
    return 1;
  xed_encoder_instruction_t enc;
  xed_inst1(&enc, dstate, inv, 64, xed_relbr(0, 32));
  if (add_new_encoded_instr(src->app_ins_addr, &enc, RegularIns) < 0)
    return -1;
  unsigned i = num_of_instr_map_entries - 1;
  instr_map[i].orig_targ_addr = new_targ;
  instr_map[i].app_ins_addr = src->app_ins_addr;
  instr_map[i].bbl_num = src->bbl_num;
  instr_map[i].targ_map_entry = -1;
  return 0;
}

static int append_cmp_reg_hot(xed_reg_enum_t reg, ADDRINT *slot, unsigned bbl)
{
  xed_encoder_instruction_t enc;
  xed_inst2(&enc, dstate, XED_ICLASS_CMP, 64,
            xed_reg(reg),
            xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
  if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
    return -1;
  unsigned i = num_of_instr_map_entries - 1;
  instr_map[i].orig_rip_addr = (ADDRINT)slot;
  instr_map[i].orig_targ_addr = 0;
  instr_map[i].app_ins_addr = 0;
  instr_map[i].bbl_num = bbl;
  return (int)i;
}

static int append_scratch_spill(xed_reg_enum_t reg, bool is_spill,
                                unsigned slot, unsigned bbl)
{
  xed_encoder_instruction_t enc;
  if (is_spill) {
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64),
              xed_reg(reg));
  } else {
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(reg),
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
  }
  if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
    return -1;
  unsigned i = num_of_instr_map_entries - 1;
  instr_map[i].orig_rip_addr = (ADDRINT)&scratch_slots[slot];
  instr_map[i].app_ins_addr = 0;
  instr_map[i].orig_targ_addr = 0;
  instr_map[i].bbl_num = bbl;
  return (int)i;
}

static xed_reg_enum_t pick_scratch(xed_reg_enum_t base, xed_reg_enum_t index)
{
  const xed_reg_enum_t cands[] = {
    XED_REG_R11, XED_REG_R10, XED_REG_R9, XED_REG_R8
  };
  for (unsigned i = 0; i < 4; i++) {
    if (cands[i] != base && cands[i] != index)
      return cands[i];
  }
  return XED_REG_R11;
}

// scratch = the jmp/call target read from memory.
static int append_mov_from_indirect(const instr_map_t *src, const IndOp &op,
                                    xed_reg_enum_t scratch, unsigned bbl)
{
  xed_encoder_instruction_t enc;
  if (op.base == XED_REG_RIP) {
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64, xed_reg(scratch),
              xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
  } else {
    xed_uint_t w = op.disp_width ? op.disp_width : 32;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64, xed_reg(scratch),
              xed_mem_bisd(op.base, op.index, op.scale, xed_disp(op.disp, w), 64));
  }
  // pc 0: this entry must not own the app address of the branch.
  if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
    return -1;
  unsigned i = num_of_instr_map_entries - 1;
  instr_map[i].orig_rip_addr = (op.base == XED_REG_RIP) ? src->orig_rip_addr : 0;
  instr_map[i].app_ins_addr = 0;
  instr_map[i].orig_targ_addr = 0;
  instr_map[i].bbl_num = bbl;
  return 0;
}

static int emit_ft_jmp(ADDRINT ft, ADDRINT next_start, unsigned bbl)
{
  if (!ft || ft == next_start)
    return 0;
  return append_rel_cf(XED_ICLASS_JMP, ft, bbl) < 0 ? -1 : 0;
}

// Guarded direct jmp/call to the hot target:
//   [spill; mov s,[mem];] cmp s|reg,[hot]; jne miss; [reload;] jmp|call hot
// The miss path (reload + original indirect branch) is emitted out of line.
// Returns 1 if this branch cannot be de-virtualized.
static int emit_devirt(unsigned b, ADDRINT hot, ADDRINT next_start, bool lay)
{
  const bbl_map_t &bm = bbl_map[b];
  unsigned T = bm.terminating_ins_entry;
  const instr_map_t *term = &g_old[T];

  xed_decoded_inst_t xedd;
  xed_decoded_inst_zero_set_mode(&xedd, &dstate);
  if (xed_decode(&xedd, reinterpret_cast<UINT8*>(const_cast<char*>(term->encoded_ins)),
                 max_inst_len) != XED_ERROR_NONE)
    return 1;
  IndOp op;
  if (!decode_indirect_op(&xedd, &op))
    return 1;
  if (op.mem) {
    if (op.base == XED_REG_RSP || op.index == XED_REG_RSP)
      return 1;
    if (op.seg == XED_REG_FS || op.seg == XED_REG_GS)
      return 1;
    if (op.base == XED_REG_RIP && !term->orig_rip_addr)
      return 1;
    if (op.addr_width && op.addr_width != 64)
      return 1;
  } else if (op.reg == XED_REG_RSP || op.reg == XED_REG_RIP) {
    return 1;
  }
  // The cmp guard clobbers RFLAGS, which a jmp's target may read: use the
  // flag-free guard  spill rcx; rcx = target - hot; jrcxz hit; jmp miss;
  // hit: reload rcx; jmp hot  (miss path reloads rcx before the original jmp).
  if (!bm.is_indirect_call && jmp_flags_maybe_live(false, op)) {
    if (hot >= 0x80000000ULL) {       // -hot must fit lea's disp32
      g_st.devirt_flags_skip++;
      return 1;
    }
    MissPath m;
    m.term_idx = T;
    m.join_idx = -1;
    m.sslot = b % NUM_SCRATCH_SLOTS;
    m.bbl = b;
    m.scratch = XED_REG_RCX;
    m.mem = true;                     // miss path reloads rcx
    m.is_call = false;

    unsigned first = num_of_instr_map_entries;
    if (append_scratch_spill(XED_REG_RCX, true, m.sslot, b) < 0)
      return -1;
    xed_reg_enum_t src = XED_REG_RCX;
    if (op.mem) {
      if (append_mov_from_indirect(term, op, XED_REG_RCX, b) < 0)
        return -1;
    } else {
      src = op.reg;
    }
    xed_encoder_instruction_t enc;
    xed_inst2(&enc, dstate, XED_ICLASS_LEA, 64, xed_reg(XED_REG_RCX),
              xed_mem_bd(src, xed_disp(-(xed_int64_t)hot, 32), 64));
    if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
      return -1;
    instr_map[num_of_instr_map_entries - 1].app_ins_addr = 0;
    instr_map[num_of_instr_map_entries - 1].orig_targ_addr = 0;
    instr_map[num_of_instr_map_entries - 1].bbl_num = b;
    instr_map[first].app_ins_addr = term->app_ins_addr;

    xed_inst1(&enc, dstate, XED_ICLASS_JRCXZ, 64, xed_relbr(0, 8));
    if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
      return -1;
    unsigned jz = num_of_instr_map_entries - 1;
    instr_map[jz].app_ins_addr = 0;
    instr_map[jz].orig_targ_addr = 0;
    instr_map[jz].bbl_num = b;
    int jmiss = append_rel_cf(XED_ICLASS_JMP, 0, b);
    if (jmiss < 0)
      return -1;
    m.jne_idx = (unsigned)jmiss;
    int hit = append_label(b);
    if (hit < 0)
      return -1;
    instr_map[jz].targ_map_entry = hit;
    if (append_scratch_spill(XED_REG_RCX, false, m.sslot, b) < 0)
      return -1;
    if (lay && next_start && next_start == hot) {
      g_st.elided++;
    } else if (append_rel_cf(XED_ICLASS_JMP, hot, b) < 0) {
      return -1;
    }
    g_miss.push_back(m);
    g_st.devirt_noflags++;
    return 0;
  }

  ADDRINT *slot = alloc_hot_const(hot);
  if (!slot)
    return 1;

  MissPath m;
  m.term_idx = T;
  m.join_idx = -1;
  m.sslot = b % NUM_SCRATCH_SLOTS;
  m.bbl = b;
  m.scratch = XED_REG_INVALID;
  m.mem = op.mem;
  m.is_call = bm.is_indirect_call;

  unsigned first = num_of_instr_map_entries;
  // -devirt_imm: hot target fits a sign-extended imm32 (static non-PIE
  // code): compare the operand itself, no scratch register or spill.
  if (KnobDevirtImm.Value() && hot < 0x80000000ULL) {
    xed_encoder_instruction_t enc;
    if (op.mem) {
      if (op.base == XED_REG_RIP)
        xed_inst2(&enc, dstate, XED_ICLASS_CMP, 64,
                  xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64), xed_simm0((INT32)hot, 32));
      else
        xed_inst2(&enc, dstate, XED_ICLASS_CMP, 64,
                  xed_mem_bisd(op.base, op.index, op.scale,
                               xed_disp(op.disp, op.disp_width ? op.disp_width : 32), 64),
                  xed_simm0((INT32)hot, 32));
    } else {
      xed_inst2(&enc, dstate, XED_ICLASS_CMP, 64, xed_reg(op.reg), xed_simm0((INT32)hot, 32));
    }
    if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
      return -1;
    unsigned ci = num_of_instr_map_entries - 1;
    instr_map[ci].orig_rip_addr = (op.mem && op.base == XED_REG_RIP) ? term->orig_rip_addr : 0;
    instr_map[ci].orig_targ_addr = 0;
    instr_map[ci].app_ins_addr = 0;
    instr_map[ci].bbl_num = b;
    m.mem = false;                  // nothing to reload on either path
    hot_const_num--;                // the pool slot is not needed
  } else if (op.mem) {
    m.scratch = pick_scratch(op.base, op.index);
    if (append_scratch_spill(m.scratch, true, m.sslot, b) < 0)
      return -1;
    if (append_mov_from_indirect(term, op, m.scratch, b) < 0)
      return -1;
    if (append_cmp_reg_hot(m.scratch, slot, b) < 0)
      return -1;
  } else {
    if (append_cmp_reg_hot(op.reg, slot, b) < 0)
      return -1;
  }
  // Branches to the original jmp/call address enter the guard.
  instr_map[first].app_ins_addr = term->app_ins_addr;

  int jne = append_rel_cf(XED_ICLASS_JNZ, 0, b);
  if (jne < 0)
    return -1;
  m.jne_idx = (unsigned)jne;

  if (m.mem && append_scratch_spill(m.scratch, false, m.sslot, b) < 0)
    return -1;

  if (m.is_call) {
    if (append_rel_cf(XED_ICLASS_CALL_NEAR, hot, b) < 0)
      return -1;
    m.join_idx = append_label(b);
    if (m.join_idx < 0)
      return -1;
    g_miss.push_back(m);
    return emit_ft_jmp(bm.fallthru_app_addr, next_start, b);
  }

  if (lay && next_start && next_start == hot) {
    g_st.elided++;              // hot target laid out right here
  } else if (append_rel_cf(XED_ICLASS_JMP, hot, b) < 0) {
    return -1;
  }
  g_miss.push_back(m);
  return 0;
}

static int emit_miss_paths()
{
  for (size_t k = 0; k < g_miss.size(); k++) {
    const MissPath &m = g_miss[k];
    unsigned miss = num_of_instr_map_entries;
    if (m.mem && append_scratch_spill(m.scratch, false, m.sslot, m.bbl) < 0)
      return -1;
    if (append_clone(&g_old[m.term_idx]) < 0)
      return -1;
    if (m.is_call) {
      int j = append_rel_cf(XED_ICLASS_JMP, 0, m.bbl);
      if (j < 0)
        return -1;
      instr_map[j].targ_map_entry = m.join_idx;
    }
    instr_map[m.jne_idx].targ_map_entry = (int)miss;
    instr_map[m.jne_idx].orig_targ_addr = 0;
  }
  return 0;
}

// Emit BBL b; next_start is the app address of what is emitted right after
// it (0 if unknown).  opt allows layout transforms and devirt.
/* ------------------------------------------------------------- */
/* -opt_inline: inline hot small leaf callees at direct call sites */
/* (course control-flow deck constraints: single-entry leaf, no    */
/* calls/indirect/rsp use except balanced push/pop, no jumps out). */
/* ------------------------------------------------------------- */
struct InlCallee {
  char state;                   // 0 unknown, 1 ok, 2 rejected
  std::vector<unsigned> body;   // g_old indices, final ret dropped
  std::vector<char> is_ret;     // inner ret -> jmp to end of copy
  std::vector<int> targ;        // internal branch: body pos (== size: end), else -1
};
static std::vector<InlCallee> g_inl;
static unsigned g_inl_sites = 0;

static bool inl_reg_is_rsp(xed_reg_enum_t r)
{
  if (r == XED_REG_INVALID)
    return false;
  if (r == XED_REG_STACKPUSH || r == XED_REG_STACKPOP)
    return true;
  return xed_get_largest_enclosing_register(r) == XED_REG_RSP;
}

// Per-instruction check; *dstack = +1/-1 for push/pop.  Returns false = reject.
static bool inl_check_ins(const instr_map_t *e, int *dstack)
{
  *dstack = 0;
  xed_decoded_inst_t xedd;
  xed_decoded_inst_zero_set_mode(&xedd, &dstate);
  if (xed_decode(&xedd, reinterpret_cast<UINT8*>(const_cast<char*>(e->encoded_ins)),
                 max_inst_len) != XED_ERROR_NONE)
    return false;
  xed_iclass_enum_t ic = xed_decoded_inst_get_iclass(&xedd);
  xed_category_enum_t cat = xed_decoded_inst_get_category(&xedd);
  const xed_inst_t *xi = xed_decoded_inst_inst(&xedd);
  if (cat == XED_CATEGORY_CALL || cat == XED_CATEGORY_SYSCALL || cat == XED_CATEGORY_SYSRET ||
      cat == XED_CATEGORY_INTERRUPT || cat == XED_CATEGORY_SYSTEM || is_rel8_only_iclass(ic))
    return false;
  if (cat == XED_CATEGORY_RET)
    return xed_decoded_inst_get_iform_enum(&xedd) == XED_IFORM_RET_NEAR;
  if (cat == XED_CATEGORY_UNCOND_BR || cat == XED_CATEGORY_COND_BR)
    return e->orig_targ_addr != 0 && e->orig_targ_addr != ZERO_ADDR_MARK;
  if (ic == XED_ICLASS_PUSH || ic == XED_ICLASS_POP) {
    if (xed_inst_noperands(xi) < 1 ||
        xed_operand_name(xed_inst_operand(xi, 0)) != XED_OPERAND_REG0)
      return false;
    xed_reg_enum_t r = xed_decoded_inst_get_reg(&xedd, XED_OPERAND_REG0);
    if (xed_reg_class(r) != XED_REG_CLASS_GPR ||
        xed_get_register_width_bits64(r) != 64 || inl_reg_is_rsp(r))
      return false;
    *dstack = (ic == XED_ICLASS_PUSH) ? 1 : -1;
    return true;
  }
  for (unsigned i = 0; i < 2; i++)
    if (inl_reg_is_rsp(xed_decoded_inst_get_base_reg(&xedd, i)) ||
        inl_reg_is_rsp(xed_decoded_inst_get_index_reg(&xedd, i)))
      return false;
  unsigned nops = xed_inst_noperands(xi);
  for (unsigned i = 0; i < nops; i++) {
    xed_operand_enum_t name = xed_operand_name(xed_inst_operand(xi, i));
    if (!xed_operand_is_register(name) && name != XED_OPERAND_BASE0 &&
        name != XED_OPERAND_BASE1 && name != XED_OPERAND_INDEX)
      continue;
    if (inl_reg_is_rsp(xed_decoded_inst_get_reg(&xedd, name)))
      return false;
  }
  return true;
}

// Analyze routine r in the TC map (g_old).  Walks the CFG from the entry:
// every reachable instruction must pass inl_check_ins, push/pop depth must be
// consistent on all paths and 0 at every ret, no path may leave the routine.
static const InlCallee *inl_get(int r)
{
  if (r < 0)
    return nullptr;
  InlCallee &ci = g_inl[r];
  if (ci.state)
    return ci.state == 1 ? &ci : nullptr;
  ci.state = 2;
  const RtnInfo &ri = g_rtns[r];
  if (ri.first_bbl >= ri.end_bbl || ri.app_addr == g_exit_addr)
    return nullptr;
  unsigned lo = bbl_map[ri.first_bbl].starting_ins_entry;
  unsigned hi = bbl_map[ri.end_bbl - 1].terminating_ins_entry;
  std::vector<unsigned> all;
  unsigned n_real = 0;
  for (unsigned i = lo; i <= hi; i++) {
    const instr_map_t *e = &g_old[i];
    if (e->ins_type == ProfilingIns || !e->size ||
        (e->ins_type == RtnHeadIns && e->xed_category == XED_CATEGORY_WIDENOP))
      continue;
    all.push_back(i);
    bool nop = e->xed_category == XED_CATEGORY_NOP || e->xed_category == XED_CATEGORY_WIDENOP;
    if (!nop && ++n_real > KnobInlMaxIns.Value())
      return nullptr;
  }
  if (all.empty() || g_old[all[0]].app_ins_addr != ri.app_addr)
    return nullptr;
  std::map<ADDRINT, unsigned> pos_of;
  for (unsigned p = 0; p < all.size(); p++)
    pos_of.emplace(g_old[all[p]].app_ins_addr, p);

  const int UNSEEN = -1000000;
  std::vector<int> depth(all.size(), UNSEEN), tpos(all.size(), -1);
  std::vector<unsigned> work(1, 0);
  depth[0] = 0;
  while (!work.empty()) {
    unsigned p = work.back();
    work.pop_back();
    const instr_map_t *e = &g_old[all[p]];
    int d = depth[p];
    unsigned succ[2], ns = 0;
    bool nop = e->xed_category == XED_CATEGORY_NOP || e->xed_category == XED_CATEGORY_WIDENOP;
    if (nop) {
      succ[ns++] = p + 1;
    } else {
      int ds = 0;
      if (!inl_check_ins(e, &ds))
        return nullptr;
      d += ds;
      if (d < 0)
        return nullptr;
      if (e->xed_category == XED_CATEGORY_RET) {
        if (d != 0)
          return nullptr;
      } else if (e->xed_category == XED_CATEGORY_UNCOND_BR ||
                 e->xed_category == XED_CATEGORY_COND_BR) {
        std::map<ADDRINT, unsigned>::const_iterator t = pos_of.find(real_addr(e->orig_targ_addr));
        if (t == pos_of.end())
          return nullptr;                     // branch out of the routine
        tpos[p] = (int)t->second;
        succ[ns++] = t->second;
        if (e->xed_category == XED_CATEGORY_COND_BR)
          succ[ns++] = p + 1;
      } else {
        succ[ns++] = p + 1;
      }
    }
    for (unsigned k = 0; k < ns; k++) {
      unsigned s = succ[k];
      if (s >= all.size())
        return nullptr;                       // falls off the routine end
      if (depth[s] == UNSEEN) {
        depth[s] = d;
        work.push_back(s);
      } else if (depth[s] != d) {
        return nullptr;
      }
    }
  }
  // Body: reachable non-nop instructions in address order.
  std::vector<int> epos(all.size() + 1, -1);
  for (unsigned p = 0; p < all.size(); p++) {
    const instr_map_t *e = &g_old[all[p]];
    bool nop = e->xed_category == XED_CATEGORY_NOP || e->xed_category == XED_CATEGORY_WIDENOP;
    if (depth[p] == UNSEEN || nop)
      continue;
    epos[p] = (int)ci.body.size();
    ci.body.push_back(all[p]);
    ci.is_ret.push_back(e->xed_category == XED_CATEGORY_RET);
    ci.targ.push_back(tpos[p]);
  }
  int next = (int)ci.body.size();               // a nop maps to the next emitted ins
  for (int p = (int)all.size() - 1; p >= 0; p--) {
    if (epos[p] >= 0)
      next = epos[p];
    else
      epos[p] = next;
  }
  for (unsigned k = 0; k < ci.targ.size(); k++)
    if (ci.targ[k] >= 0)
      ci.targ[k] = epos[ci.targ[k]];
  if (!ci.body.empty() && ci.is_ret.back()) {   // final ret falls through
    ci.body.pop_back();
    ci.is_ret.pop_back();
    ci.targ.pop_back();
  }
  ci.state = 1;
  return &ci;
}

// Copy the callee body in place of a call at app address key_addr; inner rets
// and branches to the dropped final ret go to a label after the copy.
static int emit_inline_body(const InlCallee &ci, ADDRINT key_addr, unsigned bbl)
{
  unsigned base = num_of_instr_map_entries;
  unsigned n = (unsigned)ci.body.size();
  std::vector<unsigned> end_jumps;
  for (unsigned k = 0; k < n; k++) {
    int idx;
    if (ci.is_ret[k]) {
      idx = append_rel_cf(XED_ICLASS_JMP, 0, bbl);
      if (idx < 0)
        return -1;
      end_jumps.push_back((unsigned)idx);
    } else {
      if (append_clone(&g_old[ci.body[k]]) < 0)
        return -1;
      idx = (int)num_of_instr_map_entries - 1;
      if (ci.targ[k] >= 0) {
        instr_map[idx].orig_targ_addr = 0;     // resolved via targ_map_entry
        if ((unsigned)ci.targ[k] >= n)
          end_jumps.push_back((unsigned)idx);
        else
          instr_map[idx].targ_map_entry = (int)(base + ci.targ[k]);
      }
    }
    instr_map[idx].app_ins_addr = 0;           // copies are not branch targets
    instr_map[idx].orig_ins_addr = 0;
    instr_map[idx].bbl_num = bbl;
  }
  int end = append_label(bbl);
  if (end < 0)
    return -1;
  // Branches to the original call site land on the copy.
  instr_map[base].app_ins_addr = key_addr;
  for (unsigned j : end_jumps)
    instr_map[j].targ_map_entry = end;
  g_inl_sites++;
  return 0;
}

// Inline candidate: a direct call in a hot BBL whose callee passes inl_get.
static const InlCallee *inline_target(const instr_map_t *e, unsigned b)
{
  if (!KnobOptInline.Value() || e->xed_category != XED_CATEGORY_CALL ||
      !e->orig_targ_addr || e->orig_targ_addr == ZERO_ADDR_MARK ||
      bbl_map[b].counter < (UINT64)KnobInlHot.Value())
    return nullptr;
  return inl_get(rtn_by_head(real_addr(e->orig_targ_addr)));
}

static int emit_bbl(unsigned b, ADDRINT next_start, bool opt)
{
  const bbl_map_t &bm = bbl_map[b];
  unsigned S = bm.starting_ins_entry;
  unsigned T = bm.terminating_ins_entry;
  const instr_map_t *term = &g_old[T];
  xed_category_enum_t cat = term->xed_category;
  ADDRINT ft = bm.fallthru_app_addr;
  bool lay = opt && KnobOptLayout.Value();

  bool is_cond = (cat == XED_CATEGORY_COND_BR);
  bool is_djmp = (cat == XED_CATEGORY_UNCOND_BR && term->orig_targ_addr != 0);
  ADDRINT hot = 0;
  UINT64 hot_cnt = 0;
  bool devirt = opt && KnobOptDevirt.Value() && bm.is_indirect &&
                pick_hot_indirect(b, &hot, &hot_cnt);
  bool special = (lay && (is_cond || is_djmp)) || devirt;

  for (unsigned i = S; i <= T; i++) {
    if (special && i == T)
      break;
    const instr_map_t *e = &g_old[i];
    if (e->ins_type == ProfilingIns || !e->size)
      continue;
    if (e->ins_type == RtnHeadIns && e->xed_category == XED_CATEGORY_WIDENOP)
      continue;
    // A call that ends a routine has nothing to fall through to: keep it.
    const InlCallee *ci = (i < T || ft) ? inline_target(e, b) : nullptr;
    if (ci) {
      if (emit_inline_body(*ci, e->app_ins_addr, b) < 0)
        return -1;
      continue;
    }
    if (append_branch_clone(e) < 0)
      return -1;
    // App nops are dropped but keep their address as a branch target.
    if (e->xed_category == XED_CATEGORY_NOP || e->xed_category == XED_CATEGORY_WIDENOP)
      instr_map[num_of_instr_map_entries - 1].size = 0;
  }

  if (devirt) {
    int rc = emit_devirt(b, hot, next_start, lay);
    if (rc < 0)
      return -1;
    if (rc == 0) {
      g_st.devirt++;
      return 0;
    }
    if (append_clone(term) < 0)
      return -1;
    return bm.is_indirect_call ? emit_ft_jmp(ft, next_start, b) : 0;
  }

  if (lay && is_cond) {
    ADDRINT taken = real_addr(term->orig_targ_addr);
    // Taken successor laid out next: invert so it falls through.
    if (next_start && taken == next_start && ft && ft != next_start) {
      int rc = append_inverted_jcc(term, ft);
      if (rc < 0)
        return -1;
      if (rc == 0) {
        g_st.inverted++;
        return 0;
      }
    }
    if (append_branch_clone(term) < 0)
      return -1;
    return emit_ft_jmp(ft, next_start, b);
  }

  if (lay && is_djmp) {
    if (append_clone(term) < 0)
      return -1;
    // Target laid out next: the jmp becomes a zero-size alias.
    if (next_start && real_addr(term->orig_targ_addr) == next_start) {
      instr_map[num_of_instr_map_entries - 1].size = 0;
      g_st.elided++;
    }
    return 0;
  }

  if (cat == XED_CATEGORY_RET || cat == XED_CATEGORY_UNCOND_BR)
    return 0;
  return emit_ft_jmp(ft, next_start, b);
}

// Zero-size head entry: TC head (orig_ins_addr) is patched to jump here.
static int emit_rtn_head(unsigned r)
{
  const RtnInfo &ri = g_rtns[r];
  xed_decoded_inst_t xedd;
  if (create_nop7_xedd_instr(&xedd) < 0)
    return -1;
  if (add_new_instr_entry(&xedd, ri.app_addr, RtnHeadIns) < 0)
    return -1;
  unsigned i = num_of_instr_map_entries - 1;
  instr_map[i].size = 0;
  instr_map[i].orig_ins_addr = g_old[ri.tc_head_entry].new_ins_addr;
  instr_map[i].app_ins_addr = ri.app_addr;
  instr_map[i].orig_targ_addr = 0;
  instr_map[i].orig_rip_addr = 0;
  instr_map[i].targ_map_entry = -1;
  return 0;
}

// Cold routine: head + "jmp [slot]" to the original code (Pin's trampoline
// when the routine is probed, else its unmodified original address).

static int emit_cold_stub(unsigned r)
{
  const RtnInfo &ri = g_rtns[r];
  if (stub_num >= max_rtn_count)
    return -1;
  if (emit_rtn_head(r) < 0)
    return -1;
  ADDRINT *slot = &stub_slots[stub_num++];
  // -cold_unprobe: the entry is restored at commit, so go straight to it.
  g_rtns[r].unprobe = KnobColdUnprobe.Value() && ri.orig_entry && ri.probe_len &&
                      ri.app_addr != g_exit_addr;
  *slot = (ri.orig_entry && !ri.unprobe) ? ri.orig_entry : ri.app_addr;
  g_rtns[r].stub_slot = slot;
  g_stub_heads.push_back(std::make_pair(num_of_instr_map_entries - 1, *slot));
  xed_encoder_instruction_t enc;
  xed_inst1(&enc, dstate, XED_ICLASS_JMP, 64, xed_mem_bd(XED_REG_RIP, xed_disp(0, 32), 64));
  if (add_new_encoded_instr(0, &enc, RegularIns) < 0)
    return -1;
  unsigned i = num_of_instr_map_entries - 1;
  instr_map[i].orig_rip_addr = (ADDRINT)slot;
  instr_map[i].orig_targ_addr = 0;
  instr_map[i].app_ins_addr = 0;
  return 0;
}

// BBL of routine r that starts at addr, or -1.
static int find_bbl_in_rtn(const RtnInfo &r, ADDRINT addr)
{
  if (!addr)
    return -1;
  unsigned lo = r.first_bbl, hi = r.end_bbl;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo) / 2;
    if (bbl_map[mid].bbl_addr < addr)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < r.end_bbl && bbl_map[lo].bbl_addr == addr)
    return (int)lo;
  return -1;
}

// Heaviest unplaced hot successor of b inside r (-1 if none).
static int best_succ(const RtnInfo &r, unsigned b, const std::vector<char> &placed)
{
  const bbl_map_t &bm = bbl_map[b];
  const instr_map_t *term = &g_old[bm.terminating_ins_entry];
  xed_category_enum_t cat = term->xed_category;
  ADDRINT t1 = 0, t2 = 0;       // t1 taken-like, t2 fall-through
  UINT64 w1 = 0, w2 = 0;

  if (cat == XED_CATEGORY_COND_BR) {
    t1 = real_addr(term->orig_targ_addr);
    w1 = bm.counter > bm.fallthru_counter ? bm.counter - bm.fallthru_counter : 0;
    t2 = bm.fallthru_app_addr;
    w2 = bm.fallthru_counter;
  } else if (cat == XED_CATEGORY_UNCOND_BR && term->orig_targ_addr) {
    t1 = real_addr(term->orig_targ_addr);
    w1 = bm.counter;
  } else if (cat == XED_CATEGORY_UNCOND_BR) {
    ADDRINT hot = 0;
    UINT64 cnt = 0;
    if (KnobOptDevirt.Value() && !bm.is_indirect_call && pick_hot_indirect(b, &hot, &cnt)) {
      t1 = hot;
      w1 = cnt;
    }
  } else if (cat != XED_CATEGORY_RET) {
    t2 = bm.fallthru_app_addr;
    w2 = bm.counter;
  }

  int id1 = find_bbl_in_rtn(r, t1);
  int id2 = find_bbl_in_rtn(r, t2);
  bool ok1 = id1 >= 0 && !placed[id1 - r.first_bbl] && bbl_is_hot(id1) && w1 > 0;
  bool ok2 = id2 >= 0 && !placed[id2 - r.first_bbl] && bbl_is_hot(id2) && w2 > 0;
  if (ok1 && ok2)
    return (w1 > w2) ? id1 : id2;
  if (ok1)
    return id1;
  if (ok2)
    return id2;
  return -1;
}

static bool by_count_desc(unsigned a, unsigned b)
{
  if (bbl_map[a].counter != bbl_map[b].counter)
    return bbl_map[a].counter > bbl_map[b].counter;
  return a < b;
}

// Hot chain (entry first, greedy heaviest successor) and cold rest.
static void layout_rtn(const RtnInfo &r, std::vector<unsigned> &hot,
                       std::vector<unsigned> &cold)
{
  unsigned n = r.end_bbl - r.first_bbl;
  std::vector<char> placed(n, 0);
  std::vector<unsigned> seeds;
  seeds.push_back(r.first_bbl);
  std::vector<unsigned> hots;
  for (unsigned b = r.first_bbl + 1; b < r.end_bbl; b++) {
    if (bbl_is_hot(b))
      hots.push_back(b);
  }
  std::sort(hots.begin(), hots.end(), by_count_desc);
  seeds.insert(seeds.end(), hots.begin(), hots.end());

  for (unsigned s = 0; s < seeds.size(); s++) {
    int cur = (int)seeds[s];
    while (cur >= 0 && !placed[cur - r.first_bbl]) {
      placed[cur - r.first_bbl] = 1;
      hot.push_back((unsigned)cur);
      cur = best_succ(r, (unsigned)cur, placed);
    }
  }
  for (unsigned b = r.first_bbl; b < r.end_bbl; b++) {
    if (!placed[b - r.first_bbl])
      cold.push_back(b);
  }
}

struct PhEdge {
  UINT64 w;
  unsigned a, b;
};

static bool ph_edge_less(const PhEdge &x, const PhEdge &y)
{
  if (x.w != y.w)
    return x.w > y.w;
  if (x.a != y.a)
    return x.a < y.a;
  return x.b < y.b;
}

// Pettis-Hansen: merge routine chains along the heaviest call edges,
// orienting each merge to put caller and callee closest.
static void ph_order(const std::vector<unsigned> &hot_list,
                     const std::vector<UINT64> &heat, std::vector<unsigned> &out)
{
  unsigned nh = (unsigned)hot_list.size();
  std::vector<int> loc(g_rtns.size(), -1);
  for (unsigned h = 0; h < nh; h++)
    loc[hot_list[h]] = (int)h;

  std::map<UINT64, UINT64> ew;
  for (unsigned h = 0; h < nh; h++) {
    const RtnInfo &r = g_rtns[hot_list[h]];
    for (unsigned b = r.first_bbl; b < r.end_bbl; b++) {
      const bbl_map_t &bm = bbl_map[b];
      if (!bm.counter)
        continue;
      for (unsigned i = bm.starting_ins_entry; i <= bm.terminating_ins_entry; i++) {
        const instr_map_t *e = &g_old[i];
        if (e->ins_type == ProfilingIns || !e->size || !e->orig_targ_addr)
          continue;
        xed_category_enum_t c = e->xed_category;
        if (c != XED_CATEGORY_CALL && c != XED_CATEGORY_UNCOND_BR && c != XED_CATEGORY_COND_BR)
          continue;
        int callee = rtn_by_head(real_addr(e->orig_targ_addr));
        if (callee < 0 || loc[callee] < 0 || loc[callee] == (int)h)
          continue;
        UINT64 w = bm.counter;
        if (c == XED_CATEGORY_COND_BR)
          w = bm.counter > bm.fallthru_counter ? bm.counter - bm.fallthru_counter : 0;
        unsigned x = std::min(h, (unsigned)loc[callee]);
        unsigned y = std::max(h, (unsigned)loc[callee]);
        ew[((UINT64)x << 32) | y] += w;
      }
      if (bm.is_indirect_call) {
        ADDRINT t = 0;
        UINT64 cnt = 0;
        if (pick_hot_indirect(b, &t, &cnt)) {
          int callee = rtn_by_head(t);
          if (callee >= 0 && loc[callee] >= 0 && loc[callee] != (int)h) {
            unsigned x = std::min(h, (unsigned)loc[callee]);
            unsigned y = std::max(h, (unsigned)loc[callee]);
            ew[((UINT64)x << 32) | y] += cnt;
          }
        }
      }
    }
  }

  std::vector<PhEdge> edges;
  for (std::map<UINT64, UINT64>::const_iterator it = ew.begin(); it != ew.end(); ++it) {
    if (!it->second)
      continue;
    PhEdge e;
    e.w = it->second;
    e.a = (unsigned)(it->first >> 32);
    e.b = (unsigned)(it->first & 0xFFFFFFFFu);
    edges.push_back(e);
  }
  std::sort(edges.begin(), edges.end(), ph_edge_less);

  std::vector<UINT64> sz(nh);
  for (unsigned h = 0; h < nh; h++)
    sz[h] = g_rtns[hot_list[h]].app_size ? g_rtns[hot_list[h]].app_size : 1;

  std::vector<std::vector<unsigned> > ch(nh);
  std::vector<unsigned> cof(nh);
  for (unsigned h = 0; h < nh; h++) {
    ch[h].push_back(h);
    cof[h] = h;
  }

  for (size_t k = 0; k < edges.size(); k++) {
    unsigned a = edges[k].a, b = edges[k].b;
    unsigned ca = cof[a], cb = cof[b];
    if (ca == cb)
      continue;
    std::vector<unsigned> &A = ch[ca];
    std::vector<unsigned> &B = ch[cb];
    UINT64 sza = 0, offa = 0, szb = 0, offb = 0;
    for (unsigned x : A) { if (x == a) offa = sza; sza += sz[x]; }
    for (unsigned x : B) { if (x == b) offb = szb; szb += sz[x]; }
    UINT64 ea = offa + sz[a], eb = offb + sz[b];
    UINT64 d[4];
    d[0] = (sza - ea) + offb;          // A B
    d[1] = (sza - ea) + (szb - eb);    // A rev(B)
    d[2] = offa + offb;                // rev(A) B
    d[3] = (szb - eb) + offa;          // B A
    unsigned best = 0;
    for (unsigned o = 1; o < 4; o++)
      if (d[o] < d[best])
        best = o;
    std::vector<unsigned> merged;
    merged.reserve(A.size() + B.size());
    if (best == 0) {
      merged = A; merged.insert(merged.end(), B.begin(), B.end());
    } else if (best == 1) {
      merged = A; merged.insert(merged.end(), B.rbegin(), B.rend());
    } else if (best == 2) {
      merged.assign(A.rbegin(), A.rend()); merged.insert(merged.end(), B.begin(), B.end());
    } else {
      merged = B; merged.insert(merged.end(), A.begin(), A.end());
    }
    for (unsigned x : B)
      cof[x] = ca;
    A.swap(merged);
    B.clear();
  }

  // Hottest chains first.
  std::vector<std::pair<UINT64, unsigned> > order;
  for (unsigned c = 0; c < nh; c++) {
    if (ch[c].empty())
      continue;
    UINT64 hsum = 0;
    for (unsigned x : ch[c])
      hsum += heat[hot_list[x]];
    order.push_back(std::make_pair(~hsum, c));   // ascending ~hsum = descending heat
  }
  std::sort(order.begin(), order.end());
  for (size_t k = 0; k < order.size(); k++) {
    for (unsigned x : ch[order[k].second])
      out.push_back(hot_list[x]);
  }
}

static ADDRINT item_start(const Item &it)
{
  return it.kind == IT_BBL ? bbl_map[it.id].bbl_addr : g_rtns[it.id].app_addr;
}

static void push_rtn_bbls(const RtnInfo &r, std::vector<Item> &v, bool opt)
{
  for (unsigned b = r.first_bbl; b < r.end_bbl; b++) {
    Item it = { IT_BBL, b, opt };
    v.push_back(it);
  }
}

// Build the TC2 instr_map from the TC map and the profile.
static int rebuild_instr_map_for_tc2()
{
  instr_map_t *old_map = instr_map;
  const size_t old_bytes = instr_map_bytes;
  const unsigned old_n = num_of_instr_map_entries;
  g_old = old_map;
  memset(&g_st, 0, sizeof(g_st));
  g_miss.clear();
  g_stub_heads.clear();
  stub_num = 0;
  hot_const_num = 0;

  double tp = now_s(), tq;
  unsigned nr = (unsigned)g_rtns.size();
  g_heads.clear();
  for (unsigned r = 0; r < nr; r++)
    g_heads.push_back(std::make_pair(g_rtns[r].app_addr, r));
  std::sort(g_heads.begin(), g_heads.end());
  g_inl.assign(nr, InlCallee());
  g_inl_sites = 0;

  // Routine heat: hottest BBL decides hot/cold (covers loops entered once).
  std::vector<UINT64> rmax(nr, 0), rsum(nr, 0);
  std::vector<unsigned> hot_list, cold_list;
  for (unsigned r = 0; r < nr; r++) {
    for (unsigned b = g_rtns[r].first_bbl; b < g_rtns[r].end_bbl; b++) {
      rmax[r] = std::max(rmax[r], bbl_map[b].counter);
      rsum[r] += bbl_map[b].counter;
    }
    if (g_rtns[r].force_hot || rmax[r] >= (UINT64)KnobHotRtn.Value())
      hot_list.push_back(r);
    else
      cold_list.push_back(r);
  }

  tq = now_s(); g_pt[PT_SELECT] = tq - tp; tp = tq;
  std::vector<unsigned> order;
  if (KnobOptLayout.Value())
    ph_order(hot_list, rsum, order);
  else
    order = hot_list;
  tq = now_s(); g_pt[PT_PH] = tq - tp; tp = tq;

  // Emission order: hot routines (hot chains), cold BBLs of hot routines,
  // cold routines (whole, or a stub with -hot_only).
  std::vector<Item> items, tail;
  for (size_t k = 0; k < order.size(); k++) {
    unsigned r = order[k];
    const RtnInfo &ri = g_rtns[r];
    Item h = { IT_HEAD, r, true };
    items.push_back(h);
    bool opt = !ri.no_layout;
    if (KnobOptLayout.Value() && opt) {
      std::vector<unsigned> hb, cb;
      layout_rtn(ri, hb, cb);
      for (unsigned b : hb) { Item it = { IT_BBL, b, true }; items.push_back(it); }
      for (unsigned b : cb) { Item it = { IT_BBL, b, true }; tail.push_back(it); }
      g_st.cold_bbls += (unsigned)cb.size();
    } else {
      push_rtn_bbls(ri, items, opt);
    }
  }
  items.insert(items.end(), tail.begin(), tail.end());
  for (unsigned r : cold_list) {
    if (KnobHotOnly.Value()) {
      Item s = { IT_STUB, r, false };
      items.push_back(s);
    } else {
      Item h = { IT_HEAD, r, true };
      items.push_back(h);
      push_rtn_bbls(g_rtns[r], items, !g_rtns[r].no_layout);
    }
  }
  tq = now_s(); g_pt[PT_LAYOUT] = tq - tp; tp = tq;
  g_st.hot_rtns = (unsigned)order.size();
  g_st.stub_rtns = KnobHotOnly.Value() ? (unsigned)cold_list.size() : 0;

  // Entry budget: clones + per-BBL extras (ft jmp, devirt guard, miss path).
  size_t budget = 256;
  for (size_t k = 0; k < items.size(); k++) {
    if (items[k].kind == IT_BBL) {
      const bbl_map_t &bm = bbl_map[items[k].id];
      budget += (bm.terminating_ins_entry - bm.starting_ins_entry + 1) + 16;
      if (KnobOptInline.Value())                  // room for inlined bodies
        for (unsigned i = bm.starting_ins_entry; i <= bm.terminating_ins_entry; i++)
          if (g_old[i].xed_category == XED_CATEGORY_CALL)
            budget += KnobInlMaxIns.Value() + 4;
    } else {
      budget += 4;
    }
  }

  instr_map_bytes = budget * sizeof(instr_map_t);
  instr_map = (instr_map_t *)lazy_zero_alloc(instr_map_bytes);
  if (!instr_map) {
    instr_map = old_map;
    instr_map_bytes = old_bytes;
    return -1;
  }
  cur_max_ins_count = (unsigned)budget;
  num_of_instr_map_entries = 0;

  int rc = 0;
  for (size_t k = 0; k < items.size() && rc == 0; k++) {
    const Item &it = items[k];
    ADDRINT next_start = (k + 1 < items.size()) ? item_start(items[k + 1]) : 0;
    if (it.kind == IT_HEAD)
      rc = emit_rtn_head(it.id);
    else if (it.kind == IT_STUB)
      rc = emit_cold_stub(it.id);
    else
      rc = emit_bbl(it.id, next_start, it.opt);
  }
  if (rc == 0)
    rc = emit_miss_paths();

  if (rc < 0) {
    munmap(instr_map, instr_map_bytes);
    instr_map = old_map;
    instr_map_bytes = old_bytes;
    num_of_instr_map_entries = old_n;
    cur_max_ins_count = max_ins_count;
    return -1;
  }

  munmap(old_map, old_bytes);
  g_old = nullptr;
  g_pt[PT_EMIT] = now_s() - tp;
  return 0;
}

// Size of a rel8 encoding of this jmp/jcc, 0 if it has none.
static unsigned short_branch_size(const instr_map_t &e)
{
  xed_decoded_inst_t xedd;
  xed_decoded_inst_zero_set_mode(&xedd, &dstate);
  if (xed_decode(&xedd, reinterpret_cast<UINT8*>(const_cast<char*>(e.encoded_ins)),
                 max_inst_len) != XED_ERROR_NONE)
    return 0;
  xed_iclass_enum_t ic = xed_decoded_inst_get_iclass(&xedd);
  if (ic != XED_ICLASS_JMP && invert_jcc_iclass(ic) == XED_ICLASS_INVALID)
    return 0;
  if (xed_decoded_inst_get_branch_displacement_width(&xedd) == 0)
    return 0;
  xed_encoder_request_init_from_decode(&xedd);
  xed_encoder_request_set_branch_displacement(&xedd, 0, 1);
  UINT8 buf[XED_MAX_INSTRUCTION_BYTES];
  unsigned olen = 0;
  if (xed_encode(&xedd, buf, XED_MAX_INSTRUCTION_BYTES, &olen) != XED_ERROR_NONE)
    return 0;
  return olen;
}

// Shrink TC2 jmp/jcc to rel8 where they fit.  The caller re-runs
// fix_instructions_displacements() to encode the final displacements (its
// strict pass reverts any rel8 that does not fit).
static void shrink_branches()
{
  // rel8 size per entry, computed once (0 = not shrinkable).
  std::vector<unsigned char> ss(num_of_instr_map_entries, 0);
  for (unsigned i = 0; i < num_of_instr_map_entries; i++) {
    const instr_map_t &e = instr_map[i];
    if (e.short_br || e.size <= 2 || e.targ_map_entry < 0)
      continue;
    if (e.xed_category != XED_CATEGORY_UNCOND_BR && e.xed_category != XED_CATEGORY_COND_BR)
      continue;
    unsigned s = short_branch_size(e);
    if (s && s < e.size)
      ss[i] = (unsigned char)s;
  }

  // Optimistic: all candidates short; recompute exact addresses; grow back
  // the ones that do not fit.  Sizes only grow, so few passes are needed.
  std::vector<unsigned char> long_size(num_of_instr_map_entries, 0);
  for (unsigned i = 0; i < num_of_instr_map_entries; i++) {
    if (!ss[i])
      continue;
    long_size[i] = (unsigned char)instr_map[i].size;
    instr_map[i].size = ss[i];
    instr_map[i].short_br = true;
  }
  const ADDRINT base = num_of_instr_map_entries ? instr_map[0].new_ins_addr : 0;
  bool changed = true;
  while (changed) {
    changed = false;
    ADDRINT a = base;
    for (unsigned i = 0; i < num_of_instr_map_entries; i++) {
      instr_map[i].new_ins_addr = a;
      a += instr_map[i].size;
    }
    for (unsigned i = 0; i < num_of_instr_map_entries; i++) {
      instr_map_t &e = instr_map[i];
      if (!e.short_br || !ss[i])
        continue;
      xed_int64_t d = (xed_int64_t)instr_map[e.targ_map_entry].new_ins_addr -
                      (xed_int64_t)(e.new_ins_addr + e.size);
      if (d >= -128 && d <= 127)
        continue;
      e.size = long_size[i];
      e.short_br = false;
      changed = true;
    }
  }
  for (unsigned i = 0; i < num_of_instr_map_entries; i++)
    if (ss[i] && instr_map[i].short_br)
      g_st.short_br++;
}

// Direct branches/calls that chain to a cold-routine stub go straight to
// the routine's original entry (skips "jmp [slot]") when rel32 reaches it.
// The stub stays for the TC-head patch and out-of-reach cases.
static void retarget_cold_branches()
{
  for (unsigned i = 0; i < num_of_instr_map_entries; i++) {
    instr_map_t &e = instr_map[i];
    if (!e.size || e.targ_map_entry < 0)
      continue;
    std::vector<std::pair<unsigned, ADDRINT> >::const_iterator it =
        std::lower_bound(g_stub_heads.begin(), g_stub_heads.end(),
                         std::make_pair((unsigned)e.targ_map_entry, (ADDRINT)0));
    if (it == g_stub_heads.end() || it->first != (unsigned)e.targ_map_entry)
      continue;
    INT64 d = (INT64)it->second - (INT64)(ADDRINT)tc2;
    if (d > 0x70000000LL || d < -0x70000000LL)
      continue;
    e.targ_map_entry = -1;
    e.orig_targ_addr = it->second;
    g_st.cold_direct++;
  }
}

// -ctr_derive: fill in the counts of BBLs that had no counter, from their
// single incoming edge: jmp -> count(src), jcc taken -> count(src) - ft(src),
// indirect-call return site -> count(b-1).  bbl_map[].counter holds the
// measured counts on entry.
static void resolve_derived_counters()
{
    std::map<ADDRINT, unsigned> src_bbl;          // branch app addr -> its BBL
    for (unsigned b = 0; b < bbl_num; b++)
      if (bbl_map[b].derive == 1)
        src_bbl[bbl_map[b].derive_src] = 0;
    if (!src_bbl.empty()) {
      for (unsigned b = 0; b < bbl_num; b++) {
        unsigned t = bbl_map[b].terminating_ins_entry;
        if (t >= num_of_instr_map_entries)
          continue;
        std::map<ADDRINT, unsigned>::iterator it = src_bbl.find(instr_map[t].app_ins_addr);
        if (it != src_bbl.end())
          it->second = b + 1;                     // 1-based, 0 = not found
      }
    }
    std::vector<char> done(bbl_num, 1);
    for (unsigned b = 0; b < bbl_num; b++)
      if (bbl_map[b].derive)
        done[b] = 0;
    for (unsigned pass = 0; pass < 64; pass++) {
      bool changed = false;
      for (unsigned b = 0; b < bbl_num; b++) {
        if (done[b])
          continue;
        unsigned s;
        if (bbl_map[b].derive == 2) {
          s = b - 1;
        } else {
          unsigned s1 = src_bbl[bbl_map[b].derive_src];
          if (!s1) {                              // source not found: no count
            bbl_map[b].counter = 0;
            done[b] = 1;
            changed = true;
            continue;
          }
          s = s1 - 1;
        }
        if (!done[s])
          continue;
        UINT64 c = bbl_map[s].counter;
        if (bbl_map[b].derive == 1 && bbl_map[s].is_cond) {
          UINT64 ft;
          if (bbl_map[s].ft_from_next) {
            if (!done[s + 1])
              continue;
            ft = bbl_map[s + 1].counter;
          } else {
            ft = prof_cnt[2 * (size_t)s + 1];
          }
          c = (c > ft) ? c - ft : 0;
        }
        bbl_map[b].counter = c;
        done[b] = 1;
        changed = true;
      }
      if (!changed)
        break;
    }
    for (unsigned b = 0; b < bbl_num; b++)
      if (!done[b])
        bbl_map[b].counter = 0;                   // unreachable cycle

    if (KnobCtrDeriveCheck.Value()) {            // derived vs measured
      UINT64 n = 0, exact = 0, meas = 0, diff = 0;
      for (unsigned b = 0; b < bbl_num; b++) {
        if (!bbl_map[b].derive)
          continue;
        UINT64 m = prof_cnt[2 * (size_t)b], d = bbl_map[b].counter;
        n++;
        exact += (m == d);
        meas += m;
        diff += (m > d) ? m - d : d - m;
      }
      cerr << "ctr_derive_check: derived " << n << ", exact " << exact
           << ", measured sum " << meas << ", |derived-measured| sum " << diff << endl;
    }
}

static int build_and_commit_tc2()
{
    int rc = disable_profiling_in_tc(instr_map, num_of_instr_map_entries);
    if (rc < 0)
      return -1;
    VLOG(<< "profiling disabled" << endl);

    for (unsigned b = 0; b < bbl_num; b++)
      bbl_map[b].counter = prof_cnt[2 * (size_t)b];
    resolve_derived_counters();
    for (unsigned b = 0; b < bbl_num; b++)
      bbl_map[b].fallthru_counter = bbl_map[b].ft_from_next ? bbl_map[b + 1].counter
                                                            : prof_cnt[2 * (size_t)b + 1];

    if (KnobDumpProfile)
      dump_profile();

    if (KnobNoProfile)
      return 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (rebuild_instr_map_for_tc2() < 0) {
      cerr << "failed to rebuild IR for TC2\n";
      return -1;
    }
    double tp = now_s(), tq;
    if (chain_all_direct_jmp_and_call_target_entries(0, num_of_instr_map_entries) < 0)
      return -1;
    if (KnobColdDirect.Value())
      retarget_cold_branches();
    tq = now_s(); g_pt[PT_CHAIN] = tq - tp; tp = tq;

    cur_max_tc_size = max_tc_size / 2;   // TC2 owns the second half
    if (set_initial_estimated_new_ins_addrs_in_tc(tc2) < 0) {
      cerr << "failed to set initial estimated new ins addrs in tc2\n";
      return -1;
    }
    if (fix_instructions_displacements() < 0) {
      cerr << "failed to fix displacments of translated instructions\n";
      return -1;
    }
    tq = now_s(); g_pt[PT_FIX] = tq - tp; tp = tq;
    if (KnobOptLayout.Value() && KnobShortBr.Value()) {
      shrink_branches();
      tq = now_s(); g_pt[PT_SHRINK] = tq - tp; tp = tq;
      if (fix_instructions_displacements() < 0) {
        cerr << "failed to fix displacments after branch shortening\n";
        return -1;
      }
    }

    tq = now_s(); g_pt[PT_FIX2] = tq - tp; tp = tq;
    rc = copy_instrs_to_tc(tc2);
    if (rc < 0) {
      cerr << "failed to copy the instructions to the translation cache\n";
      return -1;
    }
    tc2_size = rc;
    tq = now_s(); g_pt[PT_COPY] = tq - tp; tp = tq;

    if (!KnobDoNotCommitTranslatedCode) {
      if (commit_translated_rtns_to_tc2() < 0) {
        cerr << "failed to commit jump instructions from TC to TC2\n";
        return -1;
      }
      // -migrate_tc: frames still running in TC (entered before the switch)
      // jump into TC2 at their next stub instead of skipping it.
      for (size_t k = 0; k < g_mig.size(); k++) {
        ADDRINT cont = g_mig[k].second;
        if (!in_img(cont))
          continue;
        unsigned e = addr_idx[cont - img_lo];
        if (!e)
          continue;                               // not in TC2 (cold): keep skipping
        ADDRINT head = g_mig[k].first, dest = instr_map[e - 1].new_ins_addr;
        INT64 d = (INT64)dest - (INT64)(head + 5);
        if (d > 0x7FFF0000LL || d < -0x7FFF0000LL)
          continue;
        char jmp5[8];
        jmp5[0] = (char)0xE9;
        INT32 d32 = (INT32)d;
        memcpy(&jmp5[1], &d32, 4);
        atomic_patch5((char *)head, jmp5);
        g_migrated++;
      }
      std::vector<std::pair<ADDRINT, ADDRINT> >().swap(g_mig);
      // Timestamp first, then publish: Fini must never pair "committed"
      // with the create_tc start time.
      clock_gettime(CLOCK_MONOTONIC, &start_running_time);
      g_start_time_set = 1;
      __sync_synchronize();
      g_tc2_committed = 1;
      VLOG(<< "after commit of translated routines from TC to TC2" << endl);
    }

    g_pt[PT_COMMIT] = now_s() - tp;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (KnobDumpTranslatedCode2) {
      cerr << "Translation Cache 2 dump:" << endl;
      dump_tc(tc2, tc2_size);
    }

    if (KnobTc2Stats) {
      double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
      cerr << dec << "TC2: hot rtns " << g_st.hot_rtns
           << ", cold stubs " << g_st.stub_rtns
           << ", cold bbls outlined " << g_st.cold_bbls
           << ", jcc inverted " << g_st.inverted
           << ", jmps elided " << g_st.elided
           << ", devirt sites " << g_st.devirt
           << " (flag-free jmp guards " << g_st.devirt_noflags
           << ", skipped " << g_st.devirt_flags_skip << ")"
           << ", rel8 branches " << g_st.short_br
           << ", cold direct " << g_st.cold_direct
           << ", cold heads direct " << g_cold_heads_direct
           << ", entries direct " << g_entries_direct
           << ", unprobed " << g_unprobed
           << ", counters derived " << g_ctr_derived
           << ", inlined sites " << g_inl_sites
           << ", TC stubs migrated " << g_migrated
           << ", TC probes rebased " << g_probe_bridged << " (already direct " << g_probe_direct << ")"
           << ", TC bytes " << tc_size
           << ", TC2 bytes " << tc2_size
           << ", build " << secs << " s"
           << ", TC stubs inc/reg/spill " << n_stub_inc << "/" << n_stub_reg << "/" << n_stub_spill
           << ", ft stubs elided " << n_ft_elided
           << ", create_tc " << g_create_tc_secs << " s"
           << endl;
      cerr << "TC2 phases (s):";
      for (unsigned k = 0; k < PT_N; k++)
        cerr << " " << g_pt_name[k] << "=" << g_pt[k];
      cerr << "; fix rounds " << g_fix_rounds << ", passes " << g_fix_passes
           << ", entries " << num_of_instr_map_entries << endl;
    }
    return 0;
}

/****************************/
/* create_tc2_thread_func() */
/****************************/
void create_tc2_thread_func(void *v)
{
    (void)v;
    while (!g_tc_ready)
      usleep(20000);

    VLOG(<< " prof time: " << dec << KnobNumSecsDuringProfile.Value() << " sec\n");
    sleep(KnobNumSecsDuringProfile.Value());

    build_and_commit_tc2();
    PIN_ExitThread(0);
}

/****************************/
/* allocate_and_init_memory */
/****************************/
int allocate_and_init_memory(IMG img)
{
    ADDRINT highest_addr = 0;
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;

        if (!lowest_sec_addr || lowest_sec_addr > SEC_Address(sec))
            lowest_sec_addr = SEC_Address(sec);

        if (highest_sec_addr < SEC_Address(sec) + SEC_Size(sec))
            highest_sec_addr = SEC_Address(sec) + SEC_Size(sec);

        // No RTN_Open/RTN_NumIns here: decoding the whole image costs ~900MB.
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            if (highest_addr < RTN_Address(rtn) + RTN_Size(rtn))
                highest_addr = RTN_Address(rtn) + RTN_Size(rtn);
            max_rtn_count++;
            max_ins_count += RTN_Size(rtn);   // bytes: upper bound on ins
        }
    }

    // Per-address tables over the executable range.
    img_lo = lowest_sec_addr;
    img_hi = std::max(highest_sec_addr, highest_addr);
    if (img_hi <= img_lo) {
        cerr << "no executable code found\n";
        return -1;
    }
    img_flags = (unsigned char *)lazy_zero_alloc(img_hi - img_lo);
    img_refs = (unsigned char *)lazy_zero_alloc(img_hi - img_lo);
    addr_idx_bytes = (img_hi - img_lo) * sizeof(unsigned);
    if (!img_flags || !img_refs) {
        cerr << "failed to allocate image tables\n";
        return -1;
    }
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
            set_flag(RTN_Address(rtn), FLAG_RTN_ENTRY);
    }

    // Pre-pass (linear XED sweep, no RTN_Open): every direct branch/call
    // target in the image, and the number of indirect jmp/call.
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn)) {
            ADDRINT a = RTN_Address(rtn), end = a + RTN_Size(rtn);
            while (a < end) {
                xed_decoded_inst_t xedd;
                xed_decoded_inst_zero_set_mode(&xedd, &dstate);
                if (xed_decode(&xedd, reinterpret_cast<UINT8*>(a), max_inst_len) != XED_ERROR_NONE) {
                    a++;
                    continue;
                }
                unsigned len = xed_decoded_inst_get_length(&xedd);
                xed_category_enum_t c = xed_decoded_inst_get_category(&xedd);
                if (xed_decoded_inst_get_branch_displacement_width(&xedd) > 0) {
                    ADDRINT t = a + len + xed_decoded_inst_get_branch_displacement(&xedd);
                    set_flag(t, FLAG_BR_TARGET);
                    if (in_img(t) && img_refs[t - img_lo] < 255)
                        img_refs[t - img_lo]++;
                } else if (c == XED_CATEGORY_CALL || c == XED_CATEGORY_UNCOND_BR) {
                    g_n_indirect++;
                }
                a += len ? len : 1;
            }
        }
    }

    // A BBL has at least one instruction: bound BBLs before inflating.
    max_bbl_count = max_ins_count + 1024;

    max_ins_count *= 4;  // room for the profiling stubs
    cur_max_ins_count = max_ins_count;

    int pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize == -1) {
      perror("sysconf");
      return -1;
    }

    ADDRINT text_size = (highest_sec_addr - lowest_sec_addr) * 2 + pagesize * 4;

    max_tc_size = 10 * text_size + pagesize * 4;
    if (max_tc_size >= 0x7FFFFFFF) {
      cerr << "size of TC is beyond the range of a branch displacement" << endl;
      return -1;
    }

    max_hot_const = (max_bbl_count > 0 ? max_bbl_count : 1024);
    max_targ = g_n_indirect + 256;

    const size_t mem_size =
              max_tc_size +                        // TC + TC2
              max_rtn_count * sizeof(ADDRINT) +    // cold stub slots
              max_hot_const * sizeof(ADDRINT) +    // devirt hot targets
              NUM_SCRATCH_SLOTS * sizeof(ADDRINT) + // devirt spill slots
              2 * (size_t)max_bbl_count * sizeof(UINT64) + // BBL counters
              5 * sizeof(UINT64) +                         // RAX spill, const 1, 3 targ spills
              16 +                                         // align
              (size_t)max_targ * sizeof(targ_prof_t);      // indirect target counters
    char *addr = nullptr;
    ADDRINT max_distance = 0x7FFFFFFF;
    const size_t step = pagesize;
    // Map above the kernel's randomized brk window (1GB after the bss):
    // a cache inside it breaks early sbrk in static binaries (sgcc).
    ADDRINT gap = 0x41000000;
    while (gap > 0x1000000 &&
           (ADDRINT)highest_addr + gap + mem_size - lowest_sec_addr >= 0x7FF00000)
      gap >>= 1;
    ADDRINT aligned_target = ((ADDRINT)highest_addr + gap) & ~((ADDRINT)pagesize - 1);
    void* result = mmap((void*)aligned_target, mem_size,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       0, 0);
    if (result != MAP_FAILED &&
        (abs((long)((ADDRINT)result - aligned_target)) <= (long)max_distance)) {
        addr = (char *)result;
    }

    if (!addr) {
        for (size_t offset = step; offset <= max_distance; offset += step) {
            ADDRINT try_addr = aligned_target + offset;
            result = mmap((void*)try_addr, mem_size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         0, 0);
            if (result != MAP_FAILED &&
                (abs((long)((ADDRINT)result - try_addr)) <= (long)max_distance)) {
                addr = (char *)result;
                break;
            }
            if (result != MAP_FAILED) {
                munmap(result, mem_size);
            }

            if (highest_addr >= offset) {
                try_addr = aligned_target - offset;
                result = mmap((void*)try_addr, mem_size,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS,
                             0, 0);
                if (result != MAP_FAILED &&
                    (abs((long)((ADDRINT)result - try_addr)) <= (long)max_distance)) {
                    addr = (char *)result;
                    break;
                }
                if (result != MAP_FAILED) {
                    munmap(result, mem_size);
                }
            }
        }
    }

    if (!addr) {
        cerr << "failed to allocate memory within 32-bit range. " << endl;
        return -1;
    }

    VLOG(<< " allocated memory at: 0x" << hex << (ADDRINT)addr << "\n");

    tc = (char *)addr;
    addr += max_tc_size/2;

    tc2 = (char *)addr;
    addr += max_tc_size/2;

    stub_slots = (ADDRINT *)addr;
    addr += max_rtn_count * sizeof(ADDRINT);

    hot_const_pool = (ADDRINT *)addr;
    hot_const_num = 0;
    addr += max_hot_const * sizeof(ADDRINT);

    scratch_slots = (ADDRINT *)addr;
    addr += NUM_SCRATCH_SLOTS * sizeof(ADDRINT);

    prof_cnt = (UINT64 *)addr;
    addr += 2 * (size_t)max_bbl_count * sizeof(UINT64);
    prof_rax_rip = (UINT64 *)addr;
    prof_one_rip = prof_rax_rip + 1;
    *prof_one_rip = 1;
    targ_spill_rip = prof_rax_rip + 2;
    addr = (char *)(prof_rax_rip + 5);
    addr = (char *)(((ADDRINT)addr + 15) & ~(ADDRINT)15);
    targ_pool = (targ_prof_t *)addr;

    instr_map_bytes = (size_t)max_ins_count * sizeof(instr_map_t);
    instr_map = (instr_map_t *)lazy_zero_alloc(instr_map_bytes);
    if (instr_map == NULL) {
        perror("mmap");
        return -1;
    }

    bbl_map = (bbl_map_t *)lazy_zero_alloc((size_t)(max_bbl_count + 1) * sizeof(bbl_map_t));
    if (bbl_map == NULL) {
        perror("mmap");
        return -1;
    }


    return 0;
}



/* ============================================ */
/* Main translation routine                     */
/* ============================================ */
typedef VOID (*EXITFUNCPTR)(INT code);
EXITFUNCPTR origExit;

/********/
/* Fini */
/********/
// -run_stats (analysis only, use with a -prof_time longer than the run):
// whole-run profile summary from the TC counters -> run_stats.txt.
static void write_run_stats()
{
    std::ofstream f("run_stats.txt");
    UINT64 ins = 0, bbls = 0, cond = 0, taken = 0, ijmp = 0, icall = 0, ijmp_top = 0,
           icall_top = 0, calls = 0, rets = 0;
    std::vector<std::pair<UINT64, unsigned> > per_rtn;
    for (unsigned r = 0; r < g_rtns.size(); r++) {
        const RtnInfo &ri = g_rtns[r];
        UINT64 rins = 0;
        for (unsigned b = ri.first_bbl; b < ri.end_bbl; b++) {
            UINT64 c = prof_cnt[2 * (size_t)b];
            if (!c)
                continue;
            const bbl_map_t &bm = bbl_map[b];
            unsigned n = 0;
            for (unsigned i = bm.starting_ins_entry; i <= bm.terminating_ins_entry && i < num_of_instr_map_entries; i++) {
                const instr_map_t &e = instr_map[i];
                if (e.ins_type == ProfilingIns || !e.size ||
                    (e.ins_type == RtnHeadIns && e.xed_category == XED_CATEGORY_WIDENOP))
                    continue;
                n++;
                if (e.xed_category == XED_CATEGORY_CALL && e.orig_targ_addr)
                    calls += c;
                if (e.xed_category == XED_CATEGORY_RET)
                    rets += c;
            }
            rins += c * n;
            bbls += c;
            if (bm.is_cond) {
                UINT64 ft = bm.ft_from_next ? prof_cnt[2 * (size_t)(b + 1)] : prof_cnt[2 * (size_t)b + 1];
                cond += c;
                taken += (c > ft) ? c - ft : 0;
            }
            if (bm.is_indirect && bm.targ_slot) {
                const targ_prof_t &tp = targ_pool[bm.targ_slot - 1];
                UINT64 top = 0;
                for (unsigned j = 0; j <= MAX_TARG_ADDRS; j++)
                    top = std::max(top, (UINT64)tp.s[j].count);
                if (bm.is_indirect_call) { icall += c; icall_top += std::min(top, c); }
                else { ijmp += c; ijmp_top += std::min(top, c); }
            }
        }
        ins += rins;
        if (rins)
            per_rtn.push_back(std::make_pair(rins, r));
    }
    std::sort(per_rtn.rbegin(), per_rtn.rend());
    f << "dynamic app instructions (profiled TC part): " << ins << "\n"
      << "BBL executions: " << bbls << "  avg BBL length: " << (bbls ? (double)ins / bbls : 0) << "\n"
      << "cond branches: " << cond << "  taken: " << taken << " (" << (cond ? 100.0 * taken / cond : 0) << "%)\n"
      << "direct calls: " << calls << "  rets: " << rets << "\n"
      << "indirect jmps: " << ijmp << "  hottest-target share: " << (ijmp ? 100.0 * ijmp_top / ijmp : 0) << "%\n"
      << "indirect calls: " << icall << "  hottest-target share: " << (icall ? 100.0 * icall_top / icall : 0) << "%\n"
      << "routines executed: " << per_rtn.size() << "\n"
      << "rank  cum%  ins%  instructions  routine\n";
    UINT64 cum = 0;
    for (size_t k = 0; k < per_rtn.size(); k++) {
        cum += per_rtn[k].first;
        if (k < 40 || k + 1 == per_rtn.size() || (k < 2000 && (k + 1) % 100 == 0)) {
            PIN_LockClient();
            RTN rtn = RTN_FindByAddress(g_rtns[per_rtn[k].second].app_addr);
            string name = (rtn == RTN_Invalid()) ? string("?") : RTN_Name(rtn);
            PIN_UnlockClient();
            f << k + 1 << "  " << 100.0 * cum / ins << "  " << 100.0 * per_rtn[k].first / ins
              << "  " << per_rtn[k].first << "  " << name << "\n";
        }
    }
}

VOID Fini(INT32 code, VOID* v)
{
    (void)code; (void)v;
    clock_gettime(CLOCK_MONOTONIC, &end_running_time);
    if (KnobRunStats.Value() && !g_tc2_committed) {
      cerr << "create_tc " << g_create_tc_secs << " s" << endl;
      write_run_stats();
    }

    // Course formula only when TC2 was installed; otherwise the real
    // elapsed time since the tool started.
    double elapsed = 0.0;
    if (g_tc2_committed && g_start_time_set) {
      elapsed = (end_running_time.tv_sec - start_running_time.tv_sec) +
                (end_running_time.tv_nsec - start_running_time.tv_nsec) / 1e9 +
                KnobNumSecsDuringProfile.Value();
    } else {
      cerr << "TC2 not installed" << endl;
      elapsed = (end_running_time.tv_sec - g_app_start.tv_sec) +
                (end_running_time.tv_nsec - g_app_start.tv_nsec) / 1e9;
    }
    cerr << " Translated code run (including profiling) took: "
         << elapsed << " seconds\n";
}

/*******************/
/* ExitInProbeMode */
/*******************/
VOID ExitInProbeMode(INT code)
{
    Fini(code, 0);
    (*origExit)(code);
}

/*************/
/* create_tc */
/*************/
VOID create_tc(IMG img, VOID *v)
{
    (void)v;
    // Call Fini when reaching _exit.
    RTN exitRtn = RTN_FindByName(img, "_exit");
    if (RTN_Valid(exitRtn) && RTN_IsSafeForProbedReplacement(exitRtn)) {
      origExit = (EXITFUNCPTR)RTN_ReplaceProbed(exitRtn, AFUNPTR(ExitInProbeMode));
    }

    if (!IMG_IsMainExecutable(img))
      return;

    if (RTN_Valid(exitRtn))
      g_exit_addr = RTN_Address(exitRtn);

    if (KnobDumpOrigCode)
      dump_image_instrs(img);

    int rc = 0;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    rc = allocate_and_init_memory(img);
    if (rc < 0) {
        cerr << "failed to initialize memory for translation\n";
        return;
    }
    VLOG(<< "after memory allocation" << endl);

    rc = find_candidate_rtns_for_tc(img);
    if (rc < 0) {
        cerr << "failed to find candidates for translation\n";
        return;
    }
    VLOG(<< "after identifying candidate routines" << endl);

    if (chain_all_direct_jmp_and_call_target_entries(0, num_of_instr_map_entries) < 0)
        return;
    VLOG(<< "after chaining all branch targets" << endl);

    cur_max_tc_size = max_tc_size / 2;   // TC owns the first half
    rc = set_initial_estimated_new_ins_addrs_in_tc(tc);
    if (rc < 0 ) {
        cerr << "failed to set initial estimated new ins addrs in the TC\n";
        return;
    }

    rc = fix_instructions_displacements();
    if (rc < 0 ) {
        cerr << "failed to fix displacments of translated instructions\n";
        return;
    }
    // -tc_short_br: rel8 branches in TC too (smaller TC in the profile window).
    if (KnobTcShortBr.Value()) {
        shrink_branches();
        if (fix_instructions_displacements() < 0) {
            cerr << "failed to fix displacments after TC branch shortening\n";
            return;
        }
    }
    VLOG(<< "after fixing instructions displacements" << endl);

    rc = copy_instrs_to_tc(tc);
    if (rc < 0 ) {
        cerr << "failed to copy the instructions to the translation cache\n";
        return;
    }
    tc_size = rc;
    VLOG(<< "after write all new instructions to memory tc" << endl);

    if (KnobDumpTranslatedCode) {
       cerr << "Translation Cache dump:" << endl;
       dump_tc(tc, tc_size);
    }

    if (!KnobDoNotCommitTranslatedCode) {
      commit_translated_rtns_to_tc();
      VLOG(<< "after commit of translated routines from orig code to TC" << endl);
    }

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;
    VLOG(<< " create_tc took: " << elapsed << " seconds\n");
    g_create_tc_secs = elapsed;

    clock_gettime(CLOCK_MONOTONIC, &start_running_time);
    g_start_time_set = 1;
    g_tc_ready = 1;
}



/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
INT32 Usage()
{
    cerr << "This tool translated routines of an Intel(R) 64 binary"
         << endl;
    cerr << KNOB_BASE::StringKnobSummary();
    cerr << endl;
    return -1;
}


/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */

int main(int argc, char * argv[])
{
    clock_gettime(CLOCK_MONOTONIC, &g_app_start);
    if (PIN_Init(argc,argv))
        return Usage();

    PIN_InitSymbols();

    if (KnobDumpProfile)
      out = new std::ofstream("bprofile.out");

    IMG_AddInstrumentFunction(create_tc, 0);

    // TC2 is built by an internal thread (safe to spawn from main).
    THREADID tid = PIN_SpawnInternalThread(create_tc2_thread_func, NULL, 0, NULL);
    if (tid == INVALID_THREADID) {
        cerr << "failed to spawn a thread for commit" << endl;
    }

    PIN_StartProgramProbed();

    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
