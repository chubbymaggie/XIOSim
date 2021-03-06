/* commit-IO-DPM.cpp - Detailed In-Order Pipeline Model */
/*
 * Derived from Zesto OO model
 * Svilen Kanev, 2011
 */

/* NOTE: For compatibility and interchargability between the IO and OO models
         some structure names are inconsistent with their purpose.
         Here, the ROB is a small commit buffer that is entered after pre-commit.
*/


#ifdef ZESTO_PARSE_ARGS
  if(!strcasecmp(commit_opt_string,"IO-DPM"))
    return std::make_unique<class core_commit_IO_DPM_t>(core);
#else

class core_commit_IO_DPM_t:public core_commit_t
{
  enum commit_stall_t {CSTALL_NONE,      /* no stall */
                       CSTALL_NOT_READY, /* oldest inst not done (no uops finished) */
                       CSTALL_PARTIAL,   /* oldest inst not done (but some uops finished) */
                       CSTALL_EMPTY,     /* ROB is empty, nothing to commit */
                       CSTALL_JECLEAR_INFLIGHT, /* Mop is done, but its jeclear hasn't been handled yet */
                       CSTALL_MAX_BRANCHES, /* exceeded maximum number of branches committed per cycle */
		       CSTALL_STQ, /* no place in STQ - stores done in commit */
                       CSTALL_num
                     };

  public:

  core_commit_IO_DPM_t(struct core_t * const core);
  ~core_commit_IO_DPM_t();
  virtual void reg_stats(xiosim::stats::StatsDatabase* sdb);
  virtual void update_occupancy(void);

  virtual void step(void);
  virtual void IO_step(void);
  virtual void recover(const struct Mop_t * const Mop);
  virtual void recover(void);

  virtual bool ROB_available(void);
  virtual bool ROB_empty(void);
  virtual bool pipe_empty(void);
  virtual void ROB_insert(struct uop_t * const uop);
  virtual void ROB_fuse_insert(struct uop_t * const uop);

  virtual void pre_commit_insert(struct uop_t * const uop);
  virtual void pre_commit_fused_insert(struct uop_t * const uop);
  virtual bool pre_commit_available();
  virtual void pre_commit_step();
  virtual void pre_commit_recover(struct Mop_t * const Mop);

  virtual void squash_uop(struct uop_t * const uop);


  protected:

  struct uop_t ** ROB;
  struct uop_t ** pre_commit_pipe;
  int ROB_head;
  int ROB_tail;
  int ROB_num;
  int ROB_eff_num;

  static const char *commit_stall_str[CSTALL_num];

  enum commit_stall_t stall_reason;

  bool is_stall();

  /* additional temps to track timing of REP insts */
  tick_t when_rep_fetch_started;
  tick_t when_rep_fetched;
  tick_t when_rep_decode_started;
  tick_t when_rep_commit_started;
};

/* number of buckets in uop-flow-length histogram */
#define FLOW_HISTO_SIZE 9

/* VARIABLES/TYPES */

const char *core_commit_IO_DPM_t::commit_stall_str[CSTALL_num] = {
  "no stall                   ",
  "oldest inst not done       ",
  "oldest inst partially done ",
  "ROB is empty               ",
  "Mop done, jeclear in flight",
  "branch commit limit        ",
  "STQ full (or cache not ok) "
};

/*******************/
/* SETUP FUNCTIONS */
/*******************/

core_commit_IO_DPM_t::core_commit_IO_DPM_t(struct core_t * const arg_core):
  ROB_head(0), ROB_tail(0), ROB_num(0), ROB_eff_num(0), stall_reason(CSTALL_NONE),
  when_rep_fetch_started(0), when_rep_fetched(0),
  when_rep_decode_started(0), when_rep_commit_started(0)
{
  struct core_knobs_t * knobs = arg_core->knobs;
  core = arg_core;
  ROB = (struct uop_t**) calloc(knobs->commit.ROB_size,sizeof(*ROB));
  if(!ROB)
    fatal("couldn't calloc ROB");

  pre_commit_pipe = (struct uop_t**) calloc(knobs->commit.pre_commit_depth, sizeof(*pre_commit_pipe));
  if(!pre_commit_pipe)
    fatal("couldn't calloc pre-commit pipe");

}

core_commit_IO_DPM_t::~core_commit_IO_DPM_t() {
    free(pre_commit_pipe);
    free(ROB);
}

