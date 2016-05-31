/* ========================================================================== */
/* ========================================================================== */
/*
 * Molecool: Feeder to Zesto, fed itself by ILDJIT.
 * Copyright, Vijay Reddi, 2007 -- SimpleScalar feeder prototype
              Svilen Kanev, 2011
*/
/* ========================================================================== */
/* ========================================================================== */
#include <iostream>
#include <iomanip>
#include <map>
#include <queue>
#include <set>
#include <list>
#include <sstream>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <syscall.h>
#include <sched.h>
#include <unistd.h>
#include <utility>
#include <signal.h>

#include "xiosim/core_const.h"
#include "xiosim/knobs.h"
#include "xiosim/memory.h"
#include "xiosim/zesto-config.h"

#include "BufferManagerProducer.h"
#include "feeder.h"
#include "ignore_ins.h"
#include "ipc_queues.h"
#include "multiprocess_shared.h"
#include "scheduler.h"

#include "ildjit.h"
#include "paravirt.h"
#include "profiling.h"
#include "replace_function.h"
#include "roi.h"
#include "speculation.h"
#include "sync_pthreads.h"
#include "syscall_handling.h"
#include "tcm_hooks.h"
#include "vdso.h"

using namespace std;

/* ========================================================================== */
/* ========================================================================== */
/*                           ZESTO and PIN INTERFACE                          */
/* ========================================================================== */
/* ========================================================================== */

KNOB<string> KnobInsTraceFile(KNOB_MODE_WRITEONCE, "pintool", "trace", "",
                              "File where instruction trace is written");
KNOB<BOOL> KnobILDJIT(KNOB_MODE_WRITEONCE, "pintool", "ildjit", "false",
                      "Application run is ildjit");
KNOB<BOOL> KnobAMDHack(KNOB_MODE_WRITEONCE,
                       "pintool",
                       "amd_hack",
                       "false",
                       "Using AMD syscall hack for use with hpc cluster");
KNOB<BOOL> KnobWarmLLC(KNOB_MODE_WRITEONCE, "pintool", "warm_llc", "false",
                       "Warm LLC while fast-forwarding");
KNOB<string> KnobConfigFile(KNOB_MODE_WRITEONCE, "pintool", "config" , "",
                            "Simulator configuration file.");
KNOB<pid_t> KnobHarnessPid(KNOB_MODE_WRITEONCE, "pintool", "harness_pid", "-1",
                           "Process id of the harness process.");
KNOB<BOOL> KnobBufferSkipSpaceCheck(KNOB_MODE_WRITEONCE, "pintool", "buffer_skip_space_check",
                                    "false", "Never check for free space in BufferProducer");
KNOB<BOOL> KnobDisableControlROI(KNOB_MODE_WRITEONCE, "pintool", "disable_control_roi", "false",
                                 "Don't use InstLib control hooks");
KNOB<string> KnobBridgeDirs(KNOB_MODE_WRITEONCE, "pintool", "buffer_bridge_dirs", "/dev/shm/,/tmp/",
                            "Buffer bridge location (comma-separated list of directories)");

map<ADDRINT, string> pc_diss;

ofstream pc_file;
ofstream trace_file;

// Used to access thread-local storage
static TLS_KEY tls_key;

// Populated when a thread gets scheduled with the host tsc. All subsequent rdtsc
// insns return this plus an offset.
tick_t* initial_timestamps;

XIOSIM_LOCK thread_list_lock;
list<THREADID> thread_list;
map<THREADID, int> virtual_affinity;

static inline pid_t gettid() { return syscall(SYS_gettid); }
map<pid_t, THREADID> global_to_local_tid;
XIOSIM_LOCK lk_tid_map;

int asid;

xed_state_t dstate;
static void InitXed();

static void InitWatchdog();

/* Feeder-side version of knobs */
core_knobs_t core_knobs;
uncore_knobs_t uncore_knobs;
system_knobs_t system_knobs;

/* ========================================================================== */
/* Pinpoint related */
// Track the number of instructions executed
ICOUNT icount;

// Contains knobs and instrumentation to recognize start/stop points
CONTROL control;

EXECUTION_MODE ExecMode = EXECUTION_MODE_INVALID;

static BOOL in_fini = false;

static bool producers_sleep;
static PIN_SEMAPHORE producers_sem;
static void wait_producers();

/* Wait for all processes to finish fast-forwarding, before starting a
 * simulation
 * slice. Then notify timing_sim to start the slice -- prepare stats, etc. */
static void FastForwardBarrier(int slice_num);
/* Wait for all processes to finish with a simulation slice, and tell timing_sim
 * to mark it as finished -- scale stats, etc. */
static void SliceEndBarrier(int slice_num, int slice_length, int slice_weight_times_1000);

static void addInstrumentationCalls();
static VOID amd_hack();

// Functions to access thread-specific data
/* ========================================================================== */
thread_state_t* get_tls(THREADID threadid) {
    thread_state_t* tstate = static_cast<thread_state_t*>(PIN_GetThreadData(tls_key, threadid));
    return tstate;
}

/* Return the threads for this feeder, ordered by virtual affinity */
/* ========================================================================== */
list<THREADID> GetAffineThreads() {
    /* Order threads by affinity. */
    lk_lock(&thread_list_lock, 1);
    list<THREADID> affine_threads = thread_list;
    affine_threads.sort([](THREADID a, THREADID b) {
        int vaa = virtual_affinity[a];
        int vab = virtual_affinity[b];
        if (vaa == INVALID_CORE && vab != INVALID_CORE)
            return false;
        if (vaa != INVALID_CORE && vab == INVALID_CORE)
            return true;
        return vaa < vab;
    });
    lk_unlock(&thread_list_lock);
    return affine_threads;
}

/* ========================================================================== */
VOID ImageUnload(IMG img, VOID* v) {
    ADDRINT start = IMG_LowAddress(img);
    ADDRINT length = IMG_HighAddress(img) - start;

#ifdef FEEDER_DEBUG
    cerr << "Image unload, addr: " << hex << start << " len: " << length
         << " end_addr: " << start + length << endl;
#endif
    ipc_message_t msg;
    msg.Munmap(asid, start, length, true);
    SendIPCMessage(msg);
}

