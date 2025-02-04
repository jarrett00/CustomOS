
#include <stdio.h>
#include <usloss.h>
#include <usyscall.h>
#include <string.h>
#include "sems.h"

/* ------------------------- Prototypes ----------------------------------- */
int start3(char *);
int spawn_real(char *name, int (*func)(char *), char *arg,
               int stack_size, int priority);
int wait_real(int *status);
int launchUserMode(char *);
void check_kernel_mode(void);
void syscall_handler(int dev, void *unit);
void syscall_spawn(sysargs *pargs);
void syscall_wait(sysargs *pargs);
void syscall_terminate(sysargs *pargs);
void syscall_semCreate(sysargs *pargs);
void syscall_semP(sysargs *pargs);
void syscall_semV(sysargs *pargs);
void syscall_semFree(sysargs *pargs);
void syscall_getTimeofDay(sysargs *pargs);
void syscall_cpuTime(sysargs *pargs);
void syscall_getPID(sysargs *pargs);
void addToChildList(int, int);
void removeChild(int);
void setToKernelMode(void);
int assignSemID();
int getSemSlot(int);
void addToWaitList(int, int);

/* -------------------------- Globals ------------------------------------- */
UserProc userProcTable[MAXPROC];

Semaphore semTable[MAXSEMS];

int numSems;
int semIDAssign = 0; // used for ID assignment

int mutexBox; // Box used as a mutex for semaphores

/* The syscall vector*/
void (*sys_vec[MAXSYSCALLS])(sysargs *args);

/* -------------------------- Implementation ------------------------------------- */
int start2(char *arg)
{
    int pid;
    int status;

    /* Check kernel mode */
    check_kernel_mode();

    /* Data structure initialization */
    memset(userProcTable, 0, MAXPROC * sizeof(userProcTable[0]));
    for (int i = 0; i < MAXPROC; i++)
    {
        // create a mailbox for each user proc for synchronization
        userProcTable[i].startupMbox = MboxCreate(1, 0);
        userProcTable[i].semMbox = MboxCreate(0, 0);
    }

    memset(semTable, 0, MAXPROC * sizeof(semTable[0]));

    // initialize mutex box
    mutexBox = MboxCreate(1, 0);

    /* Initialize syscall interrupt*/
    int_vec[SYSCALL_INT] = syscall_handler;

    /* Match syscall numbers to functions*/
    sys_vec[SYS_SPAWN] = &syscall_spawn;
    sys_vec[SYS_WAIT] = &syscall_wait;
    sys_vec[SYS_TERMINATE] = &syscall_terminate;
    sys_vec[SYS_SEMCREATE] = &syscall_semCreate;
    sys_vec[SYS_SEMP] = &syscall_semP;
    sys_vec[SYS_SEMV] = &syscall_semV;
    sys_vec[SYS_SEMFREE] = &syscall_semFree;
    sys_vec[SYS_GETTIMEOFDAY] = &syscall_getTimeofDay;
    sys_vec[SYS_CPUTIME] = &syscall_cpuTime;
    sys_vec[SYS_GETPID] = &syscall_getPID;

    pid = spawn_real("start3", start3, NULL, 4 * USLOSS_MIN_STACK, 3);
    pid = wait_real(&status);

    return 0;

} /* start2 */

/*
 * Launches a function of a process in user mode. arg is the
 * argument for the function
 */
int launchUserMode(char *arg)
{
    int pid = getpid();
    int procSlot = pid % MAXPROC;

    MboxReceive(userProcTable[procSlot].startupMbox, NULL, 0);

    /* Setting to User Mode*/
    int psr = psr_get();
    psr_set(psr & 14); // 14 in binary is 1110, so this turns off the kernel mode bit

    int result = userProcTable[procSlot].entryPoint(arg);
    Terminate(9);
    return result;
} /* launchUserMode*/

/*
 *  This is called by sys_call spawn as explained earlier.
 *  It uses fork which called launchUserMode to run a function
 *  in user mode.
 */