void core_commit_IO_DPM_t::reg_stats(xiosim::stats::StatsDatabase* sdb) {
    int coreID = core->id;

    stat_reg_note(sdb, "\n#### COMMIT STATS ####");

    auto& sim_cycle_st = *stat_find_core_stat<tick_t>(sdb, coreID, "sim_cycle");
    auto& commit_insn_st = *stat_find_core_stat<counter_t>(sdb, coreID, "commit_insn");
    auto& commit_uops_st = *stat_find_core_stat<counter_t>(sdb, coreID, "commit_uops");
    auto& commit_eff_uops_st = *stat_find_core_stat<counter_t>(sdb, coreID, "commit_eff_uops");

    auto& commit_bytes_st = stat_reg_core_counter(sdb, true, coreID, "commit_bytes",
                                                  "total number of bytes committed",
                                                  &core->stat.commit_bytes, 0, true, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_BPC", "BPC (bytes per cycle) at commit",
                          commit_bytes_st / sim_cycle_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_IPC", "IPC at commit",
                          commit_insn_st / sim_cycle_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_uPC", "uPC at commit",
                          commit_uops_st / sim_cycle_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_euPC", "effective uPC at commit",
                          commit_eff_uops_st / sim_cycle_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_byte_per_inst",
                          "average bytes per instruction", commit_bytes_st / commit_insn_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_byte_per_uops", "average bytes per uop",
                          commit_bytes_st / commit_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_byte_per_eff_uop",
                          "average bytes per effective uop", commit_bytes_st / commit_eff_uops_st,
                          NULL);
    stat_reg_core_formula(sdb, true, coreID, "avg_commit_flowlen",
                          "uops per instruction at commit", commit_uops_st / commit_insn_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "avg_commit_eff_flowlen",
                          "effective uops per instruction at commit",
                          commit_eff_uops_st / commit_insn_st, NULL);

    auto& commit_fusions_st = stat_reg_core_counter(sdb, true, coreID, "commit_fusions",
                                                    "total number of fused uops committed",
                                                    &core->stat.commit_fusions, 0, true, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_fusion_uops", "fused uops at commit",
                          (commit_eff_uops_st - commit_uops_st) + commit_fusions_st, "%12.0f");
    // TODO: Duplicating the formula terms for now.
    stat_reg_core_formula(
            sdb, true, coreID, "commit_frac_fusion_uops",
            "fraction of effective uops fused at commit",
            ((commit_eff_uops_st - commit_uops_st) + commit_fusions_st) / commit_eff_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_fusion_compression",
                          "fraction of effective uops compressed via fusion at commit",
                          (commit_eff_uops_st - commit_uops_st) / commit_eff_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "commit_fusion_expansion",
                          "average number of effective uops per uop (fused or standalone) commit",
                          commit_eff_uops_st / commit_uops_st, NULL);

    reg_core_queue_occupancy_stats(sdb, coreID, "ROB", &core->stat.ROB_occupancy,
                                   &core->stat.ROB_empty_cycles,
                                   &core->stat.ROB_full_cycles);

    auto store_lookups_st = stat_find_core_stat<counter_t>(sdb, coreID, "DL1.store_lookups");
    auto& split_accesses_st =
            stat_reg_core_counter(sdb, true, coreID, "DL1_store_split_accesses",
                                  "number of stores requiring split accesses",
                                  &core->stat.DL1_store_split_accesses, 0, true, NULL);
    stat_reg_core_formula(sdb, true, coreID, "DL1_store_split_frac",
                          "fraction of stores requiring split accesses",
                          split_accesses_st / (*store_lookups_st - split_accesses_st), NULL);

    core->stat.commit_stall = stat_reg_core_dist(
            sdb, coreID, "commit_stall", "breakdown of stalls at commit", 0, CSTALL_num,
            (PF_COUNT | PF_PDF), NULL, commit_stall_str, true, NULL);

    stat_reg_note(sdb, "#### TIMING STATS ####");
    /* cumulative slip cycles (not printed) */
    auto& fetch_Tslip = stat_reg_core_counter(sdb, false, coreID, "Mop_fetch_Tslip",
                                            "total Mop fetch slip cycles",
                                            &core->stat.Mop_fetch_slip, 0, true, NULL);
    auto& f2d_Tslip = stat_reg_core_counter(
            sdb, false, coreID, "Mop_f2d_Tslip", "total Mop fetch-to-decode slip cycles",
            &core->stat.Mop_fetch2decode_slip, 0, true, NULL);
    auto& decode_Tslip = stat_reg_core_counter(sdb, false, coreID, "Mop_decode_Tslip",
                                             "total Mop decode slip cycles",
                                             &core->stat.Mop_decode_slip, 0, true, NULL);
    auto& d2a_Tslip = stat_reg_core_counter(
            sdb, false, coreID, "uop_d2a_Tslip", "total uop decode-to-alloc slip cycles",
            &core->stat.uop_decode2alloc_slip, 0, true, NULL);
    auto& a2r_Tslip = stat_reg_core_counter(
            sdb, false, coreID, "uop_a2r_Tslip", "total uop alloc-to-ready slip cycles",
            &core->stat.uop_alloc2ready_slip, 0, true, NULL);
    auto& r2i_Tslip = stat_reg_core_counter(
            sdb, false, coreID, "uop_r2i_Tslip", "total uop ready-to-issue slip cycles",
            &core->stat.uop_ready2issue_slip, 0, true, NULL);
    auto& i2e_Tslip = stat_reg_core_counter(sdb, false, coreID, "uop_i2e_Tslip",
                                          "total uop issue-to-exec slip cycles",
                                          &core->stat.uop_issue2exec_slip, 0, true, NULL);
    auto& e2w_Tslip = stat_reg_core_counter(
            sdb, false, coreID, "uop_e2w_Tslip", "total uop exec-to-WB slip cycles",
            &core->stat.uop_exec2complete_slip, 0, true, NULL);
    auto& w2c_Tslip = stat_reg_core_counter(
            sdb, false, coreID, "uop_w2c_Tslip", "total uop WB-to-commit slip cycles",
            &core->stat.uop_complete2commit_slip, 0, true, NULL);
    auto& d2c_Tslip = stat_reg_core_counter(
            sdb, false, coreID, "Mop_d2c_Tslip", "total Mop decode-to-commit slip cycles",
            &core->stat.Mop_decode2commit_slip, 0, true, NULL);
    auto& commit_Tslip = stat_reg_core_counter(sdb, false, coreID, "Mop_commit_Tslip",
                                             "total Mop commit slip cycles",
                                             &core->stat.Mop_commit_slip, 0, true, NULL);
    stat_reg_core_counter(sdb, true, coreID, "num_traps",
                          "total number of traps committed",
                          &core->stat.commit_traps, 0, true, NULL);
    /* average slip cycles */
    stat_reg_core_formula(sdb, true, coreID, "Mop_fetch_avg_slip", "Mop fetch average delay",
                          fetch_Tslip / commit_insn_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "Mop_f2d_avg_slip",
                          "Mop fetch-to-decode average delay",
                          f2d_Tslip / commit_insn_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "Mop_decode_avg_slip", "Mop decode average delay",
                          decode_Tslip / commit_insn_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "uop_d2a_avg_slip",
                          "uop decode-to-alloc average delay", d2a_Tslip / commit_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "uop2_a2r_avg_slip",
                          "uop alloc-to-ready average delay", a2r_Tslip / commit_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "uop_r2i_avg_slip",
                          "uop ready-to-issue average delay", r2i_Tslip / commit_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "uop_i2e_avg_slip",
                          "uop issue-to-exec average delay", i2e_Tslip / commit_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "uop_e2w_avg_slip", "uop exec-to-WB average delay",
                          e2w_Tslip / commit_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "uop_w2c_avg_slip", "uop WB-to-commit average delay",
                          w2c_Tslip / commit_uops_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "Mop_d2c_avg_slip",
                          "Mop decode-to-commit average delay",
                          d2c_Tslip / commit_insn_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "Mop_commit_avg_slip", "Mop commit average delay",
                          commit_Tslip / commit_insn_st, NULL);

    // TODO: Support sum of other formulas. For now, just write the whole thing out by hand.
    char stat_name[1024];
    sprintf(stat_name, "c%d.Mop_avg_end_to_end", coreID);
    Formula Mop_avg_end_to_end(stat_name, "Mop average end-to-end pipeline delay");
    Mop_avg_end_to_end =
            fetch_Tslip / commit_insn_st  +    // Mop_fetch_avg_slip.
            f2d_Tslip / commit_insn_st    +    // Mop_f2d_avg_slip.
            decode_Tslip / commit_insn_st +    // Mop_d2c_avg_slip.
            commit_Tslip / commit_insn_st;     // Mop_commit_avg_slip.

    /* instruction distribution stats */
    stat_reg_note(sdb, "\n#### INSTRUCTION STATS (no wrong-path) ####");
    stat_reg_core_formula(sdb, true, coreID, "num_insn", "total number of instructions committed",
                          commit_insn_st, NULL);
    auto& num_refs_st = stat_reg_core_counter(sdb, true, coreID, "num_refs",
                                              "total number of loads and stores committed",
                                              &core->stat.commit_refs, 0, true, NULL);
    auto& num_loads_st = stat_reg_core_counter(sdb, true, coreID, "num_loads",
                                               "total number of loads committed",
                                               &core->stat.commit_loads, 0, true, NULL);
    stat_reg_core_formula(sdb, true, coreID, "num_stores", "total number of stores committed",
                          num_refs_st - num_loads_st, "%12.0f");
    stat_reg_core_counter(sdb, true, coreID, "num_branches", "total number of branches committed",
                          &core->stat.commit_branches, 0, true, NULL);
    auto& num_rep_insn_st = stat_reg_core_counter(sdb, true, coreID, "num_rep_insn",
                                                  "total number of REP insts committed",
                                                  &core->stat.commit_rep_insn, 0, true, NULL);
    auto& num_rep_iter_st = stat_reg_core_counter(sdb, true, coreID, "num_rep_iter",
                                                  "total number of REP iterations committed",
                                                  &core->stat.commit_rep_iterations, 0, true, NULL);
    auto& num_rep_uops_st = stat_reg_core_counter(sdb, true, coreID, "num_rep_uops",
                                                  "total number of uops in REP insts committed",
                                                  &core->stat.commit_rep_uops, 0, true, NULL);
    stat_reg_core_formula(sdb, true, coreID, "num_avg_reps", "average iterations per REP inst",
                          num_rep_iter_st / num_rep_insn_st, "%12.2f");
    stat_reg_core_formula(sdb, true, coreID, "num_avg_rep_uops", "average uops per REP inst",
                          num_rep_uops_st / num_rep_insn_st, "%12.2f");
    auto& num_UROM_insn_st = stat_reg_core_counter(sdb, true, coreID, "num_UROM_insn",
                                                   "total number of insn using the UROM committed",
                                                   &core->stat.commit_UROM_insn, 0, true, NULL);
    auto& num_UROM_uops_st = stat_reg_core_counter(sdb, true, coreID, "num_UROM_uops",
                                                   "total number of uops using the UROM committed",
                                                   &core->stat.commit_UROM_uops, 0, true, NULL);
    stat_reg_core_counter(sdb, true, coreID, "num_UROM_eff_uops",
                          "total number of effective uops using the UROM committed",
                          &core->stat.commit_UROM_eff_uops, 0, true, NULL);
    stat_reg_core_formula(sdb, true, coreID, "num_avg_UROM_uops", "average uops per UROM inst",
                          num_UROM_uops_st / num_UROM_insn_st, "%12.2f");
    stat_reg_core_formula(sdb, true, coreID, "avg_flowlen", "average uops per instruction",
                          commit_uops_st / commit_insn_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "avg_eff_flowlen",
                          "average effective uops per instruction",
                          commit_eff_uops_st / commit_insn_st, NULL);
    stat_reg_core_counter(sdb, true, coreID, "regfile_writes", "number of register file writes",
                          &core->stat.regfile_writes, 0, true, NULL);
    stat_reg_core_counter(sdb, true, coreID, "fp_regfile_writes",
                          "number of fp register file writes", &core->stat.fp_regfile_writes, 0,
                          true, NULL);
    core->stat.flow_histo =
            stat_reg_core_dist(sdb, coreID, "flow_lengths", "histogram of uop flow lengths", 0,
                               FLOW_HISTO_SIZE, (PF_COUNT | PF_PDF), NULL, NULL, true, NULL);
    core->stat.eff_flow_histo = stat_reg_core_dist(
            sdb, coreID, "eff_flow_lengths", "histogram of effective uop flow lengths", 0,
            FLOW_HISTO_SIZE, (PF_COUNT | PF_PDF), NULL, NULL, true, NULL);
}

