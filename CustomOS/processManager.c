/* ------------------------------------------------------------------------
   proccessManager.c
   
   Supports the creation, joining, blocking, and scheduling of processes
 
   Functions from an external library called USLOSS are sometimes used
 
   ------------------------------------------------------------------------ */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <processManager.h>
#include "kernel.h"

/* ------------------------- Prototypes ----------------------------------- */
int sentinel(char *);
extern int start1(char *);
void dispatcher(void);
void launch();
static void disableInterrupts();
static void enableInterrupts();
static void check_deadlock();
void clock_interrupt(int, void *);
int assign_pid();
int get_pid();
void dump_processes();
int block_me(int);
int unblock_proc(int);
int zap(int);
int is_zapped();
void addToReadyList(int);
void frontToBack(procLinkedList);
void removeFromReadyList(int, int);
void addToBlockedList(int);
int removeFromBlockedList(int);
void removeFromChildList(int);

/* -------------------------- Globals ------------------------------------- */

/* Debugging global variable... */
int debugflag = 1;

/* the process table */
proc_struct ProcTable[MAXPROC];

/* Process lists  */
procLinkedList ReadyProcs[SENTINELPRIORITY + 1]; // Each slot represents a priority, slot 0 is not occupied
procLinkedList BlockedProcs;

/* current process ID */
proc_ptr Current;

/* the next pid to be assigned */
unsigned int next_pid = SENTINELPID;

/* The number of processes currently in the process table*/
int numProcs = 0;

/* -------------------------- Functions ----------------------------------- */
/* ------------------------------------------------------------------------
   Name - startup
   Purpose - Initializes process lists and clock interrupt vector.
        Start up sentinel process and the test process.
   Parameters - none, called by USLOSS
   Returns - nothing
   Side Effects - lots, starts the whole thing
   ----------------------------------------------------------------------- */
void startup()
{
   disableInterrupts();

   int i;      /* loop index */
   int result; /* value returned by call to fork1() */

   /* initialize the process table and ready list */
   memset(ProcTable, 0, MAXPROC * sizeof(ProcTable[0]));
   memset(ReadyProcs, 0, (SENTINELPRIORITY + 1) * sizeof(ReadyProcs[0]));

   if (DEBUG && debugflag)
      console("startup(): initializing the Ready & Blocked lists\n");

   /* Initialize the clock interrupt handler */
   int_vec[CLOCK_DEV] = clock_interrupt;

   /* startup a sentinel process */
   if (DEBUG && debugflag)
      console("startup(): calling fork1() for sentinel\n");

   result = fork1("sentinel", sentinel, NULL, USLOSS_MIN_STACK,
                  SENTINELPRIORITY);

   if (result < 0)
   {
      if (DEBUG && debugflag)
         console("startup(): fork1 of sentinel returned error, halting...\n");
      halt(1);
   }
   int status;

   /* start the test process */
   if (DEBUG && debugflag)
      console("startup(): calling fork1() for start1\n");
   result = fork1("start1", start1, NULL, 2 * USLOSS_MIN_STACK, 1);

   if (result < 0)
   {
      console("startup(): fork1 for start1 returned an error, halting...\n");
      halt(1);
   }

   sentinel("");

   console("startup(): Should not see this message! ");
   console("Returned from fork1 call that created start1\n");

   return;
} /* startup */

/* ------------------------------------------------------------------------
   Name - finish
   Purpose - Required by USLOSS
   Parameters - none
   Returns - nothing
   Side Effects - none
   ----------------------------------------------------------------------- */
void finish()
{
   if (DEBUG && debugflag)
      console("in finish...\n");
} /* finish */

/* ------------------------------------------------------------------------
   Name - fork1
   Purpose - Gets a new process from the process table and initializes
             information of the process.  Updates information in the
             parent process to reflect this child process creation.
   Parameters - the process procedure address, the size of the stack and
                the priority to be assigned to the child process.
   Returns - the process id of the created child or -1 if no child could
             be created or if priority is not between max and min priority.
   Side Effects - ReadyList is changed, ProcTable is changed, Current
                  process information changed
   ------------------------------------------------------------------------ */
