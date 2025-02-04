#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <usloss.h>
#include <usyscall.h>
#include <libuser.h>
#include "driver.h"

static int running;  /*semaphore to synchronize drivers and start3*/
int trackNumber = 0; // Sector where the disk head is currently pointing

const int DEBUG4 = 0;
const int debugflag4 = 1;

/* DATA STRUCTURES*/
static struct driver_proc Driver_Table[MAXPROC];
static sleepQueue sleepingProcs;
static struct diskQueue diskRequests[DISK_UNITS];

static int diskpids[DISK_UNITS];
static int num_tracks[DISK_UNITS];
static int diskSemaphores[DISK_UNITS];

/* PROTOTYPES */
static int ClockDriver(char *);
static int DiskDriver(char *);
void sleep_sys(sysargs *pArgs);
void disk_size_sys(sysargs *pArgs);
void disk_write_sys(sysargs *pArgs);
void disk_read_sys(sysargs *pArgs);
void addToSleepQueue(int);
int removeFromSleepQueue(void);
void addToDiskQueue(int, int);
void removeFromDiskQueue(int);
void handleDiskRead(int, int);
void handleDiskWrite(int, int);

int start3(char *arg)
{
    char name[128];
    char termbuf[10];
    int i;
    int clockPID;
    int pid;
    int status;

    if ((PSR_CURRENT_MODE & psr_get()) == 0)
    {
        console("Kernel Error: Not in kernel mode.\n");
        halt(1);
    }

    /* Assignment system call handlers */
    sys_vec[SYS_SLEEP] = sleep_sys;
    sys_vec[SYS_DISKSIZE] = disk_size_sys;
    sys_vec[SYS_DISKREAD] = disk_read_sys;
    sys_vec[SYS_DISKWRITE] = disk_write_sys;

    memset(Driver_Table, 0, MAXPROC * sizeof(Driver_Table[0]));
    sleepingProcs.hasProc = 0;
    diskRequests[0].hasProc = 0;
    diskRequests[1].hasProc = 0;

    for (int i = 0; i < MAXPROC; i++)
    {
        Driver_Table[i].semHandle = semcreate_real(0); // initialize private sems
    }

    for (int j = 0; j < DISK_UNITS; j++)
    {
        diskSemaphores[j] = semcreate_real(0);
    }

    running = semcreate_real(0);
    clockPID = fork1("Clock driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    if (clockPID < 0)
    {
        console("start3(): Can't create clock driver\n");
        halt(1);
    }
    /*
     * Wait for the clock driver to start. The idea is that ClockDriver
     * will V the semaphore "running" once it is running.
     */

    semp_real(running);

    for (i = 0; i < DISK_UNITS; i++)
    {
        char buf[32];
        sprintf(buf, "%d", i);
        sprintf(name, "DiskDriver%d", i);
        diskpids[i] = fork1(name, DiskDriver, buf, USLOSS_MIN_STACK, 2);
        if (diskpids[i] < 0)
        {
            console("start3(): Can't create disk driver %d\n", i);
            halt(1);
        }
    }
    semp_real(running);
    semp_real(running);

    /*
     * Create first user-level process and wait for it to finish.
     */
    pid = spawn_real("start4", start4, NULL, 8 * USLOSS_MIN_STACK, 3);
    pid = wait_real(&status);

    /*
     * Zap the device drivers
     */
    zap(clockPID); // clock driver
    join(&status); /* for the Clock Driver */

    for (int j = 0; j < DISK_UNITS; j++)
    {
        semv_real(diskSemaphores[j]); // this will break the diskdriver loop if nothing is queued for it
        join(&status);
    }

    return 0;
}

/*
 * ClockDriver Proc. Functions as the driver for the clock.
 * Waits for a clock interrupt and handles it accordingly.
 */
static int
ClockDriver(char *arg)
{
    int result;
    int status;

    /*
     * Let the parent know we are running and enable interrupts.
     */
    semv_real(running);
    psr_set(psr_get() | PSR_CURRENT_INT);
    int curTime;
    while (!is_zapped())
    {
        result = waitdevice(CLOCK_DEV, 0, &status);
        if (result != 0)
        {
            return 0;
        }
        /*
         * Compute the current time and wake up any processes
         * whose time has come.
         */
        gettimeofday_real(&curTime);
        int removedSlot;
        int privateSem;
        driver_proc_ptr procInQueue = sleepingProcs.head;

        while (procInQueue != NULL && curTime >= procInQueue->wake_time)
        {
            removedSlot = removeFromSleepQueue();
            if (removedSlot != -1) // this would mean there was nothing to remove
            {
                privateSem = Driver_Table[removedSlot].semHandle;
                semv_real(privateSem); // wake the proc up
            }
            procInQueue = sleepingProcs.head; // head should be changed after call to removeFromSleepQueue
        }
    }

    return 0;
}

/*
 * The driver process for the disk. Waits for disk interrupt on one of the units
 * and handles it accordingly.
 */
static int
DiskDriver(char *arg)
{
    int unit = atoi(arg);
    device_request my_request;
    int trackCount;
    int result;
    int waitResult;
    int status;
    int op;
    int slot;

    driver_proc_ptr current_req;

    if (DEBUG4 && debugflag4)
        console("DiskDriver(%d): started\n", unit);

    /* Get the number of tracks for this disk */
    my_request.opr = DISK_TRACKS;

    my_request.reg1 = &trackCount;

    result = device_output(DISK_DEV, unit, &my_request);

    if (result != DEV_OK)
    {
        console("DiskDriver %d: did not get DEV_OK on DISK_TRACKS call\n", unit);
        console("DiskDriver %d: is the file disk%d present???\n", unit, unit);
        halt(1);
    }

    waitResult = waitdevice(DISK_DEV, unit, &status);
    num_tracks[unit] = trackCount;

    /* initialize disk arm position to 0*/
    my_request.opr = DISK_SEEK;
    my_request.reg1 = (void *)trackNumber;
    device_output(DISK_DEV, unit, &my_request);
    waitdevice(DISK_DEV, unit, &status);

    if (DEBUG4 && debugflag4)
    {
        console("DiskDriver(%d): tracks = %d\n", unit, num_tracks[unit]);
    }

    semv_real(running);

    while (!(is_zapped()))
    {
        // wait for a request
        semp_real(diskSemaphores[unit]);

        if (diskRequests[unit].hasProc) // make sure there is a disk request in the list
        {
            slot = diskRequests[unit].head->slot;
            op = diskRequests[unit].head->operation;
            // Check if we are on the proper track, if not, seek to it
            if (Driver_Table[slot].track_start != trackNumber)
            {
                // Seek to the proper track
                my_request.opr = DISK_SEEK;
                my_request.reg1 = (void *)Driver_Table[slot].track_start;
                device_output(DISK_DEV, unit, &my_request);
                waitResult = waitdevice(DISK_DEV, unit, &status);

                if (waitResult != DEV_OK)
                {
                    console("DiskDriver %d, did not get DEV_OK on DISKS_Tracks call \n", unit);
                    halt(1);
                }

                trackNumber = Driver_Table[slot].track_start; // update the global variable
            }

            if (op == DISK_READ)
            {
                handleDiskRead(slot, unit);
            }
            else if (op == DISK_WRITE)
            {
                handleDiskWrite(slot, unit);
            }
            else
            {
                console("This should not execute \n"); // Operation should only be a read or a write
            }
        }
        else
        {
            break; // If there is no process in the queue, then sem_v occured by start3
        }
    }

    return 0;
}

/*
 * This is the function pointed by the syscall vector
 * when the sleep sys call is used. It puts a proc to sleep
 * for the given amount of seconds by using P'ing a private
 * semaphore
 */
void sleep_sys(sysargs *pArgs)
{
    int secsToSleep = (int)pArgs->arg1;

    if (secsToSleep < 0)
    {
        pArgs->arg4 = -1;
        return;
    }

    int curTime;
    gettimeofday_real(&curTime);
    int procPid;
    getPID_real(&procPid);
    int slot = procPid % MAXPROC;
    Driver_Table[slot].pid = procPid;
    Driver_Table[slot].slot = slot;

    Driver_Table[slot].time_asleep = curTime;
    Driver_Table[slot].wake_time = curTime + (secsToSleep * 1000000); // convert secs to Sleep to micro seconds
    addToSleepQueue(slot);
    int semNum = Driver_Table[slot].semHandle; // private sem for wakeup
    semp_real(semNum);                         // P the semaphore, this causes the sleep

    pArgs->arg4 = 0;
}

/*
 * System call pointed to by the sys call vector for the
 * DiskSize syscall. It returns size related information
 * about the disk
 */
void disk_size_sys(sysargs *pArgs)
{
    int unit;
    int sectorSize;
    int sectorsPerTrack;
    int trackCount;

    unit = (int)pArgs->arg1;
    if (!(unit == 0 || unit == 1))
    {
        console("Illegal value given as unit. \n");
        pArgs->arg4 = -1;
        return;
    }

    sectorSize = DISK_SECTOR_SIZE;
    sectorsPerTrack = DISK_TRACK_SIZE;
    trackCount = num_tracks[unit];

    pArgs->arg1 = sectorSize;
    pArgs->arg2 = sectorsPerTrack;
    pArgs->arg3 = trackCount;

    pArgs->arg4 = 0;
}

/*
 * Function pointed to by the syscall vector syscall
 * for DiskRead. It causes a read from the disk with the input
 * parameters.
 */
void disk_read_sys(sysargs *pArgs)
{
    void *buffer = pArgs->arg1;
    int sectorsToRead = (int)pArgs->arg2;
    int startTrack = (int)pArgs->arg3;
    int startSector = (int)pArgs->arg4;
    int unit = (int)pArgs->arg5;
    int procSlot;
    int pid;

    if (sectorsToRead < 0 || startTrack < 0 || startSector < 0)
    {
        pArgs->arg4 = -1;
        return;
    }

    if (!(unit == 0 || unit == 1))
    {
        pArgs->arg4 = -1;
        return;
    }
    getPID_real(&pid);

    // fill in the fields
    procSlot = pid % MAXPROC;
    Driver_Table[procSlot].slot = procSlot;
    Driver_Table[procSlot].operation = DISK_READ;
    Driver_Table[procSlot].num_sectors = sectorsToRead;
    Driver_Table[procSlot].disk_buf = buffer;
    Driver_Table[procSlot].track_start = startTrack;
    Driver_Table[procSlot].sector_start = startSector;
    Driver_Table[procSlot].unit = unit;
    Driver_Table[procSlot].current_track = startTrack;
    Driver_Table[procSlot].current_sector = startSector;

    pArgs->arg4 = 0;
    addToDiskQueue(procSlot, unit);
    semv_real(diskSemaphores[unit]);             // wake up the disk driver
    semp_real(Driver_Table[procSlot].semHandle); // block the calling proc till this is handled
    pArgs->arg1 = 0;
}

/*
 * Function pointed to by the syscall vector syscall
 * for DiskWrite. It causes a write from the disk with the input
 * parameters.
 */
void disk_write_sys(sysargs *pArgs)
{
    void *buffer = pArgs->arg1;

    int sectorsToWrite = (int)pArgs->arg2;
    int startTrack = (int)pArgs->arg3;
    int startSector = (int)pArgs->arg4;
    int unit = (int)pArgs->arg5;
    int procSlot;
    int pid;

    if (sectorsToWrite < 0 || startTrack < 0 || startSector < 0 ||
        startTrack > 16 || startSector > 16)
    {
        pArgs->arg4 = -1;
        return;
    }

    if (!(unit == 0 || unit == 1))
    {
        pArgs->arg4 = -1;
        return;
    }
    getPID_real(&pid);

    // fill in the fields
    procSlot = pid % MAXPROC;
    Driver_Table[procSlot].slot = procSlot;
    Driver_Table[procSlot].operation = DISK_WRITE;
    Driver_Table[procSlot].num_sectors = sectorsToWrite;
    Driver_Table[procSlot].disk_buf = pArgs->arg1;
    Driver_Table[procSlot].track_start = startTrack;
    Driver_Table[procSlot].sector_start = startSector;
    Driver_Table[procSlot].unit = unit;
    Driver_Table[procSlot].current_track = startTrack;
    Driver_Table[procSlot].current_sector = startSector;

    pArgs->arg4 = 0;
    addToDiskQueue(procSlot, unit);
    semv_real(diskSemaphores[unit]);             // wake up the disk driver
    semp_real(Driver_Table[procSlot].semHandle); // block the calling proc till this is handled
    pArgs->arg1 = 0;
}

/*
 * This is a helper function to help the disk driver to handle
 * a disk read operation. The argument is the slot in which the
 * calling process occupies within the Driver_Table table and the disk unit.
 */
void handleDiskRead(int slot, int unit)
{
    device_request dev_req;
    int status;

    // read specified number of sectors
    for (int i = 0; i < Driver_Table[slot].num_sectors; i++)
    {
        dev_req.opr = DISK_READ;
        dev_req.reg1 = Driver_Table[slot].current_sector;
        dev_req.reg2 = Driver_Table[slot].disk_buf;
        device_output(DISK_DEV, unit, &dev_req);
        waitdevice(DISK_DEV, unit, &status);

        if (Driver_Table[slot].num_sectors > 1)
        {
            // advance current sector
            Driver_Table[slot].current_sector++;

            // wrap around if needed
            if (Driver_Table[slot].current_sector > DISK_TRACK_SIZE)
            {
                trackNumber = (trackNumber + 1) % num_tracks[unit];
                Driver_Table[slot].current_sector = 0;
                Driver_Table[slot].current_track = trackNumber;

                // Seek to the proper track
                dev_req.opr = DISK_SEEK;
                dev_req.reg1 = (void *)trackNumber;
                device_output(DISK_DEV, unit, &dev_req);
                waitdevice(DISK_DEV, unit, &status);
            }
            // Since this is a pointer we advance it by sector size if needed
            Driver_Table[slot].disk_buf = Driver_Table[slot].disk_buf + DISK_SECTOR_SIZE;
        }
    }

    removeFromDiskQueue(unit);               // The request can now be removed
    semv_real(Driver_Table[slot].semHandle); // Wake up the calling proc now that this has been handled
}

/*
 * This is a helper function to help the disk driver to handle
 * a disk write operation. The argument is the slot in which the
 * calling process occupies within the Driver_Table table and the disk unit.
 */
void handleDiskWrite(int slot, int unit)
{

    device_request dev_req;
    int status;
    int waitResult;

    // write to specified number of sectors
    for (int i = 0; i < Driver_Table[slot].num_sectors; i++)
    {
        dev_req.opr = DISK_WRITE;
        dev_req.reg1 = Driver_Table[slot].current_sector;
        dev_req.reg2 = Driver_Table[slot].disk_buf;

        device_output(DISK_DEV, unit, &dev_req);
        waitdevice(DISK_DEV, unit, &status);

        if (Driver_Table[slot].num_sectors > 1)
        {
            // advance current sector
            Driver_Table[slot].current_sector++;

            // move tracks if sector boundary is crossed
            if (Driver_Table[slot].current_sector > DISK_TRACK_SIZE)
            {
                trackNumber = (trackNumber + 1) % num_tracks[unit];
                Driver_Table[slot].current_sector = 0;

                // Seek to the proper track
                dev_req.opr = DISK_SEEK;
                dev_req.reg1 = (void *)trackNumber;
                device_output(DISK_DEV, unit, &dev_req);
                waitdevice(DISK_DEV, unit, &status);
            }
            // Since this is a pointer we advance it by sector size if needed
            Driver_Table[slot].disk_buf = Driver_Table[slot].disk_buf + DISK_SECTOR_SIZE;
        }
    }
    removeFromDiskQueue(unit);               // The request can now be removed
    semv_real(Driver_Table[slot].semHandle); // Wake up the calling proc now that this has been handled
}

/*
 * Adds a process to the sleep queue. The argument should be the slot
 * that the proc occupies in the Driver_Table array.
 * Nothing is returned
 */
void addToSleepQueue(int slot)
{
    /* If this is the first proc being added to the queue, make it the head*/
    if (sleepingProcs.hasProc == 0)
    {
        sleepingProcs.head = &Driver_Table[slot];
        sleepingProcs.hasProc = 1;
    }
    else
    {
        /* Otherwise, insert it into queue, but maintain sorted order (lowest to highest)*/
        int wakeTime = Driver_Table[slot].wake_time;
        driver_proc_ptr cur = sleepingProcs.head;

        // Adding to the front of the queue
        if (wakeTime < cur->wake_time)
        {
            Driver_Table[slot].nextAsleep = cur;
            sleepingProcs.head = &Driver_Table[slot];
        }
        else // Adding anywhere but the front
        {
            while (wakeTime > cur->wake_time)
            {
                // This will cause adding to the end of the queue
                if (cur->nextAsleep == NULL)
                {
                    break;
                }

                /* If the current proc has a lower wait time time than the
                 * proc to be inserted, but the next proc has a higher wait time
                 * than the proc to be inserted, we are in the right place.
                 * This is where we break to add this slot in the queue
                 */
                if (wakeTime < cur->nextAsleep->wake_time)
                {
                    break;
                }

                // advance if none of the above is true
                cur = cur->nextAsleep;
            }

            driver_proc_ptr oldNext = cur->nextAsleep;
            cur->nextAsleep = &Driver_Table[slot]; // insert the proc in the proper place
            cur->nextAsleep->nextAsleep = oldNext;
        }
    }
}

/*
 * Removes a sleeping proc from the sleep queue/
 * Returns the slot of the proc that was removed from the queue.
 */
int removeFromSleepQueue()
{
    int removedSlot = 0;
    driver_proc_ptr cur = sleepingProcs.head;
    if (cur == NULL)
    {
        return -1; // Nothing to remove
    }
    else if (cur->nextAsleep == NULL) // removing from a queue of length 1
    {
        removedSlot = sleepingProcs.head->slot;
        sleepingProcs.head = NULL;
        sleepingProcs.hasProc = 0;
        return removedSlot;
    }
    else // Removing from a queue with length greater than 1
    {
        driver_proc_ptr oldHead = cur;
        sleepingProcs.head = oldHead->nextAsleep;
        oldHead->nextAsleep = NULL;
        removedSlot = oldHead->slot;
    }

    return removedSlot;
}

/*
 * Adds a process to the disk queue according to track number.
 * This is a queue that is sorted lowest to highest based on track number.
 * The first argument is the slot in which the calling proc resides.
 * The second argument is the disk unit.
 */
void addToDiskQueue(int slot, int unit)
{
    /* If this is the first proc being added to the queue, make it the head*/
    if (diskRequests[unit].hasProc == 0)
    {
        diskRequests[unit].head = &Driver_Table[slot];
        diskRequests[unit].hasProc = 1;
    }
    else
    {
        /* Otherwise, insert it into queue, but maintain sorted order (lowest to highest track)*/
        int track = Driver_Table[slot].track_start;
        driver_proc_ptr cur = diskRequests[unit].head;

        // Adding to the front of the queue
        if (track < cur->track_start)
        {
            Driver_Table[slot].nextDiskReq = cur;
            diskRequests[unit].head = &Driver_Table[slot];
        }
        else // Adding anywhere but the front
        {
            while (track > cur->track_start)
            {
                // This will cause adding to the end of the queue
                if (cur->nextDiskReq == NULL)
                {
                    break;
                }

                /* If the current proc has a lower start track than the
                 * proc to be inserted, but the next proc has a higher start track
                 * than the proc to be inserted, we are in the right place.
                 * This is where we break to add this slot in the queue
                 */
                if (track < cur->nextAsleep->track_start)
                {
                    break;
                }

                // advance if none of the above is true
                cur = cur->nextDiskReq;
            }

            driver_proc_ptr oldNext = cur->nextDiskReq;
            cur->nextDiskReq = &Driver_Table[slot]; // insert the proc in the proper place
            cur->nextDiskReq->nextDiskReq = oldNext;
        }
    }
}

/*
 * This causes the head of the disk queue to be removed.
 * The argument is the disk unit.
 */
void removeFromDiskQueue(int unit)
{
    driver_proc_ptr cur = diskRequests[unit].head;
    if (cur == NULL)
    {
        return; // Nothing to remove
    }
    else if (cur->nextDiskReq == NULL) // removing from a queue of length 1
    {
        diskRequests[unit].head = NULL;
        diskRequests[unit].hasProc = 0;
    }
    else // Removing from a queue with length greater than 1
    {
        driver_proc_ptr oldHead = cur;
        diskRequests[unit].head = oldHead->nextDiskReq;
        oldHead->nextDiskReq = NULL;
    }
}
