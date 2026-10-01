/*########################################################################################################*/
// Exercise 4 - bprofile.cpp
//
// Build (inside Pin kit):
//   cp bprofile.cpp makefile.rules $PIN_ROOT/source/tools/SimpleExamples/
//   cd $PIN_ROOT/source/tools/SimpleExamples
//   make obj-intel64/bprofile.so PIN_ROOT=$PIN_ROOT
//
// Run:
//   $PIN_ROOT/pin -t ./bprofile.so -prof_time 2 -- ./sgcc_base.mytest-m64 200.i -o 200.s
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

#include "pin.H"
extern "C" {
#include "xed-interface.h"
}
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <sys/mman.h>
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
#include <utility>
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

KNOB<BOOL>   KnobDoNotCommitTranslatedCode(KNOB_MODE_WRITEONCE,    "pintool",
    "no_tc_commit", "0", "Do not commit translated code");

KNOB<UINT32> KnobNumSecsDuringProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "prof_time", "2", "Number of seconds for collecting BBL counters");

KNOB<BOOL> KnobNoProfile(KNOB_MODE_WRITEONCE,    "pintool",
    "no_prof", "0", "Do not collect profile information");

// Dead-reg stub opt (0 = always spill, 1 = skip dead regs).
KNOB<BOOL> KnobOptDeadRegs(KNOB_MODE_WRITEONCE, "pintool",
    "opt_dead_regs", "1", "Skip save/restore of dead scratch regs/flags in profiling stubs");

// Commit only safe leaf routines (needed for large ex3 binaries).
KNOB<BOOL> KnobConservativeCommit(KNOB_MODE_WRITEONCE, "pintool",
    "conservative_commit", "1",
    "Commit only safe leaf routines (local direct branches OK; no calls/indirects)");

KNOB<UINT32> KnobMaxCommit(KNOB_MODE_WRITEONCE, "pintool",
    "max_commit", "0", "Maximum number of routines to commit (0 = unlimited)");


/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */

// For XED:
#if defined(TARGET_IA32E)
    xed_state_t dstate = {XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b};
#else
    xed_state_t dstate = { XED_MACHINE_MODE_LEGACY_32, XED_ADDRESS_WIDTH_32b};
#endif

//For XED: Pass in the proper length: 15 is the max. But if you do not want to
//cross pages, you can pass less than 15 bytes, of course, the
//instruction might not decode if not enough bytes are provided.
const unsigned max_inst_len = XED_MAX_INSTRUCTION_BYTES;

ADDRINT lowest_sec_addr = 0;
ADDRINT highest_sec_addr = 0;
// Large image => straight-line leaves only; small => local branches OK.
static bool g_large_main_img = false;
#define LARGE_TEXT_SPAN_BYTES (2u * 1024u * 1024u)

// tc containing the new code:
char *tc = nullptr;
unsigned tc_size = 0;
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
    ins_enum_t ins_type;
    char encoded_ins[XED_MAX_INSTRUCTION_BYTES];
    unsigned size;
    int targ_map_entry;
    unsigned bbl_num;
    xed_category_enum_t xed_category;
} instr_map_t;

instr_map_t *instr_map = NULL;
unsigned num_of_instr_map_entries = 0;
unsigned max_ins_count = 0;

#define MAX_TARG_ADDRS 0x3   // keep up to 4 indirect targets (index = addr & 3)
#define MAX_BBL_COUNT  10000  // assignment limit; also avoids OOM
#define MAX_INS_MAP_CAP 300000

// Bbl map of all the bbl exec counters to be collected at runtime:
typedef struct {
  UINT64 counter;                        // BBL execution count
  UINT64 fallthru_counter;               // fall-through count (cond-branch BBLs)
  ADDRINT targ_addr[MAX_TARG_ADDRS+1];   // indirect jump target slots
  UINT64  targ_count[MAX_TARG_ADDRS+1];  // indirect jump target counts
  ADDRINT bbl_addr;                      // orig addr of the first instr in BBL
  bool is_cond;                          // terminates with a conditional branch
  bool is_indirect;                      // terminates with an indirect jump
  unsigned starting_ins_entry;
  unsigned terminating_ins_entry;
} bbl_map_t;

bbl_map_t *bbl_map = nullptr;
unsigned bbl_num = 0;
unsigned max_bbl_count = MAX_BBL_COUNT;
std::map<ADDRINT, unsigned> entry_map;

std::set<ADDRINT> rtn_entry_addrs;              // avoid probe patch clobbering another entry
std::set<ADDRINT> direct_branch_target_addrs;   // avoid probe patch clobbering a landing pad

unsigned max_rtn_count = 0;

struct timespec start_running_time;
static bool g_profile_thread_started = false;
static bool g_exit_hooked = false;
#define MAX_LARGE_IMG_COMMITS 8   // cap commits on huge binaries
#define MAX_LARGE_LEAF_BYTES  64

static UINT64 rax_mem = 0;  // stub spill slots (moffs64)
static UINT64 rbx_mem = 0;
static UINT64 rcx_mem = 0;

// Caller-saved GPRs usable as dead scratch at ret (not RAX).
typedef struct { REG pin; xed_reg_enum_t xed; } scratch_reg_t;
// LEVEL_BASE:: needed — Pin 4 signal.h also defines REG_RAX/... .
static const scratch_reg_t kScratch[] = {
    { LEVEL_BASE::REG_RCX, XED_REG_RCX }, { LEVEL_BASE::REG_RDX, XED_REG_RDX },
    { LEVEL_BASE::REG_RSI, XED_REG_RSI }, { LEVEL_BASE::REG_RDI, XED_REG_RDI },
    { LEVEL_BASE::REG_R8,  XED_REG_R8  }, { LEVEL_BASE::REG_R9,  XED_REG_R9  },
    { LEVEL_BASE::REG_R10, XED_REG_R10 }, { LEVEL_BASE::REG_R11, XED_REG_R11 },
};
static const unsigned kNumScratch = sizeof(kScratch) / sizeof(kScratch[0]);


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

bool isJumpOrRet(INS ins)
{
   if (!INS_IsCall(ins) &&
       (INS_IsIndirectControlFlow(ins) ||
        INS_IsDirectControlFlow(ins) ||
        INS_IsRet(ins)))
     return true;

   return false;
}

bool isBackwardJump(INS ins)
{
  return (!INS_IsCall(ins) && INS_IsDirectControlFlow(ins) &&
          INS_DirectControlFlowTargetAddress(ins) < INS_Address(ins));
}