void core_commit_IO_DPM_t::update_occupancy(void)
{
    /* ROB */
  core->stat.ROB_occupancy += ROB_num;
  core->stat.ROB_eff_occupancy += ROB_eff_num;
  if(ROB_num >= core->knobs->commit.ROB_size)
    core->stat.ROB_full_cycles++;
  if(ROB_num <= 0)
    core->stat.ROB_empty_cycles++;
}

/*************************/
/* MAIN COMMIT FUNCTIONS */
/*************************/

/* In-order instruction commit.  Individual uops cannot commit
   until it is guaranteed that the entire Mop's worth of uops will
   commit. */
void core_commit_IO_DPM_t::IO_step(void)
{
  struct core_knobs_t * knobs = core->knobs;
  int commit_count = 0;
  stall_reason = CSTALL_NONE;
  int branches_committed = 0;

  /* This is just a deadlock watchdog. If something got messed up
     in the pipeline and no forward progress is being made, this
     code will eventually detect it. A global watchdog will check
     if any core is making progress and accordingly if not.*/
  if(core->active && ((core->sim_cycle - core->exec->last_completed) > deadlock_threshold))
  {
    deadlocked = true;
#ifdef ZTRACE
    ztrace_print(core->id, "Possible deadlock detected.");
#endif
    return;
  }

  /* deallocate at most one store from the (senior) STQ per cycle */
  core->exec->STQ_deallocate_senior();

  /* MAIN COMMIT LOOP */
  for(commit_count=0;commit_count<knobs->commit.width;commit_count++)
  {
    if(ROB_num <= 0) /* nothing to commit */
    {
      stall_reason = CSTALL_EMPTY;
      break;
    }

    struct Mop_t * Mop = ROB[ROB_head]->Mop;

    /* For branches, don't commit until the corresponding jeclear
       (if any) has been processed by the front-end. */
    if(Mop->commit.jeclear_in_flight)
    {
      stall_reason = CSTALL_JECLEAR_INFLIGHT;
      break;
    }

    if(Mop->decode.is_ctrl && knobs->commit.branch_limit && (branches_committed >= knobs->commit.branch_limit))
    {
      stall_reason = CSTALL_MAX_BRANCHES;
      break;
    }

    if(Mop->oracle.spec_mode)
      zesto_assert(false, (void)0);

    /* Are all uops in the Mop completed? */
    if(Mop->commit.complete_index != -1) /* still some outstanding insts */
    {
      struct uop_t * uop = &Mop->uop[Mop->commit.complete_index];

      while(uop->timing.when_completed <= core->sim_cycle
            || uop->decode.is_sta || uop->decode.is_std)
      {
        /* stores get added to the STQ at commit */
        if(uop->decode.is_sta)
        {
          if(!core->exec->exec_fused_ST(uop))
          {
             stall_reason = CSTALL_STQ;
             break;
          }
        }

        zesto_assert(uop->timing.when_completed <= core->sim_cycle, (void)0);

        Mop->commit.complete_index += uop->decode.has_imm ? 3 : 1;
        if(Mop->commit.complete_index >= (int) Mop->decode.flow_length)
        {
          Mop->commit.complete_index = -1; /* Mark this Mop as all done */
#ifdef ZTRACE
          ztrace_print(Mop,"c|complete|all uops completed execution");
#endif
          if(Mop->fetch.bpred_update)
          {
            core->fetch->bpred->update(Mop->fetch.bpred_update, Mop->decode.opflags,
                Mop->fetch.PC, Mop->fetch.ftPC, Mop->decode.targetPC, Mop->oracle.NextPC, Mop->oracle.taken_branch);
            core->fetch->bpred->return_state_cache(Mop->fetch.bpred_update);
            Mop->fetch.bpred_update = NULL;
          }
          break;
        }

        uop = &Mop->uop[Mop->commit.complete_index];
      }
    }

    if(stall_reason != CSTALL_NONE) break;

    if(Mop->commit.complete_index == -1) /* commit the uops if the Mop is done */
    {
      struct uop_t * uop = ROB[ROB_head];
      zesto_assert(uop->timing.when_completed <= core->sim_cycle,(void)0);
      zesto_assert(uop->alloc.ROB_index == ROB_head,(void)0);
      zesto_assert(uop == &Mop->uop[Mop->commit.commit_index],(void)0);

      if(uop->decode.BOM && (uop->Mop->timing.when_commit_started == TICK_T_MAX))
        uop->Mop->timing.when_commit_started = core->sim_cycle;


      //SK - load deallocation moved to end of payload pipe
      if(uop->decode.is_sta)
        core->exec->STQ_deallocate_sta();

      if(uop->decode.is_std) /* we alloc on STA, dealloc on STD */
      {
        if(!core->exec->STQ_deallocate_std(uop))
        {
          stall_reason = CSTALL_STQ;
          break;
        }
      }

      /* any remaining transactions in-flight (only for loads)
         should now be ignored - such load requests may exist, for
         example as a result of a load that completes early due to
         a hit in the STQ while the cache request is still making
         its way through the memory hierarchy. */
      if(uop->decode.is_load)
        uop->exec.action_id = core->new_action_id();

#ifdef ZTRACE
      ztrace_print(uop,"c|commit|uop committed");
#endif

      if(uop->decode.EOM)
        uop->Mop->timing.when_commit_finished = core->sim_cycle;

      /* remove uop from ROB */
      if((!uop->decode.in_fusion) || (uop->decode.fusion_next == NULL)) /* fusion dealloc's on fusion-tail */
      {
        ROB[ROB_head] = NULL;
        ROB_num --;
        ROB_eff_num --;
        ROB_head = modinc(ROB_head,knobs->commit.ROB_size); //(ROB_head+1) % knobs->commit.ROB_size;
        if(uop->decode.in_fusion)
        {
          ZESTO_STAT(core->stat.commit_fusions++;)
        }
      }
      else /* fusion body doesn't count toward commit width */
      {
        commit_count--;
        /* XXX: this is really ugly.  To avoid inserting another
           uop-traversing loop within the commit loop, we replace
           the ROB[.] pointer with the next uop to commit (for
           fusion).  That is, if we have a two-uop fused set (A,B),
           originally ROB[.] would point to A, which has a pointer
           through fusion_next to B.  Once we commit A, we will
           replace ROB[.] with B. */
        ROB[ROB_head] = uop->decode.fusion_next;
        ROB_eff_num --;
        zesto_assert(ROB_eff_num >= 0,(void)0);
      }
      uop->alloc.ROB_index = -1;

      if(!uop->Mop->decode.is_trap)
      {
        ZESTO_STAT(core->stat.uop_decode2alloc_slip += uop->timing.when_allocated - uop->timing.when_decoded;)
        ZESTO_STAT(core->stat.uop_alloc2ready_slip += uop->timing.when_ready - uop->timing.when_allocated;)
        ZESTO_STAT(core->stat.uop_ready2issue_slip += uop->timing.when_issued - uop->timing.when_ready;)
        ZESTO_STAT(core->stat.uop_issue2exec_slip += uop->timing.when_exec - uop->timing.when_issued;)
        ZESTO_STAT(core->stat.uop_exec2complete_slip += uop->timing.when_completed - uop->timing.when_exec;)
        ZESTO_STAT(core->stat.uop_complete2commit_slip += core->sim_cycle - uop->timing.when_completed;)

        zesto_assert(uop->timing.when_exec != TICK_T_MAX,(void)0);
      }

      for (size_t oreg = 0; oreg < MAX_ODEPS; oreg++) {
        if(x86::is_ireg(uop->decode.odep_name[oreg]))
          core->stat.regfile_writes++;
        else if(x86::is_freg(uop->decode.odep_name[oreg]))
          core->stat.fp_regfile_writes++;
      }

      /* this cleans up idep/odep ptrs, register mappings, and
         commit stores to the real (non-spec) memory system */
      core->oracle->commit_uop(uop);

      /* mark uop as committed in Mop */
      Mop->commit.commit_index += uop->decode.has_imm ? 3 : 1;

      if(Mop->commit.commit_index >= (int) Mop->decode.flow_length)
      {
        Mop->commit.commit_index = -1; /* The entire Mop has been committed */

        /* Update stats */
        if(Mop->uop[Mop->decode.last_uop_index].decode.EOM)
        {
          ZESTO_STAT(core->stat.commit_insn++;)
          ZESTO_STAT(core->stat.commit_bytes += Mop->fetch.len;)
        }

        if(Mop->decode.is_ctrl)
          branches_committed++;

        ZESTO_STAT(core->stat.commit_uops += Mop->stat.num_uops;)
        ZESTO_STAT(core->stat.commit_eff_uops += Mop->stat.num_eff_uops;)
        ZESTO_STAT(core->stat.commit_branches += Mop->stat.num_branches;)
        ZESTO_STAT(core->stat.commit_refs += Mop->stat.num_refs;)
        ZESTO_STAT(core->stat.commit_loads += Mop->stat.num_loads;)
        if(Mop->decode.has_rep)
        {
          if(Mop->uop[Mop->decode.last_uop_index].decode.EOM)
            ZESTO_STAT(core->stat.commit_rep_insn++;)
          if(!Mop->oracle.zero_rep)
            ZESTO_STAT(core->stat.commit_rep_iterations++;)
          ZESTO_STAT(core->stat.commit_rep_uops += Mop->stat.num_uops;)
        }
        if(Mop->stat.num_uops > knobs->decode.max_uops[0])
        {
          ZESTO_STAT(core->stat.commit_UROM_insn++;)
          ZESTO_STAT(core->stat.commit_UROM_uops += Mop->stat.num_uops;)
          ZESTO_STAT(core->stat.commit_UROM_eff_uops += Mop->stat.num_eff_uops;)
        }

        core->stat.flow_count += Mop->stat.num_uops;
        core->stat.eff_flow_count += Mop->stat.num_eff_uops;

        if(Mop->decode.has_rep) /* all iterations of a REP count as one macro-op! */
        {
          if(Mop->uop[0].decode.BOM)
          {
            when_rep_fetch_started = Mop->timing.when_fetch_started;
            when_rep_fetched = Mop->timing.when_fetched;
            zesto_assert(Mop->timing.when_fetched != TICK_T_MAX,(void)0);
            zesto_assert(Mop->timing.when_fetch_started != TICK_T_MAX,(void)0);
            when_rep_decode_started = Mop->timing.when_decode_started;
            when_rep_commit_started = Mop->timing.when_commit_started;
          }
          if(Mop->uop[Mop->decode.last_uop_index].decode.EOM)
          {
            ZESTO_STAT(stat_add_sample(core->stat.flow_histo, core->stat.flow_count);)
            ZESTO_STAT(stat_add_sample(core->stat.eff_flow_histo, core->stat.eff_flow_count);)
            core->stat.flow_count = 0;
            core->stat.eff_flow_count = 0;

            if(!Mop->decode.is_trap)
            {
              ZESTO_STAT(core->stat.Mop_fetch_slip += when_rep_fetched - when_rep_fetch_started;)
              ZESTO_STAT(core->stat.Mop_fetch2decode_slip += when_rep_decode_started - when_rep_fetched;)
              ZESTO_STAT(core->stat.Mop_decode_slip += Mop->timing.when_decode_finished - when_rep_decode_started;) /* from decode of first Mop's first uop to decode of last Mop's last uop */
              ZESTO_STAT(core->stat.Mop_decode2commit_slip += when_rep_commit_started - Mop->timing.when_decode_finished;)
              ZESTO_STAT(core->stat.Mop_commit_slip += Mop->timing.when_commit_finished - when_rep_commit_started;)
            }
            else
              ZESTO_STAT(core->stat.commit_traps++;)
          }
        }
        else
        {
          ZESTO_STAT(stat_add_sample(core->stat.flow_histo, core->stat.flow_count);)
          ZESTO_STAT(stat_add_sample(core->stat.eff_flow_histo, core->stat.eff_flow_count);)
          core->stat.flow_count = 0;
          core->stat.eff_flow_count = 0;

          if(!Mop->decode.is_trap)
          {
            ZESTO_STAT(core->stat.Mop_fetch_slip += Mop->timing.when_fetched - Mop->timing.when_fetch_started;)
            ZESTO_STAT(core->stat.Mop_fetch2decode_slip += Mop->timing.when_decode_started - Mop->timing.when_fetched;)
            ZESTO_STAT(core->stat.Mop_decode_slip += Mop->timing.when_decode_finished - Mop->timing.when_decode_started;)
            ZESTO_STAT(core->stat.Mop_decode2commit_slip += Mop->timing.when_commit_started - Mop->timing.when_decode_finished;)
            ZESTO_STAT(core->stat.Mop_commit_slip += Mop->timing.when_commit_finished - Mop->timing.when_commit_started;)
            zesto_assert(Mop->timing.when_commit_finished != TICK_T_MAX,(void)0);
            zesto_assert(Mop->timing.when_fetched != TICK_T_MAX,(void)0);
            zesto_assert(Mop->timing.when_fetch_started != TICK_T_MAX,(void)0);
            //zesto_assert(Mop->timing.when_fetched != 0,(void)0);
            //zesto_assert(Mop->timing.when_fetch_started != 0,(void)0);
          }
          else
            ZESTO_STAT(core->stat.commit_traps++;)
        }
#ifdef ZTRACE
        ztrace_print(Mop,"c|commit:EOM=%d:trap=%d|all uops in Mop committed; Mop retired",
          Mop->uop[Mop->decode.last_uop_index].decode.EOM,Mop->decode.is_trap);
#endif

        core->update_stopwatch(Mop);

        /* Let the oracle know that we are done with this Mop. */
        core->oracle->commit(Mop);
      }

    }
    else
    {
      if(Mop->commit.complete_index == 0)
        stall_reason = CSTALL_NOT_READY;
      else
        stall_reason = CSTALL_PARTIAL;
      break; /* oldest Mop not done yet */
    }
  }

  ZESTO_STAT(stat_add_sample(core->stat.commit_stall, (int)stall_reason);)
}

