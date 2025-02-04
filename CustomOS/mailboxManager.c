/* ------------------------------------------------------------------------
    mailboxManager.c
 
    Implementation of mailboxes to allow for interprocess communication

   ------------------------------------------------------------------------ */
#include <stdlib.h>
#include <processManager.h>
#include <mailboxManager.h>
#include <usloss.h>
#include <string.h>
#include <stdio.h>

#include "message.h"

/* ------------------------- Prototypes ----------------------------------- */
int start1(char *);
extern int start2(char *);
void check_kernel_mode(void);
void enableInterrupts(void);
void disableInterrupts(void);
static void nullsys(sysargs *args);
int assignMailBoxID(void);
int getSlot(int);
void freeSlots(int);
int nextOpenMailSlot(void);
void removeMSG(int);
void addToWaitingList(int);
void addToBlockedList(int);
void handleProc();
void zapWaiting(int);
void unblockBlocked(int);

void clock_handler(int, void *);
void alarm_handler(int, void *);
void disk_handler(int, void *);
void term_handler(int, void *);
void mmu_handler(int, void *);
void syscall_handler(int, void *);

/* -------------------------- Globals ------------------------------------- */

int debugflag2 = 0;

int nextMailBoxID = 1;

mbox_proc_ptr CurrentProc;

/* The current number of Mailboxes*/
int numMailBoxes;

/* The number of slots in use*/
int mail_slots_used = 0;

/* The syscall vector*/
void (*sys_vec[MAXSYSCALLS])(sysargs *args);

/* the mail boxes */
mail_box MailBoxTable[MAXMBOX];

/* shared table for all mailboxes with slots*/
mail_slot MailSlotTable[MAXSLOTS];

/* Special Proc Table */
mbox_proc MBoxProcTable[MAXPROC];

/* -----------------------------------------------------------------------
   Name - start1
   Purpose - Initializes mailboxes and interrupt vector.
   Parameters - one, default arg passed by fork1, not used here.
   Returns - one to indicate normal quit.
   Side Effects - lots since it initializes the phase2 data structures.
   ----------------------------------------------------------------------- */
int start1(char *arg)
{
   int kid_pid, status;

   if (DEBUG2 && debugflag2)
      console("start1(): at beginning\n");

   check_kernel_mode();

   /* Disable interrupts */
   disableInterrupts();

   /* Initialize the mail box table, slots, & other data structures.
    * Initialize int_vec and sys_vec, allocate mailboxes for interrupt
    * handlers */
   memset(MailBoxTable, 0, MAXMBOX * sizeof(MailBoxTable[0]));
   memset(MailSlotTable, 0, MAXSLOTS * sizeof(MailSlotTable[0]));
   memset(MBoxProcTable, 0, MAXPROC * sizeof(MBoxProcTable[0]));

   // create the 6 mailboxes for the interrupt vector
   for (int i = 0; i < 6; i++)
   {
      MboxCreate(0, 1);
   }

   int_vec[CLOCK_INT] = clock_handler;
   int_vec[ALARM_INT] = alarm_handler;
   int_vec[DISK_INT] = disk_handler;
   int_vec[TERM_INT] = term_handler;
   int_vec[MMU_INT] = mmu_handler;
   int_vec[SYSCALL_INT] = syscall_handler;

   for (int i = 0; i < MAXSYSCALLS; i++)
   {
      sys_vec[i] = &nullsys;
   }

   enableInterrupts();

   /* Create a process for start2, then block on a join until start2 quits */
   if (DEBUG2 && debugflag2)
      console("start1(): fork'ing start2 process\n");
   kid_pid = fork1("start2", start2, NULL, 4 * USLOSS_MIN_STACK, 1);
   if (join(&status) != kid_pid)
   {
      console("start2(): join returned something other than start2's pid\n");
   }

   return 0;
} /* start1 */