/* Liveness helpers for -opt_dead_regs (conservative: unknown => live). */
static bool is_caller_saved_gpr(REG reg)
{
    REG f = REG_FullRegName(reg);
    return f == LEVEL_BASE::REG_RAX || f == LEVEL_BASE::REG_RCX ||
           f == LEVEL_BASE::REG_RDX || f == LEVEL_BASE::REG_RSI ||
           f == LEVEL_BASE::REG_RDI || f == LEVEL_BASE::REG_R8  ||
           f == LEVEL_BASE::REG_R9  || f == LEVEL_BASE::REG_R10 ||
           f == LEVEL_BASE::REG_R11;
}

static bool leaves_bbl(INS ins)
{
    return INS_IsRet(ins) || INS_IsCall(ins) || INS_IsBranch(ins) ||
           INS_IsIndirectControlFlow(ins) || INS_IsDirectControlFlow(ins) ||
           INS_IsSyscall(ins);
}

static bool reg_is_dead_at(INS start, REG reg)
{
    if (!INS_Valid(start) || reg == LEVEL_BASE::REG_INVALID())
        return false;

    for (INS ins = start; INS_Valid(ins); ins = INS_Next(ins)) {
        if (INS_RegRContain(ins, reg))
            return false;                 // read before overwrite -> live
        if (INS_RegWContain(ins, reg))
            return true;                  // fully overwritten -> dead
        if (INS_IsRet(ins)) {
            // ret: caller-saved (except RAX) are dead-out.
            return is_caller_saved_gpr(reg) &&
                   REG_FullRegName(reg) != LEVEL_BASE::REG_RAX;
        }
        if (INS_IsCall(ins) || leaves_bbl(ins))
            return false;                 // live-out across the edge is unknown
    }
    return false;                         // fell off end -> assume live
}

static xed_reg_enum_t pick_dead_scratch(INS live_from, xed_reg_enum_t exclude)
{
    for (unsigned i = 0; i < kNumScratch; i++) {
        if (kScratch[i].xed == exclude)
            continue;
        if (reg_is_dead_at(live_from, kScratch[i].pin))
            return kScratch[i].xed;
    }
    return XED_REG_INVALID;
}

static bool rflags_is_dead_at(INS start)
{
    if (!INS_Valid(start))
        return false;

    for (INS ins = start; INS_Valid(ins); ins = INS_Next(ins)) {
        if (INS_RegRContain(ins, LEVEL_BASE::REG_RFLAGS))
            return false;                 // some instr reads flags -> live
        if (INS_IsRet(ins))
            return true;                  // flags not needed after ret
        if (INS_IsCall(ins))
            return true;                  // callee clobbers flags
        if (INS_RegWContain(ins, LEVEL_BASE::REG_RFLAGS))
            return true;                  // flags fully overwritten -> dead
        if (leaves_bbl(ins))
            return false;                 // successor may read flags
    }
    return false;
}


/* Probe-safety helpers (ex3). */
static const unsigned PROBE_JUMP_PATCH_BYTES = 14;

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

static bool has_no_other_rtn_entry_in_probe_patch(RTN rtn)
{
    ADDRINT start = RTN_Address(rtn);
    ADDRINT end = start + PROBE_JUMP_PATCH_BYTES;

    std::set<ADDRINT>::iterator it = rtn_entry_addrs.upper_bound(start);
    return (it == rtn_entry_addrs.end() || *it >= end);
}

static bool has_no_branch_target_inside_probe_patch(RTN rtn)
{
    ADDRINT start = RTN_Address(rtn);
    ADDRINT end = start + PROBE_JUMP_PATCH_BYTES;

    std::set<ADDRINT>::iterator it = direct_branch_target_addrs.upper_bound(start);
    return (it == direct_branch_target_addrs.end() || *it >= end);
}

static bool is_safe_candidate_rtn(RTN rtn)
{
    return RTN_Valid(rtn) &&
           RTN_IsSafeForProbedReplacement(rtn) &&
           has_decodable_probe_header(rtn) &&
           has_no_other_rtn_entry_in_probe_patch(rtn) &&
           has_no_branch_target_inside_probe_patch(rtn);
}

// Safe leaf filter: no call/indirect/RIP/LOOP; local branches only if !large.
static bool is_simple_straight_line_leaf_rtn(RTN rtn)
{
    if (!RTN_Valid(rtn))
        return false;

    if (g_large_main_img && RTN_Size(rtn) > MAX_LARGE_LEAF_BYTES)
        return false;

    const ADDRINT rtn_start = RTN_Address(rtn);
    const ADDRINT rtn_end = rtn_start + RTN_Size(rtn);

    bool ok = true;
    bool saw_ret = false;
    bool has_direct_br = false;
    std::set<ADDRINT> ins_addrs;
    std::vector<ADDRINT> local_branch_targs;
    ADDRINT expect_addr = rtn_start;

    RTN_Open(rtn);

    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
        ADDRINT ins_addr = INS_Address(ins);
        ins_addrs.insert(ins_addr);

        if (ins_addr != expect_addr) {  // gap / data in RTN
            ok = false;
            break;
        }
        expect_addr = ins_addr + INS_Size(ins);

        xed_decoded_inst_t xedd;
        xed_decoded_inst_zero_set_mode(&xedd, &dstate);

        xed_error_enum_t xed_code =
            xed_decode(&xedd, reinterpret_cast<UINT8*>(ins_addr), max_inst_len);
        if (xed_code != XED_ERROR_NONE) {
            ok = false;
            break;
        }

        xed_category_enum_t category_enum = xed_decoded_inst_get_category(&xedd);
        xed_iclass_enum_t iclass = xed_decoded_inst_get_iclass(&xedd);
        xed_iform_enum_t iform = xed_decoded_inst_get_iform_enum(&xedd);

        if (category_enum == XED_CATEGORY_RET) {
            saw_ret = true;
            if (INS_Valid(INS_Next(ins)))
                ok = false;
            break;
        }

        if (category_enum == XED_CATEGORY_CALL || INS_IsCall(ins)) {
            ok = false;
            break;
        }
        if (INS_IsIndirectControlFlow(ins)) {
            ok = false;
            break;
        }

        if (iclass == XED_ICLASS_LOOP || iclass == XED_ICLASS_LOOPE ||
            iclass == XED_ICLASS_LOOPNE || iform == XED_IFORM_JRCXZ_RELBRb) {
            ok = false;
            break;
        }

        if (INS_IsDirectControlFlow(ins)) {
            if (g_large_main_img) {
                ok = false;
                break;
            }
            has_direct_br = true;
            ADDRINT targ = INS_DirectControlFlowTargetAddress(ins);
            if (targ < rtn_start || targ >= rtn_end) {
                ok = false;
                break;
            }
            local_branch_targs.push_back(targ);
        }

        unsigned memops = xed_decoded_inst_number_of_memory_operands(&xedd);
        for (unsigned i = 0; i < memops; i++) {
            if (xed_decoded_inst_get_base_reg(&xedd, i) == XED_REG_RIP) {
                ok = false;
                break;
            }
        }
        if (!ok)
            break;
    }

    RTN_Close(rtn);

    if (!ok || !saw_ret)
        return false;

    if (has_direct_br) {
        for (size_t i = 0; i < local_branch_targs.size(); i++) {
            if (!ins_addrs.count(local_branch_targs[i]))
                return false;
        }
    }
    return true;
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
                  cerr << "0x" << hex << INS_Address(ins) << ": " << INS_Disassemble(ins) << endl;
            RTN_Close( rtn );
            cerr << endl;
        }
    }
}