void core_commit_IO_DPM_t::step()
{
  /* Compatibility: Simulation can call this */
}

/* squashes a uop, reardless in which stage it is; deallocates from LDQ and STQ; other structures and stats should be updated by the caller; retutns effective ROB entries;
   Because of clearing dependency pointers, should be called in program order, from oldest to youngest */
void core_commit_IO_DPM_t::squash_uop(struct uop_t * const uop)
{

  struct uop_t * dead_uop = uop;

  if(dead_uop->decode.in_fusion)
    dead_uop = dead_uop->decode.fusion_head;
//    zesto_assert(dead_uop->decode.is_fusion_head,0);

  while(dead_uop)
  {
    /* squash this instruction - this invalidates all in-flight actions (e.g., uop execution, cache accesses) */
    dead_uop->exec.action_id = core->new_action_id();

    /* In the following, we have to check if the uop has even been allocated yet... this has
       to do with our non-atomic implementation of allocation for fused-uops */
    if(dead_uop->decode.is_load)
    {
      if(dead_uop->alloc.LDQ_index != -1)
        core->exec->LDQ_squash(dead_uop);
    }
    else if(dead_uop->decode.is_std) /* dealloc when we get to the STD */
    {
      if(dead_uop->alloc.STQ_index != -1)
        core->exec->STQ_squash_std(dead_uop);
    }
    else if(dead_uop->decode.is_sta)
    {
      if(dead_uop->alloc.STQ_index != -1)
        core->exec->STQ_squash_sta(dead_uop);
    }

    /* clean up odep  pointers. We need to clean-up
       our children's back-pointers (ideps)
       and our own forward-pointers */
    struct odep_t * odep = dead_uop->exec.odep_uop;
    struct odep_t * odep_next;

    while(odep)
    {
      odep->uop->exec.idep_uop[odep->op_num] = NULL;

      odep_next = odep->next;

      /* return the odep struct */
      core->return_odep_link(odep);

      odep = odep_next;
    }

    dead_uop->exec.odep_uop = NULL;


    /* dead_uop may have back-pointers (ideps) to non-squashed uops;
       we should remove these */
    struct uop_t * idep;
    for(size_t i=0;i<MAX_IDEPS;i++)
    {
      idep = dead_uop->exec.idep_uop[i];

      if(idep == NULL)
        continue;

      odep = idep->exec.odep_uop;

      struct odep_t * prev_odep = NULL;
      while(odep)
      {
        if(odep->uop == dead_uop)
          break;

        prev_odep = odep;
        odep = odep->next;
      }

      zesto_assert(odep != NULL, 0);

      if(prev_odep != NULL)
        prev_odep->next = odep->next;
      else
        idep->exec.odep_uop = odep->next;

      /* return the odep struct */
      core->return_odep_link(odep);

      dead_uop->exec.idep_uop[i] = NULL;
    }

    for(size_t i=0;i<MAX_IDEPS;i++)
      zesto_assert(dead_uop->exec.idep_uop[i] == NULL, 0);


    if(!dead_uop->decode.in_fusion)
      dead_uop = NULL;
    else
      dead_uop = dead_uop->decode.fusion_next;
  }
}