int spawn_real(char *name, int (*func)(char *), char *arg,
               int stack_size, int priority)
{
    int pid;
    pid = fork1(name, launchUserMode, arg, stack_size, priority);
    if (pid > 0)
    {

        /* Initialize the proc slot*/
        int procSlot;
        procSlot = pid % MAXPROC;
        userProcTable[procSlot].pid = pid;
        userProcTable[procSlot].parentPid = getpid();
        int parentSlot = userProcTable[procSlot].parentPid % MAXPROC;
        addToChildList(parentSlot, procSlot);

        /* Save the function to call for launchUserMode*/
        userProcTable[procSlot].entryPoint = func;

        /* Tell process to start running*/
        MboxCondSend(userProcTable[procSlot].startupMbox, NULL, 0);
    }
    else
    {

        return -1;
    }
    return pid;
} /* spawn_real*/

/*
 * This function is called by the syscall_wait function.
 * it essentially causes a user proc to wait until a child
 * quits. It then returns the pid of the child that quit
 * or an error code.
 */
int wait_real(int *status)
{
    int retVal;
    retVal = join(status);

    if (retVal > 0) // this means a pid was returned
    {
        removeChild(retVal); // remove the child from the userProc linked list
    }

    int dStatus = *status;

    if (retVal == -1) // a return of -1 means the proc was zapped while waiting
    {
        Terminate(dStatus);
    }

    return retVal;
} /*wait_real*/

/*
 * This is the function pointed to in the system call vector
 * for the spawn syscall. It unpacks the information
 * from the sysargs struct and calls the spawn_real function
 * to carry out the fork and launch of the procs' function.
 */
void syscall_spawn(sysargs *pargs)
{

    char *name = (char *)pargs->arg5;
    char *arg = (char *)pargs->arg2;
    int stack_size = (int)pargs->arg3;
    int priority = (int)pargs->arg4;
    int pid = spawn_real(name, pargs->arg1, arg, stack_size, priority);

    pargs->arg1 = pid;
    pargs->arg4 = 0;

    if (name > MAXNAME || stack_size < USLOSS_MIN_STACK || priority < 1 || priority > 6)
    {
        pargs->arg4 = 1;
    }
} /*syscall_spawn */

/*
 * This is the function pointed to in the syscall vector
 * when a wait syscall is fired. The main logic for this syscall
 * is in wait_real()
 */
void syscall_wait(sysargs *pargs)
{
    int status;

    int retVal = wait_real(&status);

    pargs->arg1 = retVal;
    pargs->arg2 = status;

    if (retVal == -2) // The process didn't have children
    {
        pargs->arg4 = -1;
    }
    else
    {
        pargs->arg4 = 0;
    }
} /*syscall_wait*/

/*
 * This function is pointed to by the syscall vector and fires
 * when the Terminate syscall is used. It terminates a user process
 * by zapping the procs children and then calling quit.
 */
void syscall_terminate(sysargs *pargs)
{
    int procSlot = getpid() % MAXPROC;
    int termCode = (int)pargs->arg1;

    // consider protecting this from interrupts

    // considering zeroing the proc table for these slots

    // loop through procs children and zap
    user_proc_ptr cur = userProcTable[procSlot].firstChild;
    user_proc_ptr temp;
    while (cur != NULL)
    {
        zap(cur->pid);
        temp = cur;
        cur = cur->nextChild;
        // memset(temp, 0, sizeof(userProcTable[0]))
    }

    quit(termCode);

    pargs->arg1 = termCode;
} /*syscall_terminate*/

/*
 * This function is pointed to by the syscall handler.
 * it creates a semaphore which will have a unique ID.
 * If no there are no semaphore slot left, this is reported
 * by settings arg4 to -1.
 */
void syscall_semCreate(sysargs *pargs)
{

    int initialVal = (int)pargs->arg1;

    // Check if slots are out or negative intial cal
    if (numSems >= MAXSEMS || initialVal < 0)
    {
        pargs->arg4 = -1;
        return;
    }

    int semSlot = assignSemID();
    semTable[semSlot].value = initialVal;
    pargs->arg1 = semTable[semSlot].id;
    pargs->arg4 = 0;

} /*syscall_semCreate*/