void dump_instr_from_xedd (xed_decoded_inst_t* xedd, ADDRINT address)
{
    char disasm_buf[2048];
    xed_uint64_t runtime_address = static_cast<UINT64>(address);
    xed_format_context(XED_SYNTAX_INTEL, xedd, disasm_buf, sizeof(disasm_buf),
                       static_cast<UINT64>(runtime_address), 0, 0);
    cerr << hex << address << ": " << disasm_buf <<  endl;
}

void dump_instr_from_mem (ADDRINT *address, ADDRINT new_addr)
{
  char disasm_buf[2048];
  xed_decoded_inst_t new_xedd;
  xed_decoded_inst_zero_set_mode(&new_xedd,&dstate);
  xed_error_enum_t xed_code = xed_decode(&new_xedd, reinterpret_cast<UINT8*>(address), max_inst_len);
  BOOL xed_ok = (xed_code == XED_ERROR_NONE);
  if (!xed_ok)
      cerr << "invalid opcode" << endl;
  xed_format_context(XED_SYNTAX_INTEL, &new_xedd, disasm_buf, 2048, static_cast<UINT64>(new_addr), 0, 0);
  cerr << "0x" << hex << new_addr << ": " << disasm_buf <<  endl;
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


/* edge-profile.csv */
typedef struct {
    ADDRINT addr;
    UINT64  exec;
    UINT64  taken;
    UINT64  fallthru;
    bool    is_cond;
    bool    is_indirect;
    ADDRINT targ_addr[MAX_TARG_ADDRS+1];
    UINT64  targ_count[MAX_TARG_ADDRS+1];
} csv_row_t;

void dump_edge_profile_csv()
{
    std::vector<csv_row_t> rows;

    for (unsigned b = 0; b < bbl_num; b++) {
        if (bbl_map[b].counter == 0 && bbl_map[b].fallthru_counter == 0)
            continue;

        csv_row_t r;
        r.addr = bbl_map[b].bbl_addr;
        r.exec = bbl_map[b].counter;
        r.is_cond = bbl_map[b].is_cond;
        r.is_indirect = bbl_map[b].is_indirect;

        if (r.is_cond) {
            r.fallthru = bbl_map[b].fallthru_counter;
            r.taken = (r.exec >= r.fallthru) ? (r.exec - r.fallthru) : 0;
        } else {
            r.fallthru = 0;
            r.taken = r.exec;   // single successor is always taken
        }

        for (unsigned j = 0; j <= MAX_TARG_ADDRS; j++) {
            r.targ_addr[j]  = bbl_map[b].targ_addr[j];
            r.targ_count[j] = bbl_map[b].targ_count[j];
        }
        rows.push_back(r);
    }

    std::sort(rows.begin(), rows.end(),
              [](const csv_row_t& a, const csv_row_t& b) { return a.exec > b.exec; });  // hottest first

    std::ofstream out("edge-profile.csv");
    if (!out.is_open()) {
        cerr << "failed to open edge-profile.csv for writing" << endl;
        return;
    }

    for (size_t i = 0; i < rows.size(); i++) {
        const csv_row_t& r = rows[i];
        out << "0x" << hex << r.addr << dec
            << ", " << r.exec
            << ", " << r.taken
            << ", " << r.fallthru;

        if (r.is_indirect) {
            std::vector<std::pair<UINT64, ADDRINT> > tgts;
            for (unsigned j = 0; j <= MAX_TARG_ADDRS; j++) {
                if (r.targ_addr[j] || r.targ_count[j])
                    tgts.push_back(std::make_pair(r.targ_count[j], r.targ_addr[j]));
            }
            std::sort(tgts.begin(), tgts.end(),
                      [](const std::pair<UINT64, ADDRINT>& x,
                         const std::pair<UINT64, ADDRINT>& y) { return x.first > y.first; });
            for (size_t j = 0; j < tgts.size(); j++)
                out << ", 0x" << hex << tgts[j].second << dec << ", " << tgts[j].first;
        }

        out << "\n";
    }

    out.close();
    cerr << "wrote edge-profile.csv (" << dec << rows.size() << " BBLs)" << endl;
}


/* ============================================================= */
/* Translation routines                                         */
/* ============================================================= */

/***************************/
/* disable_profiling_in_tc */
/***************************/
// Patch NOP5 -> JMP over each stub (brief pause to reduce torn-patch races).
int disable_profiling_in_tc(instr_map_t * instr_map, unsigned num_of_instr_map_entries)
{
    usleep(50000);

    for (unsigned i = 0; i < num_of_instr_map_entries; i++) {
        if (instr_map[i].ins_type == ProfilingIns &&
            instr_map[i].xed_category == XED_CATEGORY_WIDENOP) {

            unsigned j = 1;
            xed_int64_t disp = 0;
            while (instr_map[i+j].ins_type == ProfilingIns) {
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

          memcpy((ADDRINT *)instr_map[i].new_ins_addr, encoded_jmp_ins, olen);
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

    xed_uint_t disp_byts = xed_decoded_inst_get_branch_displacement_width(xedd);
    xed_int32_t disp;
    if (disp_byts > 0) {
      disp = xed_decoded_inst_get_branch_displacement(xedd);
      orig_targ_addr = pc + xed_decoded_inst_get_length (xedd) + disp;
    }

    ADDRINT orig_rip_addr = 0x0;
    unsigned memops = xed_decoded_inst_number_of_memory_operands(xedd);
    if (memops) {
      xed_reg_enum_t base_reg = xed_decoded_inst_get_base_reg(xedd, 0);
      if (base_reg == XED_REG_RIP) {
         unsigned size = xed_decoded_inst_get_length (xedd);
         xed_int64_t rdisp = xed_decoded_inst_get_memory_displacement(xedd, 0);
         orig_rip_addr = (ADDRINT)(pc + rdisp + size);
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

    instr_map[num_of_instr_map_entries].orig_ins_addr = pc;
    instr_map[num_of_instr_map_entries].new_ins_addr = 0x0;
    instr_map[num_of_instr_map_entries].orig_targ_addr = orig_targ_addr;
    instr_map[num_of_instr_map_entries].orig_rip_addr = orig_rip_addr;
    instr_map[num_of_instr_map_entries].targ_map_entry = -1;
    instr_map[num_of_instr_map_entries].size = new_size;
    instr_map[num_of_instr_map_entries].ins_type = ins_type;
    instr_map[num_of_instr_map_entries].bbl_num = bbl_num;
    instr_map[num_of_instr_map_entries].xed_category = xed_decoded_inst_get_category(xedd);

    num_of_instr_map_entries++;

    if (num_of_instr_map_entries >= max_ins_count) {
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

static int emit_prof(ADDRINT ins_addr, xed_encoder_instruction_t *enc_instr) {
    return add_new_encoded_instr(ins_addr, enc_instr, ProfilingIns);
}

// Absolute [disp32] via SIB (plain INVALID base is RIP-relative in 64-bit).
static xed_encoder_operand_t abs32_mem(ADDRINT addr) {
    return xed_mem_bisd(XED_REG_INVALID, XED_REG_INVALID, 0,
                        xed_disp((xed_uint64_t)addr, 32), 64);
}

static bool addr_in_low32(ADDRINT p) {
    return p <= (ADDRINT)0x7FFFFFFFu;
}

// Spill/reload via moffs64 (no stack / red-zone).
static int emit_spill_rax(ADDRINT ins_addr) {
    xed_encoder_instruction_t enc;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rax_mem, 64), 64),
              xed_reg(XED_REG_RAX));
    return emit_prof(ins_addr, &enc);
}
static int emit_reload_rax(ADDRINT ins_addr) {
    xed_encoder_instruction_t enc;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rax_mem, 64), 64));
    return emit_prof(ins_addr, &enc);
}
static int emit_spill_rbx_via_rax(ADDRINT ins_addr) {
    xed_encoder_instruction_t enc;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX), xed_reg(XED_REG_RBX));
    if (emit_prof(ins_addr, &enc) < 0) return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rbx_mem, 64), 64),
              xed_reg(XED_REG_RAX));
    return emit_prof(ins_addr, &enc);
}
static int emit_spill_rcx_via_rax(ADDRINT ins_addr) {
    xed_encoder_instruction_t enc;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX), xed_reg(XED_REG_RCX));
    if (emit_prof(ins_addr, &enc) < 0) return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rcx_mem, 64), 64),
              xed_reg(XED_REG_RAX));
    return emit_prof(ins_addr, &enc);
}
static int emit_reload_rcx_via_rax(ADDRINT ins_addr) {
    xed_encoder_instruction_t enc;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rcx_mem, 64), 64));
    if (emit_prof(ins_addr, &enc) < 0) return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RCX), xed_reg(XED_REG_RAX));
    return emit_prof(ins_addr, &enc);
}
static int emit_reload_rbx_via_rax(ADDRINT ins_addr) {
    xed_encoder_instruction_t enc;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_INVALID, xed_disp((ADDRINT)&rbx_mem, 64), 64));
    if (emit_prof(ins_addr, &enc) < 0) return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RBX), xed_reg(XED_REG_RAX));
    return emit_prof(ins_addr, &enc);
}