/* Helper: for a uop, gets the number of fused uops that are also allocated in the ROB.
 * XXX: Unclear if we actually need to check if the uops are in the ROB, but leaving this
 * for now. This pipeline needs a good big refactor anyways. */
static int get_num_eff_uops_in_ROB(const struct uop_t* const uop) {
    if (!uop->decode.in_fusion)
        return (uop->alloc.ROB_index >= 0);

    int result = 0;
    struct uop_t* curr = uop->decode.fusion_head;
    while (curr) {
        result += (curr->alloc.ROB_index >= 0);
        curr = curr->decode.fusion_next;
    }
    return result;
}

/* Walk ROB from oldest uop until we find the requested Mop and start squashing.
   (NOTE: We start after all uops belonging to the Mop.  We assume
   that recovery only occurs on Mop boundaries.)
   Release resources (PREGs, RS/ROB/LSQ entries, etc. as we go). */
void
core_commit_IO_DPM_t::recover(const struct Mop_t * const Mop)
{
  assert(Mop != NULL);
  struct core_knobs_t * knobs = core->knobs;

  struct uop_t * curr_uop = NULL;
  int squashed_uops = 0;
  int old_entries = 0;

  /* flush uops in the ROB */
  if(ROB_num > 0)
  {
    int i=ROB_head;
    do
    {
      curr_uop = ROB[i];

      /* if uop older than squashed one, leave it to commit */
      if(curr_uop->decode.Mop_seq <= Mop->oracle.seq)
      {
        i=modinc(i, knobs->commit.ROB_size);
        old_entries++;
        continue;
      }

      zesto_assert(ROB_num > 0,(void)0);

      int eff_uop_num = get_num_eff_uops_in_ROB(curr_uop);
      this->squash_uop(curr_uop);

      ROB[i] = NULL;
      ROB_num --;
      ROB_eff_num-= eff_uop_num;
      zesto_assert(ROB_num >= 0,(void)0);
      zesto_assert(ROB_eff_num >= 0,(void)0);
      squashed_uops++;

      i=modinc(i, knobs->commit.ROB_size);
    } while(i != ROB_tail);
  }

  ROB_tail = (ROB_tail - squashed_uops) % knobs->commit.ROB_size;
  zesto_assert(ROB_num == old_entries, (void)0);

  /* flush uops in the pre_commit pipe */
  for(int i=knobs->commit.pre_commit_depth-1; i>-1; i--)
  {
    curr_uop = pre_commit_pipe[i];

    if(curr_uop == NULL)
      continue;

    /* if uop older than squashed one, leave it to commit */
    if(curr_uop->decode.Mop_seq <= Mop->oracle.seq)
      continue;

    this->squash_uop(curr_uop);
    pre_commit_pipe[i] = NULL;
    squashed_uops++;
  }

//FIXME: Re-enable after chaning alloc drain part
//  if(knobs->alloc.drain_flush && ROB_num > 0)
//    core->alloc->start_drain();
}