/* ========================================================================== */
VOID StartSimSlice(int slice_num) {
    /* Gather all processes -- they are all done with the FF -- and tell
     * timing_sim to start the slice */
    FastForwardBarrier(slice_num);

    /* Start instrumenting again */
    ExecMode = EXECUTION_MODE_SIMULATE;
    CODECACHE_FlushCache();
}

/* ========================================================================== */
VOID EndSimSlice(int slice_num, int slice_length, int slice_weight_times_1000) {
    /* Returning from PauseSimulation() guarantees that all sim threads
     * are spinning in SimulatorLoop. So we can safely call Slice_End
     * without racing any of them. */

    /* Now we need to make sure we gather all processes, so we don't race
     * any of them. SliceEndBarrier tells timing_sim to to end the slice */
    SliceEndBarrier(slice_num, slice_length, slice_weight_times_1000);

    /* Stop instrumenting, time to fast forward */
    ExecMode = EXECUTION_MODE_FASTFORWARD;
    CODECACHE_FlushCache();
}

/* ========================================================================== */
VOID SyncWithTimingSim(THREADID tid) {
    thread_state_t* tstate = get_tls(tid);

    /* Tell scheduler to de-schedule thread. */
    AddGiveUpHandshake(tid, false, false);

    /* And wait until it gets de-scheduled. */
    while (IsSHMThreadSimulatingMaybe(tstate->tid)) ;

    /* Feeder and timing sim are now in sync for this thread. */
}

/* ========================================================================== */
VOID AddGiveUpHandshake(THREADID tid, bool start_ignoring, bool reschedule) {
    if (ExecMode != EXECUTION_MODE_SIMULATE)
        return;

    thread_state_t* tstate = get_tls(tid);
    if (start_ignoring) {
        lk_lock(&tstate->lock, tid + 1);
        tstate->ignore = true;
        lk_unlock(&tstate->lock);
    }

    /* When the handshake is consumed, this will let the scheduler de-schedule
     * the thread. */
    handshake_container_t* handshake = xiosim::buffer_management::GetBuffer(tstate->tid);
    handshake->flags.valid = true;
    handshake->flags.real = false;
    handshake->flags.giveCoreUp = true;
    handshake->flags.giveUpReschedule = reschedule;
    xiosim::buffer_management::ProducerDone(tstate->tid);

    xiosim::buffer_management::FlushBuffers(tstate->tid);
}

/* ========================================================================== */
VOID AddBlockedHandshake(THREADID tid, pid_t blocked_on) {
    if (ExecMode != EXECUTION_MODE_SIMULATE)
        return;

    thread_state_t* tstate = get_tls(tid);
    handshake_container_t* handshake = xiosim::buffer_management::GetBuffer(tstate->tid);
    handshake->flags.valid = true;
    handshake->flags.real = false;
    handshake->flags.blockThread = true;
    /* We'll abuse mem_buffer to store the pid to block on for now. */
    handshake->mem_buffer.push_back(std::make_pair(blocked_on, 0));
    xiosim::buffer_management::ProducerDone(tstate->tid);

    xiosim::buffer_management::FlushBuffers(tstate->tid);
}


/* ========================================================================== */
VOID AddAffinityHandshake(THREADID tid, int coreID) {
    if (ExecMode != EXECUTION_MODE_SIMULATE)
        return;

    thread_state_t* tstate = get_tls(tid);
    handshake_container_t* handshake = xiosim::buffer_management::GetBuffer(tstate->tid);
    handshake->flags.valid = true;
    handshake->flags.real = false;
    handshake->flags.setThreadAffinity = true;
    /* We'll abuse mem_buffer to store the coreID we're pinned to. */
    handshake->mem_buffer.push_back(std::make_pair(coreID, 0));
    xiosim::buffer_management::ProducerDone(tstate->tid);

    xiosim::buffer_management::FlushBuffers(tstate->tid);
}

/* ========================================================================== */
VOID ScheduleThread(THREADID tid) {
    if (ExecMode != EXECUTION_MODE_SIMULATE)
        return;

    thread_state_t* tstate = get_tls(tid);
    ipc_message_t msg;
    msg.ScheduleNewThread(tstate->tid);
    SendIPCMessage(msg);

    /* Wait until the thread has been scheduled.
     * Since there is no guarantee when IPC messages are consumed, not
     * waiting can cause it to race to SyncWithTimingSim,
     * before it has been scheduled to run. */
    while (!IsSHMThreadSimulatingMaybe(tstate->tid)) ;
}

/* ========================================================================== */
VOID PPointHandler(CONTROL_EVENT ev, VOID* v, CONTEXT* ctxt, VOID* ip, THREADID tid) {
    cerr << "tid: " << dec << tid << " ip: " << hex << ip << " " << dec;

    /* If we are speculating, and we reach a start or stop point, we are done
     * (before we mess up shared state). */
    if (speculation_mode) {
        FinishSpeculation(get_tls(tid));
        return;
    }

    if (tid < ISIMPOINT_MAX_THREADS)
        cerr << " Inst. Count " << icount.Count(tid) << " ";

    switch (ev) {
    case CONTROL_START: {
        cerr << "Start" << endl;
        UINT32 slice_num = 1;
        if (control.PinPointsActive())
            slice_num = control.CurrentPp(tid);

        StartSimSlice(slice_num);
        ResumeSimulation(true);

        if (control.PinPointsActive())
            cerr << "PinPoint: " << control.CurrentPp(tid)
                 << " PhaseNo: " << control.CurrentPhase(tid) << endl;
    } break;

    case CONTROL_STOP: {
        lk_lock(printing_lock, 1);
        cerr << "Stop" << endl;
        lk_unlock(printing_lock);

        INT32 slice_num = 1;
        INT32 slice_length = 0;
        INT32 slice_weight_times_1000 = 100 * 1000;

        if (control.PinPointsActive()) {
            slice_num = control.CurrentPp(tid);
            slice_length = control.CurrentPpLength(tid);
            slice_weight_times_1000 = control.CurrentPpWeightTimesThousand(tid);
            lk_lock(printing_lock, 1);
            cerr << "PinPoint: " << slice_num << endl;
            lk_unlock(printing_lock);
        }

        PauseSimulation();
        EndSimSlice(slice_num, slice_length, slice_weight_times_1000);

        /* Stop simulation if we've reached the last slice, so we
         * don't wait for fast-forwarding a potentially very long time until the app
         * exits cleanly. */
        if ((control.PinPointsActive() && control.CurrentPp(tid) == control.NumPp(tid)) ||
            control.LengthActive()) {
            PIN_ExitProcess(EXIT_SUCCESS);
        }
    } break;

    default:
        ASSERTX(false);
        break;
    }
}