// Flag-safe counter++ via RAX + LEA.
static int emit_counter_via_rax(ADDRINT ins_addr, ADDRINT cnt) {
    xed_encoder_instruction_t enc;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_INVALID, xed_disp(cnt, 64), 64));
    if (emit_prof(ins_addr, &enc) < 0) return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_LEA, 64,
              xed_reg(XED_REG_RAX),
              xed_mem_bd(XED_REG_RAX, xed_disp(1, 8), 64));
    if (emit_prof(ins_addr, &enc) < 0) return -1;
    xed_inst2(&enc, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bd(XED_REG_INVALID, xed_disp(cnt, 64), 64),
              xed_reg(XED_REG_RAX));
    return emit_prof(ins_addr, &enc);
}

/**************************/
/* add_profiling_instrs() */
/**************************/
// opt=0: always spill RAX/RBX/RCX. opt=1: skip dead; INC when possible.
int add_profiling_instrs(INS term_ins, INS live_from, ADDRINT ins_addr,
                         UINT64 *counter_addr, unsigned bbl_idx,
                         bool is_indirect_bbl)
{
  xed_encoder_instruction_t enc_instr;
  const bool opt = KnobOptDeadRegs.Value();
  const ADDRINT cnt = (ADDRINT)counter_addr;

  xed_inst0(&enc_instr, dstate, XED_ICLASS_NOP5, 64);  // disable patches this
  if (emit_prof(ins_addr, &enc_instr) < 0) return -1;

  /* ---- indirect jump target recording ---- */
  if (is_indirect_bbl) {
    bool save_rax = !opt || !reg_is_dead_at(live_from, LEVEL_BASE::REG_RAX);
    bool save_rbx = !opt || !reg_is_dead_at(live_from, LEVEL_BASE::REG_RBX);
    bool save_rcx = !opt || !reg_is_dead_at(live_from, LEVEL_BASE::REG_RCX);

    if (save_rax && emit_spill_rax(ins_addr) < 0) return -1;

    xed_decoded_inst_t *xedd = INS_XedDec(term_ins);
    xed_reg_enum_t base_reg = xed_decoded_inst_get_base_reg(xedd, 0);
    xed_reg_enum_t index_reg = xed_decoded_inst_get_index_reg(xedd, 0);
    xed_int64_t disp = xed_decoded_inst_get_memory_displacement(xedd, 0);
    xed_uint_t scale = xed_decoded_inst_get_scale(xedd, 0);
    xed_uint_t width = xed_decoded_inst_get_memory_displacement_width_bits(xedd, 0);
    unsigned mem_addr_width = xed_decoded_inst_get_memop_address_width(xedd, 0);

    xed_reg_enum_t targ_reg = XED_REG_INVALID;
    unsigned memops = xed_decoded_inst_number_of_memory_operands(xedd);
    if (!memops)
      targ_reg = xed_decoded_inst_get_reg(xedd, XED_OPERAND_REG0);

    if (save_rbx && emit_spill_rbx_via_rax(ins_addr) < 0) return -1;
    if (save_rcx && emit_spill_rcx_via_rax(ins_addr) < 0) return -1;

    if (targ_reg == XED_REG_RAX || base_reg == XED_REG_RAX || index_reg == XED_REG_RAX) {
      if (emit_reload_rax(ins_addr) < 0) return -1;
    }

    if (base_reg == XED_REG_RIP) {
      unsigned int orig_size = xed_decoded_inst_get_length(xedd);
      xed_int64_t new_disp = ins_addr + disp + orig_size;
      if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
         cerr << "Invalid rip displacement larger than 32 bits in add_profiling_instrs\n";
         return -1;
      }
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_reg(XED_REG_RAX),
                xed_mem_bisd(XED_REG_INVALID, index_reg, scale,
                             xed_disp(new_disp, 32), mem_addr_width));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
    } else if (targ_reg != XED_REG_RAX) {
        xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                 xed_reg(XED_REG_RAX),
                 (targ_reg != XED_REG_INVALID ? xed_reg(targ_reg) :
                  xed_mem_bisd(base_reg, index_reg, scale, xed_disp(disp, width), mem_addr_width)));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
    }

    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RBX), xed_reg(XED_REG_RAX));
    if (emit_prof(ins_addr, &enc_instr) < 0) return -1;

    xed_inst2(&enc_instr, dstate, XED_ICLASS_AND, 64,
              xed_reg(XED_REG_RAX), xed_imm0(MAX_TARG_ADDRS, 8));
    if (emit_prof(ins_addr, &enc_instr) < 0) return -1;

    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RCX),
              xed_imm0((ADDRINT)&(bbl_map[bbl_idx].targ_addr[0]), 64));
    if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bisd(XED_REG_RCX, XED_REG_RAX, 8, xed_disp(0, 32), 64),
              xed_reg(XED_REG_RBX));
    if (emit_prof(ins_addr, &enc_instr) < 0) return -1;

    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RBX),
              xed_imm0((ADDRINT)&(bbl_map[bbl_idx].targ_count[0]), 64));
    if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_reg(XED_REG_RCX),
              xed_mem_bisd(XED_REG_RBX, XED_REG_RAX, 8, xed_disp(0, 32), 64));
    if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
    xed_inst2(&enc_instr, dstate, XED_ICLASS_LEA, 64,
              xed_reg(XED_REG_RCX),
              xed_mem_bd(XED_REG_RCX, xed_disp(1, 8), 64));
    if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
    xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
              xed_mem_bisd(XED_REG_RBX, XED_REG_RAX, 8, xed_disp(0, 32), 64),
              xed_reg(XED_REG_RCX));
    if (emit_prof(ins_addr, &enc_instr) < 0) return -1;

    if (save_rcx && emit_reload_rcx_via_rax(ins_addr) < 0) return -1;
    if (save_rbx && emit_reload_rbx_via_rax(ins_addr) < 0) return -1;

    if (opt && rflags_is_dead_at(live_from) && addr_in_low32(cnt)) {
      xed_inst1(&enc_instr, dstate, XED_ICLASS_INC, 64, abs32_mem(cnt));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
    } else {
      if (emit_counter_via_rax(ins_addr, cnt) < 0) return -1;
    }

    if (save_rax && emit_reload_rax(ins_addr) < 0) return -1;
    return 0;
  }

  /* ---- non-indirect BBL / fall-through counter ---- */
  if (opt) {
    bool rflags_dead = rflags_is_dead_at(live_from);
    xed_reg_enum_t sa = pick_dead_scratch(live_from, XED_REG_INVALID);
    xed_reg_enum_t sv = pick_dead_scratch(live_from, sa);
    bool rax_dead = reg_is_dead_at(live_from, LEVEL_BASE::REG_RAX);

    if (rflags_dead && addr_in_low32(cnt)) {
      xed_inst1(&enc_instr, dstate, XED_ICLASS_INC, 64, abs32_mem(cnt));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
      return 0;
    }

    if (sa != XED_REG_INVALID && rflags_dead) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_reg(sa), xed_imm0(cnt, 64));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
      xed_inst1(&enc_instr, dstate, XED_ICLASS_INC, 64,
                xed_mem_bd(sa, xed_disp(0, 8), 64));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
      return 0;
    }

    if (sa != XED_REG_INVALID && sv != XED_REG_INVALID) {
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_reg(sa), xed_imm0(cnt, 64));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_reg(sv), xed_mem_bd(sa, xed_disp(0, 8), 64));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_LEA, 64,
                xed_reg(sv), xed_mem_bd(sv, xed_disp(1, 8), 64));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
      xed_inst2(&enc_instr, dstate, XED_ICLASS_MOV, 64,
                xed_mem_bd(sa, xed_disp(0, 8), 64), xed_reg(sv));
      if (emit_prof(ins_addr, &enc_instr) < 0) return -1;
      return 0;
    }

    if (rax_dead) {
      if (emit_counter_via_rax(ins_addr, cnt) < 0) return -1;
      return 0;
    }

    if (emit_spill_rax(ins_addr) < 0) return -1;  // opt fallback
    if (emit_counter_via_rax(ins_addr, cnt) < 0) return -1;
    if (emit_reload_rax(ins_addr) < 0) return -1;
    return 0;
  }

  // naive: always spill RAX+RBX+RCX
  if (emit_spill_rax(ins_addr) < 0) return -1;
  if (emit_spill_rbx_via_rax(ins_addr) < 0) return -1;
  if (emit_spill_rcx_via_rax(ins_addr) < 0) return -1;
  if (emit_counter_via_rax(ins_addr, cnt) < 0) return -1;
  if (emit_reload_rcx_via_rax(ins_addr) < 0) return -1;
  if (emit_reload_rbx_via_rax(ins_addr) < 0) return -1;
  if (emit_reload_rax(ins_addr) < 0) return -1;

  return 0;
}

