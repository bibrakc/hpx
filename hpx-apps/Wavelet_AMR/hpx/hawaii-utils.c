// ============================================================================
//  High Performance ParalleX Library (hpx-apps)
//
//  Copyright (c) 2013-2017, Trustees of Indiana University,
//  All rights reserved.
//
//  This software may be modified and distributed under the terms of the BSD
//  license.  See the COPYING file for details.
//
//  This software was created at the Indiana University Center for Research in
//  Extreme Scale Technologies (CREST).
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>
#include <assert.h>
#include <float.h>
#include "hawaii.h"
#include "hawaii-rmhd.h"

double eps[n_variab];
double eps_scale[n_variab];
double L_dim[n_dim];
const int deriv_offset[4] = {-2, -1, 1, 2};

int floatcmp(double a,double b) {
  double epsilon = 1.e-8;
  if ( a < b + epsilon && a > b - epsilon ) return 1;
  else return 0;
}

#ifdef HPX
hpx_action_t _complete_hlle_stencils_a = 0;
hpx_action_t _complete_hlle_stencils_b = 0;
hpx_action_t _complete_hlle_stencils_c = 0;
hpx_action_t _check_extension = 0;
hpx_action_t _get_global_dt = 0;
hpx_action_t _global_dt_helper = 0;
hpx_action_t _2doutput = 0;
hpx_action_t _wavelet_int_helper = 0;
hpx_action_t _rhs_helper_a = 0;
hpx_action_t _integrator_helper_a = 0;
hpx_action_t _wavelet_adjust_grids_helper = 0;
hpx_action_t _adjust_add_neighbors_helper = 0;
hpx_action_t _status_validate_up_helper = 0;
hpx_action_t _status_validate_down_helper = 0;
hpx_action_t _set_eps_scale = 0;

int _action_set_eps_scale(EPSS *ld, size_t size)
{
  set_eps_scale(ld->valmin, ld->valmax);

  return HPX_SUCCESS;
}