/* ========================================================================== */
VOID ImageLoad(IMG img, VOID* v) {
    ADDRINT start = IMG_LowAddress(img);
    ADDRINT length = IMG_HighAddress(img) - start;

#ifdef FEEDER_DEBUG
    cerr << "Image load " << IMG_Name(img) << " addr: " << hex << start << " len: " << length
         << " end_addr: " << start + length << dec << endl;
#endif

    // Register callback interface to get notified on ILDJIT events
    if (KnobILDJIT.Value())
        AddILDJITCallbacks(img);

    if (KnobPthreads.Value())
        AddPthreadsCallbacks(img);

    AddROICallbacks(img);

    AddProfilingCallbacks(img);

    AddIgnoredInstructionPCs(img, system_knobs.ignored_pcs);

    ipc_message_t msg;
    msg.Mmap(asid, start, length, false);
    SendIPCMessage(msg);
}

/* ========================================================================== */
// Register that we are about to service Fini callbacks, which cannot
// access some state (e.g. TLS)
VOID BeforeFini(INT32 exitCode, VOID* v) { in_fini = true; }

/* ========================================================================== */
VOID Fini(INT32 exitCode, VOID* v) {
    if (exitCode != EXIT_SUCCESS)
        cerr << "[" << getpid() << "]"
             << "ERROR! Exit code = " << dec << exitCode << endl;
}

/* ========================================================================== */
VOID MakeSSRequest(THREADID tid,
                   ADDRINT pc,
                   ADDRINT npc,
                   ADDRINT tpc,
                   BOOL brtaken,
                   ADDRINT esp_value,
                   handshake_container_t* hshake) {
    hshake->asid = asid;
    /* PC already set signals that some prior instrumentation has prepped the hshake for
     * us. For now, this is only fake_ins, and only affects the real flag. */
    if (hshake->pc != pc) {
        hshake->pc = pc;
        hshake->flags.real = true;
    }
    hshake->npc = npc;
    hshake->tpc = tpc;
    hshake->flags.brtaken = brtaken;
    hshake->flags.speculative = speculation_mode;
    PIN_SafeCopy(hshake->ins, (VOID*)pc, x86::MAX_ILEN);

    // Ok, we might need the stack pointer for shadow page tables.
    hshake->rSP = esp_value;
}

/* Helper to check if producer thread @tid will grab instruction at @pc.
 * If return value is false, we can skip instrumentaion. Can hog execution
 * if producers are disabled by producer_sleep. */
BOOL CheckIgnoreConditions(THREADID tid, ADDRINT pc) {
    wait_producers();

    thread_state_t* tstate = get_tls(tid);

    /* Check thread ignore and ignore_all flags */
    lk_lock(&tstate->lock, tid + 1);
    if (tstate->ignore || tstate->ignore_all) {
        lk_unlock(&tstate->lock);
        return false;
    }
    lk_unlock(&tstate->lock);

    /* Check ignore API for unnecessary instructions */
    if (IsInstructionIgnored(pc))
        return false;

    return true;
}

/* Helper to grab the correct buffer that we put instrumentation information
 * for thread @tid.
 * Either a new one, or the last one being filled in, that is still not marked valid. */
static handshake_container_t* GetProperBuffer(pid_t tid) {
    handshake_container_t* res;
    res = xiosim::buffer_management::GetBuffer(tid);
    ASSERTX(res != NULL);
    ASSERTX(!res->flags.valid);
    return res;
}

/* Helper to mark the buffer of a fully instrumented instruction as valid,
 * and let it get eventually consumed. */
static VOID FinalizeBuffer(thread_state_t* tstate, handshake_container_t* handshake) {
    // Let simulator consume instruction from SimulatorLoop
    handshake->flags.valid = true;
    xiosim::buffer_management::ProducerDone(tstate->tid);
}

/* ========================================================================== */
/* We grab the addresses and sizes of memory operands. */
VOID GrabInstructionMemory(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT pc) {
    if (!CheckIgnoreConditions(tid, pc))
        return;

    thread_state_t* tstate = get_tls(tid);
    handshake_container_t* handshake = GetProperBuffer(tstate->tid);

    handshake->mem_buffer.push_back(std::make_pair(addr, size));
}

/* ========================================================================== */
VOID GrabInstructionContext(THREADID tid,
                            ADDRINT pc,
                            BOOL taken,
                            ADDRINT npc,
                            ADDRINT tpc,
                            ADDRINT esp_value,
                            BOOL done_instrumenting) {
    if (!CheckIgnoreConditions(tid, pc)) {
#ifdef PRINT_DYN_TRACE
        printTrace("jit", pc, tid);
#endif
        return;
    }

#ifdef PRINT_DYN_TRACE
    printTrace("sim", pc, tid);
#endif

    thread_state_t* tstate = get_tls(tid);
    handshake_container_t* handshake = GetProperBuffer(tstate->tid);

    tstate->num_inst++;

    lk_lock(&tstate->lock, tid + 1);
    BOOL first_insn = tstate->firstInstruction;
    lk_unlock(&tstate->lock);
    if (first_insn) {
        handshake->flags.isFirstInsn = true;
        lk_lock(&tstate->lock, tid + 1);
        tstate->firstInstruction = false;
        lk_unlock(&tstate->lock);
    }

    // Populate handshake buffer
    MakeSSRequest(tid, pc, NextUnignoredPC(npc), NextUnignoredPC(tpc), taken, esp_value, handshake);

    // If no more steps, instruction is ready to be consumed
    if (done_instrumenting) {
        FinalizeBuffer(tstate, handshake);
    }
}

/* ========================================================================== */
/* If we treat REP instructions as loops and pass them along to the simulator,
 * we need a good ground truth for the NPC that the simulator can rely on,
 * because Pin doesn't do that for us the way it does branch NPCs.
 * So, we add extra instrumentation for REP instructions to determine if this is
 * the last iteration. */