int fork1(char *name, int (*f)(char *), char *arg, int stacksize, int priority)
{
   disableInterrupts();
   int proc_slot;

   if (DEBUG && debugflag)
      console("fork1(): creating process %s\n", name);

   /* test if in kernel mode; halt if in user mode */
   if ((0x1 & psr_get()) == 0)
   {
      console("Not in kernel mode when fork1 was called");
      halt(1);
   }

   /* Return if stack size is too small */
   if (stacksize < USLOSS_MIN_STACK)
   {
      return -2;
   }

   /* Check for errors, return -1 if found*/
   if (numProcs >= MAXPROC || priority < 1 || priority > 6 || f == NULL)
   {
      return -1;
   }

   proc_slot = assign_pid();

   /* fill-in entry in process table */
   if (strlen(name) >= (MAXNAME - 1))
   {
      console("fork1(): Process name is too long.  Halting...\n");
      halt(1);
   }
   strcpy(ProcTable[proc_slot].name, name);
   ProcTable[proc_slot].start_func = f;
   if (arg == NULL)
      ProcTable[proc_slot].start_arg[0] = '\0';
   else if (strlen(arg) >= (MAXARG - 1))
   {
      console("fork1(): argument too long.  Halting...\n");
      halt(1);
   }
   else
      strcpy(ProcTable[proc_slot].start_arg, arg);

   /* Initialize context for this process, but use launch function pointer for
    * the initial value of the process's program counter (PC)
    */
   ProcTable[proc_slot].stack = malloc(stacksize);
   ProcTable[proc_slot].stacksize = stacksize;
   context_init(&(ProcTable[proc_slot].state), psr_get(),
                ProcTable[proc_slot].stack,
                ProcTable[proc_slot].stacksize, launch);

   ProcTable[proc_slot].priority = priority;
   ProcTable[proc_slot].slot = proc_slot;

   // Add this newly created process as the child of Current
   if ((Current != NULL) && (Current->child_proc_ptr == NULL))
   {
      ProcTable[proc_slot].parent_pid = Current->pid;
      Current->child_proc_ptr = &ProcTable[proc_slot];
      Current->num_children++;
   }
   else if (Current != NULL)
   {
      ProcTable[proc_slot].parent_pid = Current->pid;
      Current->child_proc_ptr->next_sibling_ptr = &ProcTable[proc_slot];
      Current->num_children++;
   }
   else
   {
      Current = &ProcTable[proc_slot];
   }

   ProcTable[proc_slot].status = 1; // Set the process as ready (status 1)
   addToReadyList(proc_slot);

   p1_fork(ProcTable[proc_slot].pid);

   numProcs++;

   dispatcher();

   return ProcTable[proc_slot].pid;

} /* fork1 */

/* ------------------------------------------------------------------------
   Name - launch
   Purpose - Dummy function to enable interrupts and launch a given process
             upon startup.
   Parameters - none
   Returns - nothing
   Side Effects - enable interrupts
   ------------------------------------------------------------------------ */
void launch()
{
   int result;

   if (DEBUG && debugflag)
      console("launch(): started\n");

   /* Enable interrupts */
   enableInterrupts();

   /* Call the function passed to fork1, and capture its return value */
   result = Current->start_func(Current->start_arg);

   if (DEBUG && debugflag)
      console("Process %d returned to launch\n", Current->pid);

   quit(result);

} /* launch */

/* ------------------------------------------------------------------------
   Name - join
   Purpose - Wait for a child process (if one has been forked) to quit.  If
             one has already quit, don't wait.
   Parameters - a pointer to an int where the termination code of the
                quitting process is to be stored.
   Returns - the process id of the quitting child joined on.
      -1 if the process was zapped in the join
      -2 if the process has no children
   Side Effects - If no child process has quit before join is called, the
                  parent is removed from the ready list and blocked.
   ------------------------------------------------------------------------ */