void
core_commit_IO_DPM_t::recover(void)
{
  struct core_knobs_t * knobs = core->knobs;

  struct uop_t * curr_uop = NULL;

  /* flush uops in the ROB */
  if(ROB_num > 0)
  {
    int i=ROB_head;
    do
    {
      curr_uop = ROB[i];

      zesto_assert(ROB_num > 0,(void)0);

      int eff_uop_num = get_num_eff_uops_in_ROB(curr_uop);
      this->squash_uop(curr_uop);

      ROB[i] = NULL;
      ROB_num --;
      ROB_eff_num-= eff_uop_num;
      zesto_assert(ROB_num >= 0,(void)0);
      zesto_assert(ROB_eff_num >= 0,(void)0);

      i=modinc(i, knobs->commit.ROB_size);
    } while(i != ROB_tail);
  }

  zesto_assert(ROB_num == 0,(void)0);
  zesto_assert(ROB_eff_num == 0,(void)0);
  ROB_tail = ROB_head;

  /* flush uops in the pre_commit pipe */
  for(int i=knobs->commit.pre_commit_depth-1; i>-1; i--)
  {
    curr_uop = pre_commit_pipe[i];

    if(curr_uop == NULL)
      continue;

    this->squash_uop(curr_uop);
    pre_commit_pipe[i] = NULL;
  }

  core->exec->STQ_squash_senior();
}