/*
 * This function is pointed to by the syscall vector.
 * It provides a p operation (decrement or block if value is 0)
 * On the semaphore with the id given in pargs->arg1;
 */
void syscall_semP(sysargs *pargs)
{
    int semID = (int)pargs->arg1;
    int slot = getSemSlot(semID);

    // check for bad input
    if (semTable[slot].status == 0 || semID < 0)
    {
        pargs->arg4 = -1;
        return;
    }

    // keep data from corruption
    MboxSend(mutexBox, NULL, 0);

    if (semTable[slot].value > 0)
    {
        // decrement
        semTable[slot].value = semTable[slot].value - 1;
    }
    else
    {
        // get the current proc's slot
        int procSlot = getpid() % MAXPROC;

        addToWaitList(slot, procSlot);
        MboxReceive(mutexBox, NULL, 0);

        // block
        MboxReceive(userProcTable[procSlot].semMbox, NULL, 0);
    }

    pargs->arg4 = 0;

} /*syscall_semP*/

/*
 * This function is pointed to by the syscall handler.
 * Is performs the v operation on a
 * semaphore (incrmeent and wake up waiting).
 */
void syscall_semV(sysargs *pargs)
{
    int semID = (int)pargs->arg1;
    int slot = getSemSlot(semID);

    // check for bad input
    if (semTable[slot].status == 0 || semID < 0)
    {
        pargs->arg4 = -1;
        return;
    }

    semTable[slot].value = semTable[slot].value + 1;

    // keep data from corruption
    MboxSend(mutexBox, NULL, 0);

    if (semTable[slot].firstWaiting != NULL)
    {
        user_proc_ptr curFirst = semTable[slot].firstWaiting;
        // advance the waiting list
        semTable[slot].firstWaiting = semTable[slot].firstWaiting->nextWaiting;
        MboxCondSend(curFirst->semMbox, NULL, 0);
    }

    MboxReceive(mutexBox, NULL, 0);
    pargs->arg4 = 0;

} /*syscall_semV*/

/*
 * Frees a semaphore
 */
void syscall_semFree(sysargs *pargs)
{
    int semID = (int)pargs->arg1;
    int slot = getSemSlot(semID);

    // check for bad input
    if (semTable[slot].status == 0 || semID < 0)
    {
        pargs->arg4 = -1;
        return;
    }

    if (semTable[slot].firstWaiting != NULL)
    {
        user_proc_ptr cur = semTable[slot].firstWaiting;
        user_proc_ptr temp;

        // zap all waiting processes
        while (cur != NULL)
        {
            zap(cur->pid);
            temp = cur;
            cur = cur->nextChild;
            /* Possibly clear proc entry*/
            // memset(temp, 0, sizeof(userProcTable[0]));
        }

        pargs->arg4 = 1;
    }
    else
    {
        pargs->arg4 = 0;
    }

    semTable[slot].value = 0;
    semTable[slot].status = 0;
    semTable[slot].firstWaiting = NULL;
    numSems--;

} /* syscall_semFree*/

/*
 * Returns	the	value	of	the	time-of-day	clock.
 */
void syscall_getTimeofDay(sysargs *pargs)
{
    int time = sys_clock();
    pargs->arg1 = time;

} /* syscall_getTimeofDay*/

/*
 * Returns	the	CPU	time	of	the	process	(this	is	the	actual	CPU	time	used,
 * not	just	the	time	since
 * the	current	time	slice	started).
 */
void syscall_cpuTime(sysargs *pargs)
{
    int cpuTime = readtime();
    pargs->arg1 = cpuTime;
} /* syscall_cpuTime*/

/*
 * The syscall vector points to this function.
 * Returns the process ID of the currently running process.
 */
void syscall_getPID(sysargs *pargs)
{
    int curPID = getpid();
    pargs->arg1 = curPID;

} /* syscall_getPID*/

/*
 * Checks if process is in kernel mode. Does nothing if it is, prints
 * an error and halts if the process is not in kernel mode.
 */