VOID FixRepInstructionNPC(THREADID tid,
                          ADDRINT pc,
                          BOOL rep_prefix,
                          BOOL repne_prefix,
                          ADDRINT counter_value,
                          ADDRINT rax_value,
                          UINT32 opcode) {
    if (!CheckIgnoreConditions(tid, pc))
        return;

    thread_state_t* tstate = get_tls(tid);
    handshake_container_t* handshake = GetProperBuffer(tstate->tid);

    BOOL scan = false, zf = false;
    ADDRINT op1 = 0;
    ADDRINT op2 = 0;

    // REPE and REPNE only matter for CMPS and SCAS,
    // so we special-case them
    switch (opcode) {
    case XED_ICLASS_CMPSB:
    case XED_ICLASS_CMPSW:
    case XED_ICLASS_CMPSD:
    case XED_ICLASS_CMPSQ:
        // CMPS does two mem reads of the same size
        {
            auto& mem_buffer = handshake->mem_buffer;
            ASSERTX(mem_buffer.size() == 2);
            ADDRINT addr1 = mem_buffer[0].first;
            ADDRINT addr2 = mem_buffer[1].first;
            UINT8 mem_size = mem_buffer[0].second;
            PIN_SafeCopy(&op1, (VOID*)addr1, mem_size);
            PIN_SafeCopy(&op2, (VOID*)addr2, mem_size);
        }
        scan = true;
        zf = (op1 == op2);
        break;
    case XED_ICLASS_SCASB:
    case XED_ICLASS_SCASW:
    case XED_ICLASS_SCASD:
    case XED_ICLASS_SCASQ:
        // SCAS only does one read, gets second operand from rAX
        {
            auto& mem_buffer = handshake->mem_buffer;
            ASSERTX(mem_buffer.size() == 1);
            ADDRINT addr = mem_buffer[0].first;
            UINT8 mem_size = mem_buffer[0].second;
            PIN_SafeCopy(&op1, (VOID*)addr, mem_size);

            op2 = rax_value;  // 0-extended anyways
        }
        scan = true;
        zf = (op1 == op2);
        break;
    case XED_ICLASS_INSB:
    case XED_ICLASS_INSW:
    case XED_ICLASS_INSD:
    case XED_ICLASS_OUTSB:
    case XED_ICLASS_OUTSW:
    case XED_ICLASS_OUTSD:
    case XED_ICLASS_LODSB:
    case XED_ICLASS_LODSW:
    case XED_ICLASS_LODSD:
    case XED_ICLASS_LODSQ:
    case XED_ICLASS_STOSB:
    case XED_ICLASS_STOSW:
    case XED_ICLASS_STOSD:
    case XED_ICLASS_STOSQ:
    case XED_ICLASS_MOVSB:
    case XED_ICLASS_MOVSW:
    case XED_ICLASS_MOVSD:
    case XED_ICLASS_MOVSQ:
        scan = false;
        break;
    default:
        ASSERTX(false);
        break;
    }

    ADDRINT NPC;
    // Counter says we finish after this instruction (for all prefixes)
    if (counter_value == 1 || counter_value == 0)
        NPC = handshake->tpc;
    // Zero flag and REPE/REPNE prefixes say we finish after this instruction
    else if (scan && ((repne_prefix && zf) || (rep_prefix && !zf)))
        NPC = handshake->tpc;
    // Otherwise we just keep looping
    else
        NPC = handshake->pc;

    // Update with hard-earned NPC
    handshake->npc = NPC;

    // Instruction is ready to be consumed
    FinalizeBuffer(tstate, handshake);
}

/* ========================================================================== */
// Trivial call to let us do conditional instrumentation based on an argument
ADDRINT returnArg(BOOL arg) { return arg; }

VOID WarmCacheRead(VOID* addr) {
#if 0
    xiosim::libsim::simulate_warmup((ADDRINT)addr, false);
#endif
}

VOID WarmCacheWrite(VOID* addr) {
#if 0
    xiosim::libsim::simulate_warmup((ADDRINT)addr, true);
#endif
}