bool core_commit_IO_DPM_t::ROB_available(void)
{
  struct core_knobs_t * knobs = core->knobs;
  return ROB_num < knobs->commit.ROB_size;
}

bool core_commit_IO_DPM_t::ROB_empty(void)
{
  return 0 == ROB_num;
}

bool core_commit_IO_DPM_t::pipe_empty(void)
{
  if(ROB_num > 0)
    return false;

  for(int stage=core->knobs->commit.pre_commit_depth-1; stage>-1; stage--)
    if(pre_commit_pipe[stage] != NULL)
      return false;

  return true;
}

void core_commit_IO_DPM_t::ROB_insert(struct uop_t * const uop)
{
  struct core_knobs_t * knobs = core->knobs;
  ROB[ROB_tail] = uop;
  uop->alloc.ROB_index = ROB_tail;
  ROB_num++;
  ROB_eff_num++;

  ROB_tail = modinc(ROB_tail,knobs->commit.ROB_size); //(ROB_tail+1) % knobs->commit.ROB_size;

  zesto_assert(ROB_num <= knobs->commit.ROB_size, (void)0);
}

void core_commit_IO_DPM_t::ROB_fuse_insert(struct uop_t * const uop)
{
  uop->alloc.ROB_index = uop->decode.fusion_head->alloc.ROB_index;
  ROB_eff_num++;
}

