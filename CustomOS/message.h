#define DEBUG2 1

typedef struct mail_slot *slot_ptr;
typedef struct mail_slot mail_slot;
typedef struct mailbox mail_box;
typedef struct mbox_proc mbox_proc;
typedef struct mbox_proc *mbox_proc_ptr;

struct mailbox
{
   int mbox_id;

   int num_slots;
   int slot_size;
   int unused_slots;
   int numWaiting;
   int numBlocked;
   int isReleased;
   slot_ptr first_slot;       // First slot of the mailbox, head of a linked list
   mbox_proc_ptr waitingProc; // a process that is waiting to recieve a message
   mbox_proc_ptr blockedProc;
};

struct mail_slot
{
   int mbox_id;
   int isOccupied; // whether or not the slot is occupied
   /* other items as needed... */
   int index; // The index of this slot within the table
   int messageSize;
   char message[MAX_MESSAGE];
   slot_ptr next_in_box;
   slot_ptr prev_in_box;
   mbox_proc_ptr associatedProcs;
};

struct mbox_proc
{
   int pid;
   int status; // 0 for Ready 1 for Waiting 2 for Blocked
   int index;  // Slot within the proc table
   mbox_proc_ptr next;
   mbox_proc_ptr prev;
};

struct psr_bits
{
   unsigned int cur_mode : 1;
   unsigned int cur_int_enable : 1;
   unsigned int prev_mode : 1;
   unsigned int prev_int_enable : 1;
   unsigned int unused : 28;
};

union psr_values
{
   struct psr_bits bits;
   unsigned int integer_part;
};
