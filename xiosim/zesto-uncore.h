#ifndef ZESTO_UNCORE_INCLUDED
#define ZESTO_UNCORE_INCLUDED

/* zesto-uncore.h - Zesto uncore wrapper class
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
 */

#include <memory>

#include "host.h"
#include "knobs.h"

struct bus_t;
struct MC_t;

extern std::unique_ptr<class uncore_t> uncore;

/* container class for the miscellaneous non-core objects, not
   including the L2, but up to and including the front-side bus */
class uncore_t {
  public:
    /* uncore clock (MHz) */
    tick_t sim_cycle;
    /* cycle count scaled to default frequency */
    tick_t default_cpu_cycles;
    double sim_time;

    /* Front-side bus options */
    int fsb_width;
    int fsb_bits; /* log2(fsb_width) */
    double fsb_speed;
    bool fsb_DDR;

    /* shared last-level cache */
    std::unique_ptr<struct bus_t> LLC_bus;
    std::unique_ptr<struct cache_t> LLC;
    tick_t LLC_cycle_mask;

    std::unique_ptr<struct bus_t> fsb;

    std::unique_ptr<class MC_t> MC;

    /* constructor */
    uncore_t(const uncore_knobs_t& knobs);
    virtual ~uncore_t();
};

void uncore_reg_stats(xiosim::stats::StatsDatabase* sdb);
void uncore_create(const uncore_knobs_t& knobs); /* creates MC and memory controller objects */

#endif /* ZESTO_UNCORE_INCLUDED */