/**************************************************/
/* chain_all_direct_jmp_and_call_target_entries() */
/**************************************************/
void chain_all_direct_jmp_and_call_target_entries(unsigned from_entry,
                                                 unsigned until_entry)
{
    entry_map.clear();

    for (unsigned i = from_entry; i < until_entry; i++) {
        instr_map[i].targ_map_entry = -1;
        ADDRINT orig_ins_addr = instr_map[i].orig_ins_addr;
        if (!orig_ins_addr)
          continue;
        entry_map.emplace(orig_ins_addr, i);
    }

    for (unsigned i = from_entry; i < until_entry; i++) {
        ADDRINT orig_targ_addr = instr_map[i].orig_targ_addr;
        if (orig_targ_addr == 0)
            continue;
        if (instr_map[i].targ_map_entry >= 0)
            continue;
        if (!entry_map.count(orig_targ_addr))
            continue;
        if (!instr_map[i].size)
            continue;
        instr_map[i].targ_map_entry = entry_map[orig_targ_addr];
    }
}

/***********************************************/
/* set_initial_estimated_new_ins_addrs_in_tc() */
/***********************************************/
int set_initial_estimated_new_ins_addrs_in_tc(char *tc) {
  unsigned tc_cursor = 0;
  for (unsigned i=0; i < num_of_instr_map_entries; i++) {
    instr_map[i].new_ins_addr = (ADDRINT)&tc[tc_cursor];
    tc_cursor += instr_map[i].size;
    if (tc_cursor >= max_tc_size)
      return -1;
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

    xed_int64_t new_disp = 0;
    xed_uint_t new_disp_byts = 4;

    new_disp = (xed_int64_t)(instr_map[instr_map_entry].orig_rip_addr - instr_map[instr_map_entry].new_ins_addr -
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

    return new_size;
}

/**************************************/
/* fix_direct_jmp_or_call_to_orig_addr */
/**************************************/
int fix_direct_jmp_or_call_to_orig_addr(unsigned instr_map_entry)
{
    if (!instr_map[instr_map_entry].size)
      return 0;

    if (instr_map[instr_map_entry].targ_map_entry >= 0) {
        cerr << "ERROR: Invalid jump or call instruction" << endl;
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

    // Direct rel32 back to original (probe jump or orig code).
    xed_int64_t new_disp =
        (xed_int64_t)instr_map[instr_map_entry].orig_targ_addr -
        (xed_int64_t)instr_map[instr_map_entry].new_ins_addr -
        (xed_int64_t)instr_map[instr_map_entry].size;

    xed_uint_t new_disp_byts = 4;
    xed_iclass_enum_t iclass_enum = xed_decoded_inst_get_iclass(&xedd);
    xed_iform_enum_t iform_enum = xed_decoded_inst_get_iform_enum(&xedd);

    if (iclass_enum == XED_ICLASS_LOOP ||
        iclass_enum == XED_ICLASS_LOOPE ||
        iclass_enum == XED_ICLASS_LOOPNE ||
        iform_enum == XED_IFORM_JRCXZ_RELBRb) {
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

    return olen;
}

/**************************************/
/* fix_direct_jmp_or_call_displacement */
/**************************************/
int fix_direct_jmp_or_call_displacement(unsigned instr_map_entry)
{
    if (!instr_map[instr_map_entry].size)
        return 0;
    if (instr_map[instr_map_entry].orig_targ_addr == 0)
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

    ADDRINT new_targ_addr = instr_map[instr_map[instr_map_entry].targ_map_entry].new_ins_addr;
    new_disp =
      (new_targ_addr - instr_map[instr_map_entry].new_ins_addr) - instr_map[instr_map_entry].size;
     if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
        cerr << "Invalid rip displacement larger than 32 bits in fix_direct_jmp_or_call_displacement\n";
        return -1;
    }

    xed_uint_t   new_disp_byts = 4;
    xed_iclass_enum_t iclass_enum = xed_decoded_inst_get_iclass(&xedd);
    if (iclass_enum == XED_ICLASS_LOOP ||
        iclass_enum == XED_ICLASS_LOOPE ||
        iclass_enum == XED_ICLASS_LOOPNE) {
      new_disp_byts = 1;
    }
    xed_iform_enum_t iform_enum = xed_decoded_inst_get_iform_enum (&xedd);
    if (iform_enum == XED_IFORM_JRCXZ_RELBRb){
      new_disp_byts = 1;
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

    return new_size;
}

/************************************/
/* fix_instructions_displacements() */
/************************************/
int fix_instructions_displacements()
{
    int size_diff = 0;
    bool is_diff = false;

    do {
        size_diff = 0;
        is_diff = false;

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

            new_size = fix_direct_jmp_or_call_displacement(i);
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
    } while (is_diff);

   return 0;
}

/**********************************************/
/* collect_all_direct_branch_targets_in_image() */
/**********************************************/
void collect_all_direct_branch_targets_in_image(IMG img)
{
    direct_branch_target_addrs.clear();

    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec)) {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;

        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn)) {
            RTN_Open(rtn);
            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
                ADDRINT ins_addr = INS_Address(ins);

                xed_decoded_inst_t xedd;
                xed_decoded_inst_zero_set_mode(&xedd, &dstate);
                xed_error_enum_t xed_code =
                    xed_decode(&xedd, reinterpret_cast<UINT8*>(ins_addr), max_inst_len);
                if (xed_code != XED_ERROR_NONE)
                    continue;

                xed_uint_t disp_byts =
                    xed_decoded_inst_get_branch_displacement_width(&xedd);
                if (disp_byts == 0)
                    continue;

                xed_int32_t disp = xed_decoded_inst_get_branch_displacement(&xedd);
                ADDRINT target = ins_addr + xed_decoded_inst_get_length(&xedd) + disp;

                if (target >= lowest_sec_addr && target < highest_sec_addr)
                    direct_branch_target_addrs.insert(target);
            }
            RTN_Close(rtn);
        }
    }
}

