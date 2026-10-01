/*########################################################################################################*/
// cd /nfs/iil/ptl/bt/ghaber1/pin/pin-2.10-45467-gcc.3.4.6-ia32_intel64-linux/source/tools/SimpleExamples
// make obj-intel64/btranslate.so
//  <pindir>/pin -t obj-intel64/btranslate.so -- sgcc_base.mytest-m64 200.i -o 200.s
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

/* ===================================================================== */
/*! @file
 * This probe pintool generates translated code of routines, places them in an allocated TC
 * and patches the orginal code the header of each routine to jump to its translated routine.
 *
 * Here are the steps applied by the pintool in order to generate the translated code:
 *
 * Step 1: allocate required memory:
 * - Perform a calculation of the size of the executable sections and code.
 * - Call mmap in a loop until it manages to allocate an area located close enough to the
 *   original code location.
 * Step 2: find candidate routines:
 * - Analyze to find candidate routines for translation and copy analysis content into an
 *   appropriate Internal Representation (IR).
 * Step 3: chaining � resolve targets of direct branches and direct calls in the IR:
 * - Go over the target of each direct jump/call in the IR table and add a pointer
 *   to the corresponding instruction target entry in the IR.
 * Step 4: Set initial estimated new addrs for each instruction in the TC:
 * - Calculate and set the new addrs to each instruction in the TC.
 * - The new addrs may change after branch fixups.
 * Step 5: fix encoding of translated direct branch and call offsets in the routine:
 * - fix all rip-based, direct branch and direct call displacements
 * - May require several iterations until the displacements of all long and short branches
 *   are resolved
 * - Need to have an option to rollback when routine cannot be translated
 * Step 6: write translated routines to the TC.
 * - Apply memcopy of each encoded instruction in the IR, into the TC area.
 * Step 7: Commit the translated functions:
 * - Go over the candidate routines and insert a probe jump from each routine header to
 *   the header of the corresponding translated routine in the TC.
*/

#include "pin.H"
extern "C" {
#include "xed-interface.h"
}
#include <iostream>
#include <iomanip>
#include <fstream>
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

KNOB<UINT32> KnobMaxCommit(KNOB_MODE_WRITEONCE, "pintool",
    "max_commit", "0", "Maximum number of routines to commit (0 = unlimited)");

// Conservative mode is enabled by default for this exercise binary.
// It translates only very simple leaf/straight-line routines.  Complex
// routines still execute correctly through the original code.  This avoids
// committing translated code for GCC cold blocks / jump-table-heavy routines
// that are legal RTNs for Pin but unsafe for this simple educational translator.
KNOB<BOOL> KnobConservativeCommit(KNOB_MODE_WRITEONCE, "pintool",
    "conservative_commit", "1", "Commit only simple straight-line leaf routines");



/* ===================================================================== */
/* Global Variables */
/* ===================================================================== */
std::ofstream* out = 0;

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

// tc containing the new code:
char *tc = nullptr;
unsigned tc_size = 0;


// Array of original target addresses that cannot be translated in the TC.
ADDRINT *jump_to_orig_addr_map = nullptr;
unsigned jump_to_orig_addr_num = 0;

// basic instruction types.
typedef enum {
    RegularIns = 0,
    RtnHeadIns,
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
} instr_map_t;

instr_map_t *instr_map = nullptr;
unsigned num_of_instr_map_entries = 0;
unsigned max_ins_count = 0;
std::map<ADDRINT, unsigned> entry_map;

// All routine entry addresses in the main executable.  Used to avoid
// probed replacement of a routine whose probe patch would overwrite the
// entry of another (overlapping/adjacent) routine.
std::set<ADDRINT> rtn_entry_addrs;

// Direct branch/call targets seen anywhere in the executable.
// A probed replacement must not overwrite bytes that are possible branch
// landing pads.  RTN_IsSafeForProbedReplacement() is not conservative enough
// for the GCC binary in this exercise, because GCC emits many cold/internal
// labels near function entries.
std::set<ADDRINT> direct_branch_target_addrs;

unsigned max_rtn_count = 0;

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

// In probe mode Pin overwrites the first bytes of the routine with a jump.
// RTN_Size() was not reliable enough for cpugcc_r_base.*, therefore the
// header safety check below is based on real XED byte decoding.
// Be conservative on Intel64: require enough bytes for a long probe patch,
// and do not allow a control-flow instruction or RIP-relative memory operand
// to appear inside the bytes that Pin may need to overwrite/copy.
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

    // upper_bound(start) intentionally allows normal calls/jumps to the
    // routine entry itself.  Only internal landing pads inside the bytes that
    // Pin would overwrite are unsafe.
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

