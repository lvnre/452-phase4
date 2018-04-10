#include <usloss.h>
#include <usyscall.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <providedPrototypes.h>
#include <p4structs.h>
#include <stdlib.h> /* needed for atoi() */
#include <stdio.h>
#include <string.h> /* needed for memcpy() */

#define ABS(a,b) (a-b > 0 ? a-b : -(a-b))

int debug4 = 0;
int running;

static int ClockDriver(char *);
static int DiskDriver(char *);
static int TermDriver(char *);
static int TermReader(char *);
static int TermWriter(char *);
extern int start4();

void sleep(USLOSS_Sysargs *);
void diskRead(USLOSS_Sysargs *);
void diskWrite(USLOSS_Sysargs *);
void diskSize(USLOSS_Sysargs *);
void termRead(USLOSS_Sysargs *);
void termWrite(USLOSS_Sysargs *);

int sleepReal(int);
int diskSizeReal(int, int*, int*, int*);
int diskWriteReal(int, int, int, int, void *);
int diskReadReal(int, int, int, int, void *);
int diskReadOrWriteReal(int, int, int, int, void *, int);
int termReadReal(int, int, char *);
int termWriteReal(int, int, char *);

void requireKernelMode(char *);
void emptyProc(int);
void initProc(int);
void setUserMode();
void initDiskQueue(diskQueue*);
void addDiskQ(diskQueue*, procPtr);
procPtr peekDiskQ(diskQueue*);
procPtr removeDiskQ(diskQueue*);
void initHeap(heap *);
void heapAdd(heap *, procPtr);
procPtr heapPeek(heap *);
procPtr heapRemove(heap *);

/* Globals */
procStruct ProcTable[MAXPROC];
heap sleepHeap;
int diskZapped; // indicates if the disk drivers are 'zapped' or not
diskQueue diskQs[USLOSS_DISK_UNITS]; // queues for disk drivers
int diskPids[USLOSS_DISK_UNITS]; // pids of the disk drivers

// mailboxes for terminal device
int charRecvMbox[USLOSS_TERM_UNITS]; // receive char
int charSendMbox[USLOSS_TERM_UNITS]; // send char
int lineReadMbox[USLOSS_TERM_UNITS]; // read line
int lineWriteMbox[USLOSS_TERM_UNITS]; // write line
int pidMbox[USLOSS_TERM_UNITS]; // pid to block
int termInt[USLOSS_TERM_UNITS]; // interupt for term (control writing)

int termProcTable[USLOSS_TERM_UNITS][3]; // keep track of term procs


