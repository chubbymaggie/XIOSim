/* decode-DPM.cpp - Detailed Pipeline Model */
/*
 * __COPYRIGHT__ GT
 */

#ifdef ZESTO_PARSE_ARGS
  if(!strcasecmp(decode_opt_string,"DPM")
		  || !strcasecmp(decode_opt_string,"IO-DPM"))
    return std::make_unique<class core_decode_DPM_t>(core);
#else

class core_decode_DPM_t:public core_decode_t
{
  enum decode_stall_t {DSTALL_NONE,   /* no stall */
                       DSTALL_FULL,   /* first decode stage is full */
                       DSTALL_REP,    /* REP inst waiting for microsequencer */
                       DSTALL_UROM,   /* big flow waiting for UROM */
                       DSTALL_SMALL,  /* available decoder is too small */
                       DSTALL_EMPTY,  /* no insts to decode */
                       DSTALL_PHANTOM,/* front-end predicted taken when no branch was present */
                       DSTALL_TARGET, /* target correction flushed remaining insts (only occurs when target_stage == crack_stage-1) */
                       DSTALL_MAX_BRANCHES, /* exceeded maximum number of branches decoded per cycle */
                       DSTALL_num
                      };

  public:

  /* constructor, stats registration */
  core_decode_DPM_t(struct core_t * const core);
  ~core_decode_DPM_t();
  virtual void reg_stats(xiosim::stats::StatsDatabase* sdb);
  virtual void update_occupancy(void);

  virtual void step(void);
  virtual void recover(void);
  virtual void recover(struct Mop_t * const Mop);

  /* interface functions for alloc stage */
  virtual bool uop_available(void);
  virtual struct uop_t * uop_peek(void);
  virtual void uop_consume(void);

  protected:

  struct Mop_t *** pipe; /* the actual decode pipe */
  int * occupancy;

  struct uop_t ** uopQ; /* sits between decode and alloc */
  int uopQ_head;
  int uopQ_tail;
  int uopQ_num;
  int uopQ_eff_num;

  static const char * decode_stall_str[DSTALL_num];

  enum decode_stall_t check_target(struct Mop_t * const Mop);
  bool check_flush(const int stage, const int idx);
  void recover_decode_pipe(const struct Mop_t * const Mop);
  void recover_decode_pipe(void);
  void recover_uopQ(const struct Mop_t * const Mop);
  void recover_uopQ(void);
};

const char * core_decode_DPM_t::decode_stall_str[DSTALL_num] = {
  "no stall                   ",
  "next decode stage is full  ",
  "REP inst waiting for MS    ",
  "long flow waiting for UROM ",
  "available decoder too small",
  "no insts to decode         ",
  "phantom taken branch       ",
  "target correction          ",
  "branch decode limit        ",
};

core_decode_DPM_t::core_decode_DPM_t(struct core_t * const arg_core):
  uopQ_head(0), uopQ_tail(0), uopQ_num(0), uopQ_eff_num(0)
{
  struct core_knobs_t * knobs = arg_core->knobs;
  core = arg_core;

  if(knobs->decode.depth <= 0)
    fatal("decode pipe depth must be > 0");

  if(knobs->decode.width <= 0)
    fatal("decode pipe width must be > 0");

  if(knobs->decode.target_stage <= 0 || knobs->decode.target_stage >= knobs->decode.depth)
    fatal("decode target resteer stage (%d) must be > 0, and less than decode pipe depth (currently set to %d)",knobs->decode.target_stage,knobs->decode.depth);

  /* if the pipe is N wide, we assume there are N decoders */
  pipe = (struct Mop_t***) calloc(knobs->decode.depth,sizeof(*pipe));
  if(!pipe)
    fatal("couldn't calloc decode pipe");

  for(int i=0;i<knobs->decode.depth;i++)
  {
    pipe[i] = (struct Mop_t**) calloc(knobs->decode.width,sizeof(**pipe));
    if(!pipe[i])
      fatal("couldn't calloc decode pipe stage");
  }

  occupancy = (int*) calloc(knobs->decode.depth,sizeof(*occupancy));
  if(!occupancy)
    fatal("couldn't calloc decode pipe occupancy array");

  uopQ = (struct uop_t**) calloc(knobs->decode.uopQ_size,sizeof(*uopQ));
  if(!uopQ)
    fatal("couldn't calloc uopQ");
}

core_decode_DPM_t::~core_decode_DPM_t() {
    free(uopQ);

    free(occupancy);
    for (int i = 0; i < core->knobs->decode.depth; i++)
        free(pipe[i]);
    free(pipe);
}

void core_decode_DPM_t::reg_stats(xiosim::stats::StatsDatabase* sdb) {
    int coreID = core->id;
 
    stat_reg_note(sdb, "\n#### DECODE STATS ####");
    stat_reg_core_counter(sdb, true, coreID, "target_resteers", "decode-time target resteers",
                          &core->stat.target_resteers, 0, true, NULL);
    stat_reg_core_counter(sdb, true, coreID, "phantom_resteers", "decode-time phantom resteers",
                          &core->stat.phantom_resteers, 0, true, NULL);
    auto& decode_insn_st = stat_reg_core_counter(sdb, true, coreID, "decode_insn",
                                                 "total number of instructions decodeed",
                                                 &core->stat.decode_insn, 0, true, NULL);
    auto& decode_uops_st =
            stat_reg_core_counter(sdb, true, coreID, "decode_uops", "total number of uops decodeed",
                                  &core->stat.decode_uops, 0, true, NULL);
    auto& decode_eff_uops_st = stat_reg_core_counter(sdb, true, coreID, "decode_eff_uops",
                                                     "total number of effective uops decodeed",
                                                     &core->stat.decode_eff_uops, 0, true, NULL);
    auto core_sim_cycles_st = stat_find_core_stat<tick_t>(sdb, coreID, "sim_cycle");
    assert(core_sim_cycles_st);
    stat_reg_core_formula(sdb, true, coreID, "decode_IPC", "IPC at decode",
                          decode_insn_st / *core_sim_cycles_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "decode_uPC", "uPC at decode",
                          decode_uops_st / *core_sim_cycles_st, NULL);
    stat_reg_core_formula(sdb, true, coreID, "decode_euPC", "effective uPC at decode",
                          decode_eff_uops_st / *core_sim_cycles_st, NULL);
    reg_core_queue_occupancy_stats(sdb, coreID, "uopQ", &core->stat.uopQ_occupancy,
                                   &core->stat.uopQ_empty_cycles,
                                   &core->stat.uopQ_full_cycles);
    core->stat.decode_stall = stat_reg_core_dist(
            sdb, coreID, "decode_stall", "breakdown of stalls at decode", 0, DSTALL_num,
            (PF_COUNT | PF_PDF), NULL, decode_stall_str, true, NULL);
}

void core_decode_DPM_t::update_occupancy(void)
{
    /* uopQ */
  core->stat.uopQ_occupancy += uopQ_num;
  core->stat.uopQ_eff_occupancy += uopQ_eff_num;
  if(uopQ_num >= core->knobs->decode.uopQ_size)
    core->stat.uopQ_full_cycles++;
  if(uopQ_num <= 0)
    core->stat.uopQ_empty_cycles++;
}


/*************************/
/* MAIN DECODE FUNCTIONS */
/*************************/

/* Helper functions to check for BAC-related resteers */

/* Check an individual Mop to see if it's next target is correct/reasonable, and
   recover if necessary. */
enum core_decode_DPM_t::decode_stall_t
core_decode_DPM_t::check_target(struct Mop_t * const Mop)
{
  if(!Mop->decode.is_ctrl)
  {
    /* next-PC had better be the next sequential inst */
    if(Mop->fetch.pred_NPC != Mop->fetch.ftPC)
    {
      /* it's not (phantom taken branch), so resteer */
      ZESTO_STAT(core->stat.phantom_resteers++;)
      core->oracle->recover(Mop);
      recover_decode_pipe(Mop);
      Mop->fetch.pred_NPC = Mop->fetch.ftPC;
      core->fetch->recover(Mop->fetch.pred_NPC);
      return DSTALL_PHANTOM;
    }
  }
  else /* branch or REP */
  {
    if((Mop->fetch.pred_NPC != Mop->fetch.ftPC) /* branch is predicted taken */
        || Mop->decode.opflags.UNCOND)
    {
      if(Mop->fetch.pred_NPC != Mop->decode.targetPC) /* wrong target */
      {
        if(Mop->fetch.bpred_update)
          core->fetch->bpred->recover(Mop->fetch.bpred_update,/*taken*/1);
        ZESTO_STAT(core->stat.target_resteers++;)
        core->oracle->recover(Mop);
        recover_decode_pipe(Mop);
        core->fetch->recover(Mop->decode.targetPC);
        Mop->fetch.pred_NPC = Mop->decode.targetPC;
        return DSTALL_TARGET;
      }
    }
  }
  return DSTALL_NONE;
}

/* Perform branch-address calculation check for a given decode-pipe stage
   and Mop position. */
bool core_decode_DPM_t::check_flush(const int stage, const int idx)
{
  struct core_knobs_t * knobs = core->knobs;
  /* stage-1 because we assume the following checks are acted upon only after
     the instruction has spent a cycle in the corresponding pipeline stage. */
  struct Mop_t * Mop = pipe[stage-1][idx];
  enum decode_stall_t stall_reason;

  if(Mop)
  {
    if((stage-1) == knobs->decode.target_stage)
    {
      if((stall_reason = check_target(Mop)))
      {
        pipe[stage][idx] = pipe[stage-1][idx];
        pipe[stage-1][idx] = NULL;
        if(pipe[stage][idx]) {
          occupancy[stage]++;
          occupancy[stage-1]--;
          zesto_assert(occupancy[stage] <= knobs->decode.width,false);
          zesto_assert(occupancy[stage-1] >= 0,false);
        }
        ZESTO_STAT(stat_add_sample(core->stat.decode_stall, (int)stall_reason);)
#ifdef ZTRACE
        ztrace_print(Mop,"d|target-check|mispred target resteer");
#endif
        return true;
      }
    }
  }

  return false;
}


/* attempt to send uops to the uopQ, shuffle other Mops down the decode pipe, read Mops from IQ */
void core_decode_DPM_t::step(void)
{
  struct core_knobs_t * knobs = core->knobs;
  int stage, i;
  enum decode_stall_t stall_reason = DSTALL_NONE;

  /* move decoded uops to uopQ */
  stage = knobs->decode.depth-1;
  if(occupancy[stage])
    for(i=0;i<knobs->decode.width;i++)
    {
      if(pipe[stage][i])
      {
        while(uopQ_num < knobs->decode.uopQ_size)   /* while uopQ is not full */
        {
          struct Mop_t * Mop = pipe[stage][i];      /* Mop in current decoder */
          struct uop_t * uop = &Mop->uop[Mop->decode.last_stage_index]; /* first non-queued uop */
          Mop->decode.last_stage_index += uop->decode.has_imm ? 3 : 1;  /* increment the uop pointer to next uop */
          
          if((!uop->decode.in_fusion) || uop->decode.is_fusion_head) /* don't enqueue fusion body */
          {
            uopQ[uopQ_tail] = uop;      /* queue the uop */
            uopQ_tail = modinc(uopQ_tail,knobs->decode.uopQ_size); //(uopQ_tail+1) % knobs->decode.uopQ_size;
            uopQ_num++;
            if(uop->decode.is_fusion_head)
              uopQ_eff_num += uop->decode.fusion_size;
            else
              uopQ_eff_num++;
            ZESTO_STAT(core->stat.decode_uops++;)
#ifdef ZTRACE
            ztrace_print(uop,"d|uopQ|enqueue");
#endif
          }
#ifdef ZTRACE
          else
            ztrace_print(uop,"d|uopQ|enqueue fused body");
#endif
          ZESTO_STAT(core->stat.decode_eff_uops++;)
          uop->timing.when_decoded = core->sim_cycle;
          if(uop->decode.BOM)
            uop->Mop->timing.when_decode_started = core->sim_cycle;
          if(uop->decode.EOM)
            uop->Mop->timing.when_decode_finished = core->sim_cycle;

          if(Mop->decode.last_stage_index >= Mop->decode.flow_length)
          {
            if(Mop->uop[Mop->decode.last_uop_index].decode.EOM)
              ZESTO_STAT(core->stat.decode_insn++;)
            /* all uops dispatched, remove from decoder */
            pipe[stage][i] = NULL;
            occupancy[stage]--;
            zesto_assert(occupancy[stage] >= 0,(void)0);
#ifdef ZTRACE
            ztrace_print(Mop,"d|decode-pipe|dequeue");
#endif
            break;
          }
        }
        if(uopQ_num >= knobs->decode.uopQ_size)
          break; /* uopQ is full */
      }
    }

  /* walk pipe backwards up to and but not including the first stage*/
  for(stage=knobs->decode.depth-1; stage > 0; stage--)
  {
    if(0 == occupancy[stage]) /* implementing non-serpentine pipe (no compressing) - can't advance until stage is empty */
    {
      zesto_assert(occupancy[stage] == 0,(void)0);
      tick_t when_MS_started = pipe[stage-1][0] ? pipe[stage-1][0]->timing.when_MS_started : 0;
      if((when_MS_started != TICK_T_MAX) && (when_MS_started >= core->sim_cycle)) /* checks for non-zero MS/uROM delay */
        break;

      /* move everyone from previous stage forward */
      if(occupancy[stage-1])
        for(i=0;i<knobs->decode.width;i++)
        {
          if(check_flush(stage,i))
            return;

          pipe[stage][i] = pipe[stage-1][i];
          pipe[stage-1][i] = NULL;
          if(pipe[stage][i]) {
            occupancy[stage]++;
            occupancy[stage-1]--;
            zesto_assert(occupancy[stage] <= knobs->decode.width,(void)0);
            zesto_assert(occupancy[stage-1] >= 0,(void)0);
          }
        }
    }
  }

  /* process the first stage, which reads Mops from the IQ */
  /* we're moving instructions into the decoder.  decoder must be able to handle the Mop */
  {
    if(!core->fetch->Mop_available())
    {
      stall_reason = DSTALL_EMPTY;
    }
    else
    {
      int Mops_decoded = 0;
      int branches_decoded = 0;
      for(i=0;(i<knobs->decode.width) && core->fetch->Mop_available();i++)
      {
        if(pipe[0][i] == NULL) /* decoder available */
        {
          struct Mop_t * IQ_Mop = core->fetch->Mop_peek();

          if(IQ_Mop->decode.is_ctrl && knobs->decode.branch_decode_limit && (branches_decoded >= knobs->decode.branch_decode_limit))
          {
            stall_reason = DSTALL_MAX_BRANCHES;
            break;
          }

          if(0==i) /* special case first decoder since it can access the uROM and MS */
          {
            /* consume the Mop from the IQ */
            pipe[0][i] = IQ_Mop;
            occupancy[0]++;
            zesto_assert(occupancy[0] <= knobs->decode.width,(void)0);
            core->fetch->Mop_consume();
            Mops_decoded++;

#ifdef ZTRACE
            ztrace_print(IQ_Mop,"d|decode-pipe|enqueue");
#endif

            /* does this Mop need help from the MS? */
            if((knobs->decode.max_uops[i] && (pipe[0][i]->stat.num_uops > knobs->decode.max_uops[i])) ||
                pipe[0][i]->decode.has_rep)
              pipe[0][i]->timing.when_MS_started = core->sim_cycle + knobs->decode.MS_latency; /* all other insts (non-MS) have this timestamp default to TICK_T_MAX */
            if(IQ_Mop->decode.is_ctrl)
              branches_decoded++;
          }
          else /* other decoders must check uop limits */
          {
            if((!knobs->decode.max_uops[i] || (IQ_Mop->stat.num_uops <= knobs->decode.max_uops[i])) &&
                !IQ_Mop->decode.has_rep) /* decoder0 handles reps */
            {
              /* consume the Mop from the IQ */
              pipe[0][i] = IQ_Mop;
              occupancy[0]++;
              zesto_assert(occupancy[0] <= knobs->decode.width,(void)0);
              core->fetch->Mop_consume();
              Mops_decoded++;
              if(IQ_Mop->decode.is_ctrl)
                branches_decoded++;
#ifdef ZTRACE
              ztrace_print(IQ_Mop,"d|decode-pipe|enqueue");
#endif
            }
            else
            {
              if(IQ_Mop->decode.has_rep)
                stall_reason = DSTALL_REP;
              else if((IQ_Mop->stat.num_uops < knobs->decode.max_uops[0]) && knobs->decode.max_uops[0])
                stall_reason = DSTALL_SMALL; /* flow too big for current decoder, but small enough for complex decoder */
              else
                stall_reason = DSTALL_UROM;
              break; /* can't advance Mop, wait 'til next cycle to try again */
            }
          }
        }
        if(Mops_decoded == 0)
          stall_reason = DSTALL_FULL;
      }
    }
  }

  ZESTO_STAT(stat_add_sample(core->stat.decode_stall, (int)stall_reason);)
}

void
core_decode_DPM_t::recover(void)
{
  recover_uopQ();
  recover_decode_pipe();
}

void
core_decode_DPM_t::recover(struct Mop_t * const Mop)
{
  struct core_knobs_t * knobs = core->knobs;
  /* walk pipe from youngest uop blowing everything away,
     stop if we encounter the recover-Mop */
  for(int stage=0;stage<knobs->decode.depth;stage++)
  {
    if(occupancy[stage])
      for(int i=knobs->decode.width-1;i>=0;i--)
      {
        if(pipe[stage][i])
        {
          if(pipe[stage][i] == Mop)
            return;
          else
          {
            pipe[stage][i] = NULL;
            occupancy[stage]--;
            zesto_assert(occupancy[stage] >= 0,(void)0);
          }
        }
      }
  }
  recover_uopQ(Mop);
}

/* start from most speculative; stop if we find the Mop */
void
core_decode_DPM_t::recover_uopQ(const struct Mop_t * const Mop)
{
  struct core_knobs_t * knobs = core->knobs;

  while(uopQ_num)
  {
    int index = moddec(uopQ_tail,knobs->decode.uopQ_size); //(uopQ_tail-1+knobs->decode.uopQ_size) % knobs->decode.uopQ_size;
    if(uopQ[index]->Mop == Mop)
      return;
    if(uopQ[index]->decode.is_fusion_head)
      uopQ_eff_num -= uopQ[index]->decode.fusion_size;
    else
      uopQ_eff_num --;
    uopQ_num--;
    uopQ[index] = NULL;
    uopQ_tail = index;
  }
  uopQ_eff_num = 0;
}