// This translator is intentionally simple.  Some valid GCC routines contain
// jump tables, internal cold labels, call/pop idioms, or unusual control-flow
// edges that require a full control-flow graph translator.  In conservative
// mode, commit only straight-line leaf routines.  Skipped routines are left in
// the original executable, so program correctness is preserved.
static bool is_simple_straight_line_leaf_rtn(RTN rtn)
{
    if (!RTN_Valid(rtn))
        return false;

    bool ok = true;
    bool saw_ret = false;

    RTN_Open(rtn);

    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
        ADDRINT ins_addr = INS_Address(ins);

        xed_decoded_inst_t xedd;
        xed_decoded_inst_zero_set_mode(&xedd, &dstate);

        xed_error_enum_t xed_code =
            xed_decode(&xedd, reinterpret_cast<UINT8*>(ins_addr), max_inst_len);
        if (xed_code != XED_ERROR_NONE) {
            ok = false;
            break;
        }

        xed_category_enum_t category_enum = xed_decoded_inst_get_category(&xedd);

        if (category_enum == XED_CATEGORY_RET) {
            saw_ret = true;
            if (INS_Valid(INS_Next(ins)))
                ok = false;
            break;
        }

        if (category_enum == XED_CATEGORY_CALL ||
            category_enum == XED_CATEGORY_COND_BR ||
            category_enum == XED_CATEGORY_UNCOND_BR) {
            ok = false;
            break;
        }

        if (INS_IsCall(ins) || INS_IsDirectControlFlow(ins) ||
            INS_IsIndirectControlFlow(ins)) {
            ok = false;
            break;
        }

        // Avoid PC-relative data accesses in conservative mode.  They can be
        // translated, but skipping them keeps the committed subset maximally
        // robust while the rest of the program runs from the original code.
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
    return ok && saw_ret;
}

/* ============================================================= */
/* Service dump routines                                         */
/* ============================================================= */

/*********************/
/* dump_image_instrs */
/*********************/
void dump_image_instrs(IMG img)
{
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {

            // Open the RTN.
            RTN_Open( rtn );

            cerr << RTN_Name(rtn) << ":" << endl;

            for( INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins) )
            {
                  cerr << "0x" << hex << INS_Address(ins) << ": " << INS_Disassemble(ins) << endl;
            }

            // Close the RTN.
            RTN_Close( rtn );

            cerr << endl;
        }
    }
}


/*************************/
/* dump_instr_from_xedd */
/*************************/
void dump_instr_from_xedd (xed_decoded_inst_t* xedd, ADDRINT address)
{
    // debug print decoded instr:
    char disasm_buf[2048];

    xed_uint64_t runtime_address = static_cast<UINT64>(address);  // set the runtime adddress for disassembly

    xed_format_context(XED_SYNTAX_INTEL, xedd, disasm_buf, sizeof(disasm_buf), static_cast<UINT64>(runtime_address), 0, 0);

    cerr << hex << address << ": " << disasm_buf <<  endl;
}


/************************/
/* dump_instr_from_mem */
/************************/
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


/****************************/
/*  dump_entire_instr_map() */
/****************************/
void dump_entire_instr_map()
{
    for (unsigned i=0; i < num_of_instr_map_entries; i++) {
      // Print the routine name if known.
      if (instr_map[i].ins_type == RtnHeadIns) {
        PIN_LockClient();
        RTN rtn = RTN_FindByAddress(instr_map[i].orig_ins_addr);
        if (rtn == RTN_Invalid()) {
            cerr << "Unknown"  << ":" << endl;
        } else {
            cerr << RTN_Name(rtn) << ":" << endl;
        }
        PIN_UnlockClient();
      }

      if (!instr_map[i].size)
        continue;


      dump_instr_from_mem ((ADDRINT *)instr_map[i].encoded_ins, instr_map[i].orig_ins_addr);
    }
}


/**************************/
/* dump_instr_map_entry() */
/**************************/
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


/*************/
/* dump_tc() */
/*************/
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


