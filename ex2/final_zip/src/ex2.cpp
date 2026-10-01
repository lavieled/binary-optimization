
#include "pin.H"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using std::cerr;
using std::dec;
using std::endl;
using std::hex;
using std::map;
using std::ofstream;
using std::ostringstream;
using std::pair;
using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;

static const UINT32 kMaxRegSamples = 20;
static const UINT32 kInvalidRtnIdx = static_cast<UINT32>(-1);


struct RegHistory {
    UINT64 values[kMaxRegSamples];
    UINT32 count;

    RegHistory() : count(0) {}
};

struct RtnStats {
    string image_name;
    ADDRINT image_address;
    string name;
    ADDRINT address;
    UINT64 instruction_count;
    UINT64 number_of_times_called;
    BOOL regsComplete;
    RegHistory rax;
    RegHistory rbx;
    RegHistory rcx;
    RegHistory rdx;
    RegHistory rsi;
    RegHistory rdi;

    RtnStats()
        : image_address(0),
          address(0),
          instruction_count(0),
          number_of_times_called(0),
          regsComplete(FALSE) {}
};

static vector<RtnStats> g_rtnStats;
static map<ADDRINT, UINT32> g_rtnAddrToIdx;

/* map RTN to index */
static UINT32 GetRtnIdx(RTN rtn) {
    if (!RTN_Valid(rtn)) return kInvalidRtnIdx;

    const ADDRINT addr = RTN_Address(rtn);
    map<ADDRINT, UINT32>::iterator it = g_rtnAddrToIdx.find(addr);
    if (it != g_rtnAddrToIdx.end()) return it->second;

    RTN_Open(rtn);
    IMG img = SEC_Img(RTN_Sec(rtn));
    RtnStats attr;
    if (IMG_Valid(img)) {
        attr.image_name = IMG_Name(img);
        attr.image_address = IMG_LowAddress(img);
    }
    attr.name = RTN_Name(rtn);
    attr.address = addr;
    RTN_Close(rtn);

    const UINT32 idx = static_cast<UINT32>(g_rtnStats.size());
    g_rtnAddrToIdx[addr] = idx;
    g_rtnStats.push_back(attr);
    return idx;
}

/* store sample */
static VOID RecordReg(RegHistory* hist, UINT64 val) {
    if (hist->count >= kMaxRegSamples) return;
    hist->values[hist->count++] = val;
}

static BOOL AllRtnRegsFull(const RtnStats& r) {
    return (r.rax.count >= kMaxRegSamples && r.rbx.count >= kMaxRegSamples &&
            r.rcx.count >= kMaxRegSamples && r.rdx.count >= kMaxRegSamples &&
            r.rsi.count >= kMaxRegSamples && r.rdi.count >= kMaxRegSamples);
}

VOID RoutineEntry(UINT32 rtnIdx) { g_rtnStats[rtnIdx].number_of_times_called++; }

/* sample registers */
static VOID PIN_FAST_ANALYSIS_CALL SampleRegsInRtn(UINT32 rtnIdx, ADDRINT rax, ADDRINT rbx,
                                                   ADDRINT rcx, ADDRINT rdx, ADDRINT rsi,
                                                   ADDRINT rdi) {
    RtnStats& r = g_rtnStats[rtnIdx];
    if (r.regsComplete) return;

    RecordReg(&r.rax, rax);
    RecordReg(&r.rbx, rbx);
    RecordReg(&r.rcx, rcx);
    RecordReg(&r.rdx, rdx);
    RecordReg(&r.rsi, rsi);
    RecordReg(&r.rdi, rdi);

    if (AllRtnRegsFull(r)) r.regsComplete = TRUE;
}

VOID RoutineInstrumentation(RTN rtn, VOID* v) {
    if (!RTN_Valid(rtn)) return;

    const UINT32 rtnIdx = GetRtnIdx(rtn);
    if (rtnIdx == kInvalidRtnIdx) return;

    RTN_Open(rtn);

    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)RoutineEntry, IARG_UINT32, rtnIdx, IARG_END);

    /* per-INS reg sample */
    for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins)) {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)SampleRegsInRtn, IARG_FAST_ANALYSIS_CALL,
                       IARG_UINT32, rtnIdx, IARG_REG_VALUE, REG_GAX, IARG_REG_VALUE, REG_GBX,
                       IARG_REG_VALUE, REG_GCX, IARG_REG_VALUE, REG_GDX, IARG_REG_VALUE, REG_GSI,
                       IARG_REG_VALUE, REG_GDI, IARG_END);
    }

    RTN_Close(rtn);
}