void
start3(void)
{
    char	name[128];
    char    termbuf[10];
    char    diskbuf[10];
    int		i;
    int		clockPID;
    int		pid;
    int		status;

    /*
     * Check kernel mode here.
     */
    requireKernelMode("start3");

    // initialize proc table
    for (i = 0; i < MAXPROC; i++) {
        //emptyProc(i); Idk why this was here instead of initProc
        initProc(i);
    }

    // sleep queue
    initHeap(&sleepHeap);

    // initialize systemCallVec
    systemCallVec[SYS_SLEEP] = sleep;
    systemCallVec[SYS_DISKREAD] = diskRead;
    systemCallVec[SYS_DISKWRITE] = diskWrite;
    systemCallVec[SYS_DISKSIZE] = diskSize;
    systemCallVec[SYS_TERMREAD] = termRead;
    systemCallVec[SYS_TERMWRITE] = termWrite;

    // mboxes for terminal
    for (i = 0; i < USLOSS_TERM_UNITS; i++) {
        charRecvMbox[i] = MboxCreate(1, MAXLINE);
        charSendMbox[i] = MboxCreate(1, MAXLINE);
        lineReadMbox[i] = MboxCreate(10, MAXLINE);
        lineWriteMbox[i] = MboxCreate(10, MAXLINE); 
        pidMbox[i] = MboxCreate(1, sizeof(int));
    }

    /*
     * Create clock device driver 
     * I am assuming a semaphore here for coordination.  A mailbox can
     * be used instead -- your choice.
     */
    running = semcreateReal(0);
    clockPID = fork1("Clock driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    if (clockPID < 0) {
    	USLOSS_Console("start3(): Can't create clock driver\n");
    	USLOSS_Halt(1);
    }
    /*
     * Wait for the clock driver to start. The idea is that ClockDriver
     * will V the semaphore "running" once it is running.
     */

    sempReal(running);

    /*
     * Create the disk device drivers here.  You may need to increase
     * the stack size depending on the complexity of your
     * driver, and perhaps do something with the pid returned.
     */
    int temp;
    for (i = 0; i < USLOSS_DISK_UNITS; i++) {
        sprintf(diskbuf, "%d", i);
        pid = fork1("Disk driver", DiskDriver, diskbuf, USLOSS_MIN_STACK, 2);
        if (pid < 0) {
            USLOSS_Console("start3(): Can't create disk driver %d\n", i);
            USLOSS_Halt(1);
        }

        diskPids[i] = pid;
        sempReal(running); // wait for driver to start running

        // get number of tracks
        diskSizeReal(i, &temp, &temp, &ProcTable[pid % MAXPROC].diskTrack);
    }


    /*
     * Create terminal device drivers.
     */

     for (i = 0; i < USLOSS_TERM_UNITS; i++) {
        sprintf(termbuf, "%d", i); 
        termProcTable[i][0] = fork1(name, TermDriver, termbuf, USLOSS_MIN_STACK, 2);
        termProcTable[i][1] = fork1(name, TermReader, termbuf, USLOSS_MIN_STACK, 2);
        termProcTable[i][2] = fork1(name, TermWriter, termbuf, USLOSS_MIN_STACK, 2);
        sempReal(running);
        sempReal(running);
        sempReal(running);
     }


    /*
     * Create first user-level process and wait for it to finish.
     * These are lower-case because they are not system calls;
     * system calls cannot be invoked from kernel mode.
     * I'm assuming kernel-mode versions of the system calls
     * with lower-case first letters, as shown in provided_prototypes.h
     */
    pid = spawnReal("start4", start4, NULL, 4 * USLOSS_MIN_STACK, 3);
    pid = waitReal(&status);

    /*
     * Zap the device drivers
     */

    status = 0;

     // zap clock driver
    zap(clockPID); 
    join(&status);

    // zap disk drivers
    for (i = 0; i < USLOSS_DISK_UNITS; i++) {
        semvReal(ProcTable[diskPids[i]].blockSem); 
        zap(diskPids[i]);
        join(&status);
    }

    // zap termreader
    for (i = 0; i < USLOSS_TERM_UNITS; i++) {
        MboxSend(charRecvMbox[i], NULL, 0);
        zap(termProcTable[i][1]);
        join(&status);
    }

    // zap termwriter
    for (i = 0; i < USLOSS_TERM_UNITS; i++) {
        MboxSend(lineWriteMbox[i], NULL, 0);
        zap(termProcTable[i][2]);
        join(&status);
    }

    // zap termdriver, etc
    char filename[50];
    for(i = 0; i < USLOSS_TERM_UNITS; i++)
    {
        int ctrl = 0;
        ctrl = USLOSS_TERM_CTRL_RECV_INT(ctrl);
        USLOSS_DeviceOutput(USLOSS_TERM_DEV, i, (void *)((long) ctrl));

        // file stuff
        sprintf(filename, "term%d.in", i);
        FILE *f = fopen(filename, "a+");
        fprintf(f, "last line\n");
        fflush(f);
        fclose(f);

        // actual termdriver zap
        zap(termProcTable[i][0]);
        join(&status);
    }

    // eventually, at the end:
    quit(0);
    
}

/* Clock Driver */
static int
ClockDriver(char *arg)
{
    int result;
    int status;

    // Let the parent know we are running and enable interrupts.
    semvReal(running);
    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);

    // Infinite loop until we are zap'd
    while(! isZapped()) {
	    result = waitDevice(USLOSS_CLOCK_DEV, 0, &status);
	    if (result != 0) {
	        return 0;
	    }

	    // Compute the current time and wake up any processes whose time has come.
        procPtr proc;
        while (sleepHeap.size > 0 && USLOSS_Clock() >= heapPeek(&sleepHeap)->wakeTime) {
            proc = heapRemove(&sleepHeap);
            if (debug4) 
                USLOSS_Console("ClockDriver: Waking up process %d\n", proc->pid);
            semvReal(proc->blockSem); 
        }
    }
    return 0;
}

