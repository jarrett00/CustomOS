

typedef struct driver_proc *driver_proc_ptr;

struct driver_proc
{
   driver_proc_ptr nextDiskReq;
   driver_proc_ptr nextAsleep;

   int pid;
   int slot;
   int wake_time; /* for sleep syscall */
   int been_zapped;
   int semHandle;
   int time_asleep; /* time when the proc started sleeping*/

   /* Used for disk requests */
   int operation; /* DISK_READ, DISK_WRITE, DISK_SEEK, DISK_TRACKS */
   int track_start;
   int sector_start;
   int num_sectors; // num sectors to read or write
   int sectors_read;
   int current_track;
   int current_sector;
   int unit;
   void *disk_buf;
};

typedef struct sleepQueue
{
   int hasProc;
   driver_proc_ptr head;
} sleepQueue;

typedef struct diskQueue
{
   int hasProc;
   driver_proc_ptr head;

} diskQueue;