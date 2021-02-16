#ifndef PHOTON_COLLECTIVES_H
#define PHOTON_COLLECTIVES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PHOTON_COLL_IFACE_DEFAULT = 0,
  PHOTON_COLL_IFACE_PWC,
  PHOTON_COLL_IFACE_NBC,
  PHOTON_COLL_IFACE_MAX
} photon_cfg_coll_iface_t;

static const char* const PHOTON_COLL_IFACE_TO_STRING[] = {
  "default",
  "pwc",
  "nbc",
  "INVALID_COLLECTIVE_INTERACE"
};

typedef enum {
  PHOTON_COLL_BARRIER = 1,
  PHOTON_COLL_REDUCE,
  PHOTON_COLL_ALLREDUCE,
  PHOTON_COLL_SCAN,
  PHOTON_COLL_IBARRIER,
  PHOTON_COLL_IALLREDUCE,
  PHOTON_COLL_ISCAN,
  PHOTON_COLL_MAXVAL
} photon_coll;

typedef enum {
  PHOTON_COLL_ALGO_RING = 1,
  PHOTON_COLL_ALGO_TREE,
  PHOTON_COLL_ALGO_REC_DOUBLING
} photon_coll_algo;

typedef enum {
  PHOTON_COLL_NODE_RING_HEAD = 1,
  PHOTON_COLL_NODE_RING_REGULAR,
  PHOTON_COLL_NODE_TREE_ROOT,
  PHOTON_COLL_NODE_TREE_INTERNAL,
  PHOTON_COLL_NODE_TREE_LEAF,
  PHOTON_COLL_NODE_REC_DOUBLING
} photon_coll_node;

typedef enum {
  PHOTON_COLL_DATA_DOUBLE = 1,
  PHOTON_COLL_DATA_INT
} photon_coll_data;
  
typedef enum {
  PHOTON_COLL_OP_SUM = 1,
  PHOTON_COLL_OP_MIN,
  PHOTON_COLL_OP_MAX,
  PHOTON_COLL_OP_PROD,
  PHOTON_COLL_TYPE
} photon_coll_op;

#define PHOTON_COLL_MSG_RING_AT_BR       65
#define PHOTON_COLL_MSG_RING_RELEASE_BR  67
#define PHOTON_COLL_MSG_TREE_CHILD_AT_BR 68
#define PHOTON_COLL_MSG_REC_DOUBLING_TAG 1

#ifdef __cplusplus
}
#endif

#endif