/* Disk Driver */
static int
DiskDriver(char *arg)
{
    int result;
    int status;
    int unit = atoi( (char *) arg);     // Unit is passed as arg.

    // get set up in proc table
    initProc(getpid());
    procPtr me = &ProcTable[getpid() % MAXPROC];
    initDiskQueue(&diskQs[unit]);

    if (debug4) {
        USLOSS_Console("DiskDriver: unit %d started, pid = %d\n", unit, me->pid);
    }

    // Let the parent know we are running and enable interrupts.
    semvReal(running);
    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);

    // Infinite loop until we are zap'd
    while(!isZapped()) {
        // block on sem until we get request
        sempReal(me->blockSem);
        if (debug4) {
            USLOSS_Console("DiskDriver: unit %d unblocked, zapped = %d, queue size = %d\n", unit, isZapped(), diskQs[unit].size);
        }
        if (isZapped()) // check  if we were zapped
            return 0;

        // get request off queue
        if (diskQs[unit].size > 0) {
            procPtr proc = peekDiskQ(&diskQs[unit]);
            int track = proc->diskTrack;

            if (debug4) {
                USLOSS_Console("DiskDriver: taking request from pid %d, track %d\n", proc->pid, proc->diskTrack);
            }

            // handle tracks request
            if (proc->diskRequest.opr == USLOSS_DISK_TRACKS) {
                USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &proc->diskRequest);
                result = waitDevice(USLOSS_DISK_DEV, unit, &status);
                if (result != 0) {
                    return 0;
                }
            }

            else { // handle read/write requests
                while (proc->diskSectors > 0) {
                    // seek to needed track
                    USLOSS_DeviceRequest request;
                    request.opr = USLOSS_DISK_SEEK;
                    request.reg1 = &track;
                    USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &request);
                    // wait for result
                    result = waitDevice(USLOSS_DISK_DEV, unit, &status);
                    if (result != 0) {
                        return 0;
                    }

                    if (debug4) {
                        USLOSS_Console("DiskDriver: seeked to track %d, status = %d, result = %d\n", track, status, result);
                    }

                    // read/write the sectors
                    int s;
                    for (s = proc->diskFirstSec; proc->diskSectors > 0 && s < USLOSS_DISK_TRACK_SIZE; s++) {
                        proc->diskRequest.reg1 = (void *) ((long) s);
                        USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &proc->diskRequest);
                        result = waitDevice(USLOSS_DISK_DEV, unit, &status);
                        if (result != 0) {
                            return 0;
                        }

                        if (debug4) {
                            USLOSS_Console("DiskDriver: read/wrote sector %d, status = %d, result = %d, buffer = %s\n", s, status, result, proc->diskRequest.reg2);
                        }

                        proc->diskSectors--;
                        proc->diskRequest.reg2 += USLOSS_DISK_SECTOR_SIZE;
                    }

                    // request first sector of next track
                    track++;
                    proc->diskFirstSec = 0;
                }
            }

            if (debug4) 
                USLOSS_Console("DiskDriver: finished request from pid %d\n", proc->pid, result, status);

            removeDiskQ(&diskQs[unit]); // remove proc from queue
            semvReal(proc->blockSem); // unblock caller
        }

    }

    semvReal(running); // unblock parent
    return 0;
}