/*  BBL profile */
struct BblInfo {
    ADDRINT addr;
    UINT64 exec_count;
    UINT64 taken;
    UINT64 fallthru;
    BOOL is_cond_jump;
    BOOL is_indirect_jump;
    map<ADDRINT, UINT64> indirect_targets;

    BblInfo()
        : addr(0),
          exec_count(0),
          taken(0),
          fallthru(0),
          is_cond_jump(FALSE),
          is_indirect_jump(FALSE) {}
};

static unordered_map<ADDRINT, BblInfo> bbl_map;
static unordered_set<ADDRINT> g_instrumented_bbls;

/* BBL and ins count */
VOID CountExec(ADDRINT addr, UINT32 rtnIdx, UINT32 numIns) {
    bbl_map[addr].exec_count++;
    if (rtnIdx != kInvalidRtnIdx) g_rtnStats[rtnIdx].instruction_count += numIns;
}

VOID CountCondBranch(ADDRINT addr, BOOL taken) {
    if (taken)
        bbl_map[addr].taken++;
    else
        bbl_map[addr].fallthru++;
}

VOID CountIndirectTarget(ADDRINT addr, ADDRINT target, BOOL taken) {
    if (!taken) return;
    bbl_map[addr].indirect_targets[target]++;
}

VOID Trace(TRACE trace, VOID* v) {
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        const ADDRINT addr = BBL_Address(bbl);
        /* instrument each BBL once */
        if (g_instrumented_bbls.find(addr) != g_instrumented_bbls.end()) continue;
        g_instrumented_bbls.insert(addr);

        bbl_map[addr].addr = addr;
        INS tail = BBL_InsTail(bbl);

        const UINT32 rtnIdx = GetRtnIdx(INS_Rtn(BBL_InsHead(bbl)));
        const UINT32 numIns = BBL_NumIns(bbl);

        BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)CountExec, IARG_ADDRINT, addr, IARG_UINT32,
                       rtnIdx, IARG_UINT32, numIns, IARG_END);

        /* conditional taken / fallthrough */
        if (INS_IsBranch(tail) && INS_HasFallThrough(tail)) {
            bbl_map[addr].is_cond_jump = TRUE;

            INS_InsertCall(tail, IPOINT_TAKEN_BRANCH, (AFUNPTR)CountCondBranch, IARG_ADDRINT, addr,
                           IARG_BOOL, TRUE, IARG_END);

            INS_InsertCall(tail, IPOINT_AFTER, (AFUNPTR)CountCondBranch, IARG_ADDRINT, addr,
                           IARG_BOOL, FALSE, IARG_END);
        }

        /* indirect jump targets */
        if (INS_IsIndirectControlFlow(tail) && !INS_IsCall(tail)) {
            bbl_map[addr].is_indirect_jump = TRUE;

            INS_InsertCall(tail, IPOINT_BEFORE, (AFUNPTR)CountIndirectTarget, IARG_ADDRINT, addr,
                           IARG_BRANCH_TARGET_ADDR, IARG_BRANCH_TAKEN, IARG_END);
        }
    }
}

static VOID WriteEdgeProfile() {
    vector<BblInfo> sorted;
    for (unordered_map<ADDRINT, BblInfo>::iterator it = bbl_map.begin(); it != bbl_map.end();
         ++it) {
        if (it->second.exec_count > 0) sorted.push_back(it->second);
    }

    std::sort(sorted.begin(), sorted.end(),
              [](const BblInfo& a, const BblInfo& b) { return a.exec_count > b.exec_count; });

    ofstream out("edge-profile.csv");

    /* write hottest BBLs */
    for (size_t i = 0; i < sorted.size(); ++i) {
        const BblInfo& bbl = sorted[i];

        out << "0x" << hex << bbl.addr << dec << ", " << bbl.exec_count;

        if (bbl.is_cond_jump) {
            out << ", " << bbl.taken << ", " << bbl.fallthru;
        }

        if (bbl.is_indirect_jump) {
            vector<pair<ADDRINT, UINT64> > targets(bbl.indirect_targets.begin(),
                                                   bbl.indirect_targets.end());
            std::sort(targets.begin(), targets.end(),
                      [](const pair<ADDRINT, UINT64>& a, const pair<ADDRINT, UINT64>& b) {
                          return a.second > b.second;
                      });

            const size_t limit = (targets.size() < 10) ? targets.size() : 10;
            for (size_t j = 0; j < limit; ++j) {
                out << ", 0x" << hex << targets[j].first << dec << ", " << targets[j].second;
            }
        }

        out << "\n";
    }

    out.close();
}

