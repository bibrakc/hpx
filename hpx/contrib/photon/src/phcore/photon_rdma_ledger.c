// =============================================================================
//  Photon RDMA Library (libphoton)
//
//  Copyright (c) 2016, Trustees of Indiana University,
//  All rights reserved.
//
//  This software may be modified and distributed under the terms of the BSD
//  license.  See the COPYING file for details.
//
//  This software was created at the Indiana University Center for Research in
//  Extreme Scale Technologies (CREST).
// =============================================================================

#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#include "logging.h"
#include "photon_rdma_ledger.h"
#include "photon_buffer.h"
#include "photon_exchange.h"
#include "photon_event.h"

static int _get_remote_progress(int proc, photonLedger buf);

photonLedger photon_rdma_ledger_create_reuse(void *ledger_buffer, int num_entries,
					     int entry_size, int prefix) {
  photonLedger new;
  
  new = (struct photon_rdma_ledger_t *)((uintptr_t)ledger_buffer +
					PHOTON_LEDG_SSIZE(num_entries, entry_size) -
					sizeof(struct photon_rdma_ledger_t));
  
  memset(new, 0, sizeof(struct photon_rdma_ledger_t));

  new->entries = ledger_buffer;

  // we bzero this out so that valgrind doesn't believe these are
  // "uninitialized". They get filled in via RDMA so valgrind doesn't
  // know that the values have been filled in
  memset(new->entries, 0, entry_size * num_entries);

  new->prog = 0;
  new->curr = 0;
  new->tail = 0;
  new->entry_size = entry_size;
  new->num_entries = num_entries;
  new->acct.rcur = 0;
  new->acct.rloc = 0;
  new->acct.event_prefix = prefix;

  return new;
}

void photon_rdma_ledger_free(photonLedger ledger) {
  //free(ledger);
}

int photon_rdma_ledger_get_next(int proc, photonLedger l) {
  uint64_t curr, tail, rcur;
  int rc = __photon_backend->tx_size_left(proc);
  if (rc < 2) {
    return -1;
  }
  
  do {
    rcur = sync_load(&l->acct.rcur, SYNC_RELAXED);
    curr = sync_load(&l->curr, SYNC_RELAXED);
    tail = sync_load(&l->tail, SYNC_RELAXED);
    if ((curr - tail) >= l->num_entries) {
      //log_err("Exceeded number of outstanding ledger entries - increase ledger size or wait for completion");
      return -1;
    }
    if ((curr - rcur) >= (l->num_entries)) {
      // receiver not ready, request an updated rcur
      _get_remote_progress(proc, l);
      dbg_trace("No new ledger entry until receiver catches up...");
      return -2;
    }
  } while (!sync_cas(&l->curr, &curr, curr+1, SYNC_RELAXED, SYNC_RELAXED));

  if ((curr - rcur) == (int)(l->num_entries * 0.8)) {
    // do a pro-active fetch of the remote ledger progress
    _get_remote_progress(proc, l);
  }
  
  return curr & (l->num_entries - 1);
}

static int _get_remote_progress(int proc, photonLedger buf) {
  int rc;
  uint32_t rloc;
  uint64_t cookie;
  uintptr_t rmt_addr;
  photonRequest req;
  
  rc = __photon_try_one_event(&req);
  if (rc == PHOTON_EVENT_ERROR) {
    dbg_err("Failure getting event");
    return PHOTON_ERROR;
  }

  rloc = 0;
  if (sync_cas(&buf->acct.rloc, &rloc, 1, SYNC_ACQUIRE, SYNC_RELAXED)) {
    
    dbg_trace("Fetching remote ledger (%d) curr at rcur: %llu", proc, buf->acct.rcur);
    
    rmt_addr = buf->remote.addr + PHOTON_LEDG_SSIZE(buf->num_entries, PHOTON_CID_RECV_ENTRY_SIZE) -
      sizeof(struct photon_rdma_ledger_t) + offsetof(struct photon_rdma_ledger_t, prog); 
    
    cookie = ( (uint64_t)buf->acct.event_prefix<<32) | proc;
    
    rc = __photon_backend->rdma_get(proc, (uintptr_t)&buf->acct.rcur, rmt_addr, sizeof(buf->acct.rcur),
				    &(shared_storage->buf), &buf->remote, cookie, 0);
    if (rc != PHOTON_OK) {
      dbg_err("RDMA GET for remote ledger progress counter failed");
      return PHOTON_ERROR;
    }
  }

  return PHOTON_OK;
}