/* ========================================================================== */
VOID Instrument(INS ins, VOID* v) {
    // ILDJIT is doing its initialization/compilation/...
    if (KnobILDJIT.Value() && !ILDJIT_IsExecuting())
        return;

    // Tracing
    ADDRINT pc = INS_Address(ins);
    if (!KnobInsTraceFile.Value().empty()) {
        USIZE size = INS_Size(ins);

        trace_file << pc << " " << INS_Disassemble(ins);
        pc_diss[pc] = string(INS_Disassemble(ins));
        for (INT32 curr = size - 1; curr >= 0; curr--)
            trace_file << " " << int(*(UINT8*)(curr + pc));
        trace_file << endl;
    }

    // Not executing yet, only warm caches, if needed
    if (ExecMode != EXECUTION_MODE_SIMULATE) {
        if (KnobWarmLLC.Value()) {
            UINT32 memOperands = INS_MemoryOperandCount(ins);

            // Iterate over each memory operand of the instruction.
            for (UINT32 memOp = 0; memOp < memOperands; memOp++) {
                if (INS_MemoryOperandIsRead(ins, memOp)) {
                    INS_InsertPredicatedCall(ins,
                                             IPOINT_BEFORE,
                                             (AFUNPTR)WarmCacheRead,
                                             IARG_MEMORYOP_EA,
                                             memOp,
                                             IARG_END);
                }
                if (INS_MemoryOperandIsWritten(ins, memOp)) {
                    INS_InsertPredicatedCall(ins,
                                             IPOINT_BEFORE,
                                             (AFUNPTR)WarmCacheWrite,
                                             IARG_MEMORYOP_EA,
                                             memOp,
                                             IARG_END);
                }
            }
        }
        return;
    }

    /* Add instrumentation that captures each memory operand */
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    for (UINT32 memOp = 0; memOp < memOperands; memOp++) {
        UINT32 memSize = INS_MemoryOperandSize(ins, memOp);
        INS_InsertCall(ins,
                       IPOINT_BEFORE,
                       (AFUNPTR)GrabInstructionMemory,
                       IARG_THREAD_ID,
                       IARG_MEMORYOP_EA,
                       memOp,
                       IARG_UINT32,
                       memSize,
                       IARG_INST_PTR,
                       IARG_END);
    }

    if (!INS_IsBranchOrCall(ins)) {
        BOOL extraRepInstrumentation = INS_HasRealRep(ins);
        INS_InsertCall(ins,
                       IPOINT_BEFORE,
                       (AFUNPTR)GrabInstructionContext,
                       IARG_THREAD_ID,
                       IARG_INST_PTR,
                       IARG_BOOL,
                       0,
                       IARG_ADDRINT,
                       INS_NextAddress(ins),
                       IARG_FALLTHROUGH_ADDR,
                       IARG_REG_VALUE,
                       LEVEL_BASE::REG_FullRegName(LEVEL_BASE::REG_ESP),
                       IARG_BOOL,
                       !extraRepInstrumentation,
                       IARG_END);

        if (extraRepInstrumentation)
            INS_InsertCall(ins,
                           IPOINT_BEFORE,
                           (AFUNPTR)FixRepInstructionNPC,
                           IARG_THREAD_ID,
                           IARG_INST_PTR,
                           IARG_BOOL,
                           INS_RepPrefix(ins),
                           IARG_BOOL,
                           INS_RepnePrefix(ins),
                           IARG_REG_VALUE,
                           INS_RepCountRegister(ins),
                           IARG_REG_VALUE,
                           LEVEL_BASE::REG_FullRegName(LEVEL_BASE::REG_EAX),
                           IARG_ADDRINT,
                           INS_Opcode(ins),
                           IARG_END);
    } else {
        // Branch, give instrumentation appropriate address
        INS_InsertCall(ins,
                       IPOINT_BEFORE,
                       (AFUNPTR)GrabInstructionContext,
                       IARG_THREAD_ID,
                       IARG_INST_PTR,
                       IARG_BRANCH_TAKEN,
                       IARG_ADDRINT,
                       INS_NextAddress(ins),
                       IARG_BRANCH_TARGET_ADDR,
                       IARG_REG_VALUE,
                       LEVEL_BASE::REG_FullRegName(LEVEL_BASE::REG_ESP),
                       IARG_BOOL,
                       true,
                       IARG_END);
    }
}

/* ========================================================================== */
VOID ThreadStart(THREADID threadIndex, CONTEXT* ictxt, INT32 flags, VOID* v) {
    lk_lock(&syscall_lock, 1);
#ifdef FEEDER_DEBUG
    cerr << "Thread start tid: " << dec << threadIndex << endl;
#endif

    thread_state_t* tstate = new thread_state_t(threadIndex);
    PIN_SetThreadData(tls_key, tstate, threadIndex);

    ADDRINT tos, bos;
    tos = PIN_GetContextReg(ictxt, LEVEL_BASE::REG_FullRegName(LEVEL_BASE::REG_ESP));
    // Try to find bottom of the stack if we are starting at main().
    // Walk until we reach the end of the environment.
    if (threadIndex == 0 && !PIN_IsAttaching()) {
        CHAR** sp = (CHAR**)tos;
        UINT32 argc = *(UINT32*)sp;
        for (UINT32 i = 0; i < argc; i++) {
            sp++;
        }
        CHAR* last_argv = *sp;
        sp++;  // End of argv (=NULL);

        sp++;  // Start of envp

        CHAR** envp = sp;
        while (*envp != NULL) {
            envp++;
        }  // End of envp

        CHAR* last_env = *(envp - 1);

#ifdef WE_DONT_NEED_NO_STINKING_LIBC
        auxv_start = (uintptr_t)(envp + 1);  // Skip end of envp (=NULL)
#endif

        if (last_env != NULL)
            bos = (ADDRINT)last_env + strlen(last_env) + 1;
        else
            bos = (ADDRINT)last_argv + strlen(last_argv) + 1;  // last_argv != NULL

        // Reserve space for environment and arguments in case
        // execution starts on another thread.
        ADDRINT tos_start = xiosim::memory::page_round_down(tos);
        ADDRINT bos_end = xiosim::memory::page_round_up(bos);
        ipc_message_t bos_msg;
        bos_msg.Mmap(asid, tos_start, bos_end - tos_start, false);
        SendIPCMessage(bos_msg);
    } else {
        bos = tos;
    }

    // Map a page for the VDSO.
    if (threadIndex == 0) {
        ADDRINT vdso = vdso_addr();
#ifdef FEEDER_DEBUG
        cerr << "VDSO address: " << hex << vdso << endl;
#endif
        ipc_message_t vdso_msg;
        vdso_msg.Mmap(asid, vdso, xiosim::memory::PAGE_SIZE, false);
        SendIPCMessage(vdso_msg);
    }

    // Application threads only -- create buffers for them
    if (!KnobILDJIT.Value() || (KnobILDJIT.Value() && ILDJIT_IsCreatingExecutor())) {
        /* Store globally unique thread id */
        tstate->tid = gettid();

        // Create new buffer to store thread context
        xiosim::buffer_management::AllocateThreadProducer(tstate->tid);

        lk_lock(&lk_tid_map, 1);
        global_to_local_tid[tstate->tid] = threadIndex;
        lk_unlock(&lk_tid_map);

        // Mark thread as belonging to this process
        lk_lock(lk_threadProcess, 1);
        threadProcess->operator[](tstate->tid) = asid;
        lk_unlock(lk_threadProcess);

        // Remember bottom of stack for this thread globally
        lk_lock(lk_thread_bos, 1);
        thread_bos->operator[](tstate->tid) = bos;
        lk_unlock(lk_thread_bos);

        ScheduleThread(threadIndex);

        if (!KnobILDJIT.Value() && ExecMode == EXECUTION_MODE_SIMULATE) {
            lk_lock(&tstate->lock, threadIndex + 1);
            tstate->ignore = false;
            tstate->ignore_all = false;
            lk_unlock(&tstate->lock);
        }

        /* Add to thread_list in the end, so other threads that iterate
         * it don't race on a not yet fully initialized structure.
         */
        lk_lock(&thread_list_lock, threadIndex + 1);
        virtual_affinity[threadIndex] = INVALID_CORE;
        thread_list.push_back(threadIndex);
        lk_unlock(&thread_list_lock);
    }
    lk_unlock(&syscall_lock);
}

