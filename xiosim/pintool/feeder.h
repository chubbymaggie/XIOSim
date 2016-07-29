#ifndef __FEEDER_ZESTO__
#define __FEEDER_ZESTO__

/*
 * Molecool: Feeder to Zesto, fed itself by ILDJIT.
 * Copyright, Vijay Reddi, 2007 -- SimpleScalar feeder prototype
              Svilen Kanev, 2011
*/

#include <map>
#include <stack>
#include <unordered_map>

extern "C" {
#include "xed-interface.h"
}

#include "pin.H"
#include "third_party/InstLib/legacy_instlib.H"
using namespace INSTLIB;

#include "xiosim/knobs.h"
#include "xiosim/size_class_cache.h"
#include "xiosim/synchronization.h"
#include "xiosim/zesto-bpred.h"

class handshake_container_t;

extern KNOB<BOOL> KnobILDJIT;
extern KNOB<std::string> KnobSizeClassMode;

/* A list of the threads in this feeder. */
extern list<THREADID> thread_list;
extern XIOSIM_LOCK thread_list_lock;
/* Mapping from a feeder thread to a *virtual* core. We use it to let the application
 * enforce an ordering among threads. When we do something that affects all threads,
 * say, pause the simulation, we traverse thread_list in the order set by virtual_affinity.
 * Only used by HELIX for now, but we can easily hijack pthread_setaffinity. */
extern std::map<THREADID, int> virtual_affinity;

/* Mapping from system-wide thread pid to the Pin-local, zero-based thread id. */
extern std::map<pid_t, THREADID> global_to_local_tid;
extern XIOSIM_LOCK lk_tid_map;

/* Unique address space id -- the # of this feeder among all */
extern int asid;

/* Host TSC values for timing virtualization. */
extern tick_t* initial_timestamps;

#define ATOMIC_ITERATE(_list, _it, _lock)                                                          \
    for (lk_lock(&(_lock), 1), (_it) = (_list).begin(), lk_unlock(&(_lock)); [&] {                 \
        lk_lock(&(_lock), 1);                                                                      \
        bool res = (_it) != (_list).end();                                                         \
        lk_unlock(&(_lock));                                                                       \
        return res;                                                                                \
         }();                                                                                      \
         lk_lock(&(_lock), 1), (_it)++, lk_unlock(&(_lock)))

/* ========================================================================== */
/* Thread-local state for instrument threads that we need to preserve between
 * different instrumentation calls */
class thread_state_t {
    class per_loop_state_t {
      public:
        per_loop_state_t() { unmatchedWaits = 0; }

        INT32 unmatchedWaits;
    };

  public:
    thread_state_t(THREADID instrument_tid) {
        last_syscall_number = last_syscall_arg1 = 0;
        last_syscall_arg2 = last_syscall_arg3 = 0;
        firstIteration = false;
        lastSignalAddr = 0xdecafbad;

        ignore = true;
        ignore_all = true;
        ignore_taken = false;
        firstInstruction = true;

        num_inst = 0;
        lk_init(&lock);

        bpred = new bpred_t(
          nullptr,
          core_knobs.fetch.num_bpred_components,
          core_knobs.fetch.bpred_opt_str,
          core_knobs.fetch.fusion_opt_str,
          core_knobs.fetch.dirjmpbtb_opt_str,
          core_knobs.fetch.indirjmpbtb_opt_str,
          core_knobs.fetch.ras_opt_str
        );
        lastBranchPrediction = 0;
        size_class_cache.set_size(core_knobs.exec.size_class_cache.size);
        size_class_cache.set_tid(tid);
    }

    VOID push_loop_state() {
        per_loop_stack.push(per_loop_state_t());
        loop_state = &(per_loop_stack.top());
    }

    VOID pop_loop_state() {
        per_loop_stack.pop();
        if (per_loop_stack.size()) {
            loop_state = &(per_loop_stack.top());
        }
    }

    // Used by syscall capture code
    ADDRINT last_syscall_number;
    ADDRINT last_syscall_arg1;
    ADDRINT last_syscall_arg2;
    ADDRINT last_syscall_arg3;

    // Return PC for routines that we ignore (e.g. ILDJIT callbacks)
    ADDRINT retPC;

    // How many instructions have been produced
    UINT64 num_inst;

    // Have we executed a wait on this thread
    BOOL firstIteration;

    // Address of the last signal executed
    ADDRINT lastSignalAddr;

    // Global tid for this thread
    pid_t tid;

    // Per Loop State
    per_loop_state_t* loop_state;

    class bpred_t* bpred;
    ADDRINT lastBranchPrediction;

    typedef std::vector<std::pair<ADDRINT, uint8_t>> replacement_mem_ops_t;
    // Addresses for replacement API. Maps from fake ins PC to a list of (address, size) operands.
    // Each fake instruction with a memory operand will grab its addresses from here.
    // Addresses can be set at instrumentation time (if known), or from an earlier analysis
    // routine.
    std::unordered_map<ADDRINT, replacement_mem_ops_t> replacement_mem_ops;