/* Terminal Driver */
static int
TermDriver(char *arg)
{
    int result;
    int status;
    int unit = atoi( (char *) arg);     // Unit is passed as arg.

    semvReal(running);
    if (debug4) 
        USLOSS_Console("TermDriver (unit %d): running\n", unit);

    while (!isZapped()) {

        result = waitDevice(USLOSS_TERM_INT, unit, &status);
        if (result != 0) {
            return 0;
        }

        // Try to receive character
        int recv = USLOSS_TERM_STAT_RECV(status);
        if (recv == USLOSS_DEV_BUSY) {
            MboxCondSend(charRecvMbox[unit], &status, sizeof(int));
        }
        else if (recv == USLOSS_DEV_ERROR) {
            if (debug4) 
                USLOSS_Console("TermDriver RECV ERROR\n");
        }

        // Try to send character
        int xmit = USLOSS_TERM_STAT_XMIT(status);
        if (xmit == USLOSS_DEV_READY) {
            MboxCondSend(charSendMbox[unit], &status, sizeof(int));
        }
        else if (xmit == USLOSS_DEV_ERROR) {
            if (debug4) 
                USLOSS_Console("TermDriver XMIT ERROR\n");
        }
    }

    return 0;
}

/* Terminal Reader */
static int 
TermReader(char * arg) 
{
    int unit = atoi( (char *) arg);     // Unit is passed as arg.
    int i;
    int receive; // char to receive
    char line[MAXLINE]; // line being created/read
    int next = 0; // index in line to write char

    for (i = 0; i < MAXLINE; i++) { 
        line[i] = '\0';
    }

    semvReal(running);
    while (!isZapped()) {
        // receieve characters
        MboxReceive(charRecvMbox[unit], &receive, sizeof(int));
        char ch = USLOSS_TERM_STAT_CHAR(receive);
        line[next] = ch;
        next++;

        // receive line
        if (ch == '\n' || next == MAXLINE) {
            if (debug4) 
                USLOSS_Console("TermReader (unit %d): line send\n", unit);

            line[next] = '\0'; // end with null
            MboxSend(lineReadMbox[unit], line, next);

            // reset line
            for (i = 0; i < MAXLINE; i++) {
                line[i] = '\0';
            } 
            next = 0;
        }


    }
    return 0;
}

/* Terminal Writer */
static int 
TermWriter(char * arg) 
{
    int unit = atoi( (char *) arg);     // Unit is passed as arg.
    int size;
    int ctrl = 0;
    int next;
    int status;
    char line[MAXLINE];

    semvReal(running);
    if (debug4) 
        USLOSS_Console("TermWriter (unit %d): running\n", unit);

    while (!isZapped()) {
        size = MboxReceive(lineWriteMbox[unit], line, MAXLINE); // get line and size

        if (isZapped())
            break;

        // enable xmit interrupt and receive interrupt
        ctrl = USLOSS_TERM_CTRL_XMIT_INT(ctrl);
        USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void *) ((long) ctrl));

        // xmit the line
        next = 0;
        while (next < size) {
            MboxReceive(charSendMbox[unit], &status, sizeof(int));

            // xmit the character
            int x = USLOSS_TERM_STAT_XMIT(status);
            if (x == USLOSS_DEV_READY) {
                //USLOSS_Console("%c string %d unit\n", line[next], unit);

                ctrl = 0;
                //ctrl = USLOSS_TERM_CTRL_RECV_INT(ctrl);
                ctrl = USLOSS_TERM_CTRL_CHAR(ctrl, line[next]);
                ctrl = USLOSS_TERM_CTRL_XMIT_CHAR(ctrl);
                ctrl = USLOSS_TERM_CTRL_XMIT_INT(ctrl);

                USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void *) ((long) ctrl));
            }

            next++;
        }

        // enable receive interrupt
        ctrl = 0;
        if (termInt[unit] == 1) 
            ctrl = USLOSS_TERM_CTRL_RECV_INT(ctrl);
        USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void *) ((long) ctrl));
        termInt[unit] = 0;
        int pid; 
        MboxReceive(pidMbox[unit], &pid, sizeof(int));
        semvReal(ProcTable[pid % MAXPROC].blockSem);
        
        
    }

    return 0;
}

/* sleep function value extraction */
void sleep(USLOSS_Sysargs * args) {
    requireKernelMode("sleep");
    int seconds = (long) args->arg1;
    int retval = sleepReal(seconds);
    args->arg4 = (void *) ((long) retval);
    setUserMode();
}

