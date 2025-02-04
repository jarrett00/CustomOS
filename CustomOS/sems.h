#pragma once

typedef struct Semaphore Semaphore;
typedef struct UserProc UserProc;
typedef struct UserProc *user_proc_ptr;

struct Semaphore
{
    int status; // 1 for in use, 0 for not in use
    int id;
    int value;
    user_proc_ptr firstWaiting;
};

struct UserProc
{

    user_proc_ptr nextChild;

    int pid;
    int (*entryPoint)(char *);
    int parentPid;
    int startupMbox; // the ID of the private mailbox
    int semMbox;     // the zero slot box for sems
    user_proc_ptr firstChild;
    user_proc_ptr nextWaiting;
};