    // Each thread has its own size class cache state.
    SizeClassCache size_class_cache;

    XIOSIM_LOCK lock;
    // XXX: SHARED -- lock protects those
    // Is thread not instrumenting instructions ?
    BOOL ignore;
    // Similar effect to above, but produced differently for sequential code
    BOOL ignore_all;
    // Similar to above, but for when we need to distinguish ignoring the
    // taken path of a branch. Only set when ignore is also set.
    BOOL ignore_taken;
    // Stores the ID of the wait between before and afterWait. -1 outside.
    INT32 lastWaitID;

    BOOL firstInstruction;
    // XXX: END SHARED

  private:
    std::stack<per_loop_state_t> per_loop_stack;
};
thread_state_t* get_tls(THREADID tid);

/* ========================================================================== */
/* Execution mode allows easy querying of exactly what the pin tool is doing at
 * a given time, and also helps ensuring that certain parts of the code are run
 * in only certain modes. */
enum EXECUTION_MODE { EXECUTION_MODE_FASTFORWARD, EXECUTION_MODE_SIMULATE, EXECUTION_MODE_INVALID };
extern EXECUTION_MODE ExecMode;

/* Pause/Resume API. The underlying abstraction is a *simulation slice*.
 * It's just an ROI that we simulate, and ignore anything in between.
 * For regular programs, that's typically one SimPoint, but we abuse it
 * for HELIX to ignore things between parallel loops.
 * The typical sequence of calls is:
 * StartSimSlice(), ResumeSimulation(), SIMULATION_HAPPENS_HERE, PauseSimulation(), EndSimSlice().
 * For HELIX, we do ResumeSimulation(), -----------------------, PauseSimulation() for every
 * parallel loop.
*/

/* Make sure that all sim threads drain any handshake buffers that could be in
 * their respective scheduler run queues.
 * Start ignoring all produced instructions. Deallocate all cores.
 * Invariant: after this call, all sim threads are spinning in SimulatorLoop */
VOID PauseSimulation();

/* Allocate cores. Make sure threads start producing instructions.
 * Schedule threads for simulation. */
VOID ResumeSimulation(bool allocate_cores);

/* Start a new simulation slice (after waiting for all processes).
 * Add instrumentation calls and set ExecMode. */
VOID StartSimSlice(int slice_num);

/* End simulation slice (after waiting for all processes).
 * Remove all instrumetation so we can FF fast between slices. */
VOID EndSimSlice(int slice_num, int slice_length, int slice_weight_times_1000);

/* Add a deschedule handshake to thread @tid. When the consumer eventually
 * consumes that handshake, the scheduler de-schedules thread @tid from its
 * core.
 * If @start_ignoring, the feeder won't produce instructions past the point of
 * this call, until re-enabled.
 * If @reschedule, on de-scheduling the scheduler will re-add the thread to the
 * end of the same core's run queue.
 * */
VOID AddGiveUpHandshake(THREADID tid, bool start_ignoring, bool reschedule);

/* Let the timing simulator catch up with the feeder so the feeder can depend on
 * some timing state -- like returning simulated time to the app.
 * This will call AddGiveUpHandshake(), and then wait until the current thread is
 * de-scheduled. It should be rescheduled with ScheduleThread(). */
VOID SyncWithTimingSim(THREADID tid);

/* Similar to AddGiveUpHandshake(). Except, once consumed, the thread will
 * wait to join @blocked_on. */
void AddBlockedHandshake(THREADID tid, pid_t blocked_on);

void AddAffinityHandshake(THREADID tid, int coreID);

/* Tell the scheduler to schedule a thread. */
VOID ScheduleThread(THREADID tid);

/* Call the current core allocator and get the # of cores we are allowed to use.
 * Some allocators use profiled scaling and serial runtimes to make their decisions.
 * Check out base_allocator.h for the API. */
int AllocateCores(std::vector<double> scaling, double serial_runtime);

/* Helper to check if producer thread @tid will grab instruction at @pc.
 * If return value is false, we can skip instrumentaion. Can hog execution
 * if producers are disabled by producer_sleep. */
BOOL CheckIgnoreConditions(THREADID tid, ADDRINT pc);

/* Insert instrumentation that we didn't add so we can skip ILDJIT compilation even faster. */
VOID doLateILDJITInstrumentation();

/* Print an instruction to the dynamic pc trace. */
/* XXX: We probably need a cleaner "insert fake instruction" API */
VOID printTrace(string stype, ADDRINT pc, pid_t tid);

/* Control the "putting producer threads to sleep" optimization.
 * It helps significantly when we are crunched for cores on the simulation host
 * (e.g. simulating 16-cores on a 16-core machine). */
void disable_producers();
void enable_producers();

#endif /*__FEEDER_ZESTO__ */