/* real sleep function */
int sleepReal(int seconds) {
    requireKernelMode("sleepReal");

    if (debug4) 
        USLOSS_Console("sleepReal: called for process %d with %d seconds\n", getpid(), seconds);

    if (seconds < 0) {
        return -1;
    }

    // init/get the process
    if (ProcTable[getpid() % MAXPROC].pid == -1) {
        initProc(getpid());
    }
    procPtr proc = &ProcTable[getpid() % MAXPROC];
    
    // set wake time
    proc->wakeTime = USLOSS_Clock() + seconds*1000000;
    if (debug4) 
        USLOSS_Console("sleepReal: set wake time for process %d to %d, adding to heap...\n", proc->pid, proc->wakeTime);

    heapAdd(&sleepHeap, proc); // add to sleep heap
    if (debug4) 
        USLOSS_Console("sleepReal: Process %d going to sleep until %d\n", proc->pid, proc->wakeTime);
    sempReal(proc->blockSem); // block the process
    if (debug4) 
        USLOSS_Console("sleepReal: Process %d woke up, time is %d\n", proc->pid, USLOSS_Clock());
    return 0;
}

/* extract values from sysargs and call diskReadReal */
void diskRead(USLOSS_Sysargs * args) {
    requireKernelMode("diskRead");

    int sectors = (long) args->arg2;
    int track = (long) args->arg3;
    int first = (long) args->arg4;
    int unit = (long) args->arg5;

    int retval = diskReadReal(unit, track, first, sectors, args->arg1);

    if (retval == -1) {
        args->arg1 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) -1);
    } else {
        args->arg1 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) 0);
    }
    setUserMode();
}

/* extract values from sysargs and call diskWriteReal */
void diskWrite(USLOSS_Sysargs * args) {
    requireKernelMode("diskWrite");

    int sectors = (long) args->arg2;
    int track = (long) args->arg3;
    int first = (long) args->arg4;
    int unit = (long) args->arg5;

    int retval = diskWriteReal(unit, track, first, sectors, args->arg1);

    if (retval == -1) {
        args->arg1 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) -1);
    } else {
        args->arg1 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) 0);
    }
    setUserMode();
}

int diskWriteReal(int unit, int track, int first, int sectors, void *buffer) {
    requireKernelMode("diskWriteReal");
    return diskReadOrWriteReal(unit, track, first, sectors, buffer, 1);
}

int diskReadReal(int unit, int track, int first, int sectors, void *buffer) {
    requireKernelMode("diskWriteReal");
    return diskReadOrWriteReal(unit, track, first, sectors, buffer, 0);
}

/*------------------------------------------------------------------------
    diskReadOrWriteReal: Reads or writes to the desk depending on the 
                        value of write; write if write == 1, else read.
    Returns: -1 if given illegal input, 0 otherwise
 ------------------------------------------------------------------------*/
int diskReadOrWriteReal(int unit, int track, int first, int sectors, void *buffer, int write) {
    if (debug4)
        USLOSS_Console("diskReadOrWriteReal: called with unit: %d, track: %d, first: %d, sectors: %d, write: %d\n", unit, track, first, sectors, write);

    // check for illegal args
    if (unit < 0 || unit > 1 || track < 0 || track > ProcTable[diskPids[unit]].diskTrack ||
        first < 0 || first > USLOSS_DISK_TRACK_SIZE || buffer == NULL  ||
        (first + sectors)/USLOSS_DISK_TRACK_SIZE + track > ProcTable[diskPids[unit]].diskTrack) {
        return -1;
    }

    procPtr driver = &ProcTable[diskPids[unit]];

    // init/get the process
    if (ProcTable[getpid() % MAXPROC].pid == -1) {
        initProc(getpid());
    }
    procPtr proc = &ProcTable[getpid() % MAXPROC];

    if (write)
        proc->diskRequest.opr = USLOSS_DISK_WRITE;
    else
        proc->diskRequest.opr = USLOSS_DISK_READ;
    proc->diskRequest.reg2 = buffer;
    proc->diskTrack = track;
    proc->diskFirstSec = first;
    proc->diskSectors = sectors;
    proc->diskBuffer = buffer;

    addDiskQ(&diskQs[unit], proc); // add to disk queue 
    semvReal(driver->blockSem);  // wake up disk driver
    sempReal(proc->blockSem); // block

    int status;
    int result = USLOSS_DeviceInput(USLOSS_DISK_DEV, unit, &status);

    if (debug4)
        USLOSS_Console("diskReadOrWriteReal: finished, status = %d, result = %d\n", status, result);

    return result;
}

