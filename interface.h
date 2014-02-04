/* 
 * Interface to instruction feeder.
 * Copyright, Svilen Kanev, 2011
*/

#ifndef __PIN_ZESTO_INTERFACE__
#define __PIN_ZESTO_INTERFACE__

#include "synchronization.h"
#include "machine.h"
#include "pin.h"
#include "helix.h"

/* Calls from feeder to Zesto */
int Zesto_SlaveInit(int argc, char **argv);
void Zesto_Resume(int coreID, handshake_container_t * handshake);
void Zesto_Destroy();

void Zesto_Notify_Mmap(int asid, unsigned int addr, unsigned int length, bool mod_brk);
void Zesto_Notify_Munmap(int asid, unsigned int addr, unsigned int length, bool mod_brk);
void Zesto_SetBOS(int asid, unsigned int stack_base);
void Zesto_UpdateBrk(int asid, unsigned int brk_end, bool do_mmap);
 
void Zesto_Slice_Start(unsigned int slice_num);
void Zesto_Slice_End(unsigned int slice_num, unsigned long long feeder_slice_length, unsigned long long slice_weight_times_1000);
void Zesto_Add_WriteByteCallback(ZESTO_WRITE_BYTE_CALLBACK callback);
void Zesto_WarmLLC(int asid, unsigned int addr, bool is_write);

void activate_core(int coreID);
void deactivate_core(int coreID);
bool is_core_active(int coreID);
void sim_drain_pipe(int coreID);

void CheckIPCMessageQueue(bool isEarly, int caller_coreID);

enum ipc_message_id_t { SLICE_START, SLICE_END, MMAP, MUNMAP, UPDATE_BRK, WARM_LLC, STOP_SIMULATION, ACTIVATE_CORE, DEACTIVATE_CORE, SCHEDULE_NEW_THREAD, ALLOCATE_THREAD, THREAD_AFFINITY, INVALID_MSG };

struct ipc_message_t {
    ipc_message_id_t id;
    int64_t arg0;
    int64_t arg1;
    int64_t arg2;
    int64_t arg3;

    ipc_message_t() : id(INVALID_MSG) {}

    /* Some messages need to be conusmed early in timing_sim.
     * Mostly related to setup. */
    bool ConsumableEarly() const {
        switch (this->id) {
          case SLICE_START:
          case SLICE_END:
          case ACTIVATE_CORE:
          case ALLOCATE_THREAD:
          case SCHEDULE_NEW_THREAD:
          case STOP_SIMULATION:
          case THREAD_AFFINITY:
            return true;
          default:
            return false;
        }
    }

    void SliceStart(unsigned int slice_num)
    {
        this->id = SLICE_START;
        this->arg0 = slice_num;
    }

    void SliceEnd(unsigned int slice_num, unsigned long long feeder_slice_length, unsigned long long slice_weight_times_1000)
    {
        this->id = SLICE_END;
        this->arg0 = slice_num;
        this->arg1 = feeder_slice_length;
        this->arg2 = slice_weight_times_1000;
    }

    void Mmap(int asid, unsigned int addr, unsigned int length, bool mod_brk)
    {
        this->id = MMAP;
        this->arg0 = asid;
        this->arg1 = addr;
        this->arg2 = length;
        this->arg3 = mod_brk;
    }

    void Munmap(int asid, unsigned int addr, unsigned int length, bool mod_brk)
    {
        this->id = MUNMAP;
        this->arg0 = asid;
        this->arg1 = addr;
        this->arg2 = length;
        this->arg3 = mod_brk;
    }

    void UpdateBrk(int asid, unsigned int brk_end, bool do_mmap)
    {
        this->id = UPDATE_BRK;
        this->arg0 = asid;
        this->arg1 = brk_end;
        this->arg2 = do_mmap;
    }

    void StopSimulation(bool kill_sim_threads)
    {
        this->id = STOP_SIMULATION;
        this->arg0 = kill_sim_threads;
    }

    void ActivateCore(int coreID)
    {
        this->id = ACTIVATE_CORE;
        this->arg0 = coreID;
    }

    void DeactivateCore(int coreID)
    {
        this->id = DEACTIVATE_CORE;
        this->arg0 = coreID;
    }

    void ScheduleNewThread(int tid)
    {
        this->id = SCHEDULE_NEW_THREAD;
        this->arg0 = tid;
    }

    void BufferManagerAllocateThread(int tid, int buffer_capacity)
    {
        this->id = ALLOCATE_THREAD;
        this->arg0 = tid;
        this->arg1 = buffer_capacity;
    }

    void SetThreadAffinity(int tid, int coreID)
    {
        this->id = THREAD_AFFINITY;
        this->arg0 = tid;
        this->arg1 = coreID;
    }
};

#endif /*__PIN_ZESTO_INTERFACE__*/