/********************************/
/* find_candidate_rtns_for_tc() */
/********************************/
int find_candidate_rtns_for_tc(IMG img)
{
    int rc = 0;
    unsigned translated_rtns = 0;

    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;

        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            unsigned rtn_entry = num_of_instr_map_entries;

            if (RTN_Name(rtn) == "_exit")
                continue;

            if (!is_safe_candidate_rtn(rtn))
                continue;

            if (KnobConservativeCommit.Value() &&
                !is_simple_straight_line_leaf_rtn(rtn))
                continue;

            if (g_large_main_img && translated_rtns >= MAX_LARGE_IMG_COMMITS)
                continue;
            if (KnobMaxCommit.Value() != 0 && translated_rtns >= KnobMaxCommit.Value())
                continue;

            RTN_Open( rtn );

            std::map<ADDRINT, bool> is_targ_map;
            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
               if (INS_IsDirectControlFlow(ins))
                 is_targ_map[INS_DirectControlFlowTargetAddress(ins)] = true;
            }

            bool rtn_head_emitted = false;

            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {

                ADDRINT ins_addr = INS_Address(ins);

                if (!rtn_head_emitted) {  // NOP landing pad before stubs
                  xed_encoder_instruction_t nop_enc;
                  xed_inst0(&nop_enc, dstate, XED_ICLASS_NOP, 64);
                  if (add_new_encoded_instr(RTN_Address(rtn), &nop_enc, RtnHeadIns) < 0) {
                    RTN_Close(rtn);
                    return -1;
                  }
                  rtn_head_emitted = true;
                }

                if (bbl_map[bbl_num].bbl_addr == 0)
                    bbl_map[bbl_num].bbl_addr = ins_addr;

                INS next_ins = INS_Next(ins);
                bool isNextInsJumpTarget =
                    (!INS_Valid(next_ins) ? false : is_targ_map[INS_Address(next_ins)]);
                bool isInsTerminatesBBL = (isJumpOrRet(ins) || isNextInsJumpTarget);
                bool isIndirect = (INS_IsIndirectControlFlow(ins) &&
                                   !INS_IsRet(ins) && !INS_IsCall(ins));

                if (!KnobNoProfile && isInsTerminatesBBL) {
                    rc = add_profiling_instrs(ins, ins, ins_addr,
                                              &bbl_map[bbl_num].counter,
                                              bbl_num, isIndirect);
                    if (rc < 0) { RTN_Close(rtn); return -1; }
                }

                xed_decoded_inst_t xedd;
                xed_decoded_inst_zero_set_mode(&xedd,&dstate);
                xed_error_enum_t xed_code =
                    xed_decode(&xedd, reinterpret_cast<UINT8*>(ins_addr), max_inst_len);
                if (xed_code != XED_ERROR_NONE) {
                    cerr << "ERROR: xed decode failed for instr at: " << "0x" << hex << ins_addr << endl;
                    RTN_Close(rtn);
                    return -1;
                }
                rc = add_new_instr_entry(&xedd, ins_addr, RegularIns);
                if (rc < 0) {
                    cerr << "ERROR: failed during instructon translation." << endl;
                    RTN_Close(rtn);
                    return -1;
                }

                if (isInsTerminatesBBL) {
                    bbl_map[bbl_num].terminating_ins_entry = num_of_instr_map_entries - 1;
                    bbl_map[bbl_num].is_cond = (INS_Category(ins) == XED_CATEGORY_COND_BR);
                    bbl_map[bbl_num].is_indirect = isIndirect;
                    bbl_num++;
                    if (bbl_num >= max_bbl_count) {
                        cerr << "exceeded MAX_BBL_COUNT (" << dec << max_bbl_count << ")\n";
                        RTN_Close(rtn);
                        return -1;
                    }
                    bbl_map[bbl_num].starting_ins_entry = num_of_instr_map_entries;
                }

                // Fall-through edge counter (after conditional branch).
                if (!KnobNoProfile && INS_Category(ins) == XED_CATEGORY_COND_BR) {
                    INS live_from = INS_Valid(next_ins) ? next_ins : ins;
                    rc = add_profiling_instrs(ins, live_from, ins_addr,
                                              &bbl_map[bbl_num - 1].fallthru_counter,
                                              bbl_num - 1, false);
                    if (rc < 0) { RTN_Close(rtn); return -1; }
                }

            } // end for INS...

            RTN_Close( rtn );

            // Local chaining only (cross-rtn stays as jump-to-orig).
            chain_all_direct_jmp_and_call_target_entries(rtn_entry, num_of_instr_map_entries);
            translated_rtns++;

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
inline void commit_translated_rtns_to_tc()
{
    UINT32 committed_count = 0;

    for (unsigned i=0; i < num_of_instr_map_entries; i++) {

        if (instr_map[i].ins_type != RtnHeadIns)
          continue;

        RTN rtn = RTN_FindByAddress(instr_map[i].orig_ins_addr);
        if (rtn == RTN_Invalid()) {
           if (KnobVerbose)
             cerr << "invalid rtN for commit for addr: 0x" << instr_map[i].orig_ins_addr << "\n";
           continue;
        }

        if (!RTN_IsSafeForProbedReplacement(rtn) ||
            !has_no_other_rtn_entry_in_probe_patch(rtn) ||
            !has_no_branch_target_inside_probe_patch(rtn))
            continue;

        if (KnobConservativeCommit.Value() &&
            !is_simple_straight_line_leaf_rtn(rtn))
            continue;

        if (KnobMaxCommit.Value() != 0 && committed_count >= KnobMaxCommit.Value())
            continue;
        if (g_large_main_img && committed_count >= MAX_LARGE_IMG_COMMITS)
            continue;

        AFUNPTR origFptr = RTN_ReplaceProbed(rtn, (AFUNPTR)instr_map[i].new_ins_addr);
        if (origFptr == NULL) {
            if (KnobVerbose) {
                cerr << "RTN_ReplaceProbed failed for: " << RTN_Name(rtn)
                     << " at 0x" << hex << RTN_Address(rtn) << endl;
            }
            continue;
        }

        committed_count++;
    }

    cerr << "committed " << dec << committed_count << " routines to TC" << endl;
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

        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            rtn_entry_addrs.insert(RTN_Address(rtn));
            if (highest_addr < RTN_Address(rtn) + RTN_Size(rtn))
                highest_addr = RTN_Address(rtn) + RTN_Size(rtn);
            max_rtn_count++;
            max_ins_count += RTN_NumIns  (rtn);
        }
    }

    {
        ADDRINT span = (highest_sec_addr > lowest_sec_addr)
                           ? (highest_sec_addr - lowest_sec_addr) : 0;
        g_large_main_img = (span >= LARGE_TEXT_SPAN_BYTES);
        cerr << " text span=" << dec << span
             << " large_img=" << (g_large_main_img ? 1 : 0)
             << " (branched leaves " << (g_large_main_img ? "OFF" : "ON") << ")\n";
    }

    // Cap maps — full size*12 OOMs large GCC binaries.
    max_ins_count *= 5;
    if (max_ins_count > MAX_INS_MAP_CAP)
        max_ins_count = MAX_INS_MAP_CAP;
    if (max_ins_count < 10000)
        max_ins_count = 10000;

    int pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize == -1) {
      perror("sysconf");
      return -1;
    }

    ADDRINT text_size = (highest_sec_addr - lowest_sec_addr) * 2 + pagesize * 4;

    max_tc_size = 4 * text_size + pagesize * 4;
    if (max_tc_size > 256 * 1024 * 1024u)
        max_tc_size = 256 * 1024 * 1024u;
    if (max_tc_size >= 0x7FFFFFFF) {
      cerr << "size of TC is beyond the range of a branch displacement" << endl;
      return -1;
    }

    const size_t mem_size =
              max_tc_size +
              max_rtn_count * sizeof(ADDRINT);  // small trailing slack
    char *addr = nullptr;
    ADDRINT max_distance = 0x7FFFFFFF;
    const size_t step = pagesize;
    ADDRINT aligned_target = ((ADDRINT)highest_addr) & ~(pagesize - 1);

    void* result = mmap((void*)aligned_target, mem_size,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    if (result != MAP_FAILED &&
        (abs((long)((ADDRINT)result - aligned_target)) <= (long)max_distance)) {
        addr = (char *)result;
    }

    if (!addr) {
        for (size_t offset = step; offset <= max_distance; offset += step) {
            ADDRINT try_addr = aligned_target + offset;
            result = mmap((void*)try_addr, mem_size,
                         PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
            if (result != MAP_FAILED &&
                (abs((long)((ADDRINT)result - try_addr)) <= (long)max_distance)) {
                addr = (char *)result;
                break;
            }
            if (result != MAP_FAILED) munmap(result, mem_size);

            if (highest_addr >= offset) {
                try_addr = aligned_target - offset;
                result = mmap((void*)try_addr, mem_size,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
                if (result != MAP_FAILED &&
                    (abs((long)((ADDRINT)result - try_addr)) <= (long)max_distance)) {
                    addr = (char *)result;
                    break;
                }
                if (result != MAP_FAILED) munmap(result, mem_size);
            }
        }
    }

    if (!addr) {
        cerr << "failed to allocate memory within 32-bit range. " << endl;
        return -1;
    }

    cerr << " allocated memory at: 0x" << hex << (ADDRINT)addr
         << " max_ins_count=" << dec << max_ins_count
         << " max_tc_size=" << max_tc_size << "\n";

    tc = (char *)addr;

    instr_map = (instr_map_t *)calloc(max_ins_count, sizeof(instr_map_t));
    if (instr_map == NULL) {
        perror("calloc instr_map");
        return -1;
    }

    max_bbl_count = MAX_BBL_COUNT;
    {
        size_t bbl_bytes = (size_t)(max_bbl_count + 1) * sizeof(bbl_map_t);
        void *p = mmap(NULL, bbl_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED) {
            const ADDRINT hints[] = {
                0x10000000ul, 0x20000000ul, 0x30000000ul, 0x40000000ul
            };
            for (unsigned h = 0; h < sizeof(hints)/sizeof(hints[0]); h++) {
                p = mmap((void *)hints[h], bbl_bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p != MAP_FAILED && (ADDRINT)p <= 0x7FFFFFFFul)
                    break;
                if (p != MAP_FAILED) {
                    munmap(p, bbl_bytes);
                    p = MAP_FAILED;
                }
            }
        }
        if (p == MAP_FAILED) {
            cerr << "low32 bbl_map mmap failed; falling back to calloc\n";
            bbl_map = (bbl_map_t *)calloc(max_bbl_count + 1, sizeof(bbl_map_t));
        } else {
            memset(p, 0, bbl_bytes);
            bbl_map = (bbl_map_t *)p;
            cerr << " bbl_map at: 0x" << hex << (ADDRINT)bbl_map << "\n";
        }
    }
    if (bbl_map == NULL) {
        perror("bbl_map alloc");
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
VOID Fini(INT32 code, VOID* v)
{
    (void)code; (void)v;
    cerr << "Reached _exit - writing edge-profile.csv" << endl;
    if (bbl_map)
        dump_edge_profile_csv();
}

/*******************/
/* ExitInProbeMode */
/*******************/
VOID ExitInProbeMode(INT code)
{
    Fini(code, 0);
    (*origExit)(code);
}

/**********************************************/
/* start_stop_profile_gathering_thread_func() */
/**********************************************/
VOID start_stop_profile_gathering_thread_func(VOID *v)
{
    cerr << " prof time: " << dec << KnobNumSecsDuringProfile.Value() << " sec\n";
    sleep(KnobNumSecsDuringProfile.Value());

    cerr << "disabling profile gathering\n";
    disable_profiling_in_tc(instr_map, num_of_instr_map_entries);
}

/*************/
/* create_tc */
/*************/
VOID create_tc(IMG img, VOID *v)
{
    if (!g_exit_hooked) {  // flush CSV on _exit
      RTN exitRtn = RTN_FindByName(img, "_exit");
      if (RTN_Valid(exitRtn) && RTN_IsSafeForProbedReplacement(exitRtn)) {
        origExit = (EXITFUNCPTR)RTN_ReplaceProbed(exitRtn, AFUNPTR(ExitInProbeMode));
        g_exit_hooked = true;
      }
    }

    if (!IMG_IsMainExecutable(img))
      return;

    if (KnobDumpOrigCode)
      dump_image_instrs(img);

    int rc = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    rc = allocate_and_init_memory(img);
    if (rc < 0) { cerr << "failed to initialize memory for translation\n"; return; }
    cerr << "after memory allocation" << endl;

    collect_all_direct_branch_targets_in_image(img);
    cerr << "after collecting branch targets" << endl;

    rc = find_candidate_rtns_for_tc(img);
    if (rc < 0) { cerr << "failed to find candidates for translation\n"; return; }
    cerr << "after identifying candidate routines" << endl;

    rc = set_initial_estimated_new_ins_addrs_in_tc(tc);
    if (rc < 0) { cerr << "failed to set initial estimated new ins addrs in the TC\n"; return; }
    cerr << "after setting initial estimated new ins addrs in the TC" << endl;

    rc = fix_instructions_displacements();
    if (rc < 0) { cerr << "failed to fix displacments of translated instructions\n"; return; }
    cerr << "after fixing instructions displacements" << endl;

    rc = copy_instrs_to_tc(tc);
    if (rc < 0) { cerr << "failed to copy the instructions to the translation cache\n"; return; }
    tc_size = rc;
    cerr << "after write all new instructions to memory tc" << endl;

    if (KnobDumpTranslatedCode) {
       cerr << "Translation Cache dump:" << endl;
       dump_tc(tc, tc_size);
    }

    if (!KnobDoNotCommitTranslatedCode) {
      commit_translated_rtns_to_tc();
      cerr << "after commit of translated routines from orig code to TC" << endl;
    }

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    cerr << " create_tc took: " << elapsed << " seconds\n";

    clock_gettime(CLOCK_MONOTONIC, &start_running_time);

    // Start prof window only after TC commit.
    if (!KnobNoProfile && !g_profile_thread_started) {
        THREADID tid = PIN_SpawnInternalThread(start_stop_profile_gathering_thread_func,
                                               NULL, 0, NULL);
        if (tid == INVALID_THREADID)
            cerr << "failed to spawn profiling-window thread" << endl;
        else
            g_profile_thread_started = true;
    }
}


/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */
INT32 Usage()
{
    cerr << "This tool translates routines of an Intel(R) 64 binary and profiles their BBLs"
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
    if( PIN_Init(argc,argv) )
        return Usage();

    PIN_InitSymbols();

    IMG_AddInstrumentFunction(create_tc, 0);

    PIN_StartProgramProbed();

    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */
