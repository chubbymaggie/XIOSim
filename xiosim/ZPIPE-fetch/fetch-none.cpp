#ifdef ZESTO_PARSE_ARGS
  if(!strcasecmp(fetch_opt_string,"none"))
    return std::make_unique<class core_fetch_NONE_t>(core);
#else

class core_fetch_NONE_t:public core_fetch_t
{

  public:

  /* constructor, stats registration */
  core_fetch_NONE_t(struct core_t * const core) { this->core = core; }
  virtual void reg_stats(xiosim::stats::StatsDatabase* sdb) { }
  virtual void update_occupancy(void) { }

  /* simulate one cycle */
  virtual void pre_fetch(void) { }
  virtual bool do_fetch(void) {
    if (core->oracle->is_draining())
        return false;
    // Invoke oracle to crack the Mop.
    struct Mop_t* Mop = core->oracle->exec(PC);
    // Oracle stall (we're looking too far ahead).
    if (Mop == nullptr)
        return false;
    core->oracle->consume(Mop);
    PC = Mop->oracle.NextPC;
    return false;
  }
  virtual void post_fetch(void) { }

  /* decode interface */
  virtual bool Mop_available(void) { return false; }
  virtual struct Mop_t * Mop_peek(void) { return NULL;}
  virtual void Mop_consume(void) { }

  /* enqueue a jeclear event into the jeclear_pipe */
  virtual void jeclear_enqueue(struct Mop_t * const Mop, const md_addr_t New_PC) { }
  /* recover the front-end after the recover request actually
     reaches the front end. */
  virtual void recover(const md_addr_t new_PC) { }
};

#endif