/* ------------------------------------------------------------------------
   Name - MboxCreate
   Purpose - gets a free mailbox from the table of mailboxes and initializes it
   Parameters - maximum number of slots in the mailbox and the max size of a msg
                sent to the mailbox.
   Returns - -1 to indicate that no mailbox was created, or a value >= 0 as the
             mailbox id.
   Side Effects - initializes one element of the mail box array.
   ----------------------------------------------------------------------- */
int MboxCreate(int slots, int slot_size)
{
   check_kernel_mode();
   handleProc();
   if (numMailBoxes >= MAXMBOX || slot_size < 0 || slot_size > MAX_MESSAGE || slots < 0)
   {
      return -1;
   }

   disableInterrupts();
   int slot = assignMailBoxID();
   int assignedID = MailBoxTable[slot].mbox_id;
   MailBoxTable[slot].num_slots = slots;
   MailBoxTable[slot].slot_size = slot_size;
   MailBoxTable[slot].unused_slots = slots;
   MailBoxTable[slot].numWaiting = 0;
   numMailBoxes++;
   enableInterrupts();

   return assignedID;

} /* MboxCreate */

/* ------------------------------------------------------------------------
   Name - MboxRelease
   Purpose - Releases a previously created mailbox.
   Parameters - The mailbox ID of the mailbox to release
   Returns - -3 if the process was zapped while releasing the mailbox.
             -1 if the mailboxID is not a mailbox that is in use
              0 if the mailbox was released successfully
   Side Effects - Zaps processes waiting on the mailbox. Unblocks waiting procs.
   ----------------------------------------------------------------------- */
int MboxRelease(int mailboxID)
{
   check_kernel_mode();
   handleProc();
   int mBoxTableSlot = getSlot(mailboxID);

   if (mBoxTableSlot == -1)
   {
      return -1;
   }
   else if (is_zapped())
   {
      return -3;
   }

   disableInterrupts();

   MailBoxTable[mBoxTableSlot].isReleased = 1; // mark it as released

   zapWaiting(mBoxTableSlot);
   unblockBlocked(mBoxTableSlot);
   freeSlots(mBoxTableSlot);

   memset(&MailBoxTable[mBoxTableSlot], 0, sizeof(MailBoxTable[0])); // Free the mailbox slot in table
   numMailBoxes--;

   enableInterrupts();

   return 0;

} /* MboxRelease*/

/* ------------------------------------------------------------------------
   Name - MboxSend
   Purpose - Put a message into a slot for the indicated mailbox.
             Block the sending process if no slot available.
   Parameters - mailbox id, pointer to data of msg, # of bytes in msg.
   Returns - zero if successful, -1 if invalid args.
   Side Effects - none.
   ----------------------------------------------------------------------- */
