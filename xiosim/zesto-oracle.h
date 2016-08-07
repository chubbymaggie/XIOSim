#ifndef ZESTO_ORACLE_INCLUDED
#define ZESTO_ORACLE_INCLUDED

/* zesto-oracle.h - Zesto oracle functional simulator class
 *
 * Copyright � 2009 by Gabriel H. Loh and the Georgia Tech Research Corporation
 * Atlanta, GA  30332-0415
 * All Rights Reserved.
 * 
 * THIS IS A LEGAL DOCUMENT BY DOWNLOADING ZESTO, YOU ARE AGREEING TO THESE
 * TERMS AND CONDITIONS.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the Georgia Tech Research Corporation nor the names of
 * its contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * 4. Zesto is distributed freely for commercial and non-commercial use.
 * 
 * 5. No nonprofit user may place any restrictions on the use of this software,
 * including as modified by the user, by any other authorized user.
 * 
 * 6. Noncommercial and nonprofit users may distribute copies of Zesto in
 * compiled or executable form as set forth in Section 2, provided that either:
 * (A) it is accompanied by the corresponding machine-readable source code, or
 * (B) it is accompanied by a written offer, with no time limit, to give anyone
 * a machine-readable copy of the corresponding source code in return for
 * reimbursement of the cost of distribution. This written offer must permit
 * verbatim duplication by anyone, or (C) it is distributed by someone who
 * received only the executable form, and is accompanied by a copy of the
 * written offer of source code.
 * 
 * 7. Zesto was developed by Gabriel H. Loh, Ph.D.  US Mail: 266 Ferst Drive,
 * Georgia Institute of Technology, Atlanta, GA 30332-0765
 *
 */

#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "host.h"
#include "shadow_MopQ.h"

/* Until we replace it with xiosim_core_assert(). As before, there's always
 * a core_t* core pointer defined when we call zesto_assert(). */
#define zesto_assert(cond, retval) xiosim_core_assert((cond), core->id)

class handshake_container_t;  // fwd
namespace xiosim {
namespace stats {
class StatsDatabase;  // fwd
}
}

enum buffer_result_t { ALL_GOOD, HANDSHAKE_NOT_NEEDED, HANDSHAKE_NOT_CONSUMED };

class core_oracle_t {
  friend class shadow_MopQ_t;

  /* struct for tracking all in-flight writers of registers */
  struct map_node_t {
    struct uop_t * uop;
    struct map_node_t * prev;
    struct map_node_t * next;
  };

  public:

  bool spec_mode;  /* are we currently on a wrong-path? */

  core_oracle_t(struct core_t * const core);
  ~core_oracle_t();
  void reg_stats(xiosim::stats::StatsDatabase* sdb);
  void update_occupancy(void);

  struct Mop_t * get_Mop(const int index);
  int get_index(const struct Mop_t * const Mop);
  int next_index(const int index);
  struct Mop_t * get_oldest_Mop();

  buffer_result_t buffer_handshake(handshake_container_t * handshake);

  /* Can we absorb a new Mop. */
  bool can_exec() {
      return (current_Mop == nullptr) && // not stalled, so will need a new Mop
             !drain_pipeline && // not draining from a trap
             (MopQ_num < MopQ_size); // ...and has space for the new Mop
  }
  struct Mop_t * exec(const md_addr_t requested_PC);
  void consume(const struct Mop_t * const Mop);
  void commit_uop(struct uop_t * const uop);
  void commit(const struct Mop_t * const commit_Mop);

  void recover(const struct Mop_t * const Mop); /* recovers oracle state */
  void pipe_recover(struct Mop_t * const Mop, const md_addr_t New_PC); /* unrolls pipeline state */
  void pipe_flush(struct Mop_t * const Mop);

  bool is_draining(void) const { return drain_pipeline; }

  void complete_flush(void);

  void trace_in_flight_ops(void);

  /* The difference between non-spec entries in the shadow_MopQ and MopQ -- these
   * are the ones we need to re-execute before getting back to the feeder. */
  unsigned int num_Mops_before_feeder(void) const;
  /* Is the oracle in the process of recovering from a nuke. */
  bool on_nuke_recovery_path(void) const { return num_Mops_before_feeder() > 0; }

  /* Print instruction type stats. */
  void dump_instruction_histograms(const std::string iclass_prefix, const std::string iform_prefix);
  protected:

  seq_t Mop_seq; /* Mop sequence number */

  struct Mop_t * MopQ;
  int MopQ_head;
  int MopQ_tail;
  int MopQ_non_spec_tail; /* pointer to the youngest non-speculative Mop */
  int MopQ_num;
  int MopQ_size;
  struct Mop_t * current_Mop; /* pointer to track a Mop that has been executed but not consumed (i.e. due to fetch stall) */
  int MopQ_spec_num;
  bool drain_pipeline; /* Are we draining from a trap */

  shadow_MopQ_t shadow_MopQ;

  struct core_t * core;
  /* dependency tracking used by oracle */
  std::unordered_map<xed_reg_enum_t, std::list<struct uop_t *>, std::hash<unsigned long> > dep_map;

  /* Instruction type stats. */
  std::vector<counter_t> iclass_histogram;
  std::vector<counter_t> iform_histogram;

  void undo(struct Mop_t * const Mop, bool nuke);

  void install_mapping(struct uop_t * const uop);
  void commit_mapping(const struct uop_t * const uop);
  void undo_mapping(const struct uop_t * const uop);

  void install_dependencies(struct uop_t * const uop);
  void commit_dependencies(struct uop_t * const uop);
  void undo_dependencies(struct uop_t * const uop);
  void get_magic_insn_operands(struct Mop_t* Mop, const handshake_container_t* handshake,
                               xed_iclass_enum_t replacement_type);

  void update_stats(struct Mop_t * const Mop);

  /* # non-speculative Mops in the MopQ. */
  unsigned int num_non_spec_Mops(void) const;

  /* Create a fake NOP handshake in case we don't have a real one from feeder. */
  handshake_container_t get_fake_spec_handshake();

  public:
  /* Did we manage to absorb a handshake from instruction feeder */
  bool consumed;

};

#endif /* ZESTO_ORACLE_INCLUDED */
