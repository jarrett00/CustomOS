#define DEBUG 0

typedef struct proc_struct proc_struct;

typedef struct proc_struct *proc_ptr;

typedef struct procLinkedList
{
   int hasProc;
   proc_ptr head;
   proc_ptr tail;
} procLinkedList;

struct proc_struct
{
   proc_ptr next_proc_ptr;
   proc_ptr child_proc_ptr;
   proc_ptr next_sibling_ptr;

   // points to next proc in list whether it is the ready list or blocked list
   proc_ptr next_in_list;

   char name[MAXNAME];     /* process's name */
   char start_arg[MAXARG]; /* args passed to process */
   context state;          /* current context for process */
   int pid;                /* process id */
   int priority;
   int (*start_func)(char *); /* function where process begins -- launch */
   void *stack;
   unsigned int stacksize;
   int status; /* READY = 1 JOIN BLOCKED = 2 ZAPPED = 3 QUIT = 4 BLOCKED = 11 AND UP */
   /* other fields as needed... */
   int parent_pid;
   int num_children;
   int cur_start_time;
   long total_cpu_time;
   int status_to_parent;
   int slot; // the slot in the ProcTable
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

#define NO_CURRENT_PROCESS NULL
#define MINPRIORITY 5
#define MAXPRIORITY 1
#define SENTINELPID 1
#define SENTINELPRIORITY LOWEST_PRIORITY