/* extract values from sysargs and call diskSizeReal */
void diskSize(USLOSS_Sysargs * args) {
    requireKernelMode("diskSize");
    int unit = (long) args->arg1;
    int sector, track, disk;
    int retval = diskSizeReal(unit, &sector, &track, &disk);
    args->arg1 = (void *) ((long) sector);
    args->arg2 = (void *) ((long) track);
    args->arg3 = (void *) ((long) disk);
    args->arg4 = (void *) ((long) retval);
    setUserMode();
}

/*------------------------------------------------------------------------
    diskSizeReal: Puts values into pointers for the size of a sector, 
    number of sectors per track, and number of tracks on the disk for the 
    given unit. 
    Returns: -1 if given illegal input, 0 otherwise
 ------------------------------------------------------------------------*/
int diskSizeReal(int unit, int *sector, int *track, int *disk) {
    requireKernelMode("diskSizeReal");

    // check for illegal args
    if (unit < 0 || unit > 1 || sector == NULL || track == NULL || disk == NULL) {
        if (debug4)
            USLOSS_Console("diskSizeReal: given illegal argument(s), returning -1\n");
        return -1;
    }

    procPtr driver = &ProcTable[diskPids[unit]];

    // get the number of tracks for the first time
    if (driver->diskTrack == -1) {
        // init/get the process
        if (ProcTable[getpid() % MAXPROC].pid == -1) {
            initProc(getpid());
        }
        procPtr proc = &ProcTable[getpid() % MAXPROC];

        // set variables
        proc->diskTrack = 0;
        USLOSS_DeviceRequest request;
        request.opr = USLOSS_DISK_TRACKS;
        request.reg1 = &driver->diskTrack;
        proc->diskRequest = request;

        addDiskQ(&diskQs[unit], proc); // add to disk queue 
        semvReal(driver->blockSem);  // wake up disk driver
        sempReal(proc->blockSem); // block

        if (debug4)
            USLOSS_Console("diskSizeReal: number of tracks on unit %d: %d\n", unit, driver->diskTrack);
    }

    *sector = USLOSS_DISK_SECTOR_SIZE;
    *track = USLOSS_DISK_TRACK_SIZE;
    *disk = driver->diskTrack;
    return 0;
}

void termRead(USLOSS_Sysargs * args) {
    if (debug4)
        USLOSS_Console("termRead\n");
    requireKernelMode("termRead");
    
    char *buffer = (char *) args->arg1;
    int size = (long) args->arg2;
    int unit = (long) args->arg3;

    long retval = termReadReal(unit, size, buffer);

    if (retval == -1) {
        args->arg2 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) -1);
    } else {
        args->arg2 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) 0);
    }
    setUserMode();
}

int termReadReal(int unit, int size, char *buffer) {
    if (debug4)
        USLOSS_Console("termReadReal\n");
    requireKernelMode("termReadReal");

    if (unit < 0 || unit > USLOSS_TERM_UNITS - 1 || size < 0) {
        return -1;
    }
    char line[MAXLINE];
    int ctrl = 0;

    //enable term interrupts
    if (termInt[unit] == 0) {
        if (debug4)
            USLOSS_Console("termReadReal enable interrupts\n");
        ctrl = USLOSS_TERM_CTRL_RECV_INT(ctrl);
        USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void *) ((long) ctrl));
        termInt[unit] = 1;
    }
    int retval = MboxReceive(lineReadMbox[unit], &line, MAXLINE);

    if (debug4) 
        USLOSS_Console("termReadReal (unit %d): size %d retval %d \n", unit, size, retval);

    if (retval > size) {
        retval = size;
    }
    memcpy(buffer, line, retval);

    return retval;
}