int MboxSend(int mbox_id, void *msg_ptr, int msg_size)
{
   check_kernel_mode();
   handleProc();

   if (mail_slots_used >= MAXSLOTS)
   {
      console("ERROR: THE SYSTEM IS OUT OF MAILBOX SLOTS \n");
      halt(1);
   }
   int mboxTableSlot = getSlot(mbox_id);

   if (mboxTableSlot == -1)
   {
      return -1;
   }

   else if (msg_size > MailBoxTable[mboxTableSlot].slot_size)
   {
      return -1;
   }

   /*Block the process if theres no space to queue and no procs waiting */
   if (MailBoxTable[mboxTableSlot].numWaiting == 0 && MailBoxTable[mboxTableSlot].unused_slots == 0)
   {
      addToBlockedList(mboxTableSlot);
      block_me(11);
   }

   if (is_zapped() || MailBoxTable[mboxTableSlot].isReleased)
   {
      return -3;
   }

   disableInterrupts();
   int slotTableIndex = nextOpenMailSlot();

   MailSlotTable[slotTableIndex].isOccupied = 1;
   MailSlotTable[slotTableIndex].index = slotTableIndex;
   MailSlotTable[slotTableIndex].mbox_id = MailBoxTable[mboxTableSlot].mbox_id;
   MailSlotTable[slotTableIndex].messageSize = msg_size;
   MailSlotTable[slotTableIndex].next_in_box = NULL;                 // make sure thee's nothing in the next field yet
   memcpy(MailSlotTable[slotTableIndex].message, msg_ptr, msg_size); // Put the message in the slot

   // Put the mailslot in the appropriate place in the mailbox
   if (MailBoxTable[mboxTableSlot].first_slot == NULL)
   {
      MailBoxTable[mboxTableSlot].first_slot = &MailSlotTable[slotTableIndex];
   }
   else
   {
      slot_ptr curSlot = MailBoxTable[mboxTableSlot].first_slot;

      // find the end of the linked list
      while (curSlot->next_in_box != NULL)
      {
         curSlot = curSlot->next_in_box;
      }
      // add to the end of the linked list
      curSlot->next_in_box = &MailSlotTable[slotTableIndex];
   }

   MailBoxTable[mboxTableSlot].unused_slots--;

   // Wake up the next waiting process
   if (MailBoxTable[mboxTableSlot].numWaiting > 0)
   {
      int waiting_pid = MailBoxTable[mboxTableSlot].waitingProc->pid;
      mbox_proc_ptr old = MailBoxTable[mboxTableSlot].waitingProc;

      // Advance the queue
      MailBoxTable[mboxTableSlot].waitingProc = MailBoxTable[mboxTableSlot].waitingProc->next;
      MailBoxTable[mboxTableSlot].numWaiting--;
      old->next = NULL;
      unblock_proc(waiting_pid);
   }
   enableInterrupts();

   return 0;
} /* MboxSend */

/* ------------------------------------------------------------------------
   Name - MboxReceive
   Purpose - Get a msg from a slot of the indicated mailbox.
             Block the receiving process if no msg available.
   Parameters - mailbox id, pointer to put data of msg, max # of bytes that
                can be received.
   Returns - actual size of msg if successful, -1 if invalid args.
   Side Effects - none.
   ----------------------------------------------------------------------- */
int MboxReceive(int mbox_id, void *msg_ptr, int msg_size)
{
   check_kernel_mode();
   handleProc();

   int mboxTableSlot = getSlot(mbox_id);

   if (mboxTableSlot == -1)
   {
      return -1;
   }

   if (MailBoxTable[mboxTableSlot].first_slot == NULL)
   {
      addToWaitingList(mboxTableSlot);
      block_me(11);
   }

   if (is_zapped() || MailBoxTable[mboxTableSlot].isReleased)
   {
      return -3;
   }

   if (MailBoxTable[mboxTableSlot].first_slot->messageSize > msg_size)
   {
      return -1;
   }

   disableInterrupts();
   int received_msg_size = MailBoxTable[mboxTableSlot].first_slot->messageSize;
   memcpy(msg_ptr, MailBoxTable[mboxTableSlot].first_slot->message, msg_size);
   removeMSG(mboxTableSlot);

   if (MailBoxTable[mboxTableSlot].numBlocked > 0 && MailBoxTable[mboxTableSlot].unused_slots > 0)
   {
      int blocked_pid = MailBoxTable[mboxTableSlot].blockedProc->pid;
      mbox_proc_ptr old = MailBoxTable[mboxTableSlot].blockedProc;

      // Advance the queue
      MailBoxTable[mboxTableSlot].blockedProc = MailBoxTable[mboxTableSlot].blockedProc->next;
      MailBoxTable[mboxTableSlot].numBlocked--;
      old->next = NULL;

      enableInterrupts();

      unblock_proc(blocked_pid);
   }

   return received_msg_size;

} /* MboxReceive */

/* ------------------------------------------------------------------------
   Name - MboxCondSend
   Purpose - Sends a message to a mailbox, but doesn't block the current
               process if a slot isn't available. Returns -2 instead.
   Parameters - mailbox id, pointer to data of msg, # of bytes in msg.
   Returns - zero if successful, -1 if invalid args, 02 if mailbox is full
   or the system is out of slots
   Side Effects - none.
   ----------------------------------------------------------------------- */