int _action_status_validate_down_helper(void *UNUSED, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *neighbor = NULL;

  if (!hpx_gas_try_pin(local, (void**)&neighbor))
    return HPX_RESEND;

  neighbor->status[FUTURE_STATUS] =
     superior_status(neighbor->status[FUTURE_STATUS], neighboring);
  if (neighbor->status[FUTURE_STATUS] == essential) {
    status_validate_down_helper(neighbor);
  }

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _action_status_validate_up_helper(void *UNUSED, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *neighbor = NULL;

  if (!hpx_gas_try_pin(local, (void**)&neighbor))
    return HPX_RESEND;

  status_validate_up_helper(neighbor);

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _action_adjust_add_neighbors_helper(void *UNUSED, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *neighbor = NULL;

  if (!hpx_gas_try_pin(local, (void**)&neighbor))
    return HPX_RESEND;

  if (neighbor->status[CURRENT_STATUS] == essential) {
    //descend if this is a currently essential point
    adjust_add_neighbors_helper(neighbor);
  } else if (neighbor->status[FUTURE_STATUS] == essential) {
    //or if it is to become essential, we add neighbors
    create_neighboring_point(neighbor);
  }

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _action_wavelet_adjust_grids_helper(void *UNUSED, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *neighbor = NULL;

  if (!hpx_gas_try_pin(local, (void**)&neighbor))
    return HPX_RESEND;

  int wavelet_mask[n_variab + n_aux] = {0};
  for (int ivar = 0; ivar < n_variab; ivar++)
    wavelet_mask[ivar] = 1;
  const int gen = 0;

  check_wavelet_stencil(neighbor, gen);
  wavelet_trans(neighbor, primary_set_wavelet, gen, wavelet_mask);
  coll_status_t self = set_point_status(neighbor->wavelet[gen]);

  if (has_superior_status(self, neighbor->status[FUTURE_STATUS])) {
    // set the future status to be self. Notice that at an essential point,
    // self can be nonessential. we set it to be nonessential here
    // regardless and will make necessary changes in status validatation
    // phase.
    neighbor->status[FUTURE_STATUS] = self;
    if (neighbor->status[CURRENT_STATUS] == neighboring &&
        self == essential) {
      create_neighboring_point(neighbor);
    }
  }
  //hpx_lco_sema_v_sync(_table_locks[morton_key(&neighbor->index) % LOCK_TBL_SIZE]);

  // Continue visiting other active points if the point's old status is
  // essential
  if (neighbor->status[CURRENT_STATUS] == essential)
    wavelet_adjust_grids_helper(neighbor);

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _action_integrator_helper_a(ATI *ld, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *neighbor = NULL;

  if (!hpx_gas_try_pin(local, (void**)&neighbor))
    return HPX_RESEND;

  double dt = ld->dt;
  int gen = ld->gen;

  int descend = 0;
  hpx_lco_sema_p(_table_locks[morton_key(&neighbor->index) % LOCK_TBL_SIZE]);
  if (neighbor->time_stamp < time_stamp) {
    if ( gen == 0 ) {
      rk4_helper1(neighbor,dt);
      descend = 1;
    } else if ( gen == 1 ) {
      rk4_helper2(neighbor,dt);
      descend = 1;
    } else if ( gen == 2 ) {
      rk4_helper3(neighbor,dt);
      descend = 1;
    } else if ( gen == 3 ) {
      rk4_helper4(neighbor,dt);
      descend = 1;
    }
  }
  hpx_lco_sema_v_sync(_table_locks[morton_key(&neighbor->index) % LOCK_TBL_SIZE]);
  if (descend && neighbor->status[CURRENT_STATUS] == essential)
    integrator_helper(neighbor, dt, gen);

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _action_rhs_helper_a(RHSH *ld, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *neighbor = NULL;

  if (!hpx_gas_try_pin(local, (void**)&neighbor))
    return HPX_RESEND;

  if ( ld->func_type == hllerhsstage1 ) {
    hlle_rhs_stage1(neighbor,ld->gen);
  } else if ( ld->func_type == hllerhsstage2 ) {
    hlle_rhs_stage2(neighbor,ld->gen);
  } else {
    printf(" UNKOWN func type in rhs_helper_a\n");
    hpx_abort();
  }

  if (neighbor->status[CURRENT_STATUS] == essential) {
    rhs_helper(neighbor, ld->gen, ld->func_type);
  }

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _action_wavelet_int_helper(WIH *ld, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *neighbor = NULL;

  if (!hpx_gas_try_pin(local, (void**)&neighbor))
    return HPX_RESEND;

  //int stagecheck = neighbor->stage[RHS_WAVELET]++;
  int stagecheck = __atomic_fetch_add(&neighbor->stage[RHS_WAVELET], 1, memory_order_acq_rel);

  if (!stagecheck) {
    wavelet_trans(neighbor, ld->type, ld->gen, ld->wavelet_mask);
    if (neighbor->status[CURRENT_STATUS] == essential) {
      wavelet_integrator_helper(neighbor, ld->gen, ld->type, ld->wavelet_mask);
    }
  }

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _action_get_global_dt(GGD *ld, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *point = NULL;

  if (!hpx_gas_try_pin(local, (void**)&point))
    return HPX_RESEND;

  double local_dt = get_local_dt(point);
  ld->global_dt = fmin(local_dt, ld->global_dt);

  minmax_range_reducer(point->u[0], ld->valmin, ld->valmax);

  if (point->level == 1 && point->status[CURRENT_STATUS] == essential)
    global_dt_helper(point, &(ld->global_dt),ld->valmin,ld->valmax);

  hpx_gas_unpin(local);
  return HPX_THREAD_CONTINUE(*ld);
}

int _action_global_dt_helper(GGD *ld, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *neighbor = NULL;

  if (!hpx_gas_try_pin(local, (void**)&neighbor))
    return HPX_RESEND;

  const int gen = 0;
  double local_dt = get_local_dt(neighbor);
  ld->global_dt = fmin(ld->global_dt, local_dt);

  minmax_range_reducer(neighbor->u[gen], ld->valmin, ld->valmax);

  if (neighbor->status[0] == essential)
    global_dt_helper(neighbor, &(ld->global_dt),ld->valmin,ld->valmax);

  hpx_gas_unpin(local);
  return HPX_THREAD_CONTINUE(*ld);
}

int _complete_hlle_stencils_a_action(void *UNUSED, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *point = NULL;

  if (!hpx_gas_try_pin(local, (void**)&point))
    return HPX_RESEND;

  hlle_stencil_completion_helper(point);

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _complete_hlle_stencils_b_action(void *UNUSED, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *point = NULL;

  if (!hpx_gas_try_pin(local, (void**)&point))
    return HPX_RESEND;

  for (int j = 0; j < ADJUST_STAGES; ++j) {
    point->adjust[j] = 0;
  }

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}

int _complete_hlle_stencils_c_action(void *UNUSED, size_t size)
{
  for (int i = 0; i < HASH_TBL_SIZE; ++i) {
    hpx_hash_entry_t *curr = hpx_coll_points.hash_table[i];
    while (curr != NULL) {
      int e = hpx_call_sync(curr->point, _complete_hlle_stencils_b, NULL, 0, NULL, 0);
      if (e != HPX_SUCCESS) hpx_abort();
      curr = curr->next;
    }
  }
  return HPX_SUCCESS;
}

int _action_check_extension(CES *ld, size_t size)
{
  hpx_addr_t local = hpx_thread_current_target();
  coll_point_t *temp = NULL;

  if (!hpx_gas_try_pin(local, (void**)&temp))
    return HPX_RESEND;

  if ( !(ld->flag) ) {
    create_nonessential_point(temp, &(ld->index));
  } else if (temp->time_stamp < time_stamp) {
    temp->status[CURRENT_STATUS] = nonessential;
    temp->status[FUTURE_STATUS] = uninitialized;
    advance_time_stamp(temp, ld->gen);
  }

  hpx_gas_unpin(local);
  return HPX_SUCCESS;
}
#endif

// quickSort {{{
int cmpvertices(const void *vpa, const void *vpb)
{
  const vizpoint *va = vpa;
  const vizpoint *vb = vpb;

  int diff = (va->z > vb->z) - (va->z < vb->z);
  if(0 == diff) /* primary keys the same? */
  {
    diff = (va->y > vb->y) - (va->y < vb->y);
  }
  if(0 == diff) /* secondary keys the same? */
  {
    diff = (va->x > vb->x) - (va->x < vb->x);
  }
  return diff;
}
// }}}

#define _skip_0 ((int64_t)1 << JJ)
const int64_t _num_x = ns_x * _skip_0;
const int64_t _num_y = ns_y * _skip_0;
const int64_t _num_z = ns_z * _skip_0;

void validate_parameters(void) {
  const int max_direction = 3;
  for (dir_t dir = x_dir; dir < n_dim; dir++)
    assert(ns[dir] != 0);

  for (dir_t dir = n_dim + 1; dir < max_direction; dir++)
    assert(ns[dir] == 0);

  // The index in each direction takes (JJ + log_2(ns)) bits to represent and
  // this should not exceed 21. Otherwise, the current hashing function does not
  // work.
  const int max_bits_per_direction = 21;
  const char direction[3] = {'x', 'y', 'z'};
  for (dir_t dir = x_dir; dir < n_dim; dir++) {
    if (ceil(log2(ns[dir])) + JJ > max_bits_per_direction) {
      fprintf(stderr,
              "index in %c direction requires more than 21 bits\n",
              direction[dir]);
      abort();
    }
  }

// RK4
  assert(n_gen == 4);
}

index_t linear_to_index(const int rank) {
  index_t retval;
#if n_dim == 1
  retval.idx[x_dir] = rank * level_one_step_size;
#elif n_dim == 2
  div_t temp = div(rank, 2 * ns_y + 1);
  retval.idx[x_dir] = temp.quot * level_one_step_size;
  retval.idx[y_dir] = temp.rem * level_one_step_size;
#elif n_dim == 3
  div_t temp = div(rank, (2 * ns_y + 1) * (2 * ns_z + 1));
  retval.idx[x_dir] = temp.quot * level_one_step_size;
  temp = div(temp.rem, 2 * ns_z + 1);
  retval.idx[y_dir] = temp.quot * level_one_step_size;
  retval.idx[z_dir] = temp.rem * level_one_step_size;
#endif
  return retval;
}

int index_to_linear(const index_t *index) {
  int rank;
#if n_dim == 1
  rank = index->idx[x_dir] / level_one_step_size;
#elif n_dim == 2
  int ix = index->idx[x_dir] / level_one_step_size;
  int iy = index->idx[y_dir] / level_one_step_size;
  rank = ix * (2 * ns_y + 1) + iy;
#elif n_dim == 3
  int ix = index->idx[x_dir] / level_one_step_size;
  int iy = index->idx[y_dir] / level_one_step_size;
  int iz = index->idx[z_dir] / level_one_step_size;
  rank = ix * (2 * ns_y + 1) * (2 * ns_z + 1) +
    iy * (2 * ns_z + 1) + iz;
#endif
  return rank;
}

index_t add_index(const index_t *lhs, const index_t *rhs, const int scalar) {
  index_t retval;
  for (dir_t dir = x_dir; dir < n_dim; dir++)
    retval.idx[dir] = lhs->idx[dir] + rhs->idx[dir] * scalar;
  return retval;
}

bool check_index(const index_t *index) {
  bool retval = true;
  for (dir_t dir = x_dir; dir < n_dim; dir++)
    retval &= (index->idx[dir] >= 0) && (index->idx[dir] <= max_index[dir]);
  return retval;
}

coord_t set_coordinate(const index_t *index) {
  coord_t coord;

  coord.pos[x_dir] = L_dim[x_dir] * index->idx[x_dir] / max_index[x_dir]
                   + pars.xmin;

#if n_dim == 2 || n_dim == 3
  coord.pos[y_dir] = L_dim[y_dir] * index->idx[y_dir] / max_index[y_dir]
                   + pars.ymin;
#endif
#if n_dim == 3
  coord.pos[z_dir] = L_dim[z_dir] * index->idx[z_dir] / max_index[z_dir]
                   + pars.zmin;
#endif

  return coord;
}

double norm2(const coord_t *point1, const coord_t *point2) {
  double dist = 0.0;
  for (dir_t dir = x_dir; dir < n_dim; dir++)
    dist += pow(point1->pos[dir] - point2->pos[dir], 2);
  return sqrt(dist);
}

coll_status_t set_point_status(const double *wavelet) {
  coll_status_t retval = nonessential;
  for (int ivar = 0; ivar < n_variab; ivar++) {
    if (fabs(wavelet[ivar]) >= eps_scale[ivar]) {
      retval = essential;
      break;
    }
  }
  return retval;
}

coll_status_t superior_status(const coll_status_t lhs,
                              const coll_status_t rhs) {
  return (lhs >= rhs ? lhs : rhs);
}

coll_status_t inferior_status(const coll_status_t lhs,
                              const coll_status_t rhs) {
  return (lhs <= rhs ? lhs : rhs);
}

bool has_superior_status(const coll_status_t lhs,
                         const coll_status_t rhs) {
  return (lhs > rhs ? true : false);
}

bool has_inferior_status(const coll_status_t lhs,
                         const coll_status_t rhs) {
  return (lhs < rhs ? true : false);
}

bool has_same_status(const coll_status_t lhs,
                     const coll_status_t rhs) {
  return (lhs == rhs ? true : false);
}

int get_level(const index_t *index) {
  int level = 0;
  for (dir_t dir = x_dir; dir < n_dim; dir++) {
    for (int i = JJ; i >= 0; i--) {
      int step = 1 << i;
      if (index->idx[dir] % step == 0) {
        level = fmax(level, JJ - i);
        break;
      }
    }
  }
  return level;
}

void check_wavelet_stencil(coll_point_t *point, const int gen) {
  // Step size of the point's refinement level
  const int h = 1 << (JJ - point->level);
  const int h2 = 2 * h;
  const int upper_limit = wavelet_offset[3];
  const int lower_limit = wavelet_offset[0];
  const int delta_limit = upper_limit - lower_limit;

  //how many directions should we interpolate?
  int interp_count = 0;
  int dirs[n_dim];
  for (int idir = 0; idir < n_dim; ++idir) {
    dirs[idir] = -1;
    if (point->index.idx[idir] % h2) {
      dirs[interp_count] = idir;
      ++interp_count;
    }
  }

  //get the starting index for each direction
  int starting[n_dim];
  for (int idir = 0; idir < interp_count; ++idir) {
    starting[idir] = point->index.idx[dirs[idir]] + h * wavelet_offset[0];
    if (starting[idir] < 0) {
      starting[idir] = 0;
    } else if ((starting[idir] + delta_limit * h) > max_index[dirs[idir]]) {
      starting[idir] = max_index[dirs[idir]] - delta_limit * h;
    }
  }

  //loop over stencil to check for existence
  if (interp_count == 1) {
    index_t stcl = point->index;
    for (int loopa = 0; loopa < 4; ++loopa) {
      //get index
      stcl.idx[dirs[0]] = starting[0] + h2 * loopa;
      //check point
      int flag;
#ifdef HPX
      hpx_addr_t temp = hpx_add_coll_point(&stcl, &flag);
      CES ld;
      ld.flag = flag;
      ld.gen = gen;
      ld.index = stcl;
      int e = hpx_call_sync(temp, _check_extension, NULL, 0,  &ld, sizeof(ld));
      assert(e == HPX_SUCCESS);
#else
      coll_point_t *temp = add_coll_point(&stcl, &flag);
      if (!flag) {
        create_nonessential_point(temp, &stcl);
      } else if (temp->time_stamp < time_stamp) {
        temp->status[CURRENT_STATUS] = nonessential;
        temp->status[FUTURE_STATUS] = uninitialized;
        advance_time_stamp(temp, gen);
      }
      assert(temp->level <point->level);
#endif
    }
  }
#if n_dim > 1
  else if (interp_count == 2) {
    index_t stcl = point->index;
    for (int loopa = 0; loopa < 4; ++loopa) {
      stcl.idx[dirs[0]] = starting[0] + h2 * loopa;
      for (int loopb = 0; loopb < 4; ++loopb) {
        stcl.idx[dirs[1]] = starting[1] + h2 * loopb;

        int flag;
#ifdef HPX
        hpx_addr_t temp = hpx_add_coll_point(&stcl, &flag);
        CES ld;
        ld.flag = flag;
        ld.gen = gen;
        ld.index = stcl;
        int e = hpx_call_sync(temp, _check_extension, NULL, 0, &ld, sizeof(ld));
        assert(e == HPX_SUCCESS);
#else
        coll_point_t *temp = add_coll_point(&stcl, &flag);
        if (!flag) {
          create_nonessential_point(temp, &stcl);
        } else if (temp->time_stamp < time_stamp) {
          temp->status[CURRENT_STATUS] = nonessential;
          temp->status[FUTURE_STATUS] = uninitialized;
          advance_time_stamp(temp, gen);
        }
#endif
      }
    }
  }
#endif
#if n_dim > 2
  else if (interp_count == 3) {
    index_t stcl = point->index;
    for (int loopa = 0; loopa < 4; ++loopa) {
      stcl.idx[dirs[0]] = starting[0] + h2 * loopa;
      for (int loopb = 0; loopb < 4; ++loopb) {
        stcl.idx[dirs[1]] = starting[1] + h2 * loopb;
        for (int loopc = 0; loopc < 4; ++loopc) {
          stcl.idx[dirs[2]] = starting[2] + h2 * loopc;

          int flag;
#ifdef HPX
          hpx_addr_t temp = hpx_add_coll_point(&stcl, &flag);
          CES ld;
          ld.flag = flag;
          ld.gen = gen;
          ld.index = stcl;
          int e = hpx_call_sync(temp, _check_extension, NULL, 0, &ld, sizeof(ld));
          assert(e == HPX_SUCCESS);
#else
          coll_point_t *temp = add_coll_point(&stcl, &flag);
          if (!flag) {
            create_nonessential_point(temp, &stcl);
          } else if (temp->time_stamp < time_stamp) {
            temp->status[CURRENT_STATUS] = nonessential;
            temp->status[FUTURE_STATUS] = uninitialized;
            advance_time_stamp(temp, gen);
          }
#endif
        }
      }
    }
  }
#endif
  else {
    assert(0 && "Problem with checking wavelet stencils");
  }
}

void check_derivative_stencil(coll_point_t *point, const int gen) {
  const int h = 1 << (JJ - point->level);
  const int upper_limit = deriv_offset[3];
  const int lower_limit = deriv_offset[0];

  // Check extension stencil
  if (point->level > 1) {
    for (dir_t dir = x_dir; dir < n_dim; dir++) {
      if (point->index.idx[dir] + lower_limit * h < 0) {
        check_extension_stencil(point, gen, dir, 'L');
      } else if (point->index.idx[dir] + upper_limit * h > max_index[dir]) {
        check_extension_stencil(point, gen, dir, 'U');
      }
    }
  }

  for (dir_t dir = x_dir; dir < n_dim; dir++) {
    index_t index = point->index;
    for (int i = 0; i < 4; i++) {
      index.idx[dir] = point->index.idx[dir] + deriv_offset[i] * h;
      if (check_index(&index)) {
        int flag;
#ifdef HPX
        hpx_addr_t temp = hpx_add_coll_point(&index, &flag);
        CES ld;
        ld.flag = flag;
        ld.gen = gen;
        ld.index = index;
        int e = hpx_call_sync(temp, _check_extension, NULL, 0, &ld, sizeof(ld));
        assert(e == HPX_SUCCESS);
#else
        coll_point_t *temp = add_coll_point(&index, &flag);
        if (!flag) {
          create_nonessential_point(temp, &index);
        } else if (temp->time_stamp < time_stamp) {
          temp->status[CURRENT_STATUS] = nonessential;
          temp->status[FUTURE_STATUS] = uninitialized;
          advance_time_stamp(temp, gen);
        }
#endif
      }
    }
  }
}

void check_extension_stencil(coll_point_t *point, const int gen,
                             const int dir, const char type) {

  // This function does not need to be called on level 0 and level 1 points
  assert(point->level > 1);
  int flag[4];
#ifdef HPX
  hpx_addr_t temp[4];
#else
  coll_point_t *temp[4];
#endif
  index_t index[4];
  const int exten_stencil_step_size = 1 << (JJ - point->level + 1);

  switch (type) {
  case 'L':
    for (int i = 0; i < 4; i++) {
      index[i] = point->index;
      index[i].idx[dir] = i * exten_stencil_step_size;
#ifdef HPX
      temp[i] = hpx_add_coll_point(&index[i], &flag[i]);
#else
      temp[i] = add_coll_point(&index[i], &flag[i]);
#endif
    }
    break;
  case 'U':
    for (int i = 0; i < 4; i++) {
      index[i] = point->index;
      index[i].idx[dir] = (ns[dir] + i - 3) * exten_stencil_step_size;
#ifdef HPX
      temp[i] = hpx_add_coll_point(&index[i], &flag[i]);
#else
      temp[i] = add_coll_point(&index[i], &flag[i]);
#endif
    }
    break;
  }

  for (int i = 0; i < 4; i++) {
#ifdef HPX
    CES ld;
    ld.flag = flag[i];
    ld.gen = gen;
    ld.index = index[i];
    int e = hpx_call_sync(temp[i], _check_extension, NULL, 0, &ld, sizeof(ld));
    assert(e == HPX_SUCCESS);
#else
    if (!flag[i]) {
      create_nonessential_point(temp[i], &index[i]);
    } else if (temp[i]->time_stamp < time_stamp) {
      temp[i]->status[CURRENT_STATUS] = nonessential;
      temp[i]->status[FUTURE_STATUS] = uninitialized;
      advance_time_stamp(temp[i], gen);
    }
#endif
  }
}

void advance_time_stamp(coll_point_t *point, const int gen) {
  check_wavelet_stencil(point, gen);
  wavelet_trans(point, primary_set_approx, gen, primary_mask);
  point->time_stamp = time_stamp;
}

double get_global_dt(void) {
  double global_dt = DBL_MAX;
  double valmin[n_variab];
  double valmax[n_variab];
  for (int ivar = 0; ivar < n_variab; ++ivar) {
    valmin[ivar] = DBL_MAX;
    valmax[ivar] = -DBL_MAX;
  }

  for (int i = 0; i < npts_in_array; i++) {
#ifdef HPX
    GGD ld;
    ld.global_dt = global_dt;
    for (int ivar = 0; ivar < n_variab; ++ivar) {
      ld.valmin[ivar] = valmin[ivar];
      ld.valmax[ivar] = valmax[ivar];
    }
    hpx_addr_t point = hpx_addr_add(hpx_coll_points.array,i*sizeof(coll_point_t),sizeof(coll_point_t));
    hpx_call_sync(point,_get_global_dt,&ld,sizeof(ld),&ld,sizeof(ld));

    global_dt = ld.global_dt;
    for (int ivar = 0; ivar < n_variab; ++ivar) {
      valmin[ivar] = ld.valmin[ivar];
      valmax[ivar] = ld.valmax[ivar];
    }
#else
    coll_point_t *point = &coll_points->array[i];
    double local_dt = get_local_dt(point);
    global_dt = fmin(local_dt, global_dt);

    minmax_range_reducer(point->u[0], valmin, valmax);

    if (point->level == 1 && point->status[CURRENT_STATUS] == essential)
      global_dt_helper(point, &global_dt, valmin, valmax);
#endif
  }

#ifdef HPX
  hpx_addr_t complete = hpx_lco_and_new(hpx_get_num_ranks());
  for (int i=0;i<hpx_get_num_ranks();i++) {
    EPSS ld;
    for (int ivar = 0; ivar < n_variab; ++ivar) {
      ld.valmin[ivar] = valmin[ivar];
      ld.valmax[ivar] = valmax[ivar];
    }
    hpx_call(HPX_THERE(i),_set_eps_scale,complete,&ld,sizeof(ld));
  }
  hpx_lco_wait(complete);
  hpx_lco_delete(complete, HPX_NULL);
#else
  //now find the ranges, and set epsilon accordingly
  set_eps_scale(valmin, valmax);
#endif

  return global_dt;
}

void global_dt_helper(coll_point_t *point, double *dt,
                                double *mins, double *maxs) {
#ifdef HPX
  int skip = __atomic_fetch_add(&point->stage[RHS_DT], 1, memory_order_acq_rel);
#else
  int skip = point->stage[RHS_DT]++;
#endif
  if (skip) {
    return;
  }

#ifndef HPX
  const int gen = 0;
#endif
  const int h2 = 1 << (JJ - point->level - 1);
  for (int i = 0; i < n_neighbors; i++) {
    index_t index = add_index(&point->index, &neighbor_offset[i], h2);
    if (check_index(&index)) {
#ifdef HPX
      GGD ld;
      ld.global_dt = *dt;
      for (int ivar = 0; ivar < n_variab; ++ivar) {
        ld.valmin[ivar] = mins[ivar];
        ld.valmax[ivar] = maxs[ivar];
      }
      hpx_addr_t neighbor = hpx_get_coll_point(&index);
      hpx_call_sync(neighbor,_global_dt_helper,&ld,sizeof(ld),&ld,sizeof(ld));
      *dt = ld.global_dt;
      for (int ivar = 0; ivar < n_variab; ++ivar) {
        mins[ivar] = ld.valmin[ivar];
        maxs[ivar] = ld.valmax[ivar];
      }
#else
      coll_point_t *neighbor = get_coll_point(&index);
      double local_dt = get_local_dt(neighbor);
      *dt = fmin(*dt, local_dt);

      minmax_range_reducer(neighbor->u[gen], mins, maxs);

      if (neighbor->status[0] == essential)
        global_dt_helper(neighbor, dt, mins, maxs);
#endif
    }
  }
}

void minmax_range_reducer(double *u, double *mins, double *maxs) {
  for (int ivar = 0; ivar < n_variab; ++ivar) {
    mins[ivar] = fmin(mins[ivar], u[ivar]);
    maxs[ivar] = fmax(maxs[ivar], u[ivar]);
  }
}

void set_eps_scale(double *mins, double *maxs) {
  double range[n_variab];
  for (int i = 0; i < n_variab; ++i) {
    range[i] = maxs[i] - mins[i];

    //we deal with strange ranges by setting them to one
    if (range[i] <= 0.0) {
      range[i] = 1.0;
    }
  }

  for (int i = 0; i < n_variab; ++i) {
    eps_scale[i] = eps[i];
  }
#ifndef NO_SCALED_EPS
  //fprintf(stdout, "ranges: ");
  for (int i = 0; i < n_variab; ++i) {
    eps_scale[i] = eps[i] * range[i];
   // fprintf(stdout, "%lg ", range[i]);
  }
  //fprintf(stdout, "\n");
#endif

}

#ifdef HPX
  // not safe to pass function points with HPX actions
int rhs_helper(coll_point_t *point, const int gen,function_pointer_t func_type) {
#else
int rhs_helper(coll_point_t *point, const int gen,
                void(*func)(coll_point_t *, const int)) {
#endif
  const int h2 = 1 << (JJ - point->level - 1);
  for (int i = 0; i < n_neighbors; i++) {
    index_t index = add_index(&point->index, &neighbor_offset[i], h2);
    if (check_index(&index)) {
#ifdef HPX
      hpx_addr_t neighbor = hpx_get_coll_point(&index);

      RHSH ld;
      ld.gen = gen;
      ld.func_type = func_type;
      int e = hpx_call_sync(neighbor, _rhs_helper_a, NULL, 0,  &ld, sizeof(ld));
      assert(e == HPX_SUCCESS);
#else
      coll_point_t *neighbor = get_coll_point(&index);
      func(neighbor, gen);
      if (neighbor->status[CURRENT_STATUS] == essential) {
        rhs_helper(neighbor, gen, func);
      }
#endif
    }
  }
  return 0;
}

#ifdef HPX
int integrator_helper(coll_point_t *point, const double dt,int gen){
#else
int integrator_helper(coll_point_t *point, const double dt,
                       void(*func)(coll_point_t *, const double)) {
#endif
  const int h2 = 1 << (JJ - point->level - 1);
  for (int i = 0; i < n_neighbors; i++) {
    index_t index = add_index(&point->index, &neighbor_offset[i], h2);
    if (check_index(&index)) {
#ifdef HPX
      hpx_addr_t neighbor = hpx_get_coll_point(&index);
      ATI ld;
      ld.dt = dt;
      ld.gen = gen;
      int e = hpx_call_sync(neighbor, _integrator_helper_a, NULL, 0, &ld, sizeof(ld));
      assert(e == HPX_SUCCESS);
#else
      coll_point_t *neighbor = get_coll_point(&index);
      // Applying integrator on an active point requires computing the
      // right-hand side of the problem. The time stamp of the active point is
      // incremented by 1 each time it is involved in the right-hand side
      // computation. We need to make sure that no redundant computation is
      // performed.
      int descend = 0;
      if (neighbor->time_stamp < time_stamp) {
        func(neighbor, dt);
        descend = 1;
      }
      if (descend && neighbor->status[CURRENT_STATUS] == essential)
        integrator_helper(neighbor, dt, func);
#endif
    }
  }
  return 0;
}

void wavelet_adjust_grids_helper(coll_point_t *point) {
  int skipadjust = point->adjust[ADJUST_WAVELET]++;
  if (skipadjust) {
    return;
  }

#ifndef HPX
  int wavelet_mask[n_variab + n_aux] = {0};
  for (int ivar = 0; ivar < n_variab; ivar++)
    wavelet_mask[ivar] = 1;
  const int gen = 0;
#endif
  // Step size of the point's neighbor's refinement level
  const int h2 = 1 << (JJ - point->level - 1);

  for (int i = 0; i < n_neighbors; i++) {
    index_t index = add_index(&point->index, &neighbor_offset[i], h2);
    if (check_index(&index)) { // Neighbor point's index is within range
#ifdef HPX
      //hpx_lco_sema_p(_table_locks[morton_key(&index) % LOCK_TBL_SIZE]);
      hpx_addr_t neighbor = hpx_get_coll_point(&index);
      int e = hpx_call_sync(neighbor, _wavelet_adjust_grids_helper, NULL, 0, NULL,0);
      assert(e == HPX_SUCCESS);
#else
      coll_point_t *neighbor = get_coll_point(&index);
      check_wavelet_stencil(neighbor, gen);
      wavelet_trans(neighbor, primary_set_wavelet, gen, wavelet_mask);
      coll_status_t self = set_point_status(neighbor->wavelet[gen]);

      if (has_superior_status(self, neighbor->status[FUTURE_STATUS])) {
        // set the future status to be self. Notice that at an essential point,
        // self can be nonessential. we set it to be nonessential here
        // regardless and will make necessary changes in status validatation
        // phase.
        neighbor->status[FUTURE_STATUS] = self;
        if (neighbor->status[CURRENT_STATUS] == neighboring &&
            self == essential) {
          create_neighboring_point(neighbor);
        }
      }

      // Continue visiting other active points if the point's old status is
      // essential
      if (neighbor->status[CURRENT_STATUS] == essential) {
        wavelet_adjust_grids_helper(neighbor);
      }
#endif
    }
  }
}

void adjust_add_neighbors_helper(coll_point_t *point) {
  int skipadjust = point->adjust[ADJUST_NGBADD]++;
  if (skipadjust) {
    return;
  }

  const int h2 = 1 << (JJ - point->level - 1);
  for (int i = 0; i < n_neighbors; ++i) {
    index_t index = add_index(&point->index, &neighbor_offset[i], h2);
    if (!check_index(&index)) {
      continue;
    }
#ifdef HPX
    hpx_addr_t neighbor = hpx_get_coll_point(&index);
    int e = hpx_call_sync(neighbor, _adjust_add_neighbors_helper, NULL, 0, NULL, 0);
    assert(e == HPX_SUCCESS);
#else
    //We know this point exists, as this routine is only called on currently
    // essential points.
    coll_point_t *neighbor = get_coll_point(&index);

    if (neighbor->status[CURRENT_STATUS] == essential) {
      //descend if this is a currently essential point
      adjust_add_neighbors_helper(neighbor);
    } else if (neighbor->status[FUTURE_STATUS] == essential) {
      //or if it is to become essential, we add neighbors
      create_neighboring_point(neighbor);
    }
#endif
  }
}

int wavelet_integrator_helper(coll_point_t *point, const int gen,
                               const wavelet_trans_t type,
                               const int *wavelet_mask) {
  const int h2 = 1 << (JJ - point->level - 1);

  for (int i = 0; i < n_neighbors; i++) {
    index_t index = add_index(&point->index, &neighbor_offset[i], h2);
    if (check_index(&index)) { // Neighbor point's index is within range
#ifdef HPX
      hpx_addr_t neighbor = hpx_get_coll_point(&index);
      WIH ld;
      ld.type = type;
      ld.gen = gen;
      memcpy(ld.wavelet_mask,wavelet_mask,sizeof(int)*(n_variab + n_aux));
      int e = hpx_call_sync(neighbor, _wavelet_int_helper, NULL, 0, &ld, sizeof(ld));
      assert(e == HPX_SUCCESS);
#else
      coll_point_t *neighbor = get_coll_point(&index);
      int stagecheck = neighbor->stage[RHS_WAVELET]++;

      if (!stagecheck) {
        wavelet_trans(neighbor, type, gen, wavelet_mask);
        if (neighbor->status[CURRENT_STATUS] == essential) {
          wavelet_integrator_helper(neighbor, gen, type, wavelet_mask);
        }
      }
#endif
    }
  }
  return 0;
}

void status_validate_up_helper(coll_point_t *point) {
  int skipadjust = point->adjust[ADJUST_UP];
  if (skipadjust) {
    return;
  }
  const int h2 = 1 << (JJ - point->level - 1);

  if (point->status[CURRENT_STATUS] == essential) {
    for (int i = 0; i < n_neighbors; i++) {
      index_t index = add_index(&point->index, &neighbor_offset[i], h2);
      if (check_index(&index)) {
#ifdef HPX
        hpx_addr_t neighbor = hpx_get_coll_point(&index);
        int e = hpx_call_sync(neighbor, _status_validate_up_helper, NULL,0,NULL,0);
        assert(e == HPX_SUCCESS);
#else
        coll_point_t *neighbor = get_coll_point(&index);
        status_validate_up_helper(neighbor);
#endif
      }
    }

    for (int i = 0; i < n_neighbors; i++) {
      index_t index = add_index(&point->index, &neighbor_offset[i], h2);
      if (check_index(&index)) {
#ifdef HPX
        hpx_addr_t neighbor = hpx_get_coll_point(&index);
        hpx_addr_t u = hpx_addr_add(neighbor,offsetof(coll_point_t,status),sizeof(coll_point_t));
        coll_status_t status[2];
        hpx_gas_memget_sync(&status,u,sizeof(status));
        point->status[FUTURE_STATUS] =
          superior_status(point->status[FUTURE_STATUS],
                          status[FUTURE_STATUS]);
#else
        coll_point_t *neighbor = get_coll_point(&index);
        point->status[FUTURE_STATUS] =
          superior_status(point->status[FUTURE_STATUS],
                          neighbor->status[FUTURE_STATUS]);
#endif
      }
    }
  }
  ++point->adjust[ADJUST_UP];
}

void status_validate_down_helper(coll_point_t *point) {
#ifdef HPX
  int skipadjust = __atomic_fetch_add(&point->adjust[ADJUST_DOWN], 1, memory_order_acq_rel);
#else
  int skipadjust = point->adjust[ADJUST_DOWN]++;
#endif
  if (skipadjust) {
    return;
  }
  const int h2 = 1 << (JJ - point->level - 1);

  for (int i = 0; i < n_neighbors; i++) {
    index_t index = add_index(&point->index, &neighbor_offset[i], h2);
    if (check_index(&index)) {
#ifdef HPX
      hpx_addr_t neighbor = hpx_get_coll_point(&index);
      int e = hpx_call_sync(neighbor, _status_validate_down_helper, NULL,0,NULL,0);
      assert(e == HPX_SUCCESS);
#else
      coll_point_t *neighbor = get_coll_point(&index);
      neighbor->status[FUTURE_STATUS] =
        superior_status(neighbor->status[FUTURE_STATUS], neighboring);
      if (neighbor->status[FUTURE_STATUS] == essential) {
        status_validate_down_helper(neighbor);
      }
#endif
    }
  }
}

//We need this function, so that we can start the loop.
void complete_hlle_stencils(void) {
#ifdef HPX
  hpx_addr_t complete = hpx_lco_and_new(npts_in_array);
  for (int i = 0; i < npts_in_array; ++i) {
    hpx_addr_t point = hpx_addr_add(hpx_coll_points.array,i*sizeof(coll_point_t),sizeof(coll_point_t));
    hpx_call(point,_complete_hlle_stencils_a,complete,NULL,0);
  }
  hpx_lco_wait(complete);
  hpx_lco_delete(complete, HPX_NULL);

  // FUTURE parfor here
  complete = hpx_lco_and_new(npts_in_array);
  for (int i = 0; i < npts_in_array; ++i) {
    hpx_addr_t point = hpx_addr_add(hpx_coll_points.array,i*sizeof(coll_point_t),sizeof(coll_point_t));
    hpx_call(point,_complete_hlle_stencils_b,complete,NULL,0);
  }
  hpx_lco_wait(complete);
  hpx_lco_delete(complete, HPX_NULL);

  // FUTURE parfor here
  complete = hpx_lco_and_new(hpx_get_num_ranks());
  for (int i=0;i<hpx_get_num_ranks();i++) {
    hpx_call(HPX_THERE(i),_complete_hlle_stencils_c,complete,NULL,0);
  }
  hpx_lco_wait(complete);
  hpx_lco_delete(complete, HPX_NULL);
#else
  for (int i = 0; i < npts_in_array; ++i) {
    coll_point_t *point = &coll_points->array[i];
    hlle_stencil_completion_helper(point);
  }
  for (int i = 0; i < npts_in_array; ++i) {
    for (int j = 0; j < ADJUST_STAGES; ++j) {
      coll_points->array[i].adjust[j] = 0;
    }
  }
  for (int i = 0; i < HASH_TBL_SIZE; ++i) {
    hash_entry_t *curr = coll_points->hash_table[i];
    while (curr != NULL) {
      for (int j = 0; j < ADJUST_STAGES; ++j) {
        curr->point->adjust[j] = 0;
      }
      curr = curr->next;
    }
  }
#endif
}

void hlle_stencil_completion_helper(coll_point_t *point) {
  int skipadjust = point->adjust[ADJUST_HLLE]++;
  if (skipadjust) {
    return;
  }

  const int gen = 0;
#ifdef HPX
  //First check that this active point's HLLE stencil is complete
  for (int dir = x_dir; dir < n_dim; ++dir) {
    int closest_level = hpx_get_closest_level(point, dir);
    assert(closest_level >= 1);
    int h = 1 << (JJ - closest_level);

    for (int stcl = 0; stcl < 5; ++stcl) {
      if (stcl == 2) {
        continue;
      }
      else {
        index_t index = point->index;
        index.idx[dir] += (stcl - 2) * h;

        if (check_index(&index)) {
          int flag;
          hpx_addr_t temp = hpx_add_coll_point(&index, &flag);
          CES ld;
          ld.flag = flag;
          ld.gen = gen;
          ld.index = index;
          int e = hpx_call_sync(temp, _check_extension, NULL, 0,  &ld, sizeof(ld));
          assert(e == HPX_SUCCESS);
        }
      }
    }
  }

  //Now go ahead and descend if needed
  if (point->level > 0 && point->status[CURRENT_STATUS] == essential) {
    const int h2 = 1 << (JJ - point->level - 1);
    for (int i = 0; i < n_neighbors; ++i) {
      index_t index = add_index(&point->index, &neighbor_offset[i], h2);
      if (check_index(&index)) {
        hpx_addr_t ngb = hpx_get_coll_point(&index);
        int e = hpx_call_sync(ngb, _complete_hlle_stencils_a, NULL, 0,  NULL, 0);
        assert(e == HPX_SUCCESS);
      }
    }
  }
#else
  //First check that this active point's HLLE stencil is complete
  for (int dir = x_dir; dir < n_dim; ++dir) {
    int closest_level = get_closest_level(point, dir);
    assert(closest_level >= 1);
    int h = 1 << (JJ - closest_level);

    for (int stcl = 0; stcl < 5; ++stcl) {
      if (stcl == 2) {
        continue;
      }
      else {
        index_t index = point->index;
        index.idx[dir] += (stcl - 2) * h;

        if (check_index(&index)) {
          int flag;
          coll_point_t *stcl_point = add_coll_point(&index, &flag);
          if (!flag) {
            create_nonessential_point(stcl_point, &index);
          } else if (stcl_point->time_stamp < time_stamp) {
            stcl_point->status[CURRENT_STATUS] = nonessential;
            stcl_point->status[FUTURE_STATUS] = uninitialized;
            advance_time_stamp(stcl_point, gen);
          }
        }
      }
    }
  }

  //Now go ahead and descend if needed
  if (point->level > 0 && point->status[CURRENT_STATUS] == essential) {
    const int h2 = 1 << (JJ - point->level - 1);
    for (int i = 0; i < n_neighbors; ++i) {
      index_t index = add_index(&point->index, &neighbor_offset[i], h2);
      if (check_index(&index)) {
        coll_point_t *ngb = get_coll_point(&index);
        hlle_stencil_completion_helper(ngb);
      }
    }
  }
#endif
}



// index_on_boundary
int index_on_boundary(const index_t index, int dir) {
  assert(dir <= n_dim);

  if(index.idx[dir] == 0) {
    return -1;
  } else if(index.idx[dir] == max_index[dir]) {
    return 1;
  } else {
    return 0;
  }
}

/// Gets the boundary status of an index
void boundary_check(const index_t index, int *bdy_check) {
  for(int dir = 0; dir < n_dim; ++dir) {
    bdy_check[dir] = index_on_boundary(index, dir);
  }
}

//
// get the skip at the supplied level
//
int64_t skip_at_level(int level) {
  assert( level <= JJ );
  return ( (int64_t)1 << (JJ - level) );
}

//
// shift the index by so many units in the given direction at level
//
index_t index_shift(index_t idx, int dir, int delta, int level, int *flag) {
  assert(dir <= n_dim);
  assert(level <= JJ);

  int64_t shift = skip_at_level(level);

  index_t retval;
  for(int i = 0; i < n_dim; ++i) {
    retval.idx[i] = idx.idx[i];
  }
  retval.idx[dir] += delta * shift;

  int64_t bounds = max_index[dir];

  //check bounds
  if ( retval.idx[dir] <= bounds && retval.idx[dir] >= 0 ) {
    *flag = 0;
  } else {
    *flag = 1;
  }

  return retval;
}

#ifdef HPX
int hpx_get_closest_level(const coll_point_t *point, const int dir) {
  index_t search_index;
  int got_it = 0;
  index_t index = point->index;
  int level = point->level;

  int type = 0;

  int bdy_check[n_dim];
  boundary_check(index, bdy_check);

  int flag;

  //The closest active point will be at max_level or lower
  int ll;
  coll_point_t *search_point;
  search_point = (coll_point_t *) calloc(1,sizeof(coll_point_t));
  for(ll = max_level; ll >= level; --ll) {
    if(bdy_check[dir] <= 0) {
      //search (+)dir at level ll for a point
      search_index = index_shift(index, dir, 1, ll, &flag);
      hpx_addr_t sp = hpx_get_coll_point(&search_index);
      if(sp != HPX_NULL) {
        hpx_gas_memget_sync(search_point, sp, sizeof(coll_point_t));
        if(search_point != NULL) {
          if(search_point->status[type] == essential ||
                search_point->status[type] == neighboring) {
            got_it = 1;
            break;
          }
        }
      }
    }

    if(bdy_check[dir] >= 0) {
      //search (-)dir at level ll for a point
      search_index = index_shift(index, dir, -1, ll, &flag);
      hpx_addr_t sp = hpx_get_coll_point(&search_index);
      if(sp != HPX_NULL) {
        hpx_gas_memget_sync(search_point, sp, sizeof(coll_point_t));
        if(search_point != NULL) {
          if(search_point->status[type] == essential ||
                search_point->status[type] == neighboring) {
            got_it = 1;
            break;
          }
        }
      }
    }
  }

  free(search_point);
  assert(got_it);

#ifndef SKIPTWOAWAYCHECK
  search_point = (coll_point_t *) calloc(1,sizeof(coll_point_t));

  //check if the point two away is essential
  int increment = 0;
  if (point->status[type] == neighboring) {
    search_index = index_shift(index, dir, 2, ll, &flag);
    if (!flag) {
      hpx_addr_t sp = hpx_get_coll_point(&search_index);
      if(sp != HPX_NULL) {
        hpx_gas_memget_sync(search_point, sp, sizeof(coll_point_t));
        if (search_point != NULL && search_point->status[type] == essential) {
          //The point two units away is essential, which means there is another
          // active point between the outer two stencil points for this point in
          // this direction. So we actually want to increase the level so that
          // the outermost stencil point is at the point we found; we increase
          // the level by 1.
          increment = 1;
        }
      }
    }
    search_index = index_shift(index, dir, -2, ll, &flag);
    if (!flag) {
      hpx_addr_t sp = hpx_get_coll_point(&search_index);
      if(sp != HPX_NULL) {
        hpx_gas_memget_sync(search_point, sp, sizeof(coll_point_t));
        if (search_point != NULL && search_point->status[type] == essential) {
          increment = 1;
        }
      }
    }
  }

  ll += increment;
  if (ll > JJ) {
    ll = JJ;
  }

  free(search_point);
#endif

  //search index is the one we want
  return ll;
}
#endif

int get_closest_level(const coll_point_t *point, const int dir) {
  index_t search_index;
  int got_it = 0;
  index_t index = point->index;
  int level = point->level;

  int type = 0;

  int bdy_check[n_dim];
  boundary_check(index, bdy_check);

  int flag;

  //The closest active point will be at max_level or lower
  int ll;
  for(ll = max_level; ll >= level; --ll) {
    if(bdy_check[dir] <= 0) {
      //search (+)dir at level ll for a point
      search_index = index_shift(index, dir, 1, ll, &flag);
      coll_point_t *search_point = get_coll_point(&search_index);
      if(search_point != NULL) {
        if(search_point->status[type] == essential ||
                search_point->status[type] == neighboring) {
          got_it = 1;
          break;
        }
      }
    }

    if(bdy_check[dir] >= 0) {
      //search (-)dir at level ll for a point
      search_index = index_shift(index, dir, -1, ll, &flag);
      coll_point_t *search_point = get_coll_point(&search_index);
      if(search_point != NULL) {
        if(search_point->status[type] == essential ||
                search_point->status[type] == neighboring) {
          got_it = 1;
          break;
        }
      }
    }
  }

  assert(got_it);

#ifndef SKIPTWOAWAYCHECK
  //check if the point two away is essential
  int increment = 0;
  if (point->status[type] == neighboring) {
    search_index = index_shift(index, dir, 2, ll, &flag);
    if (!flag) {
      coll_point_t *search_point = get_coll_point(&search_index);
      if (search_point != NULL && search_point->status[type] == essential) {
        //The point two units away is essential, which means there is another
        // active point between the outer two stencil points for this point in
        // this direction. So we actually want to increase the level so that
        // the outermost stencil point is at the point we found; we increase
        // the level by 1.
        increment = 1;
      }
    }
    search_index = index_shift(index, dir, -2, ll, &flag);
    if (!flag) {
      coll_point_t *search_point = get_coll_point(&search_index);
      if (search_point != NULL && search_point->status[type] == essential) {
        increment = 1;
      }
    }
  }

  ll += increment;
  if (ll > JJ) {
    ll = JJ;
  }
#endif


  //search index is the one we want
  return ll;
}

#ifdef HPX
// distributed 2d output {{{
int _2doutput_action(OUT *ld,size_t size)
{
  unsigned int stamp = ld->stamp;
  int filenum = ld->filenum;
  int numpoints = npts_in_array;
  coll_point_t *point;
  point = (coll_point_t *) calloc(1,sizeof(coll_point_t));
  for (int i = 0; i < HASH_TBL_SIZE; i++) {
    hpx_hash_entry_t *ptr = hpx_coll_points.hash_table[i];
    while (ptr != NULL) {
      hpx_gas_memget_sync(point, ptr->point, sizeof(coll_point_t));
      if (point->time_stamp == stamp &&
               (point->status[CURRENT_STATUS] == essential ||
                point->status[CURRENT_STATUS] == neighboring) ) {
        ++numpoints;
      }
      ptr = ptr->next;
    }
  }

  int *outfields;
  outfields = (int *) malloc(pars.num_req_fields*sizeof(int));
  int count = 0;
  for (int n=0;n<n_variab+n_aux;n++) {
    if ( pars.output_fields[n] == normal_output ) {
      outfields[count] = n;
      count++;
    }
  }
  assert(count == pars.num_req_fields);

  int full_indx = 0;
  float *xarray,*yarray;
  xarray = (float *) malloc(numpoints*sizeof(float));
  yarray = (float *) malloc(numpoints*sizeof(float));
  float **sdf_func;
  sdf_func = (float **) malloc(pars.num_req_fields*sizeof(float *));
  for (int l=0;l<pars.num_req_fields;l++) {
    sdf_func[l] = malloc(numpoints*sizeof(float));
  }

  if ( HPX_LOCALITY_ID == 0 ) {
    // the array information needs only be written out by one locality
    for (int i = 0; i < npts_in_array; i++) {
      hpx_addr_t pt = hpx_addr_add(hpx_coll_points.array,i*sizeof(coll_point_t),sizeof(coll_point_t));
      hpx_gas_memget_sync(point, pt, sizeof(coll_point_t));
      xarray[full_indx] = point->coords.pos[x_dir];
      yarray[full_indx] = point->coords.pos[y_dir];
      for (int l=0;l<pars.num_req_fields;l++) {
        sdf_func[l][full_indx] = point->u[0][outfields[l]];
      }
      full_indx++;

      if ( full_indx > numpoints ) {
        printf(" PROBLEM:  ran out of storage for output!\n");
        exit(0);
      }
    }
  }

  for (int i = 0; i < HASH_TBL_SIZE; i++) {
    hpx_hash_entry_t *ptr = hpx_coll_points.hash_table[i];
    while (ptr != NULL) {
      hpx_gas_memget_sync(point, ptr->point, sizeof(coll_point_t));
      if (point->time_stamp == stamp &&
                (point->status[CURRENT_STATUS] == essential ||
                 point->status[CURRENT_STATUS] == neighboring) ) {
        xarray[full_indx] = point->coords.pos[x_dir];
        yarray[full_indx] = point->coords.pos[y_dir];
        for (int l=0;l<pars.num_req_fields;l++) {
          sdf_func[l][full_indx] = point->u[0][outfields[l]];
        }
        full_indx++;
        if ( full_indx > numpoints ) {
          printf(" PROBLEM:  ran out of storage for output!\n");
          exit(0);
        }
      }
      ptr = ptr->next;
    }
  }

  FILE *fp;
  char fname[80];
  char filename[80];
  sprintf(filename,"%dmesh.%05d.hawaii",hpx_get_my_rank(),filenum);
  fp = fopen(filename,"w");
  fwrite(&full_indx,sizeof(int),1,fp);
  fwrite(xarray,sizeof(xarray[0]),full_indx,fp);
  fwrite(yarray,sizeof(yarray[0]),full_indx,fp);
  fclose(fp);

  for (int n=0;n<pars.num_req_fields;n++) {
    if (outfields[n] == 0 ) sprintf(fname,"%s","D");
    if (outfields[n] == 1 ) sprintf(fname,"%s","Sx");
    if (outfields[n] == 2 ) sprintf(fname,"%s","Sy");
    if (outfields[n] == 3 ) sprintf(fname,"%s","Sz");
    if (outfields[n] == 4 ) sprintf(fname,"%s","tau");
    if (outfields[n] == 5 ) sprintf(fname,"%s","rho");
    if (outfields[n] == 6 ) sprintf(fname,"%s","vx");
    if (outfields[n] == 7 ) sprintf(fname,"%s","vy");
    if (outfields[n] == 8 ) sprintf(fname,"%s","vz");
    if (outfields[n] == 9 ) sprintf(fname,"%s","p");
    sprintf(filename,"%dfields.%s.%05d.hawaii",hpx_get_my_rank(),fname,filenum);
    fp = fopen(filename,"w");
    fwrite(sdf_func[n],sizeof(sdf_func[0]),full_indx,fp);
    fclose(fp);
  }

  // cleanup memory
  free(point);
  free(xarray);
  free(yarray);
  for (int l=0;l<pars.num_req_fields;l++) {
    free(sdf_func[l]);
  }
  free(sdf_func);
  free(outfields);
  return HPX_SUCCESS;
} // }}}
#endif

// grid_sdf_output {{{
void grid_sdf_output(unsigned int stamp,double t)
{
#ifdef RNPL
#if n_dim == 1
  // dim = 1 {{{
  // FIXME -- we shouldn't have fixed limits like this in output
  const int max_points_level = 5000;
  int max_levels = JJ+1;

  vizpoint **vpoints;
  int *indx;

  vpoints = (vizpoint **) malloc(max_levels*sizeof(vizpoint *));
  for (int j=0;j<max_levels;j++) {
    vpoints[j] = (vizpoint *) malloc(max_points_level*sizeof(vizpoint));
  }
  indx = (int *) calloc(max_levels,sizeof(int));

  vizpoint *full;
  full = (vizpoint *) malloc(max_points_level*max_levels*sizeof(vizpoint));
  int full_indx = 0;


  for (int i = 0; i < npts_in_array; i++) {
    coll_point_t *point = &coll_points->array[i];
    int j = indx[point->level];
    vpoints[point->level][j].x = point->coords.pos[x_dir];
    vpoints[point->level][j].y = 0.0;
    vpoints[point->level][j].z = 0.0;
    full[full_indx].x = point->coords.pos[x_dir];
    full[full_indx].y = 0.0;
    full[full_indx].z = 0.0;
    for (int k=0;k<n_variab+n_aux;k++) {
      vpoints[point->level][j].value[k] = point->u[0][k];
      full[full_indx].value[k] = point->u[0][k];
    }
    indx[point->level] += 1;
    full_indx++;

    if ( full_indx >= max_points_level*max_levels ||
         indx[point->level] >= max_points_level ) {
      printf(" PROBLEM:  increase max_points_level in 1-d output!\n");
      exit(0);
    }
  }

  for (int i = 0; i < HASH_TBL_SIZE; i++) {
    hash_entry_t *ptr = coll_points->hash_table[i];
    while (ptr != NULL) {
      coll_point_t *point = ptr->point;
      if (point->time_stamp == stamp) {
        //printf("%8.7f %d %14.13e\n", point->coords, point->level, point->u[0]);
        int j = indx[point->level];
        //printf("level=%d, j=%d, JJ=%d\n",point->level,j,JJ);
        vpoints[point->level][j].x = point->coords.pos[x_dir];
        vpoints[point->level][j].y = 0.0;
        vpoints[point->level][j].z = 0.0;
        full[full_indx].x = point->coords.pos[x_dir];
        full[full_indx].y = 0.0;
        full[full_indx].z = 0.0;
        for (int k=0;k<n_variab+n_aux;k++) {
          vpoints[point->level][j].value[k] = point->u[0][k];
          full[full_indx].value[k] = point->u[0][k];
        }
        indx[point->level] += 1;
        full_indx++;
        if ( full_indx >= max_points_level*max_levels ||
          indx[point->level] >= max_points_level ) {
          printf(" PROBLEM:  increase max_points_level in 1-d output!\n");
          exit(0);
        }
      }
      ptr = ptr->next;
    }
  }

  // sort the values for each level
  for (int i=0;i<max_levels;i++) {
    qsort(vpoints[i],indx[i],sizeof(vpoints[i][0]),cmpvertices);
  }
  // sort everything
  qsort(full,full_indx,sizeof(full[0]),cmpvertices);

  // establish bboxes
  const int numbbox = 500;
  double **bbox_minx = (double **) calloc(max_levels,sizeof(double *));
  double **bbox_maxx = (double **) calloc(max_levels,sizeof(double *));
  int *bbox_counter = (int *) calloc(max_levels,sizeof(int));
  for (int i=0;i<max_levels;i++) {
    bbox_minx[i] = (double *) calloc(numbbox,sizeof(double));
    bbox_maxx[i] = (double *) calloc(numbbox,sizeof(double));
  }

  double dx0 = L_dim[x_dir] / max_index[x_dir];

  // the base grid is unigrid -- no issues there
  bbox_minx[0][0] = pars.xmin;
  bbox_maxx[0][0] = pars.xmax;
  bbox_counter[0] = 1;

  // get the rest of the levels
  for (int i=1;i<max_levels;i++) {
    int h = 1 << (JJ - i);
    // "effective" search resolution for this level (ignoring contribution from lower level)
    double search_dx = 2*dx0*h;

    bbox_minx[i][bbox_counter[i]] = vpoints[i][0].x;
    for (int j=1;j<indx[i];j++) {
      if ( floatcmp(vpoints[i][j].x,vpoints[i][j-1].x  + search_dx) ) {
        bbox_maxx[i][bbox_counter[i]] = vpoints[i][j].x;
      } else {
        bbox_counter[i] += 1;
        if ( bbox_counter[i] >= numbbox ) {
          printf(" PROBLEM: increase the numbbox parameter size -- limit reached for output!\n");
          exit(0);
        }
        bbox_minx[i][bbox_counter[i]] = vpoints[i][j].x;
      }
      // end condition
      if (j==indx[i]-1) {
        bbox_counter[i] += 1;
        if ( bbox_counter[i] >= numbbox ) {
          printf(" PROBLEM: increase the numbbox parameter size -- limit reached for output!\n");
          exit(0);
        }
      }
    }
  }
  // DEBUG
  //for (int i = 1; i < max_levels; i++) {
  //  printf(" Number of bbx %d for level %d\n",bbox_counter[i],i);
  //  for (int j=0;j<bbox_counter[i];j++) {
  //    printf(" bbox minx %g maxx %g\n",bbox_minx[i][j],bbox_maxx[i][j]);
  //  }
  //}

 /* output sdf's */
  int rank = 1;
  char *cnames = "x";
  char fname[16];
  double **sdf_func;
  int *outfields;
  double *sdf_coords;
  int shape[3];
  sdf_func = (double **) malloc(pars.num_req_fields*sizeof(double *));
  outfields = (int *) malloc(pars.num_req_fields*sizeof(int));
  int count = 0;
  for (int n=0;n<n_variab+n_aux;n++) {
    if ( pars.output_fields[n] == normal_output ) {
      outfields[count] = n;
      count++;
    }
  }

  assert(count == pars.num_req_fields);

  for (int i = 0; i < max_levels; i++) {
    int h = 1 << (JJ - i);
    double dx = dx0*h;
    for (int j=0;j<bbox_counter[i];j++) {
      int nx = nearbyint((bbox_maxx[i][j] - bbox_minx[i][j])/dx)+1;
      shape[0] = nx;
      for (int n=0;n<pars.num_req_fields;n++) {
        sdf_func[n] = malloc(nx*sizeof(double));
      }
      sdf_coords = malloc(nx*sizeof(double));
      int m;
      for (m=0;m<full_indx;m++) {
        if ( floatcmp(full[m].x,bbox_minx[i][j]) ) break;
      }
      for (int k=0;k<nx;k++) {
        sdf_coords[k] = bbox_minx[i][j] + dx*k;

        // base grid case
        if ( i ==0 ) {
          for (int n=0;n<pars.num_req_fields;n++) {
            sdf_func[n][k] = vpoints[i][k].value[outfields[n]];
          }
        } else {
          while (floatcmp(full[m].x,sdf_coords[k]) != 1) {
            m++;
            if (full[m].x > sdf_coords[k] + dx) {
              printf(" PROBLEM: point not found! full %g coord %g level %d\n",full[m].x,sdf_coords[k],i);
              exit(0);
            }
          }
          for (int n=0;n<pars.num_req_fields;n++) {
            sdf_func[n][k] = full[m].value[outfields[n]];
          }
        }
      }
      for (int n=0;n<pars.num_req_fields;n++) {
        if (outfields[n] == 0 ) sprintf(fname,"%s","D");
        if (outfields[n] == 1 ) sprintf(fname,"%s","Sx");
        if (outfields[n] == 2 ) sprintf(fname,"%s","Sy");
        if (outfields[n] == 3 ) sprintf(fname,"%s","Sz");
        if (outfields[n] == 4 ) sprintf(fname,"%s","tau");
        if (outfields[n] == 5 ) sprintf(fname,"%s","rho");
        if (outfields[n] == 6 ) sprintf(fname,"%s","vx");
        if (outfields[n] == 7 ) sprintf(fname,"%s","vy");
        if (outfields[n] == 8 ) sprintf(fname,"%s","vz");
        if (outfields[n] == 9 ) sprintf(fname,"%s","p");
        gft_out_full(fname, t, shape, cnames, rank,
                   sdf_coords, sdf_func[n]);
        free(sdf_func[n]);
      }
      free(sdf_coords);
    }
    if ( pars.output_by_level == 1 ) {
      if (indx[i] > 0) {
        for (int n=0;n<pars.num_req_fields;n++) {
          sdf_func[n] = malloc(indx[i]*sizeof(double));
        }
        sdf_coords = malloc(indx[i]*sizeof(double));
        for (int k=0;k<indx[i];k++) {
          sdf_coords[k] = vpoints[i][k].x;
        }

        for (int k=0;k<indx[i];k++) {
          for (int n=0;n<pars.num_req_fields;n++) {
            sdf_func[n][k] = vpoints[i][k].value[outfields[n]];
          }
        }

        for (int n=0;n<pars.num_req_fields;n++) {
          if (outfields[n] == 0 ) sprintf(fname,"%s%d","D",i);
          if (outfields[n] == 1 ) sprintf(fname,"%s%d","Sx",i);
          if (outfields[n] == 2 ) sprintf(fname,"%s%d","Sy",i);
          if (outfields[n] == 3 ) sprintf(fname,"%s%d","Sz",i);
          if (outfields[n] == 4 ) sprintf(fname,"%s%d","tau",i);
          if (outfields[n] == 5 ) sprintf(fname,"%s%d","uBx",i);
          if (outfields[n] == 6 ) sprintf(fname,"%s%d","uBy",i);
          if (outfields[n] == 7 ) sprintf(fname,"%s%d","uBz",i);
          if (outfields[n] == 8 ) sprintf(fname,"%s%d","upsi",i);
          if (outfields[n] == 9 ) sprintf(fname,"%s%d","rho",i);
          if (outfields[n] == 10 ) sprintf(fname,"%s%d","vx",i);
          if (outfields[n] == 11 ) sprintf(fname,"%s%d","vy",i);
          if (outfields[n] == 12 ) sprintf(fname,"%s%d","vz",i);
          if (outfields[n] == 13 ) sprintf(fname,"%s%d","p",i);
          if (outfields[n] == 14 ) sprintf(fname,"%s%d","Bx",i);
          if (outfields[n] == 15 ) sprintf(fname,"%s%d","By",i);
          if (outfields[n] == 16 ) sprintf(fname,"%s%d","Bz",i);
          if (outfields[n] == 17 ) sprintf(fname,"%s%d","psi",i);
          gft_out_full(fname, t, (indx+i), cnames, rank,
                   sdf_coords, sdf_func[n]);
          free(sdf_func[n]);
        }
        free(sdf_coords);
      }
    }
  }
  for (int i=0;i<max_levels;i++) {
    free(bbox_minx[i]);
    free(bbox_maxx[i]);
  }
  free(bbox_counter);
  free(bbox_minx);
  free(bbox_maxx);

  free(indx);
  for (int i = 0; i < max_levels; i++) {
    free(vpoints[i]);
  }
  free(full);
  free(vpoints);
  // }}}
#endif
#endif

static int filenum = 0;

#ifdef SILO

#ifdef HPX

#if n_dim == 2
  hpx_addr_t complete = hpx_lco_and_new(hpx_get_num_ranks());
  for (int i=0;i<hpx_get_num_ranks();i++) {
    OUT ld;
    ld.stamp = stamp;
    ld.filenum = filenum;
    hpx_call(HPX_THERE(i),_2doutput,complete,&ld,sizeof(ld));
  }
  hpx_lco_wait(complete);
  hpx_lco_delete(complete, HPX_NULL);
#endif

// Note: distributed dim = 3 not yet implemented

#else // else ifdef HPX

#if n_dim == 2
  // dim = 2 {{{
  int numpoints = npts_in_array;
  for (int i = 0; i < HASH_TBL_SIZE; i++) {
    hash_entry_t *ptr = coll_points->hash_table[i];
    while (ptr != NULL) {
      if (ptr->point->time_stamp == stamp &&
               (ptr->point->status[CURRENT_STATUS] == essential ||
                ptr->point->status[CURRENT_STATUS] == neighboring) ) {
        ++numpoints;
      }
      ptr = ptr->next;
    }
  }

  int *outfields;
  outfields = (int *) malloc(pars.num_req_fields*sizeof(int));
  int count = 0;
  for (int n=0;n<n_variab+n_aux;n++) {
    if ( pars.output_fields[n] == normal_output ) {
      outfields[count] = n;
      count++;
    }
  }
  assert(count == pars.num_req_fields);

  int full_indx = 0;
  float *xarray,*yarray;
  xarray = (float *) malloc(numpoints*sizeof(float));
  yarray = (float *) malloc(numpoints*sizeof(float));
  float **sdf_func;
  sdf_func = (float **) malloc(pars.num_req_fields*sizeof(float *));
  for (int l=0;l<pars.num_req_fields;l++) {
    sdf_func[l] = malloc(numpoints*sizeof(float));
  }

  for (int i = 0; i < npts_in_array; i++) {
    coll_point_t *point = &coll_points->array[i];
    xarray[full_indx] = point->coords.pos[x_dir];
    yarray[full_indx] = point->coords.pos[y_dir];
    for (int l=0;l<pars.num_req_fields;l++) {
      sdf_func[l][full_indx] = point->u[0][outfields[l]];
    }
    full_indx++;

    if ( full_indx > numpoints ) {
      printf(" PROBLEM:  ran out of storage for output!\n");
      exit(0);
    }
  }

  for (int i = 0; i < HASH_TBL_SIZE; i++) {
    hash_entry_t *ptr = coll_points->hash_table[i];
    while (ptr != NULL) {
      coll_point_t *point = ptr->point;
      if (point->time_stamp == stamp &&
                (point->status[CURRENT_STATUS] == essential ||
                 point->status[CURRENT_STATUS] == neighboring) ) {
        xarray[full_indx] = point->coords.pos[x_dir];
        yarray[full_indx] = point->coords.pos[y_dir];
        for (int l=0;l<pars.num_req_fields;l++) {
          sdf_func[l][full_indx] = point->u[0][outfields[l]];
        }
        full_indx++;
        if ( full_indx > numpoints ) {
          printf(" PROBLEM:  ran out of storage for output!\n");
          exit(0);
        }
      }
      ptr = ptr->next;
    }
  }

 /* Note: it may seem that we want full_indx = numpoints - 1, but full_indx
  * is incremented after points are defined, so we assert that they are equal
  */
  //printf("... full_indx=%d, numpoints=%d\n",full_indx,numpoints);

  assert(full_indx == numpoints);

  DBfile         *dbfile = NULL;
  static char     meshname[] = {"pmesh"};
  int             driver=DB_PDB;
  char            filename[256];

  sprintf(filename,"mhd.%05d.pdb",filenum);
  dbfile = DBCreate(filename, 0, DB_LOCAL, "rmhd data", driver);
  assert(dbfile != NULL);

  DBoptlist      *optlist = NULL;
  optlist = DBMakeOptlist(1);
  DBAddOption(optlist, DBOPT_DTIME, &t);
  int cycle = time_stamp/n_gen;
  DBAddOption(optlist, DBOPT_CYCLE, &cycle);
  float          *coords[2], *vars[3];
  int dims = 2;

  coords[0] = xarray;
  coords[1] = yarray;

  DBPutPointmesh(dbfile, meshname, dims, coords, numpoints, DB_FLOAT, optlist);

  char fname[80];

  for (int n=0;n<pars.num_req_fields;n++) {
    if (outfields[n] == 0 ) sprintf(fname,"%s","D");
    if (outfields[n] == 1 ) sprintf(fname,"%s","Sx");
    if (outfields[n] == 2 ) sprintf(fname,"%s","Sy");
    if (outfields[n] == 3 ) sprintf(fname,"%s","Sz");
    if (outfields[n] == 4 ) sprintf(fname,"%s","tau");
    if (outfields[n] == 5 ) sprintf(fname,"%s","rho");
    if (outfields[n] == 6 ) sprintf(fname,"%s","vx");
    if (outfields[n] == 7 ) sprintf(fname,"%s","vy");
    if (outfields[n] == 8 ) sprintf(fname,"%s","vz");
    if (outfields[n] == 9 ) sprintf(fname,"%s","p");
    vars[0] = sdf_func[n];
    DBPutPointvar(dbfile, fname, meshname, 1, vars, numpoints, DB_FLOAT, optlist);
  }
  DBFreeOptlist(optlist);
  DBClose(dbfile);

  free(xarray);
  free(yarray);
  for (int l=0;l<pars.num_req_fields;l++) {
    free(sdf_func[l]);
  }
  free(sdf_func);
  free(outfields);
  // }}}
#endif

#if n_dim == 3
  // dim = 3 {{{

  int numpoints = npts_in_array;
  for (int i = 0; i < HASH_TBL_SIZE; i++) {
    hash_entry_t *ptr = coll_points->hash_table[i];
    while (ptr != NULL) {
      if (ptr->point->time_stamp == stamp &&
               (ptr->point->status[CURRENT_STATUS] == essential ||
                ptr->point->status[CURRENT_STATUS] == neighboring) ) {
        ++numpoints;
      }
      ptr = ptr->next;
    }
  }

  int *outfields;
  outfields = (int *) malloc(pars.num_req_fields*sizeof(int));
  int count = 0;
  for (int n=0;n<n_variab+n_aux;n++) {
    if ( pars.output_fields[n] == normal_output ) {
      outfields[count] = n;
      count++;
    }
  }
  assert(count == pars.num_req_fields);

  int full_indx = 0;
  float *xarray,*yarray,*zarray;
  xarray = (float *) malloc(numpoints*sizeof(float));
  yarray = (float *) malloc(numpoints*sizeof(float));
  zarray = (float *) malloc(numpoints*sizeof(float));
  float **sdf_func;
  sdf_func = (float **) malloc(pars.num_req_fields*sizeof(float *));
  for (int l=0;l<pars.num_req_fields;l++) {
    sdf_func[l] = malloc(numpoints*sizeof(float));
  }

  for (int i = 0; i < npts_in_array; i++) {
    coll_point_t *point = &coll_points->array[i];
    xarray[full_indx] = point->coords.pos[x_dir];
    yarray[full_indx] = point->coords.pos[y_dir];
    zarray[full_indx] = point->coords.pos[z_dir];
    for (int l=0;l<pars.num_req_fields;l++) {
      sdf_func[l][full_indx] = (float) point->u[0][outfields[l]];
    }
    full_indx++;

    if ( full_indx > numpoints ) {
      printf(" PROBLEM:  ran out of storage for output!\n");
      exit(0);
    }
  }

  for (int i = 0; i < HASH_TBL_SIZE; i++) {
    hash_entry_t *ptr = coll_points->hash_table[i];
    while (ptr != NULL) {
      coll_point_t *point = ptr->point;
      if (point->time_stamp == stamp &&
                    (point->status[CURRENT_STATUS] == essential ||
                     point->status[CURRENT_STATUS] == neighboring) ) {
        xarray[full_indx] = point->coords.pos[x_dir];
        yarray[full_indx] = point->coords.pos[y_dir];
        zarray[full_indx] = point->coords.pos[z_dir];
        for (int l=0;l<pars.num_req_fields;l++) {
          sdf_func[l][full_indx] = (float) point->u[0][outfields[l]];
        }
        full_indx++;
        if ( full_indx > numpoints ) {
          printf(" PROBLEM:  ran out of storage for output!\n");
          exit(0);
        }
      }
      ptr = ptr->next;
    }
  }

  DBfile         *dbfile = NULL;
  static char     meshname[] = {"pmesh"};
  int             driver=DB_PDB;
  char            filename[256];

  sprintf(filename,"mhd.%05d.pdb",filenum);
  dbfile = DBCreate(filename, 0, DB_LOCAL, "rmhd data", driver);
  assert(dbfile != NULL);

  DBoptlist      *optlist = NULL;
  optlist = DBMakeOptlist(1);
  DBAddOption(optlist, DBOPT_DTIME, &t);
  int cycle = time_stamp/n_gen;
  DBAddOption(optlist, DBOPT_CYCLE, &cycle);
  float          *coords[3], *vars[3];
  int dims = 3;

  coords[0] = xarray;
  coords[1] = yarray;
  coords[2] = zarray;

 /* Note: it may seem that we want full_indx = numpoints - 1, but full_indx
  * is incremented after points are defined, so we assert that they are equal
  */
  assert(numpoints == full_indx);

  DBPutPointmesh(dbfile, meshname, dims, coords, numpoints, DB_FLOAT, optlist);

  char fname[80];

  for (int n=0;n<pars.num_req_fields;n++) {
    if (outfields[n] == 0 ) sprintf(fname,"%s","D");
    if (outfields[n] == 1 ) sprintf(fname,"%s","Sx");
    if (outfields[n] == 2 ) sprintf(fname,"%s","Sy");
    if (outfields[n] == 3 ) sprintf(fname,"%s","Sz");
    if (outfields[n] == 4 ) sprintf(fname,"%s","tau");
    if (outfields[n] == 5 ) sprintf(fname,"%s","rho");
    if (outfields[n] == 6 ) sprintf(fname,"%s","vx");
    if (outfields[n] == 7 ) sprintf(fname,"%s","vy");
    if (outfields[n] == 8 ) sprintf(fname,"%s","vz");
    if (outfields[n] == 9 ) sprintf(fname,"%s","p");
    vars[0] = sdf_func[n];
    DBPutPointvar(dbfile, fname, meshname, 1, vars, numpoints, DB_FLOAT, optlist);
  }
  DBFreeOptlist(optlist);
  DBClose(dbfile);

  free(xarray);
  free(yarray);
  free(zarray);
  for (int l=0;l<pars.num_req_fields;l++) {
    free(sdf_func[l]);
  }
  free(sdf_func);
  free(outfields);
  // }}}
#endif

#endif // end ifdef/else HPX
#endif // end ifdef SILO

  filenum++;
}
// }}}