/*************************/
/* add_new_instr_entry() */
/*************************/
int add_new_instr_entry(xed_decoded_inst_t *xedd, ADDRINT pc, ins_enum_t ins_type)
{
    // copy target addr to instr map:
    ADDRINT orig_targ_addr = 0x0;

    // Check if the instruction has a branch displacement:
    xed_uint_t disp_byts = xed_decoded_inst_get_branch_displacement_width(xedd);
    xed_int32_t disp;
    if (disp_byts > 0) { // there is a branch offset.
      disp = xed_decoded_inst_get_branch_displacement(xedd);
      orig_targ_addr = pc + xed_decoded_inst_get_length (xedd) + disp;
    }

    // copy rip-relative addr to instr map:
    ADDRINT orig_rip_addr = 0x0;

    // check for a rip-relative displacement:
    unsigned memops = xed_decoded_inst_number_of_memory_operands(xedd);
    if (memops) {
      xed_reg_enum_t base_reg = xed_decoded_inst_get_base_reg(xedd, 0);
      if (base_reg == XED_REG_RIP) {
         unsigned size = xed_decoded_inst_get_length (xedd);
         xed_int64_t disp = xed_decoded_inst_get_memory_displacement(xedd, 0);
         orig_rip_addr = (ADDRINT)(pc + disp + size);
      }
    }

    // Converts the decoder request to a valid encoder request:
    xed_encoder_request_init_from_decode (xedd);

    unsigned new_size = 0;

    xed_error_enum_t xed_error =
       xed_encode (xedd, reinterpret_cast<UINT8*>(instr_map[num_of_instr_map_entries].encoded_ins),
                   max_inst_len , &new_size);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
        return -1;
    }

    // Add a new entry to instr_map:
    //
    instr_map[num_of_instr_map_entries].orig_ins_addr = pc;
    instr_map[num_of_instr_map_entries].new_ins_addr = 0x0;
    instr_map[num_of_instr_map_entries].orig_targ_addr = orig_targ_addr;
    instr_map[num_of_instr_map_entries].orig_rip_addr = orig_rip_addr;
    instr_map[num_of_instr_map_entries].targ_map_entry = -1;
    instr_map[num_of_instr_map_entries].size = new_size;
    instr_map[num_of_instr_map_entries].ins_type = ins_type;

    num_of_instr_map_entries++;

    if (num_of_instr_map_entries >= max_ins_count) {
        cerr << "out of memory for map_instr" << endl;
        return -1;
    }

    // debug print new encoded instr:
    if (KnobVerbose) {
        cerr << "    new instr:";
        dump_instr_from_mem((ADDRINT *)instr_map[num_of_instr_map_entries-1].encoded_ins,
                            instr_map[num_of_instr_map_entries-1].new_ins_addr);
    }

    return new_size;
}