int MboxCondSend(int mbox_id, void *message, int msg_size)
{
   check_kernel_mode();
   handleProc();

   if (mail_slots_used >= MAXSLOTS)
   {
      return -2;
   }
   int mboxTableSlot = getSlot(mbox_id);

   if (mboxTableSlot == -1)
   {
      return -1;
   }

   else if (msg_size > MailBoxTable[mboxTableSlot].slot_size)
   {
      return -1;
   }

   /*Block the process if theres no space to queue and no procs waiting */
   if (MailBoxTable[mboxTableSlot].unused_slots == 0)
   {
      return -2;
   }

   if (is_zapped() || MailBoxTable[mboxTableSlot].isReleased)
   {
      return -3;
   }

   disableInterrupts();
   int slotTableIndex = nextOpenMailSlot();

   MailSlotTable[slotTableIndex].isOccupied = 1;
   MailSlotTable[slotTableIndex].index = slotTableIndex;
   MailSlotTable[slotTableIndex].mbox_id = MailBoxTable[mboxTableSlot].mbox_id;
   MailSlotTable[slotTableIndex].messageSize = msg_size;
   MailSlotTable[slotTableIndex].next_in_box = NULL;                 // make sure thee's nothing in the next field yet
   memcpy(MailSlotTable[slotTableIndex].message, message, msg_size); // Put the message in the slot

   // Put the mailslot in the appropriate place in the mailbox
   if (MailBoxTable[mboxTableSlot].first_slot == NULL)
   {
      MailBoxTable[mboxTableSlot].first_slot = &MailSlotTable[slotTableIndex];
   }
   else
   {

      slot_ptr curSlot = MailBoxTable[mboxTableSlot].first_slot;
      // Find the end of the linked list
      while (curSlot->next_in_box != NULL)
      {
         curSlot = curSlot->next_in_box;
      }
      // add to the end of the linked list
      curSlot->next_in_box = &MailSlotTable[slotTableIndex];
   }

   MailBoxTable[mboxTableSlot].unused_slots--;

   // Wake up the next waiting process
   if (MailBoxTable[mboxTableSlot].numWaiting > 0)
   {
      int waiting_pid = MailBoxTable[mboxTableSlot].waitingProc->pid;
      mbox_proc_ptr old = MailBoxTable[mboxTableSlot].waitingProc;

      // Advance the queue
      MailBoxTable[mboxTableSlot].waitingProc = MailBoxTable[mboxTableSlot].waitingProc->next;
      MailBoxTable[mboxTableSlot].numWaiting--;
      old->next = NULL;
      unblock_proc(waiting_pid);
   }
   enableInterrupts();

   return 0;
} /*MboxCondSend*/

/* ------------------------------------------------------------------------
 Name - MboxCondReceive
Purpose - Gets a message from a mailbox, but does not block if
               no messages are available
Parameters - mailbox id, pointer to put data of msg, max # of bytes that
               can be received.
Returns - actual size of msg if successful, -1 if invalid args, -2 if
            no messages are available
Side Effects - none.
----------------------------------------------------------------------- */
int MboxCondReceive(int mbox_id, void *message, int msg_size)
{
   check_kernel_mode();
   handleProc();

   int mboxTableSlot = getSlot(mbox_id);

   if (mboxTableSlot == -1)
   {
      return -1;
   }

   if (MailBoxTable[mboxTableSlot].first_slot == NULL)
   {
      return -2;
   }

   if (is_zapped() || MailBoxTable[mboxTableSlot].isReleased)
   {
      return -3;
   }

   if (MailBoxTable[mboxTableSlot].first_slot->messageSize > msg_size)
   {
      return -1;
   }

   disableInterrupts();
   int received_msg_size = MailBoxTable[mboxTableSlot].first_slot->messageSize;
   memcpy(message, MailBoxTable[mboxTableSlot].first_slot->message, msg_size);
   removeMSG(mboxTableSlot);

   if (MailBoxTable[mboxTableSlot].numBlocked > 0 && MailBoxTable[mboxTableSlot].unused_slots > 0)
   {
      int blocked_pid = MailBoxTable[mboxTableSlot].blockedProc->pid;
      mbox_proc_ptr old = MailBoxTable[mboxTableSlot].blockedProc;

      // Advance the queue
      MailBoxTable[mboxTableSlot].blockedProc = MailBoxTable[mboxTableSlot].blockedProc->next;
      MailBoxTable[mboxTableSlot].numBlocked--;
      old->next = NULL;

      enableInterrupts();

      unblock_proc(blocked_pid);
   }
   return received_msg_size;
} /*MboxCondReceive*/