void termWrite(USLOSS_Sysargs * args) {
    if (debug4)
        USLOSS_Console("termWrite\n");
    requireKernelMode("termWrite");
    
    char *text = (char *) args->arg1;
    int size = (long) args->arg2;
    int unit = (long) args->arg3;

    long retval = termWriteReal(unit, size, text);

    if (retval == -1) {
        args->arg2 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) -1);
    } else {
        args->arg2 = (void *) ((long) retval);
        args->arg4 = (void *) ((long) 0);
    }
    setUserMode(); 
}

int termWriteReal(int unit, int size, char *text) {
    if (debug4)
        USLOSS_Console("termWriteReal\n");
    requireKernelMode("termWriteReal");

    if (unit < 0 || unit > USLOSS_TERM_UNITS - 1 || size < 0) {
        return -1;
    }

    int pid = getpid();
    MboxSend(pidMbox[unit], &pid, sizeof(int));

    MboxSend(lineWriteMbox[unit], text, size);
    sempReal(ProcTable[pid % MAXPROC].blockSem);
    return size;
}

/* ------------------------------------------------------------------------
   Name - requireKernelMode
   Purpose - Checks if we are in kernel mode and prints an error messages
              and halts USLOSS if not.
   Parameters - The name of the function calling it, for the error message.
   Side Effects - Prints and halts if we are not in kernel mode
   ------------------------------------------------------------------------ */
void requireKernelMode(char *name)
{
    if( (USLOSS_PSR_CURRENT_MODE & USLOSS_PsrGet()) == 0 ) {
        USLOSS_Console("%s: called while in user mode, by process %d. Halting...\n", 
             name, getpid());
        USLOSS_Halt(1); 
    }
} 

/* ------------------------------------------------------------------------
   Name - setUserMode
   Purpose - switches to user mode
   Parameters - none
   Side Effects - switches to user mode
   ------------------------------------------------------------------------ */
void setUserMode()
{
    USLOSS_PsrSet( USLOSS_PsrGet() & ~USLOSS_PSR_CURRENT_MODE );
}

/* initializes proc struct */
void initProc(int pid) {
    requireKernelMode("initProc()"); 

    int i = pid % MAXPROC;

    ProcTable[i].pid = pid; 
    ProcTable[i].mboxID = MboxCreate(0, 0);
    ProcTable[i].blockSem = semcreateReal(0);
    ProcTable[i].wakeTime = -1;
    ProcTable[i].diskTrack = -1;
    ProcTable[i].nextDiskPtr = NULL;
    ProcTable[i].prevDiskPtr = NULL;
}

/* empties proc struct */
void emptyProc(int pid) {
    requireKernelMode("emptyProc()"); 

    int i = pid % MAXPROC;

    ProcTable[i].pid = -1; 
    ProcTable[i].mboxID = -1;
    ProcTable[i].blockSem = -1;
    ProcTable[i].wakeTime = -1;
    ProcTable[i].nextDiskPtr = NULL;
    ProcTable[i].prevDiskPtr = NULL;
}

/* ------------------------------------------------------------------------
  Functions for the dskQueue and heap.
   ----------------------------------------------------------------------- */

/* Initialize the given diskQueue */
void initDiskQueue(diskQueue* q) {
    q->head = NULL;
    q->tail = NULL;
    q->curr = NULL;
    q->size = 0;
}

/* Adds the proc pointer to the disk queue in sorted order */
void addDiskQ(diskQueue* q, procPtr p) {
    if (debug4)
        USLOSS_Console("addDiskQ: adding pid %d, track %d to queue\n", p->pid, p->diskTrack);

    // first add
    if (q->head == NULL) { 
        q->head = q->tail = p;
        q->head->nextDiskPtr = q->tail->nextDiskPtr = NULL;
        q->head->prevDiskPtr = q->tail->prevDiskPtr = NULL;
    }
    else {
        // find the right location to add
        procPtr prev = q->tail;
        procPtr next = q->head;
        while (next != NULL && next->diskTrack <= p->diskTrack) {
            prev = next;
            next = next->nextDiskPtr;
            if (next == q->head)
                break;
        }
        if (debug4)
            USLOSS_Console("addDiskQ: found place, prev = %d\n", prev->diskTrack);
        prev->nextDiskPtr = p;
        p->prevDiskPtr = prev;
        if (next == NULL)
            next = q->head;
        p->nextDiskPtr = next;
        next->prevDiskPtr = p;
        if (p->diskTrack < q->head->diskTrack)
            q->head = p; // update head
        if (p->diskTrack >= q->tail->diskTrack)
            q->tail = p; // update tail
    }
    q->size++;
    if (debug4)
        USLOSS_Console("addDiskQ: add complete, size = %d\n", q->size);
} 