/* ========================================================================== */
int AllocateCores(vector<double> scaling, double serial_runtime) {
    /* Ask timing_sim for an allocation, and block until it comes back. */
    ipc_message_t msg;
    msg.AllocateCores(asid, scaling, serial_runtime);
    SendIPCMessage(msg, /*blocking*/ true);

    /* Here we've finished with the allocation decision. */
    int allocation = GetProcessCoreAllocation(asid);
#ifdef ALLOCATOR_DEBUG
    cerr << "ASID: " << asid << " allocated " << allocation << " cores." << endl;
#endif
    ASSERTX(allocation > 0);
    return allocation;
}

/* ========================================================================== */
VOID DeallocateCores() {
    CoreSet empty_set;
    UpdateProcessCoreSet(asid, empty_set);

    ipc_message_t msg;
    msg.DeallocateCores(asid);
    SendIPCMessage(msg, /*blocking*/ true);
}

/* ========================================================================== */
VOID PauseSimulation() {
    /* An INT 80 instruction */
    static const UINT8 syscall_template[] = { 0xcd, 0x80 };

    /* Here we have produced everything, we can only consume */
    disable_producers();

    /* Flush all buffers and cleanly let the scheduler know to deschedule
     * once all instructions are conusmed.
     * XXX: This can be called from a Fini callback (end of program).
     * We can't access any TLS in that case. */
    if (!in_fini) {
        /* Get threads ordered by affinity */
        auto affine_threads = GetAffineThreads();

        for (THREADID tid : affine_threads) {
            thread_state_t* tstate = get_tls(tid);

            lk_lock(&tstate->lock, 1);
            tstate->ignore_all = true;
            lk_unlock(&tstate->lock);

            /* If the thread is not seen in the scheduler queues
             * *before de-scheduling it*, just ignore it. This covers
             * the HELIX case, where we ignore extra threads past the
             * number of allocated cores. */
            if (!IsSHMThreadSimulatingMaybe(tstate->tid))
                continue;

            bool first_thread = (tid == affine_threads.front());
            if (KnobILDJIT.Value())
                InsertHELIXPauseCode(tid, first_thread);

            /* Insert a trap. This will ensure that the pipe drains before
             * consuming the next instruction.*/
            pid_t curr_tid = tstate->tid;
            auto trap_hshake = xiosim::buffer_management::GetBuffer(curr_tid);
            trap_hshake->flags.real = false;
            trap_hshake->asid = asid;
            trap_hshake->flags.valid = true;

            trap_hshake->pc = (ADDRINT)syscall_template;
            trap_hshake->npc = (ADDRINT)syscall_template + sizeof(syscall_template);
            trap_hshake->tpc = (ADDRINT)syscall_template + sizeof(syscall_template);
            trap_hshake->flags.brtaken = false;
            memcpy(trap_hshake->ins, syscall_template, sizeof(syscall_template));
            xiosim::buffer_management::ProducerDone(curr_tid, true);

            /* When the handshake is consumed, this will let the scheduler de-schedule the thread */
            AddGiveUpHandshake(tid, false, false);
        }
    }

    /* Wait until all the processes' consumer threads are done consuming.
     * Since the scheduler is updated from the sim threads in SimulatorLoop,
     * seeing empty run queues is enough to determine that the sim thread
     * is done simulating instructions. No need for fancier synchronization. */
    bool threads_done;
    do {
        threads_done = true;
        list<THREADID>::iterator it;
        ATOMIC_ITERATE(thread_list, it, thread_list_lock) {
            thread_state_t* tstate = get_tls(*it);
            threads_done &= !IsSHMThreadSimulatingMaybe(tstate->tid);
        }
        if (!threads_done && *sleeping_enabled)
            PIN_Sleep(10);
    } while (!threads_done);

    /* Re-enable producers once we are past the barrier. This way, they can
     * continue
     * ignoring instructions until the next slice. */
    enable_producers();

    /* We are done with de-scheduling threads, now we can de-allocate the cores
     * for this process */
    DeallocateCores();
}

/* ========================================================================== */
VOID ResumeSimulation(bool allocate_cores) {
    /* Get cores for the allocator */
    if (allocate_cores)
        AllocateCores(vector<double>(), 0);

    /* Make sure threads start producing instructions */
    list<THREADID>::iterator it;
    ATOMIC_ITERATE(thread_list, it, thread_list_lock) {
        thread_state_t* tstate = get_tls(*it);
        lk_lock(&tstate->lock, 1);
        tstate->firstInstruction = true;
        tstate->ignore_all = false;
        if (!KnobILDJIT.Value())
            tstate->ignore = false;
        lk_unlock(&tstate->lock);
    }

    /* Get threads ordered by affinity */
    auto affine_threads = GetAffineThreads();

    size_t allocation = GetProcessCoreAllocation(asid);
    ASSERTX(allocation > 0);

    list<pid_t> scheduled_threads;
    bool no_oversubscribe = KnobILDJIT.Value();
    /* If the scheduler doesn't oversubscribe (HELIX), pick the first
     * @allocation threads, based on virtual affinity. Otherwise,
     * schedule all threads and let them round-robin on the scheduler queues. */
    for (THREADID curr_tid : affine_threads) {
        thread_state_t* tstate = get_tls(curr_tid);
        scheduled_threads.push_back(tstate->tid);

        if (no_oversubscribe && (scheduled_threads.size() == allocation))
            break;
    }

    /* Let the scheduler schedule all these threads. */
    ipc_message_t msg;
    msg.ScheduleProcessThreads(asid, scheduled_threads);
    SendIPCMessage(msg);

    /* Wait until all threads have been scheduled.
     * Since there is no guarantee when IPC messages are consumed, not
     * waiting can cause a fast thread to race to PauseSimulation,
     * before everyone has been scheduled to run. */
    bool done;
    do {
        done = true;
        for (pid_t curr_tid : scheduled_threads) {
            done &= IsSHMThreadSimulatingMaybe(curr_tid);
        }
    } while (!done);

    // Record initial timestamps for these cores if not recorded yet.
    for (pid_t curr_tid : scheduled_threads) {
        int core_id = GetSHMThreadCore(curr_tid);
        if (initial_timestamps[core_id] == TICK_T_MAX) {
            tick_t host_timestamp = 0;
            uint32_t lo, hi;
            __asm__("rdtsc" : "=a"(lo), "=d"(hi));
            host_timestamp = hi;
            host_timestamp <<= 32;
            host_timestamp |= lo;
            initial_timestamps[core_id] = host_timestamp;
        }
    }
}