int check_io()
{
   return 0;
}

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
 * A function for nonintialized sys calls to point to
 */
static void nullsys(sysargs *args)
{
   printf("Invalid syscall %d. Halting... \n", args->number);
   halt(1);
} /* null sys*/

/*
 * Assigns an ID to a mailbox and returns the slot the mailbox
 * occupies within the mailbox table.
 */
int assignMailBoxID()
{
   int table_slot;
   while (1)
   {
      table_slot = nextMailBoxID % MAXMBOX;
      if (MailBoxTable[table_slot].mbox_id == 0) // no mbox will have an id of zero unless uninitialized (free slot)
      {
         MailBoxTable[table_slot].mbox_id = nextMailBoxID;
         return table_slot;
      }

      nextMailBoxID++;
   }

   return 0;
} /* assignMailboxID*/

/*
 * Returns the slot in the table in which the mailbox with the given ID resides.
 * Returns -1 if the given mboxID is not in use or doesn't exist yet.
 */
int getSlot(int mboxID)
{
   int slot = mboxID % MAXMBOX;
   if (MailBoxTable[slot].mbox_id == 0)
   {
      return -1;
   }
   return slot;
} /*getSlot*/

/* Frees the mailbox slots associated with the mailbox that occupies
 * the given slot (tableSlot) in the MailBoxTable
 */
void freeSlots(int tableSlot)
{
   mail_box mBox = MailBoxTable[tableSlot];
   slot_ptr cur = mBox.first_slot;
   slot_ptr temp;

   while (cur != NULL)
   {
      temp = cur;
      cur = cur->next_in_box;
      memset(temp, 0, sizeof(MailBoxTable[0])); // free the slot
      mail_slots_used--;
   }

} /* freeSlots*/

/*
 * Returns the index of the next open mail slot within the
 * MailSlotTable
 */
int nextOpenMailSlot()
{
   for (int i = 0; i < MAXSLOTS; i++)
   {
      if (MailSlotTable[i].isOccupied == 0)
      {
         return i;
      }
   }

   return -1; // Will only be returned if there is MAXSLOTS slots

} /*nextOpenMailSlot*/

/*
 * Removes the first message from a mailbox and advances to the next.
 * The mail slot where the message was held is freed.
 */
void removeMSG(int mBoxTableSlot)
{

   if (MailBoxTable[mBoxTableSlot].first_slot == NULL)
   {
      return;
   }

   int slotIndex = MailBoxTable[mBoxTableSlot].first_slot->index;

   // first_slot becomes next
   MailBoxTable[mBoxTableSlot].first_slot = MailBoxTable[mBoxTableSlot].first_slot->next_in_box;
   memset(&MailSlotTable[slotIndex], 0, sizeof(MailBoxTable[0])); // free the slot
   MailBoxTable[mBoxTableSlot].unused_slots++;
   mail_slots_used--;
} /*removeMSG*/

/*
 * Adds the current process to the waiting queue of the mailbox
 * that is at index mBoxTableSlot
 */