/*************************************************/
/* chain_all_direct_jmp_and_call_target_entries() */
/*************************************************/
void chain_all_direct_jmp_and_call_target_entries(unsigned from_entry,
                                                 unsigned until_entry)
{
    entry_map.clear();

    for (unsigned i = from_entry; i < until_entry; i++) {
        instr_map[i].targ_map_entry = -1;
        ADDRINT orig_ins_addr = instr_map[i].orig_ins_addr;
        if (!orig_ins_addr)
          continue;
        // For instrs with same orig_addr, give precedence to the first one.
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
void set_initial_estimated_new_ins_addrs_in_tc(char *tc) {
  unsigned tc_cursor = 0;
  // Set initial estimated new addrs for each instruction in the tc.
  for (unsigned i=0; i < num_of_instr_map_entries; i++) {
    instr_map[i].new_ins_addr = (ADDRINT)&tc[tc_cursor];
    // update expected size of tc.
    tc_cursor += instr_map[i].size;
  }
}


/**************************/
/* fix_rip_displacement() */
/**************************/
int fix_rip_displacement(unsigned instr_map_entry)
{
    // uncond jumps instructions with size=0
    // should remain with size=0 for beeing removed from tc
    if (!instr_map[instr_map_entry].size)
        return 0;

    // Check if it is a RIP-relative instr.
    if (!instr_map[instr_map_entry].orig_rip_addr)
      return 0;

    // Check if it is a direct jmp or call instruction.
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

    //debug print:
    if (KnobVerbose) {
      cerr << " Before fixing rip offset\n";
      dump_instr_map_entry(instr_map_entry);
    }

    //xed_uint_t disp_byts = xed_decoded_inst_get_memory_displacement_width(xedd,i); // how many byts in disp ( disp length in byts - for example FFFFFFFF = 4
    xed_int64_t new_disp = 0;
    xed_uint_t new_disp_byts = 4;   // set maximal num of byts for now.

    // Modify rip displacement. use rip-relative direct addressing mode.
    new_disp = (xed_int64_t)(instr_map[instr_map_entry].orig_rip_addr - instr_map[instr_map_entry].new_ins_addr -
                               instr_map[instr_map_entry].size);
    // Code when using direct addressing mode.
    //xed_encoder_request_set_base0 (&xedd, XED_REG_INVALID);
    //new_disp = instr_map[instr_map_entry].orig_rip_addr;
    if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
        cerr << "Invalid rip displacement larger than 32 bits in fix_rip_displacement\n";
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    // Set the memory displacement using a bit length.
    xed_encoder_request_set_memory_displacement (&xedd, new_disp, new_disp_byts);

    unsigned max_size = XED_MAX_INSTRUCTION_BYTES;
    unsigned new_size = 0;

    // Converts the decoder request to a valid encoder request:
    xed_encoder_request_init_from_decode (&xedd);

    xed_error_enum_t xed_error =
       xed_encode (&xedd, reinterpret_cast<UINT8*>(instr_map[instr_map_entry].encoded_ins),
                   max_size , &new_size);
    if (xed_error != XED_ERROR_NONE) {
        cerr << "ENCODE ERROR: " << xed_error_enum_t2str(xed_error) << endl;
        dump_instr_map_entry(instr_map_entry);
        return -1;
    }

    //debug print:
    if (KnobVerbose) {
      cerr << " After fixing rip offset\n";
      dump_instr_map_entry(instr_map_entry);
    }

    return new_size;
}


/**************************************/
/* fix_direct_jmp_or_call_to_orig_addr */
/**************************************/
int fix_direct_jmp_or_call_to_orig_addr(unsigned instr_map_entry)
{
    // Ignore instructions of zero size.
    if (!instr_map[instr_map_entry].size)
      return 0;

    if (KnobVerbose) {
        cerr << "branch/call to orig addr: 0x" << hex << instr_map[instr_map_entry].orig_targ_addr << " : ";
        dump_instr_from_mem ((ADDRINT *)instr_map[instr_map_entry].encoded_ins,
                             instr_map[instr_map_entry].orig_ins_addr);
    }

    // check for cases of direct jumps/calls back to the orginal target address:
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

    // Previous versions rewrote CALL/JMP-to-original as an indirect
    // RIP-relative memory branch through jump_to_orig_addr_map.  That is fragile
    // in probe mode and can jump through the wrong address if the rewritten
    // instruction length changes during fixup.  Since allocate_and_init_memory()
    // places the TC within +/-2GB of the original image, the safe/simple fix is
    // to keep the instruction as a direct rel32 branch/call and retarget it to
    // the original address.  If the original routine was probed, the branch will
    // naturally land on Pin's probe jump; otherwise it will execute original code.
    xed_int64_t new_disp =
        (xed_int64_t)instr_map[instr_map_entry].orig_targ_addr -
        (xed_int64_t)instr_map[instr_map_entry].new_ins_addr -
        (xed_int64_t)instr_map[instr_map_entry].size;

    xed_uint_t new_disp_byts = 4;
    xed_iclass_enum_t iclass_enum = xed_decoded_inst_get_iclass(&xedd);
    xed_iform_enum_t iform_enum = xed_decoded_inst_get_iform_enum(&xedd);

    // LOOP/JRCXZ have only 8-bit displacements.  They cannot be promoted to
    // rel32, so fail cleanly if the original target is too far.
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
    //uncond jumps instructions with size=0 should remain with size=0
    // for beeing removed from tc
    if (!instr_map[instr_map_entry].size)
        return 0;

    // Check if it is indeed a direct branch or a direct call instr:
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

    // fix direct branches/calls to original targ addresses or
    // indirect branches via a rip offset which had previously been
    // formed by previouis calls to fix_direct_jmp_or_call_to_orig_addr()
    // in order to relpace direct jumps to orig targ addrs.
    if (instr_map[instr_map_entry].targ_map_entry < 0) {
       int rc = fix_direct_jmp_or_call_to_orig_addr(instr_map_entry);
       return rc;
    }

    ADDRINT new_targ_addr;
    new_targ_addr = instr_map[instr_map[instr_map_entry].targ_map_entry].new_ins_addr;

    new_disp =
      (new_targ_addr - instr_map[instr_map_entry].new_ins_addr) - instr_map[instr_map_entry].size; // orig_size;
     if (new_disp > 0x7FFFFFFF || new_disp < -0x7FFFFFFF) {
        cerr << "Invalid rip displacement larger than 32 bits in fix_direct_jmp_or_call_displacement\n";
        return -1;
    }

    xed_uint_t   new_disp_byts = 4; // num_of_bytes(new_disp);  ???

    // the max displacement size of loop instructions is 1 byte:
    xed_iclass_enum_t iclass_enum = xed_decoded_inst_get_iclass(&xedd);
    if (iclass_enum == XED_ICLASS_LOOP ||
        iclass_enum == XED_ICLASS_LOOPE ||
        iclass_enum == XED_ICLASS_LOOPNE) {
      new_disp_byts = 1;
    }

    // the max displacement size of jecxz instructions is ???:
    xed_iform_enum_t iform_enum = xed_decoded_inst_get_iform_enum (&xedd);
    if (iform_enum == XED_IFORM_JRCXZ_RELBRb){
      new_disp_byts = 1;
    }

    // Converts the decoder request to a valid encoder request:
    xed_encoder_request_init_from_decode (&xedd);

    //Set the branch displacement:
    xed_encoder_request_set_branch_displacement (&xedd, new_disp, new_disp_byts);

    //xed_uint8_t enc_buf[XED_MAX_INSTRUCTION_BYTES];
    //xed_error_enum_t xed_error = xed_encode (&xedd, enc_buf, max_size , &new_size);
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

    //debug print of new instruction in tc:
    if (KnobVerbose) {
        dump_instr_map_entry(instr_map_entry);
    }

    return new_size;
}

/************************************/
/* fix_instructions_displacements() */
/************************************/
int fix_instructions_displacements()
{
   // fix displacemnets of direct branch or call instructions:

    int size_diff = 0;
    bool is_diff = false;

    do {

        size_diff = 0;
        is_diff = false;

        if (KnobVerbose) {
            cerr << "starting a pass of fixing instructions displacements: " << endl;
        }

        for (unsigned i=0; i < num_of_instr_map_entries; i++) {

            instr_map[i].new_ins_addr += size_diff;

            // fix rip displacement:
            int new_size = fix_rip_displacement(i);
            if (new_size) {
              if (new_size < 0)
                  return -1;
              if (instr_map[i].size != (unsigned)new_size) { // this was a rip-based instruction which was fixed.
                  if (instr_map[i].size < (unsigned)new_size)
                     size_diff += (new_size - instr_map[i].size);
                  else
                     size_diff -= (instr_map[i].size - new_size);
                  instr_map[i].size = (unsigned)new_size;
                  is_diff = true;
                  continue;
              }
            }

            // fix instr displacement for direct jump or call:
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

        }  // end int i=0; i ..

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

                // We only need code-section targets for probe safety.
                if (target >= lowest_sec_addr && target < highest_sec_addr)
                    direct_branch_target_addrs.insert(target);
            }
            RTN_Close(rtn);
        }
    }

    if (KnobVerbose) {
        cerr << "collected " << dec << direct_branch_target_addrs.size()
             << " direct branch/call targets" << endl;
    }
}

