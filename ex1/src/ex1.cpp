/*
 * ex1 binary optimization
 * used pin 4 (what i had installed)
 */

#include "pin.H"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using std::cerr;
using std::dec;
using std::endl;
using std::hex;
using std::map;
using std::ofstream;
using std::string;
using std::vector;

static const UINT32 kMaxRegSamples = 20;  // 20 values per register

// info we keep for each RTN
struct routine_attributes {
    string image_name;
    ADDRINT image_address;
    string name;
    ADDRINT address;
    UINT64 instruction_count;
    UINT64 number_of_times_called;

    routine_attributes()
        : image_address(0), address(0), instruction_count(0), number_of_times_called(0) {}
};

// stores successive register readings
struct RegHistory {
    UINT64 values[kMaxRegSamples];
    UINT32 count;

    RegHistory() : count(0) {}
};

static map<ADDRINT, routine_attributes> routineMap;  // rtn addr -> stats
static ofstream OutFile;

// main exe address range 
static ADDRINT MainImgLow = 0;
static ADDRINT MainImgHigh = 0;

static RegHistory RegRax;
static RegHistory RegRbx;
static RegHistory RegRcx;
static RegHistory RegRdx;
static RegHistory RegRsi;
static RegHistory RegRdi;

static BOOL InMainExecutable(ADDRINT addr) {
    return (addr >= MainImgLow && addr < MainImgHigh);
}

// stop sampling regs when we have 20 each
static BOOL AllRegsFull() {
    return (RegRax.count >= kMaxRegSamples && RegRbx.count >= kMaxRegSamples &&
            RegRcx.count >= kMaxRegSamples && RegRdx.count >= kMaxRegSamples &&
            RegRsi.count >= kMaxRegSamples && RegRdi.count >= kMaxRegSamples);
}

//  callback count
VOID CountInstruction(ADDRINT rtnAddr) { routineMap[rtnAddr].instruction_count++; }

VOID RoutineEntry(ADDRINT rtnAddr) { routineMap[rtnAddr].number_of_times_called++; }

static VOID RecordReg(RegHistory* hist, UINT64 val) {
    if (hist->count >= kMaxRegSamples) return;
    hist->values[hist->count++] = val;
}

// read regs from context
static VOID SampleRegsFromContext(CONTEXT* ctxt, VOID*) {
    RecordReg(&RegRax, PIN_GetContextReg(ctxt, REG_GAX));
    RecordReg(&RegRbx, PIN_GetContextReg(ctxt, REG_GBX));
    RecordReg(&RegRcx, PIN_GetContextReg(ctxt, REG_GCX));
    RecordReg(&RegRdx, PIN_GetContextReg(ctxt, REG_GDX));
    RecordReg(&RegRsi, PIN_GetContextReg(ctxt, REG_GSI));
    RecordReg(&RegRdi, PIN_GetContextReg(ctxt, REG_GDI));
}

// average diff between samples
static UINT64 AverageAbsDelta(const RegHistory& hist) {
    UINT64 sum = 0;
    for (UINT32 i = 1; i < hist.count; ++i) {
        const UINT64 prev = hist.values[i - 1];
        const UINT64 cur = hist.values[i];
        sum += (cur >= prev) ? (cur - prev) : (prev - cur);
    }
    return sum / (hist.count - 1);
}

// one register line at end of csv
static VOID WriteRegLine(const char* regName, const RegHistory& hist) {
    OutFile << regName << " values:";
    for (UINT32 i = 0; i < hist.count; ++i) {
        OutFile << (i == 0 ? " " : " -> ") << "0x" << hex << hist.values[i] << dec;
    }
    OutFile << " Has an Average delta: ";
    if (hist.count >= 2) {
        OutFile << "Yes Average delta: 0x" << hex << AverageAbsDelta(hist) << dec;
    } else {
        OutFile << "No";
    }
    OutFile << "\n";
}

//  instrumention of each routine
VOID RoutineInstrumentation(RTN rtn, VOID* v) {
    if (!RTN_Valid(rtn)) return;

    RTN_Open(rtn);

    IMG img = SEC_Img(RTN_Sec(rtn));
    if (!IMG_Valid(img)) {
        RTN_Close(rtn);
        return;
    }

    // save rtn data 
    routine_attributes attr;
    attr.image_name = IMG_Name(img);
    attr.image_address = IMG_LowAddress(img);
    attr.name = RTN_Name(rtn);
    attr.address = RTN_Address(rtn);
    routineMap[attr.address] = attr;

    if (IMG_IsMainExecutable(img)) {
        MainImgLow = IMG_LowAddress(img);
        MainImgHigh = IMG_HighAddress(img);
    }

    // count times rtn was entered
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)RoutineEntry, IARG_ADDRINT, attr.address, IARG_END);

    // instruction count per rtn
    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)CountInstruction, IARG_ADDRINT, attr.address, IARG_END);
    }

    RTN_Close(rtn);
}

// sample registers in main
VOID Instruction(INS ins, VOID* v) {
    if (AllRegsFull()) return;
    if (!InMainExecutable(INS_Address(ins))) return;

    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)SampleRegsFromContext, IARG_CONTEXT, IARG_END);
}

// write csv when program ends
VOID Fini(INT32 code, VOID* v) {
    OutFile.open("rtn-output.csv");

    vector<routine_attributes> executed;
    for (map<ADDRINT, routine_attributes>::iterator it = routineMap.begin(); it != routineMap.end();
         ++it) {
        if (it->second.instruction_count > 0) executed.push_back(it->second);
    }

    // sort by instruction count high to low
    std::sort(executed.begin(), executed.end(), [](const routine_attributes& a,
                                                    const routine_attributes& b) {
        return a.instruction_count > b.instruction_count;
    });

    for (UINT32 i = 0; i < executed.size(); ++i) {
        const routine_attributes& r = executed[i];
        OutFile << r.image_name << ", 0x" << hex << r.image_address << dec << ", " << r.name
                << ", 0x" << hex << r.address << dec << ", " << r.instruction_count << ", "
                << r.number_of_times_called << "\n";
    }

    // register info out
    WriteRegLine("RAX", RegRax);
    WriteRegLine("RBX", RegRbx);
    WriteRegLine("RCX", RegRcx);
    WriteRegLine("RDX", RegRdx);
    WriteRegLine("RSI", RegRsi);
    WriteRegLine("RDI", RegRdi);

    OutFile.close();
}

int main(int argc, char* argv[]) {
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) {
        cerr << "Usage: pin -t ex1.so -- <target_app> [args]\n";
        return 1;
    }

    RTN_AddInstrumentFunction(RoutineInstrumentation, 0);
    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);

    PIN_StartProgram();
    return 0;
}