int join(int *code)
{

   disableInterrupts();
   if (Current->num_children == 0)
   {
      return -2;
   }

   proc_ptr cur = Current->child_proc_ptr;
   int quit_pid;

   Current->status = 9;
   removeFromReadyList(Current->priority, Current->pid);
   addToBlockedList(Current->slot);
   dispatcher();

   while (1)
   {
      if (cur->status == 4)
      {
         *code = cur->status_to_parent;
         quit_pid = cur->pid;

         // reset the procs slot
         removeFromChildList(cur->pid);
         memset(&ProcTable[cur->slot], 0, sizeof(ProcTable[0]));
         Current->num_children--;
         numProcs--;
         return quit_pid;
      }
      else if (cur->next_sibling_ptr == NULL)
      {
         cur = Current->child_proc_ptr;
      }
      else
      {
         cur = cur->next_sibling_ptr;
      }
   }
} /* join */

/* ------------------------------------------------------------------------
   Name - quit
   Purpose - Stops the child process and notifies the parent of the death by
             putting child quit info on the parents child completion code
             list.
   Parameters - the code to return to the grieving parent
   Returns - nothing
   Side Effects - changes the parent of pid child completion status list.
   ------------------------------------------------------------------------ */
void quit(int code)
{

   disableInterrupts();
   if (Current->num_children > 0)
   {
      console("ERROR: Cannot call quit on a process with children.");
      halt(1);
   }

   Current->status = 4; // 4 is the status number for a quit process
   Current->status_to_parent = code;

   removeFromBlockedList(Current->parent_pid);    // wake the parent for join
   addToReadyList(Current->parent_pid % MAXPROC); // the parents slot
   removeFromReadyList(Current->priority, Current->pid);

   p1_quit(Current->pid);

   dispatcher();
} /* quit */

/* ------------------------------------------------------------------------
   Name - dispatcher
   Purpose - dispatches ready processes.  The process with the highest
             priority (the first on the ready list) is scheduled to
             run.  The old process is swapped out and the new process
             swapped in.
   Parameters - none
   Returns - nothing
   Side Effects - the context of the machine is changed
   ----------------------------------------------------------------------- */
void dispatcher(void)
{
   disableInterrupts();
   proc_ptr next_process;
   proc_ptr old_process;

   for (int i = 1; i < SENTINELPRIORITY + 1; i++)
   {

      if (ReadyProcs[i].hasProc)
      {
         next_process = ReadyProcs[i].head;
         old_process = Current;
         old_process->total_cpu_time += (sys_clock() - read_cur_start_time());
         Current = next_process;
         Current->cur_start_time = sys_clock();
         p1_switch(old_process->pid, next_process->pid);
         enableInterrupts();
         context_switch(&(old_process->state), &(next_process->state));
         break;
      }
   }

} /* dispatcher */

/* ------------------------------------------------------------------------
   Name - sentinel
   Purpose - The purpose of the sentinel routine is two-fold.  One
             responsibility is to keep the system going when all other
        processes are blocked.  The other is to detect and report
        simple deadlock states.
   Parameters - none
   Returns - nothing
   Side Effects -  if system is in deadlock, print appropriate error
         and halt.
   ----------------------------------------------------------------------- */
int sentinel(char *dummy)
{
   if (DEBUG && debugflag)
      console("sentinel(): called\n");
   while (1)
   {
      check_deadlock();
      waitint();
   }

   return 0;
} /* sentinel */

/* check to determine if deadlock has occurred... */
static void check_deadlock()
{
   int status;
   join(&status);
   console("All processes completed. \n");
   halt(0);
} /* check_deadlock */

/*
 * Disables the interrupts.
 */