/********************************/
/* find_candidate_rtns_for_tc() */
/********************************/
int find_candidate_rtns_for_tc(IMG img)
{
    int rc = 0;
    // go over routines and check if they are candidates for translation and mark them for translation:

    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;

        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            // Keep the entry num of the rtn head in case we need to
            // revert the insertin of the instruction in rtn into the instructions
            // map due to an invalid decoding.
            unsigned rtn_entry = num_of_instr_map_entries;

            // Do not translate _exit: it is already probed above in order
            // to call Fini() in probe mode.
            if (RTN_Name(rtn) == "_exit")
                continue;

            //if (RTN_Name(rtn) == ".plt")
            //    continue;
            //
            if (!is_safe_candidate_rtn(rtn)) {
                if (KnobVerbose) {
                    cerr << "skipping unsafe/small rtn: " << RTN_Name(rtn)
                         << " at 0x" << hex << RTN_Address(rtn) << endl;
                }
                continue;
            }

            if (KnobConservativeCommit.Value() &&
                !is_simple_straight_line_leaf_rtn(rtn)) {
                if (KnobVerbose) {
                    cerr << "skipping complex rtn in conservative mode: "
                         << RTN_Name(rtn) << " at 0x" << hex
                         << RTN_Address(rtn) << endl;
                }
                continue;
            }

            //static unsigned rtn_count = 0;
            //rtn_count++;
            //if (RTN_Address(rtn) != 0x55555575847a &&
            //    RTN_Address(rtn) != 0x555555acef0b &&
            //    RTN_Address(rtn) != 0x555555ace723 &&
            //    RTN_Address(rtn) != 0x555555af088a &&
            //    RTN_Address(rtn) != 0x555555a7b1e1 &&
            //    RTN_Address(rtn) != 0x5555559d6f04 &&
            //    RTN_Address(rtn) != 0x5555559fa01e &&
            //    RTN_Address(rtn) != 0x5555559f9f8b &&
            //    RTN_Address(rtn) != 0x555555932484 &&
            //    RTN_Address(rtn) != 0x555555926f46) {
            //  if (rtn_count > 5000)
            //    continue;
            //}
            
            // Open the RTN.
            RTN_Open( rtn );

            for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {

                //debug print of orig instruction:
                if (KnobVerbose) {
                    cerr << "old instr: ";
                    cerr << "0x" << hex << INS_Address(ins) << ": " << INS_Disassemble(ins) <<  endl;
                    //xed_print_hex_line(reinterpret_cast<UINT8*>(INS_Address (ins)), INS_Size(ins));
                }

                ADDRINT ins_addr = INS_Address(ins);

                xed_decoded_inst_t xedd;
                xed_error_enum_t xed_code;

                // Add instr into instr map:
                bool isRtnHeadIns = (RTN_Address(rtn) == ins_addr);
                ins_enum_t ins_type = (isRtnHeadIns ? RtnHeadIns : RegularIns);
          
                // Add ins to instr_map:
                //
                xed_decoded_inst_zero_set_mode(&xedd,&dstate);
                xed_code = xed_decode(&xedd, reinterpret_cast<UINT8*>(ins_addr), max_inst_len);
                if (xed_code != XED_ERROR_NONE) {
                    cerr << "ERROR: xed decode failed for instr at: " << "0x" << hex << ins_addr << endl;
                    return -1;
                }

                // Add the instr into the instr_map table.
                rc = add_new_instr_entry(&xedd, INS_Address(ins), ins_type);
                if (rc < 0) {
                    cerr << "ERROR: failed during instructon translation." << endl;
                    return -1;
                }

            } // end for INS...

            // debug print of routine name:
            if (KnobVerbose) {
                cerr <<   "rtn name: " << RTN_Name(rtn) << endl;
            }

            // Close the RTN.
            RTN_Close( rtn );

            // Apply local chaining only inside this routine.
            // Cross-routine direct calls/jumps are intentionally left pointing
            // to the original code. If the callee was committed, the original
            // entry probe will route it to the TC; if not, the original code runs.
            chain_all_direct_jmp_and_call_target_entries(rtn_entry, num_of_instr_map_entries);

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
    // Commit the translated routines:
    // Go over the routines and replace the original ones
    // by their new successfully translated ones:

    UINT32 committed_count = 0;

    for (unsigned i=0; i < num_of_instr_map_entries; i++) {

        //replace routine by new routine in tc

        if (instr_map[i].ins_type != RtnHeadIns)
          continue;

        RTN rtn = RTN_FindByAddress(instr_map[i].orig_ins_addr);
        if (rtn == RTN_Invalid()) {
           cerr << "invalid rtN for commit for addr: 0x"
                << instr_map[i].orig_ins_addr << "\n";
           continue;
        }

        if (!RTN_IsSafeForProbedReplacement(rtn) ||
            !has_no_other_rtn_entry_in_probe_patch(rtn) ||
            !has_no_branch_target_inside_probe_patch(rtn)) {
            if (KnobVerbose) {
                cerr << "Skipping unsafe/overlapping/branched-into routine for probed replacement: "
                     << RTN_Name(rtn) << " at 0x" << hex << RTN_Address(rtn) << endl;
            }
            continue;
        }

        if (KnobConservativeCommit.Value() &&
            !is_simple_straight_line_leaf_rtn(rtn)) {
            if (KnobVerbose) {
                cerr << "Skipping complex routine at commit in conservative mode: "
                     << RTN_Name(rtn) << " at 0x" << hex << RTN_Address(rtn) << endl;
            }
            continue;
        }

        if (KnobMaxCommit.Value() != 0 && committed_count >= KnobMaxCommit.Value()) {
            if (KnobVerbose) {
                cerr << "max_commit reached before routine: " << RTN_Name(rtn)
                     << " at 0x" << hex << RTN_Address(rtn) << endl;
            }
            continue;
        }

        // Debug print.
        // cerr << "committing rtN: " << RTN_Name(rtn);
        // cerr << " from: 0x" << hex << RTN_Address(rtn)
        //      << " to: 0x" << hex << instr_map[i].new_ins_addr << endl;


        AFUNPTR origFptr = RTN_ReplaceProbed(rtn,  (AFUNPTR)instr_map[i].new_ins_addr);

        if (origFptr == NULL) {
            if (KnobVerbose) {
                cerr << "RTN_ReplaceProbed failed.";
                cerr << " orig routine addr: 0x" << hex << RTN_Address(rtn)
                     << " translated routine addr: 0x" << hex
                     << instr_map[i].new_ins_addr << endl;
                dump_instr_from_mem ((ADDRINT *)RTN_Address(rtn), RTN_Address(rtn));
            }
            continue;
        }

        committed_count++;
        if (KnobVerbose) {
            cerr << "committed #" << dec << committed_count << ": " << RTN_Name(rtn)
                 << " orig=0x" << hex << RTN_Address(rtn)
                 << " tc=0x" << hex << instr_map[i].new_ins_addr << endl;
        }

        // debug print.
        //if (origFptr != NULL) {
        //  cerr << "RTN_ReplaceProbed succeeded. ";
        //  cerr << " orig routine addr: 0x" << hex << RTN_Address(rtn)
        //       << " translated routine addr: 0x" << hex
        //       << instr_map[i].new_ins_addr << endl;
        //  dump_instr_from_mem ((ADDRINT *)RTN_Address(rtn), RTN_Address(rtn));
        //}
    }
}