void check_kernel_mode()
{
    if ((PSR_CURRENT_MODE & psr_get()) == 0)
    {
        console("Kernel Error: Not in kernel mode.\n");
        halt(1);
    }
} /*check_kernel_mode*/

/*
 * The sys call handler. The syscall interrupt points to this method
 * The correct syscall is then carried out as long as the value is valid
 */
void syscall_handler(int dev, void *unit)
{
    sysargs *sys_ptr;
    sys_ptr = (sysargs *)unit;

    int callNumber = sys_ptr->number;

    if (callNumber < 0 || callNumber >= MAXSYSCALLS)
    {
        console("sys number %d is wrong.  Halting... \n", callNumber);
        halt(1);
    }
    else
    {
        sys_vec[callNumber](sys_ptr);
    }
} /*syscall_handler*/

/* Adds a child to a user process' child linked list */
void addToChildList(int parentSlot, int childSlot)
{
    // add as first child
    if (userProcTable[parentSlot].firstChild == NULL)
    {
        userProcTable[parentSlot].firstChild = &userProcTable[childSlot];
    }
    else
    {
        user_proc_ptr cur = userProcTable[parentSlot].firstChild;
        while (cur->nextChild != NULL)
        {
            // advance till nextChild is null
            cur = cur->nextChild;
        }

        // add the child to the list
        cur->nextChild = &userProcTable[childSlot];
    }
} /*addToChildList*/

/*
 * Removes a child from the child list of the user proc with
 * the given childPid
 */
void removeChild(int childPid)
{
    int childSlot = childPid % MAXPROC;
    int parentSlot = userProcTable[childSlot].parentPid % MAXPROC;

    // theres no child to remove
    if (userProcTable[parentSlot].firstChild == NULL)
    {
        return;
    }
    else if (userProcTable[parentSlot].firstChild->pid == childPid)
    {
        // if child with given pid is first child, remove it from list and advance to next
        userProcTable[parentSlot].firstChild = userProcTable[parentSlot].firstChild->nextChild;
    }
    else
    {
        user_proc_ptr cur = userProcTable[parentSlot].firstChild;
        // find child with the given pid, remove it from the list
        while (cur->nextChild != NULL)
        {
            if (cur->nextChild->pid == childPid)
            {
                cur->nextChild = cur->nextChild->nextChild;
                return;
            }

            cur = cur->nextChild;
        }
    }
} /*removeChild*/

/*Sets to kernel mode*/
void setToKernelMode()
{
    psr_set(psr_get() | 1);
} /*setToKernelMode*/

/*
 * Assigns an ID to a semaphore, returns the slot it occupies in the
 * semTable.
 */
int assignSemID()
{
    int slot;

    while (1)
    {
        slot = semIDAssign % MAXSEMS;

        if (semTable[slot].status == 0) // then the slot is free
        {
            semTable[slot].id = semIDAssign;
            semTable[slot].status = 1; // mark as occupied
            semIDAssign++;
            numSems++;
            return slot;
        }

        semIDAssign++;
    }

    return -1; // this shouldn't execute

} /*assignSemID*/

/*
 * Returns the slot which the semaphore with the given ID
 * occupies in the semTable
 */
int getSemSlot(int semID)
{
    return semID % MAXSEMS;
} /*getSemSlot*/

/*
 * Adds a process to the waiting list. The semSlot is the
 * slot of the semaphore in the semTable which has
 * the process waiting on it. The proc slot is the slot of
 * the user proc in the user proc table that is waiting.
 */
void addToWaitList(int semSlot, int procSlot)
{

    if (semTable[semSlot].firstWaiting == NULL)
    {
        semTable[semSlot].firstWaiting = &userProcTable[procSlot];
    }
    else
    {
        user_proc_ptr cur = semTable[semSlot].firstWaiting;

        // advance till end of the list
        while (cur->nextWaiting != NULL)
        {
            cur = cur->nextWaiting;
        }

        // add to end of the list
        cur->nextWaiting = &userProcTable[procSlot];
    }
} /* addToWaitList*/