void disableInterrupts()
{
   /* turn the interrupts OFF iff we are in kernel mode */
   if ((PSR_CURRENT_MODE & psr_get()) == 0)
   {
      // not in kernel mode
      console("Kernel Error: Not in kernel mode, may not disable interrupts\n");
      halt(1);
   }
   else
      /* We ARE in kernel mode */
      psr_set(psr_get() & ~PSR_CURRENT_INT);
} /* disableInterrupts */

/*
 * Enables the interrupts
 */
void enableInterrupts()
{
   /* turn the interrupts OPN iff we are in kernel mode */
   if ((PSR_CURRENT_MODE & psr_get()) == 0)
   {
      // not in kernel mode
      console("Kernel Error: Not in kernel mode, may not enable interrupts\n");
      halt(1);
   }
   else
   {
      psr_set(psr_get() | 0x2);
   }
} /* enableInterrupts*/

void clock_interrupt(int interrupt_num, void *unit_num)
{
   time_slice();
}

/* ------------------------------------------------------------------------
   Name - assign_pid
   Purpose - assigns a pid for a process that is to be created
   Parameters - none
   Returns - the slot in the process table that the process will occupy
   Side Effects - a slot in the process table is occupied
   ----------------------------------------------------------------------- */
int assign_pid()
{
   int proc_slot;

   while (ProcTable[next_pid % MAXPROC].pid != 0 && numProcs < MAXPROC)
   {
      next_pid++;
   }

   proc_slot = next_pid % MAXPROC;
   ProcTable[proc_slot].pid = next_pid;

   return proc_slot;
} /* assign_pid*/

int get_pid()
{
   return Current->pid;
}

void dump_processes()
{
   for (int i = 0; i < MAXPROC; i++)
   {
      if (ProcTable[i].pid != 0)
      {
         console("PROC NAME: %s \n", ProcTable[i].name);
         console("PROC ID: %d \n", ProcTable[i].pid);
         console("PROC PARENT ID: %d \n", ProcTable[i].parent_pid);
         console("PROC PRIORITY %d \n", ProcTable[i].priority);
         console("PROC STATUS: %d \n", ProcTable[i].status);
         console("PROC NUM CHILDREN: %d \n", ProcTable[i].num_children);
         console("PROC TOTAL CPU TIME: %d \n", ProcTable[i].total_cpu_time);
         console("--------------------------------------- \n");
      }
   }
}

int read_cur_start_time()
{
   return Current->cur_start_time;
}

void time_slice()
{
   int startTime = read_cur_start_time();

   // 80,000 microseconds = 80ms
   if (sys_clock() - startTime >= 80000)
   {
      // Remove process from front of queue, put on back
      frontToBack(ReadyProcs[Current->priority]);
      dispatcher();
   }
}

int block_me(int new_status)
{

   if (new_status >= 10)
   {
      console("ERROR: NEW STATUS MUST BE >= 10 \n");
      halt(1);
   }

   Current->status = new_status;
   removeFromReadyList(Current->priority, Current->pid);
   addToBlockedList(Current->slot);
   dispatcher();

   return 0;
}

int unblock_proc(int pid)
{
   int slot = pid % MAXPROC;

   if (slot > MAXPROC - 1 || ProcTable[slot].pid == 0)
   {
      return -2;
   }
   proc_struct theProc = ProcTable[slot];

   if (theProc.status <= 10 || theProc.pid == Current->pid)
   {
      return -2;
   }

   if (theProc.status == 3)
   {
      return -1;
   }

   removeFromBlockedList(pid);
   addToReadyList(slot);
   return 0;
}

int is_zapped()
{
   if (Current->status == 3)
   {
      return 1;
   }
   return 0;
}

/*
 * Adds the process that occupies the given slot in the ProcTable to
 * the ready list based on it's priority.
 */