static UINT64 AverageAbsDelta(const RegHistory& hist) {
    UINT64 sum = 0;
    for (UINT32 i = 1; i < hist.count; ++i) {
        const UINT64 prev = hist.values[i - 1];
        const UINT64 cur = hist.values[i];
        sum += (cur >= prev) ? (cur - prev) : (prev - cur);
    }
    return sum / (hist.count - 1);
}


static VOID WriteCsvField(ofstream& out, const string& field) {
    const bool need_quote =
        field.find_first_of(",\"\n\r") != string::npos || field.find(' ') != string::npos;
    if (!need_quote) {
        out << field;
        return;
    }
    out << '"';
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '"')
            out << "\"\"";
        else
            out << field[i];
    }
    out << '"';
}

static string FormatRegValues(const RegHistory& hist) {
    ostringstream oss;
    for (UINT32 i = 0; i < hist.count; ++i) {
        if (i > 0) oss << " -> ";
        oss << "0x" << hex << hist.values[i] << dec;
    }
    return oss.str();
}

static string FormatAvgDeltaHex(UINT64 delta) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(delta));
    return string(buf);
}

static VOID WriteRegCsvFields(ofstream& out, const RegHistory& hist) {
    const string values = FormatRegValues(hist);
    const string has_delta = (hist.count >= 2) ? "Yes" : "No";
    const string avg_delta = (hist.count >= 2) ? FormatAvgDeltaHex(AverageAbsDelta(hist)) : "";

    out << ",";
    WriteCsvField(out, values);
    out << ",";
    WriteCsvField(out, has_delta);
    out << ",";
    WriteCsvField(out, avg_delta);
}

static VOID WriteRtnOutput() {
    ofstream out("rtn-output.csv");

    out << "image,image_base,routine_name,routine_address,instruction_count,call_count,"
        << "RAX_values,RAX_has_avg_delta,RAX_avg_delta,"
        << "RBX_values,RBX_has_avg_delta,RBX_avg_delta,"
        << "RCX_values,RCX_has_avg_delta,RCX_avg_delta,"
        << "RDX_values,RDX_has_avg_delta,RDX_avg_delta,"
        << "RSI_values,RSI_has_avg_delta,RSI_avg_delta,"
        << "RDI_values,RDI_has_avg_delta,RDI_avg_delta\n";

    vector<const RtnStats*> executed;
    for (vector<RtnStats>::iterator it = g_rtnStats.begin(); it != g_rtnStats.end(); ++it) {
        if (it->instruction_count > 0) executed.push_back(&(*it));
    }

    std::sort(executed.begin(), executed.end(), [](const RtnStats* a, const RtnStats* b) {
        return a->instruction_count > b->instruction_count;
    });

    for (UINT32 i = 0; i < executed.size(); ++i) {
        const RtnStats& r = *executed[i];

        WriteCsvField(out, r.image_name);
        out << ",0x" << hex << r.image_address << dec;
        out << ",";
        WriteCsvField(out, r.name);
        out << ",0x" << hex << r.address << dec << "," << r.instruction_count << ","
            << r.number_of_times_called;

        WriteRegCsvFields(out, r.rax);
        WriteRegCsvFields(out, r.rbx);
        WriteRegCsvFields(out, r.rcx);
        WriteRegCsvFields(out, r.rdx);
        WriteRegCsvFields(out, r.rsi);
        WriteRegCsvFields(out, r.rdi);
        out << "\n";
    }

    out.close();
}

VOID Fini(INT32 code, VOID* v) {
    WriteEdgeProfile();
    WriteRtnOutput();
}

int main(int argc, char* argv[]) {
    PIN_InitSymbols();
    if (PIN_Init(argc, argv)) {
        cerr << "Usage: pin -t ex2.so -- <target_app> [args]\n";
        return 1;
    }

    TRACE_AddInstrumentFunction(Trace, 0);
    RTN_AddInstrumentFunction(RoutineInstrumentation, 0);
    PIN_AddFiniFunction(Fini, 0);

    PIN_StartProgram();
    return 0;
}
