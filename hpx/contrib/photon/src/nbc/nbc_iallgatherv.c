/*
 * Copyright (c) 2006 The Trustees of Indiana University and Indiana
 *                    University Research and Technology
 *                    Corporation.  All rights reserved.
 * Copyright (c) 2006 The Technical University of Chemnitz. All
 *                    rights reserved.
 *
 * Author(s): Torsten Hoefler <htor@cs.indiana.edu>
 *
 */
#include "nbc_internal.h"

/* an allgatherv schedule can not be cached easily because the contents
 * ot the recvcounts array may change, so a comparison of the address
 * would not be sufficient ... we simply do not cache it */

#ifdef HAVE_SYS_WEAK_ALIAS_PRAGMA
#pragma weak NBC_Iallgatherv = PNBC_Iallgatherv
#define NBC_Iallgatherv PNBC_Iallgatherv
#endif

/* simple linear MPI_Iallgatherv
 * the algorithm uses p-1 rounds
 * first round:
 *   each node sends to it's left node (rank+1)%p sendcount elements
 *   each node begins with it's right node (rank-11)%p and receives from it
 * recvcounts[(rank+1)%p] elements
 * second round:
 *   each node sends to node (rank+2)%p sendcount elements
 *   each node receives from node (rank-2)%p recvcounts[(rank+2)%p] elements */
int NBC_Iallgatherv(void *sendbuf, int sendcount, MPI_Datatype sendtype,
                    void *recvbuf, int *recvcounts, int *displs,
                    MPI_Datatype recvtype, NBC_Comminfo *comm,
                    NBC_Handle *handle) {
  int rank, p, res, r, speer, rpeer;
  MPI_Aint rcvext, sndext;
  NBC_Schedule *schedule;
  char *rbuf, inplace;

  NBC_IN_PLACE(sendbuf, recvbuf, inplace);

  res = NBC_Init_handle(handle, comm);
  if (res != NBC_OK) {
    printf("Error in NBC_Init_handle(%i)\n", res);
    return res;
  }
  res = NBC_Comm_rank(comm, &rank);
  if (NBC_SUCCESS != res) {
    printf("MPI Error in NBC_Comm_rank() (%i)\n", res);
    return res;
  }
  res = NBC_Comm_size(comm, &p);
  if (NBC_SUCCESS != res) {
    printf("MPI Error in NBC_Comm_size() (%i)\n", res);
    return res;
  }
  res = MPI_Type_extent(sendtype, &sndext);
  if (MPI_SUCCESS != res) {
    printf("MPI Error in MPI_Type_extent() (%i)\n", res);
    return res;
  }
  res = MPI_Type_extent(recvtype, &rcvext);
  if (MPI_SUCCESS != res) {
    printf("MPI Error in MPI_Type_extent() (%i)\n", res);
    return res;
  }

  schedule = (NBC_Schedule *)malloc(sizeof(NBC_Schedule));
  if (NULL == schedule) {
    printf("Error in malloc() (%i)\n", res);
    return res;
  }

  handle->tmpbuf = NULL;

  res = NBC_Sched_create(schedule);
  if (res != NBC_OK) {
    printf("Error in NBC_Sched_create, (%i)\n", res);
    return res;
  }

  if (!inplace) {
    /* copy my data to receive buffer */
    rbuf = ((char *)recvbuf) + (displs[rank] * rcvext);
    NBC_Copy(sendbuf, sendcount, sendtype, rbuf, recvcounts[rank], recvtype);
    if (NBC_OK != res) {
      printf("Error in NBC_Copy() (%i)\n", res);
      return res;
    }
  }

  /* do p-1 rounds */
  for (r = 1; r < p; r++) {
    speer = (rank + r) % p;
    rpeer = (rank - r + p) % p;
    rbuf = ((char *)recvbuf) + (displs[rpeer] * rcvext);

    res = NBC_Sched_recv(rbuf, false, recvcounts[rpeer], recvtype, rpeer,
                         schedule, comm);
    if (NBC_OK != res) {
      printf("Error in NBC_Sched_recv() (%i)\n", res);
      return res;
    }
    res = NBC_Sched_send(sendbuf, false, sendcount, sendtype, speer, schedule,
                         comm);
    if (NBC_OK != res) {
      printf("Error in NBC_Sched_send() (%i)\n", res);
      return res;
    }
  }

  res = NBC_Sched_commit(schedule);
  if (NBC_OK != res) {
    printf("Error in NBC_Sched_commit() (%i)\n", res);
    return res;
  }

  res = NBC_Start(handle, schedule);
  if (NBC_OK != res) {
    printf("Error in NBC_Start() (%i)\n", res);
    return res;
  }

  return NBC_OK;
}