/* ========================================================================== */
VOID ThreadFini(THREADID tid, const CONTEXT* ctxt, INT32 code, VOID* v) {
    /* Speculation, stop before we corrupt global state. */
    if (speculation_mode) {
        FinishSpeculation(get_tls(tid));
        return;
    }

    thread_state_t* tstate = get_tls(tid);

    lk_lock(printing_lock, tid + 1);
    cerr << "[" << tstate->tid << "] Thread exit. ID: " << tid << endl;
    lk_unlock(printing_lock);

    /* There will be no further instructions instrumented (on this thread).
     * Mark it as finishing and let the handshake buffer drain.
     * Once this last handshake gets executed by a core, it will make
       sure to clean up all thread resources. */
    handshake_container_t* handshake = xiosim::buffer_management::GetBuffer(tstate->tid);
    handshake->flags.killThread = true;
    handshake->flags.valid = true;
    handshake->flags.real = false;
    xiosim::buffer_management::ProducerDone(tstate->tid);

    xiosim::buffer_management::FlushBuffers(tstate->tid);

    /* Ignore subsequent instructions that we may see on this thread before
     * destroying its tstate.
     * XXX: This might be bit paranoid depending on when Pin inserts the
     * ThreadFini callback. */
    lk_lock(&tstate->lock, tid + 1);
    tstate->ignore = true;
    lk_unlock(&tstate->lock);
}

/* ========================================================================== */
INT32 main(INT32 argc, CHAR** argv) {
#ifdef FEEDER_DEBUG
    cerr << "[" << getpid() << "]"
         << " feeder_zesto args: ";
    for (int i = 0; i < argc; i++)
        cerr << argv[i] << " ";
    cerr << endl;
#endif

    // Obtain  a key for TLS storage.
    tls_key = PIN_CreateThreadDataKey(0);

    PIN_Init(argc, argv);
    PIN_InitSymbols();

    PIN_SemaphoreInit(&producers_sem);

    read_config_file(KnobConfigFile.Value(), &core_knobs, &uncore_knobs, &system_knobs);
    int num_cores = system_knobs.num_cores;

    // Synchronize all processes here to ensure that in multiprogramming mode,
    // no process will start too far before the others.
    asid = InitSharedState(true, KnobHarnessPid.Value(), num_cores);
    xiosim::buffer_management::InitBufferManagerProducer(
            KnobHarnessPid.Value(), KnobBufferSkipSpaceCheck.Value(), KnobBridgeDirs.Value());

    if (KnobAMDHack.Value()) {
        amd_hack();
    }

    if (KnobILDJIT.Value()) {
        MOLECOOL_Init();
    }

    if (!KnobDisableControlROI.Value() && !KnobILDJIT.Value() && !KnobROI.Value()) {
        // Try activate pinpoints alarm, must be done before PIN_StartProgram
        if (control.CheckKnobs(PPointHandler, 0) != 1) {
            cerr << "Error reading control parametrs, exiting." << endl;
            return 1;
        }
    }

    icount.Activate();

    if (!KnobInsTraceFile.Value().empty()) {
        trace_file.open(KnobInsTraceFile.Value().c_str());
        trace_file << hex;
        pc_file.open("pcs.trace");
        pc_file << hex;
    }

    // Delay this instrumentation until startSimulation call in ILDJIT.
    // This cuts down HELIX compilation noticably for integer benchmarks.

    if (!KnobILDJIT.Value()) {
        addInstrumentationCalls();
    }

    PIN_AddThreadStartFunction(ThreadStart, NULL);
    PIN_AddThreadFiniFunction(ThreadFini, NULL);
    //    IMG_AddUnloadFunction(ImageUnload, 0);
    IMG_AddInstrumentFunction(ImageLoad, 0);
    PIN_AddFiniUnlockedFunction(BeforeFini, 0);
    PIN_AddFiniFunction(Fini, 0);
    InitSyscallHandling();
    InitXed();
    InitWatchdog();

    for (std::string func : system_knobs.ignored_funcs)
        IgnoreFunction(func);

    initial_timestamps = new tick_t[num_cores];
    for (int i = 0; i < num_cores; i++)
        initial_timestamps[i] = TICK_T_MAX;

    *sleeping_enabled = true;
    enable_producers();

    PIN_StartProgram();

    return 0;
}

static VOID amd_hack() {
#ifdef _LP64
    cerr << "AMD hack only matters on ia32." << endl;
    abort();
#endif
    // use kernel version to distinguish between RHEL5 and RHEL6
    bool rhel6 = false;

    ifstream procversion("/proc/version");
    string version((istreambuf_iterator<char>(procversion)), istreambuf_iterator<char>());

    if (version.find(".el6.") != string::npos) {
        rhel6 = true;
    } else if (version.find(".el5 ") == string::npos) {
        if (version.find(".el5.") == string::npos) {
            if (version.find(".el5_") == string::npos) {
                cerr << "ERROR! Neither .el5 nor .el6 occurs in /proc/version" << endl;
                abort();
            }
        }
    }

    // under RHEL6, the VDSO page location can vary from build to build
    unsigned long vdso_begin, vdso_end = 0;

    if (rhel6) {
        ifstream maps("/proc/self/maps");
        string line;

        while (getline(maps, line))
            if (line.find("[vdso]") != string::npos) {
                istringstream linestream(line);
                linestream >> hex;

                if (linestream >> vdso_begin && linestream.get() == '-' && linestream >> vdso_end)
                    break;
                cerr << "ERROR! Badly formatted [vdso] map line: " << line << endl;
                abort();
            }

        if (vdso_end == 0) {
            cerr << "ERROR! No VDSO page map in /proc/self/maps" << endl;
            abort();
        } else if (vdso_end - vdso_begin != 0x1000) {
            cerr << "ERROR! VDSO page size isn't 0x1000 in /proc/self/maps" << endl;
            abort();
        }
    } else {
        vdso_begin = 0xffffe000;
    }

    int returnval = mprotect((void*)vdso_begin, 0x1000, PROT_EXEC | PROT_READ | PROT_WRITE);
    if (returnval != 0) {
        perror("mprotect");
        cerr << hex << "VDSO page is at " << vdso_begin << endl;
        abort();
    }

    // offset of __kernel_vsyscall() is slightly later under RHEL6 than under
    // RHEL5
    unsigned vsyscall_offset = rhel6 ? 0x420 : 0x400;

    // write int80 at the begining of __kernel_vsyscall()
    *(char*)(vdso_begin + vsyscall_offset + 0) = 0xcd;
    *(char*)(vdso_begin + vsyscall_offset + 1) = 0x80;

    // ... and follow it by a ret
    *(char*)(vdso_begin + vsyscall_offset + 2) = 0xc3;
}