void addToWaitingList(int mBoxTableSlot)
{
   disableInterrupts();
   if (MailBoxTable[mBoxTableSlot].waitingProc == NULL)
   {
      MailBoxTable[mBoxTableSlot].waitingProc = &MBoxProcTable[CurrentProc->index];
   }
   else
   {
      mbox_proc_ptr cur = MailBoxTable[mBoxTableSlot].waitingProc;
      while (cur->next != NULL)
      {
         cur = cur->next;
      }
      cur->next = &MBoxProcTable[CurrentProc->index];
   }

   MailBoxTable[mBoxTableSlot].numWaiting++;
   enableInterrupts();
} /*addToWaitingList*/

/*Adds the current proc to the blocked list of the mailbox in the given slot*/
void addToBlockedList(int mBoxTableSlot)
{
   disableInterrupts();
   if (MailBoxTable[mBoxTableSlot].blockedProc == NULL)
   {
      MailBoxTable[mBoxTableSlot].blockedProc = &MBoxProcTable[CurrentProc->index];
   }
   else
   {
      mbox_proc_ptr cur = MailBoxTable[mBoxTableSlot].blockedProc;
      while (cur->next != NULL)
      {
         cur = cur->next;
      }
      cur->next = &MBoxProcTable[CurrentProc->index];
   }

   MailBoxTable[mBoxTableSlot].numBlocked++;
   enableInterrupts();
} /*addToBlockedList*/

/*
 * Checks to see if the current process is already in the table.
 * If the current process is in the table already nothing happens.
 * If it is not, the process is added to the table.
 * Also updates Current.
 */
void handleProc()
{

   disableInterrupts();
   int nextAvailableSlot = -1;
   int current_pid = getpid();
   for (int i = 0; i < MAXPROC; i++)
   {
      if (MBoxProcTable[i].pid == current_pid)
      {
         return;
      }

      // only update nextAvailableSlot if this is the first time finding a free slot
      if (nextAvailableSlot == -1 && MBoxProcTable[i].pid == 0)
      {
         nextAvailableSlot = i;
      }
   }

   /* Only reached if the process is not in the table*/
   MBoxProcTable[nextAvailableSlot].pid = current_pid;
   MBoxProcTable[nextAvailableSlot].index = nextAvailableSlot;
   CurrentProc = &MBoxProcTable[nextAvailableSlot];

   enableInterrupts();

} /*handleProc*/

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

/*
 * The handler for the clock interrupt
 */
void clock_handler(int dev, void *unit)
{

} /*clock_handler*/

/*
 * The handler for the alarm interrupt
 */
void alarm_handler(int dev, void *unit)
{

} /*alarm_handler*/

/*
 * The handler for the disk interrupt
 */
void disk_handler(int dev, void *unit)
{

} /*disk_handler*/

/*
 * The handler for the term interrupt
 */
void term_handler(int dev, void *unit)
{

} /*term_handler*/

/*
 * The handler for the mmu interrupt
 */
void mmu_handler(int dev, void *unit)
{

} /*mmu_handler*/

int waitdevice(int type, int unit, int *status)
{
   return 0;
} /*waitdevice*/

/*
 * Zaps all procs that are waiting on the mailbox in the given slot
 */
void zapWaiting(int mBoxTableSlot)
{
   mbox_proc_ptr curProc = MailBoxTable[mBoxTableSlot].waitingProc;
   mbox_proc_ptr temp;

   while (curProc != NULL)
   {
      zap(curProc->pid);
      temp = curProc;
      curProc = curProc->next;
      temp->next = NULL;
   }
} /*zapWaiting*/

/*Blocks all procs that are blocked on the mailbox in the given slot*/
void unblockBlocked(int mBoxTableSlot)
{
   mbox_proc_ptr curProc = MailBoxTable[mBoxTableSlot].blockedProc;
   mbox_proc_ptr temp;

   disableInterrupts();
   while (curProc != NULL)
   {
      unblock_proc(curProc->pid);
      temp = curProc;
      curProc = curProc->next;
      temp->next = NULL;
   }
   enableInterrupts();
} /*unblockBlocked*/
