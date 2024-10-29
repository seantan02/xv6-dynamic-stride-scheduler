#include "pstat.h"

#include "types.h"
#include "user.h"

struct pstat pstats = {0};

int
getpinfo(struct pstat *ret_pstat)
{
  if(ret_pstat != 0) // We only expect a pointer to a struct. Not with mem allocated
    return -1;

  //Following convention from sh.c for malloc
  struct pstat *pstats_copy;
  pstats_copy = malloc(sizeof(*pstats_copy));
  memset(pstats_copy, 0, sizeof(*pstats_copy));

  if(pstats_copy == 0)
    return -1;

  // acquire lock for pstats struct
  for(int i = 0; i < NPROC; i++) {
    pstats_copy->inuse[i] = pstats.inuse[i];
    pstats_copy->tickets[i] = pstats.tickets[i];
    pstats_copy->pid[i] = pstats.pid[i];
    pstats_copy->pass[i] = pstats.pass[i];
    pstats_copy->remain[i] = pstats.remain[i];
    pstats_copy->stride[i] = pstats.stride[i];
    pstats_copy->rtime[i] = pstats.rtime[i];
  }

  // release lock for pstats struct
  ret_pstat = (struct pstat*)pstats_copy;
      
  return 0;
}
