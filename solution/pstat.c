/*
#include "pstat.h"
//#include "spinlock.h"

struct pstat pstats = {0};

int
getpinfo(struct pstat *ret_pstat)
{

  if(ret_pstat == 0)
    return -1;

  //  acquire(&pstats.lock);

  for(int i = 0; i < NPROC; i++) {
    ret_pstat->inuse[i] = pstats.inuse[i];
    ret_pstat->tickets[i] = pstats.tickets[i];
    ret_pstat->pid[i] = pstats.pid[i];
    ret_pstat->pass[i] = pstats.pass[i];
    ret_pstat->remain[i] = pstats.remain[i];
    ret_pstat->stride[i] = pstats.stride[i];
    ret_pstat->rtime[i] = pstats.rtime[i];
  }

  //  release(&pstats.lock);
  
  return 0;
}

void
pstatinit(void)
{
  initlock(&pstats.lock, "pstats");
}
*/