/* Returns the next proc on the disk queue */
procPtr peekDiskQ(diskQueue* q) {
    if (q->curr == NULL) {
        q->curr = q->head;
    }

    return q->curr;
}

/* Returns and removes the next proc on the disk queue */
procPtr removeDiskQ(diskQueue* q) {
    if (q->size == 0)
        return NULL;

    if (q->curr == NULL) {
        q->curr = q->head;
    }

    if (debug4)
        USLOSS_Console("removeDiskQ: called, size = %d, curr pid = %d, curr track = %d\n", q->size, q->curr->pid, q->curr->diskTrack);

    procPtr temp = q->curr;

    if (q->size == 1) { // remove only node
        q->head = q->tail = q->curr = NULL;
    }

    else if (q->curr == q->head) { // remove head
        q->head = q->head->nextDiskPtr;
        q->head->prevDiskPtr = q->tail;
        q->tail->nextDiskPtr = q->head;
        q->curr = q->head;
    }

    else if (q->curr == q->tail) { // remove tail
        q->tail = q->tail->prevDiskPtr;
        q->tail->nextDiskPtr = q->head;
        q->head->prevDiskPtr = q->tail;
        q->curr = q->head;
    }

    else { // remove other
        q->curr->prevDiskPtr->nextDiskPtr = q->curr->nextDiskPtr;
        q->curr->nextDiskPtr->prevDiskPtr = q->curr->prevDiskPtr;
        q->curr = q->curr->nextDiskPtr;
    }

    q->size--;

    if (debug4)
        USLOSS_Console("removeDiskQ: done, size = %d, curr pid = %d, curr track = %d\n", q->size, temp->pid, temp->diskTrack);

    return temp;
} 


/* Setup heap, implementation based on https://gist.github.com/aatishnn/8265656 */
void initHeap(heap* h) {
    h->size = 0;
}

/* Add to heap */
void heapAdd(heap * h, procPtr p) {
    // start from bottom and find correct place
    int i, parent;
    for (i = h->size; i > 0; i = parent) {
        parent = (i-1)/2;
        if (h->procs[parent]->wakeTime <= p->wakeTime)
            break;
        // move parent down
        h->procs[i] = h->procs[parent];
    }
    h->procs[i] = p; // put at final location
    h->size++;
    if (debug4) 
        USLOSS_Console("heapAdd: Added proc %d to heap at index %d, size = %d\n", p->pid, i, h->size);
} 

/* Return min process on heap */
procPtr heapPeek(heap * h) {
    return h->procs[0];
}

/* Remove earlist waking process form the heap */
procPtr heapRemove(heap * h) {
  if (h->size == 0)
    return NULL;

    procPtr removed = h->procs[0]; // remove min
    h->size--;
    h->procs[0] = h->procs[h->size]; // put last in first spot

    // re-heapify
    int i = 0, left, right, min = 0;
    while (i*2 <= h->size) {
        // get locations of children
        left = i*2 + 1;
        right = i*2 + 2;

        // get min child
        if (left <= h->size && h->procs[left]->wakeTime < h->procs[min]->wakeTime) 
            min = left;
        if (right <= h->size && h->procs[right]->wakeTime < h->procs[min]->wakeTime) 
            min = right;

        // swap current with min child if needed
        if (min != i) {
            procPtr temp = h->procs[i];
            h->procs[i] = h->procs[min];
            h->procs[min] = temp;
            i = min;
        }
        else
            break; // otherwise we're done
    }
    if (debug4) 
        USLOSS_Console("heapRemove: Called, returning pid %d, size = %d\n", removed->pid, h->size);
    return removed;
}
