This is a project completed in class CS537 (Operating Systems) at UW-Madison. This repository is strictly for viewing purposes, any copying, especially for academic reference, should request an approval from authors.

Authors:
1. Sean Tan Siong Ann, contact Sean at sccf5bykus@privaterelay.appleid.com
  - Implemented Stride Scheduler 100% by himself
  - Fixed syscalls implemented by Pritheswar to have expected behavior when run "workload &" after starting the xv6
  - Design the overall idea of implementation of the scheduler
3. Pritheswar
  - Impleted syscalls according to the implementation of Sean's Stride Scheduler

This project contains the modified code for XV6 to allow Dynamic Stride Scheduler. The main modified fiels are:
1. proc.c
2. trap.c
3. main.c
4. All files needed for craeting new syscall

To run the xv6 with Dynamic Stride Scheduler, in your terminal run:
- git clone HTTP_URL of this repo
- Go into this repository and go into folder "solution"
- type "make clean" and hit enter
- type "make qemu-nox SCHEDULER=STRIDE" and hit enter
- To visualize how dynamic scheduler works, type "workload &" after the OS has started

Analysis:

- RR
- This scheduler schedules the process whenever the next one is in the loop and in the csv (from workload), we can see that the run time for all processes increased with same proportion (very close to exact same)

- Stride Scheduler
- This scheduler schedules the process that have more tickets and it shows in the csv that the growth of the run time is with similar proportion to the tickets the process holds. Note that the csv shows how processes started late will not catch up to the process with same tickets that ran before them but increase with the same proportion in runtime as them.