/****************************/
/* allocate_and_init_memory */
/****************************/
int allocate_and_init_memory(IMG img)
{
    // Calculate size of executable sections and allocate required memory:
    //
    ADDRINT highest_addr = 0;
    for (SEC sec = IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec))
    {
        if (!SEC_IsExecutable(sec) || SEC_IsWriteable(sec) || !SEC_Address(sec))
            continue;

        if (!lowest_sec_addr || lowest_sec_addr > SEC_Address(sec))
            lowest_sec_addr = SEC_Address(sec);

        if (highest_sec_addr < SEC_Address(sec) + SEC_Size(sec))
            highest_sec_addr = SEC_Address(sec) + SEC_Size(sec);

        // need to avouid using RTN_Open as it is expensive...
        for (RTN rtn = SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn))
        {
            rtn_entry_addrs.insert(RTN_Address(rtn));
            if (highest_addr < RTN_Address(rtn) + RTN_Size(rtn))
                highest_addr = RTN_Address(rtn) + RTN_Size(rtn);
            max_rtn_count++;
            max_ins_count += RTN_NumIns  (rtn);
        }
    }

    max_ins_count *= 10; // estimating that the num of instrs of the inlined
                         // functions will not exceed the total nunmber of the entire code.


    // get a page size in the system:
    int pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize == -1) {
      perror("sysconf");
      return -1;
    }

    ADDRINT text_size = (highest_sec_addr - lowest_sec_addr) * 2 + pagesize * 4;

    unsigned tclen = 2 * text_size + pagesize * 4;   // FIXME: need a better estimate
    // Check thet tclen is not larger than a 32 bit branch displacement
    if (tclen >= 0x7FFFFFFULL) {
      cerr << "size of TC is beyond the range of a branch displacement" << endl;
      return -1;
    }

    // Allocate the needed memory for all data structures and for tc
    // with RW+EXEC permissions which is not
    // located in an address that is more than 32bits afar:
    const size_t mem_size =
              tclen +                           // TC size
              max_rtn_count * sizeof(ADDRINT);  // jump_to_orig_addr_map size
    char *addr = nullptr;
    ADDRINT max_distance = 0x7FFFFFFF;
    const size_t step = pagesize; // Try every page
    // Align target address to page boundary
    ADDRINT aligned_target = ((ADDRINT)highest_addr) & ~(pagesize - 1);
    // Try exact address first
    void* result = mmap((void*)aligned_target, mem_size,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       0, 0);
    if (result != MAP_FAILED &&
        (abs((long)((ADDRINT)result - aligned_target)) <= (long)max_distance)) {
        addr = (char *)result;
    }

    if (!addr) {
        // Search in expanding rings around target
        for (size_t offset = step; offset <= max_distance; offset += step) {
            // Try above target address
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

            // Try below target address (if not underflow)
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

    // debug print.
    cerr << " allocated memory at: 0x" << hex << (ADDRINT)addr << "\n";

    // TC is allocated first.
    tc = (char *)addr;
    addr += tclen;

    // Allocate memory to the jump map to orig addrs which cannot be relocated.
    jump_to_orig_addr_map = (ADDRINT *)addr;

    // Allocate memory for teh instr_map table.
    instr_map = (instr_map_t *)calloc(2 * max_ins_count, sizeof(instr_map_t));
    if (instr_map == NULL) {
        perror("calloc");
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
    cerr << "Reached _exit." << endl;
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
    // Insert a call to function Fini when raching the _exit routine.
    RTN exitRtn = RTN_FindByName(img, "_exit");
    if (RTN_Valid(exitRtn) && RTN_IsSafeForProbedReplacement(exitRtn)) {
      origExit = (EXITFUNCPTR)RTN_ReplaceProbed(exitRtn, AFUNPTR(ExitInProbeMode));
    }

    // Step 0: Check the image and the CPU:
    if (!IMG_IsMainExecutable(img))
      return;

    if (KnobDumpOrigCode)
      dump_image_instrs(img);

    int rc = 0;

    clock_t start_clock = clock();

    // step 1: Check size of executable sections and allocate required memory:
    rc = allocate_and_init_memory(img);
    if (rc < 0) {
        cerr << "failed to initialize memory for translation\n";
        return;
    }
    cerr << "after memory allocation" << endl;

    // Collect all direct branch targets before candidate filtering.
    // This prevents probing a routine whose first bytes are an internal
    // branch landing pad for another routine/cold block.
    collect_all_direct_branch_targets_in_image(img);
    cerr << "after collecting branch targets" << endl;

    // Step 2: go over all routines and identify candidate routines and copy
    //         their code into the instr map IR:
    rc = find_candidate_rtns_for_tc(img);
    if (rc < 0) {
        cerr << "failed to find candidates for translation\n";
        return;
    }
    cerr << "after identifying candidate routines" << endl;

    // Step 3: Chaining was already applied locally per routine in
    // find_candidate_rtns_for_tc().  Do not globally chain cross-routine
    // targets: a translated caller should call/jump to the original routine
    // entry, so Pin's probe decides whether to route it to TC or leave it in
    // original code.
    cerr << "after local chaining of branch targets" << endl;

    // Step 4: Set initial estimated new addrs for each instruction in the tc.
    set_initial_estimated_new_ins_addrs_in_tc(tc);
    cerr << "after setting initial estimated new ins addrs in tc" << endl;

    // Step 5: fix rip-based, direct branch and direct call displacements:
    rc = fix_instructions_displacements();
    if (rc < 0 ) {
        cerr << "failed to fix displacments of translated instructions\n";
        return;
    }
    cerr << "after fixing instructions displacements" << endl;

    // Step 6: write translated instructions to the tc:
    rc = copy_instrs_to_tc(tc);
    if (rc < 0 ) {
        cerr << "failed to copy the instructions to the translation cache\n";
        return;
    }
    tc_size = rc;
    cerr << "after write all new instructions to memory tc" << endl;

    if (KnobDumpTranslatedCode) {
       cerr << "Translation Cache dump:" << endl;
       dump_tc(tc, tc_size);  // dump the entire tc

       //cerr << endl << "instructions map dump:" << endl;
       //dump_entire_instr_map_with_prof();     // dump all translated instructions in map_instr
    }

    // Step 7: Commit the translated routines:
    //         Go over the candidate functions and replace the original ones
    //         by their new successfully translated ones:
    if (!KnobDoNotCommitTranslatedCode) {
      commit_translated_rtns_to_tc();
      cerr << "after commit of translated routines from orig code to TC" << endl;
    }

    clock_t end_clock = clock();
    cerr << " create tc took: " << (double)(end_clock - start_clock) / CLOCKS_PER_SEC << " seconds\n";
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
    // Initialize pin & symbol manager
    if( PIN_Init(argc,argv) )
        return Usage();

    PIN_InitSymbols();

    // Register create_tc
    IMG_AddInstrumentFunction(create_tc, 0);

    // Start the program, never returns
    PIN_StartProgramProbed();

    return 0;
}

/* ===================================================================== */
/* eof */
/* ===================================================================== */