static void addInstrumentationCalls() {
    /* Order matters here.
     * TCMHoks sets up data for InsIgnoring, so it has to come before it.
     * InsIgnoring sets up data for Instrument, so ditto. */
    TRACE_AddInstrumentFunction(InstrumentTCMHooks, 0);
    TRACE_AddInstrumentFunction(InstrumentInsIgnoring, 0);
    INS_AddInstrumentFunction(Instrument, 0);
    INS_AddInstrumentFunction(InstrumentSpeculation, 0);
    TRACE_AddInstrumentFunction(InstrumentParavirt, 0);
}

VOID doLateILDJITInstrumentation() {
    static bool calledAlready = false;

    ASSERTX(!calledAlready);

    GetVmLock();
    addInstrumentationCalls();
    CODECACHE_FlushCache();
    ReleaseVmLock();

    calledAlready = true;
}

VOID printTrace(string stype, ADDRINT pc, pid_t tid) {
    if (ExecMode != EXECUTION_MODE_SIMULATE) {
        return;
    }

    lk_lock(printing_lock, tid + 1);
    pc_file << tid << " "
            << " " << speculation_mode << " " << stype << " " << pc << " " << pc_diss[pc] << endl;
    pc_file.flush();
    lk_unlock(printing_lock);
}

void disable_producers() {
    if (*sleeping_enabled) {
        if (!producers_sleep)
            PIN_SemaphoreClear(&producers_sem);
        producers_sleep = true;
    }
}

void enable_producers() {
    if (producers_sleep)
        PIN_SemaphoreSet(&producers_sem);
    producers_sleep = false;
}

static void wait_producers() {
    if (!*sleeping_enabled)
        return;

    if (producers_sleep)
        PIN_SemaphoreWait(&producers_sem);
}

/* ========================================================================== */
static void FastForwardBarrier(int slice_num) {
    lk_lock(lk_num_done_fastforward, 1);
    (*num_done_fastforward)++;
    int processes_at_barrier = *num_done_fastforward;

    /* Wait until all processes have gathered at this barrier.
     * Disable own producers -- they are not doing anything useful.
     */
    while (*fastforward_epoch < slice_num && *num_done_fastforward < *num_processes) {
        lk_unlock(lk_num_done_fastforward);
        disable_producers();
        xio_sleep(100);
        lk_lock(lk_num_done_fastforward, 1);
    }

    /* If this is the last process on the barrier, start a simulation slice. */
    if (processes_at_barrier == *num_processes) {
        (*fastforward_epoch)++;
        *num_done_fastforward = 0;

        ipc_message_t msg;
        msg.SliceStart(slice_num);
        SendIPCMessage(msg);
    }

    /* And let producers produce. */
    enable_producers();
    lk_unlock(lk_num_done_fastforward);
}

/* ========================================================================== */
static void SliceEndBarrier(int slice_num, int slice_length, int slice_weight_times_1000) {
    lk_lock(lk_num_done_slice, 1);
    (*num_done_slice)++;
    int processes_at_barrier = *num_done_slice;

    /* Wait until all processes have gathered at this barrier. */
    while (*slice_epoch < slice_num && *num_done_slice < *num_processes) {
        lk_unlock(lk_num_done_slice);
        disable_producers();
        xio_sleep(100);
        lk_lock(lk_num_done_slice, 1);
    }

    /* If this is the last process on the barrier, end a simulation slice. */
    if (processes_at_barrier == *num_processes) {
        (*slice_epoch)++;
        *num_done_slice = 0;

        ipc_message_t msg;
        msg.SliceEnd(slice_num, slice_length, slice_weight_times_1000);
        SendIPCMessage(msg);
    }

    lk_unlock(lk_num_done_slice);
}

/* ========================================================================== */
static void InitXed() {
    xed_tables_init();
#ifdef _LP64
    xed_state_init2(&dstate, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);
#else
    xed_state_init2(&dstate, XED_MACHINE_MODE_LONG_COMPAT_32, XED_ADDRESS_WIDTH_32b);
#endif
}

/* ========================================================================== */
void WatchdogHandler(int signal) {
    if (signal != SIGVTALRM)
        return;

    feeder_watchdogs[asid] = time(nullptr);
}

/* ========================================================================== */
static void InitWatchdog() {
    /* Unblock SIGVTALRM. */
    sigset_t oldset;
    sigemptyset(&oldset);
    sigaddset(&oldset, SIGVTALRM);
    sigprocmask(SIG_UNBLOCK, &oldset, nullptr);

    /* Set SIGVTALRM signal handler.
     * We don't use PIN_AddContextChangeFunction() because we run pin with
     * -catch_signals false -- see speculation.cpp. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &WatchdogHandler;
    int res = sigaction(SIGVTALRM, &sa, nullptr);
    if (res != 0) {
        perror(nullptr);
        abort();
    }

    /* Set up watchdog timer. */
    const time_t INTERVAL_SEC = 5;
    const struct itimerval watchdog_interval {
        { INTERVAL_SEC, 0 }
        , { INTERVAL_SEC, 0 }
    };
    res = setitimer(ITIMER_VIRTUAL, &watchdog_interval, nullptr);
    if (res != 0) {
        perror(nullptr);
        abort();
    }
}