void addToReadyList(int slot)
{
   int priority = ProcTable[slot].priority;

   if (ReadyProcs[priority].hasProc)
   {
      // Add this to the end of the ready list with the correct priority
      ReadyProcs[priority].tail->next_in_list = &ProcTable[slot];
      // Added proc becomes the new tail
      ReadyProcs[priority].tail = &ProcTable[slot];
   }
   else
   {
      ReadyProcs[priority].hasProc = 1;
      ReadyProcs[priority].head = &ProcTable[slot];
      ReadyProcs[priority].tail = &ProcTable[slot];
   }
}

/* Moves item from from of linked list to back*/
void frontToBack(procLinkedList theList)
{
   // if the list is a single element, no need to do anything
   if (theList.head != theList.tail)
   {
      theList.tail->next_in_list = theList.head;
      theList.tail = theList.head;
      theList.head = theList.head->next_in_list;
      theList.tail->next_in_list = NULL;
   }
}

void removeFromReadyList(int priority, int pidToRemove)
{
   proc_ptr cur = ReadyProcs[priority].head;
   proc_ptr temp;

   if (cur->pid == pidToRemove && cur->next_in_list == NULL)
   {
      ReadyProcs[priority].head = NULL;
      ReadyProcs[priority].tail = NULL;
      ReadyProcs[priority].hasProc = 0;
   }
   else if (cur->pid == pidToRemove)
   {
      ReadyProcs[priority].head = cur->next_in_list;
      cur->next_in_list = NULL;
   }
   else
   {
      while (cur->next_in_list != NULL)
      {
         if (cur->next_in_list->pid == pidToRemove)
         {
            temp = cur->next_in_list;
            cur->next_in_list = temp->next_in_list;
            temp->next_in_list = NULL;
         }
         cur = cur->next_in_list;
      }
   }
}

void addToBlockedList(int slot)
{

   if (BlockedProcs.hasProc)
   {
      // Add this to the end of the ready list with the correct priority
      BlockedProcs.tail->next_in_list = &ProcTable[slot];
      // Added proc becomes the new tail
      BlockedProcs.tail = &ProcTable[slot];
   }
   else
   {
      BlockedProcs.hasProc = 1;
      BlockedProcs.head = &ProcTable[slot];
      BlockedProcs.tail = &ProcTable[slot];
   }
}

int removeFromBlockedList(int pidToRemove)
{
   proc_ptr cur = BlockedProcs.head;
   proc_ptr temp;

   if (cur == NULL)
   {
      return -1;
   }

   if (cur->pid == pidToRemove && cur->next_in_list == NULL)
   {
      cur->status = 1;
      BlockedProcs.head = NULL;
      BlockedProcs.tail = NULL;
      BlockedProcs.hasProc = 0;
      return 0;
   }
   else if (cur->pid == pidToRemove)
   {
      cur->status = 1;
      BlockedProcs.head = cur->next_in_list;
      cur->next_in_list = NULL;
      return 0;
   }
   else
   {
      while (cur->next_in_list != NULL)
      {
         if (cur->next_in_list->pid == pidToRemove)
         {
            cur->next_in_list->status = 1;
            temp = cur->next_in_list;
            cur->next_in_list = temp->next_in_list;
            temp->next_in_list = NULL;
         }
         cur = cur->next_in_list;
      }
      return 0;
   }

   return -1;
}

void removeFromChildList(int pidToRemove)
{
   proc_ptr cur = Current->child_proc_ptr;
   proc_ptr temp;

   if (cur == NULL)
   {
      return;
   }

   if (cur->pid == pidToRemove && cur->next_sibling_ptr == NULL)
   {
      Current->child_proc_ptr = NULL;
   }
   else if (cur->pid == pidToRemove)
   {
      Current->child_proc_ptr = cur->next_sibling_ptr;
   }
   else
   {
      while (cur->next_sibling_ptr != NULL)
      {
         if (cur->next_sibling_ptr->pid == pidToRemove)
         {
            temp = cur->next_sibling_ptr;
            cur->next_in_list = temp->next_in_list;
            temp->next_in_list = NULL;
         }
         cur = cur->next_sibling_ptr;
      }
   }
}