/* blow away the entire uopQ */
void
core_decode_DPM_t::recover_uopQ(void)
{
  struct core_knobs_t * knobs = core->knobs;

  /*
  while(uopQ_num)
  {
    int index = moddec(uopQ_tail,knobs->decode.uopQ_size); //(uopQ_tail-1+knobs->decode.uopQ_size) % knobs->decode.uopQ_size;
    uopQ[index] = NULL;
    uopQ_tail = index;
    uopQ_num--;
  }
  */
  memzero(uopQ,sizeof(*uopQ)*knobs->decode.uopQ_size);
  uopQ_head = 0;
  uopQ_tail = 0;
  uopQ_num = 0;
  uopQ_eff_num = 0;
}

void
core_decode_DPM_t::recover_decode_pipe(const struct Mop_t * const Mop)
{
  struct core_knobs_t * knobs = core->knobs;
  /* walk pipe from youngest uop blowing everything away,
     stop if we encounter the recover-Mop */
  for(int stage=0;stage<knobs->decode.depth;stage++)
  {
    if(occupancy[stage])
      for(int i=knobs->decode.width-1;i>=0;i--)
      {
        if(pipe[stage][i])
        {
          if(pipe[stage][i] == Mop)
            return;
          else
          {
            pipe[stage][i] = NULL;
            occupancy[stage]--;
            zesto_assert(occupancy[stage] >= 0,(void)0);
          }
        }
      }
  }
}

/* same as above, but blow away the entire decode pipeline */
void
core_decode_DPM_t::recover_decode_pipe(void)
{
  struct core_knobs_t * knobs = core->knobs;
  for(int stage=0;stage<knobs->decode.depth;stage++)
  {
    if(occupancy[stage])
      for(int i=knobs->decode.width-1;i>=0;i--)
      {
        if(pipe[stage][i])
        {
          pipe[stage][i] = NULL;
          occupancy[stage]--;
          zesto_assert(occupancy[stage] >= 0,(void)0);
        }
      }
    zesto_assert(occupancy[stage] == 0,(void)0);
  }
}

bool core_decode_DPM_t::uop_available(void)
{
  return uopQ_num > 0;
}

struct uop_t * core_decode_DPM_t::uop_peek(void)
{
  return uopQ[uopQ_head];
}

void core_decode_DPM_t::uop_consume(void)
{
  struct core_knobs_t * knobs = core->knobs;
  struct uop_t * uop = uopQ[uopQ_head];
  uopQ[uopQ_head] = NULL;
  uopQ_num --;
  if(uop->decode.is_fusion_head)
    uopQ_eff_num -= uop->decode.fusion_size;
  else
    uopQ_eff_num --;
  uopQ_head = modinc(uopQ_head,knobs->decode.uopQ_size); //(uopQ_head+1)%knobs->decode.uopQ_size;
}

#endif
