#include "BufferManager.h"


#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

ostream& operator<< (ostream &out, handshake_container_t &hand)
{
  out << "hand:" << " ";
  out << hand.valid;
  out << hand.mem_released;
  out << hand.isFirstInsn;
  out << hand.isLastInsn;
  out << hand.killThread;
  out << " ";
  out << hand.mem_buffer.size();
  out << " ";
  out << "pc:" << hand.handshake.pc << " "; 
  out << "coreid:" << hand.handshake.coreID << " "; 
  out << "npc:" << hand.handshake.npc << " "; 
  out << "tpc:" << hand.handshake.tpc << " "; 
  out << "brtaken:" << hand.handshake.brtaken << " "; 
  out << "ins:" << hand.handshake.ins << " ";
  out << "flags:" << hand.handshake.sleep_thread << hand.handshake.resume_thread;
  out << hand.handshake.iteration_correction << hand.handshake.real;
  out << hand.handshake.in_critical_section;
  out << " slicenum:" << hand.handshake.slice_num << " ";
  out << "feederslicelen:" << hand.handshake.feeder_slice_length << " ";
  out << "feedersliceweight:" << hand.handshake.slice_weight_times_1000 << " ";
  out.flush();
  return out;
}

BufferManager::BufferManager()
{  
}

handshake_container_t* BufferManager::front(THREADID tid)
{
  checkFirstAccess(tid);
  GetLock(locks_[tid], tid+1);

  assert(queueSizes_[tid] > 0);
  
  if(consumeBuffer_[tid]->size() > 0) {
    ReleaseLock(locks_[tid]);
    return consumeBuffer_[tid]->front();
  }

  assert(produceBuffer_[tid]->size() > 0);

  copyProducerToConsumer(tid, true);

  ReleaseLock(locks_[tid]);
  return consumeBuffer_[tid]->front();
}

handshake_container_t* BufferManager::back(THREADID tid)
{
  checkFirstAccess(tid);
  GetLock(locks_[tid], tid+1);
  assert(queueSizes_[tid] > 0);
  assert(produceBuffer_[tid]->size() > 0);
  ReleaseLock(locks_[tid]);
  return produceBuffer_[tid]->back();
}

bool BufferManager::empty(THREADID tid)
{
  checkFirstAccess(tid);   
  GetLock(locks_[tid], tid+1);
  bool result = queueSizes_[tid] == 0;
  ReleaseLock(locks_[tid]);
  return result;
}

void BufferManager::push(THREADID tid, handshake_container_t* handshake, bool fromILDJIT)
{
  checkFirstAccess(tid);    
  GetLock(locks_[tid], tid+1);
 
  handshake_container_t* free = getPooledHandshake(tid, fromILDJIT);  
  bool first = free->isFirstInsn;
  *free = *handshake;  
  free->isFirstInsn = first;

  produceBuffer_[tid]->push(free);
  queueSizes_[tid]++;
  ReleaseLock(locks_[tid]);
}

void BufferManager::pop(THREADID tid, handshake_container_t* handshake)
{
  checkFirstAccess(tid);
  GetLock(locks_[tid], tid+1);

  assert(queueSizes_[tid] > 0);
  assert(consumeBuffer_[tid]->size() > 0);
  consumeBuffer_[tid]->pop();

  releasePooledHandshake(tid, handshake);
  queueSizes_[tid]--;
  ReleaseLock(locks_[tid]);
}

bool BufferManager::hasThread(THREADID tid) 
{
  GetLock(locks_[tid], tid+1);
  bool result = queueSizes_.count(tid);
  ReleaseLock(locks_[tid]);
  return (result != 0);
}

unsigned int BufferManager::size()
{
  unsigned int result = queueSizes_.size();
  return result;
}

void BufferManager::checkFirstAccess(THREADID tid)
{
  if(queueSizes_.count(tid) == 0) {
    
    PIN_Yield();
    sleep(5);
    PIN_Yield();
    
    queueSizes_[tid] = 0;
    consumeBuffer_[tid] = new Buffer();
    produceBuffer_[tid] = new Buffer();
    
    for (int i=0; i < 50000; i++) {
      handshake_container_t* new_handshake = new handshake_container_t();
      handshake_pool_[tid].push(new_handshake);      
    }
    
    locks_[tid] = new PIN_LOCK();
    InitLock(locks_[tid]);
    
    char s_tid[100];
    sprintf(s_tid, "%d", tid);

    string logName = "./output_ring_cache/handshake_" + string(s_tid) + ".log"; 
    logs_[tid] = new ofstream();
    (*(logs_[tid])).open(logName.c_str());    
  }
  assert((consumeBuffer_[tid]->size() + produceBuffer_[tid]->size()) == queueSizes_[tid]);
}

handshake_container_t* BufferManager::getPooledHandshake(THREADID tid, bool fromILDJIT)
{
  checkFirstAccess(tid);    

  handshake_queue_t *pool;
  pool = &(handshake_pool_[tid]);

  long long int spins = 0;  
  while(pool->empty()) {
    ReleaseLock(&simbuffer_lock);
    PIN_Yield();
    GetLock(&simbuffer_lock, tid+1);
    spins++;
    if(spins >= 7000000LL) {
      cerr << tid << " [getPooledHandshake()]: That's a lot of spins!" << endl;
      spins = 0;
    }
  }

  handshake_container_t* result = pool->front();
  ASSERTX(result != NULL);

  pool->pop();
  
  if((didFirstInsn_.count(tid) == 0) && (!fromILDJIT)) {    
    result->isFirstInsn = true;
    didFirstInsn_[tid] = true;
  }
  else {
    result->isFirstInsn = false;
  }
  
  return result;
}

void BufferManager::releasePooledHandshake(THREADID tid, handshake_container_t* handshake)
{
  handshake_pool_[tid].push(handshake);  
  return;
}

bool BufferManager::isFirstInsn(THREADID tid)
{
  GetLock(locks_[tid], tid+1);
  bool isFirst = (didFirstInsn_.count(tid) == 0);
  ReleaseLock(locks_[tid]);
  return isFirst;
}

void BufferManager::copyProducerToConsumer(THREADID tid, bool all)
{
  while(produceBuffer_[tid]->size() > 1 || (produceBuffer_[tid]->size() > 0 && all)) {
    if(consumeBuffer_[tid]->full()) {
      break;
    }

    assert(produceBuffer_[tid]->front()->valid == true);
    consumeBuffer_[tid]->push(produceBuffer_[tid]->front());
    produceBuffer_[tid]->pop();
  }
}