bool core_commit_IO_DPM_t::pre_commit_available()
{
  struct core_knobs_t * knobs = core->knobs;
  for(int i=knobs->commit.width-1; i>-1; i--)
    if(pre_commit_pipe[i] == NULL)
      return true;

  return false;
}

void core_commit_IO_DPM_t::pre_commit_insert(struct uop_t * const uop)
{
  struct core_knobs_t * knobs = core->knobs;
  int i = knobs->commit.width-1;
  for(; i>-1; i--)
    if(pre_commit_pipe[i] == NULL)
    {
       pre_commit_pipe[i] = uop;
       break;
    }

  zesto_assert(i > -1, (void)0);
}

void core_commit_IO_DPM_t::pre_commit_fused_insert(struct uop_t * const uop)
{
  //for now, simply do nothing - only purpose would be to update stats - not

  //since we use fusing to implement atomic execution, if this gets called, we are sending an atomic operation by its actual op part (not LOAD or STORE), so simply add the head and shuffle the whole thing forward (deal with fusion at commit)

  pre_commit_insert(uop->decode.fusion_head);
}

void core_commit_IO_DPM_t::pre_commit_step()
{
  struct core_knobs_t * knobs = core->knobs;
  int stage = knobs->commit.pre_commit_depth-1;

  struct uop_t * uop;

  bool stall = false;

  //send uops at the end of pipe to commit
  for(int j=0; j < knobs->commit.width; j++)
  {
    stage-=j;
    uop = pre_commit_pipe[stage];

    while(uop)
    {
      if((!uop->decode.in_fusion) || uop->decode.is_fusion_head)
      {
        if(!this->ROB_available())// || this->is_stall()) - wrong! stall doesn't mean no new uops in ROB - breaks whole system
        {
          stall = true;
          break;
        }
        else
          this->ROB_insert(uop);
      }
      else
        this->ROB_fuse_insert(uop);

      if(uop->decode.in_fusion)
        uop = uop->decode.fusion_next;
      else
        uop = NULL;
    }

    if(stall)
      return;


    pre_commit_pipe[stage] = NULL;
  }

  //shuffle the rest of the pipe forward
  stage--;
  int dest_stage;
  for(;stage > -1; stage--)
  {
    dest_stage = stage + knobs->commit.width;
    if(pre_commit_pipe[stage])
    {
       //should already be NULL-ed
       zesto_assert(pre_commit_pipe[dest_stage] == NULL, (void)0);
       pre_commit_pipe[dest_stage] = pre_commit_pipe[stage];
       pre_commit_pipe[stage] = NULL;
    }
  }
}

bool core_commit_IO_DPM_t::is_stall()
{
  return !(stall_reason == CSTALL_NONE || stall_reason == CSTALL_EMPTY);
}

void core_commit_IO_DPM_t::pre_commit_recover(struct Mop_t * const Mop)
{
  assert(Mop != NULL);
  struct core_knobs_t * knobs = core->knobs;

  int stage = knobs->commit.pre_commit_depth-1;

  while(stage >= 0)
  {
    struct uop_t * curr_uop = pre_commit_pipe[stage];

    if(!curr_uop)
      continue;

    if(curr_uop->Mop == Mop)
      return;

    this->squash_uop(curr_uop);
    pre_commit_pipe[stage] = NULL;
    stage--;
  }
}

#endif
