#include <usloss.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <stdlib.h> /* needed for atoi() */
#include <phase4-structs.h>

int  semRunning;

int  ClockDriver(char *);
int  DiskDriver(char *);

int diskSizeReal(int unit, int *sector, int *track, int *disk);

/*HELPER FUNCTION DECLARATION*/
int isKernelMode();
void procInit(int index);
void emptyProc(int index);




proc     procTable[MAXPROC];

//DISK GLOBALS
int      zappedDisk;                  //Flag for if a disk is zapped
diskList disks[USLOSS_DISK_UNITS];    //List for disk drivers
int      diskPIDs[USLOSS_DISK_UNITS]; //Disk drivers' pids

//Term device mailboxes
int termProcs[USLOSS_TERM_UNITS][3];   //Term procs
int termInterrupts[USLOSS_TERM_UNITS]; //Term interrupts
int mboxPIDs[USLOSS_TERM_UNITS];       //PIDs to block
int mboxCharRec[USLOSS_TERM_UNITS];    //Receiver character
int mboxCharSend[USLOSS_TERM_UNITS];   //Send Character
int mboxLineRead[USLOSS_TERM_UNITS];   //Read line
int mboxLineWrite[USLOSS_TERM_UNITS];  //Write line

pQueue sleeping;


void
start3(void)
{
    char	name[128];
    char        termbuf[10];
    int		i;
    int		clockPID;
    int		pid;
    int		status;
    char        diskbuf[10];
    /*
     * Check kernel mode here.
     */
      //Check which mode we are in
    if (!isKernelMode()) {
      USLOSS_Console("start3(): called while in user mode. Halting...\n");
      USLOSS_Halt(1);
    }

    //Set sleeping queue size to 0
    sleeping.size = 0;
    
    //Initialize the procTable
    for(i = 0; i < MAXPROC; i++)
      procInit(i);

    //Initialize term mboxes
    for(i = 0; i < USLOSS_TERM_UNITS; i++){
      mboxPIDs[i] = MboxCreate(1, sizeof(int));  
      mboxCharRec[i] = MboxCreate(1, MAXLINE);  
      mboxCharSend[i] = MboxCreate(1, MAXLINE);   
      mboxLineRead[i] = MboxCreate(10, MAXLINE);   
      mboxLineWrite[i] = MboxCreate(10, MAXLINE);  
    }
    
    //Initialize systemCallVec
    systemCallVec[SYS_SLEEP]     = sleep;
    systemCallVec[SYS_DISKWRITE] = diskWrite;
    systemCallVec[SYS_DISKREAD]  = diskRead;
    systemCallVec[SYS_DISKSIZE]  = diskSize;
    systemCallVec[SYS_TERMWRITE] = termWrite;
    systemCallVec[SYS_TERMREAD]  = termRead;
    


    /*
     * Create clock device driver 
     * I am assuming a semaphore here for coordination.  A mailbox can
     * be used instead -- your choice.
     */
    semRunning = semcreateReal(0);
    clockPID = fork1("Clock driver", ClockDriver, NULL, USLOSS_MIN_STACK, 2);
    if (clockPID < 0) {
	USLOSS_Console("start3(): Can't create clock driver\n");
	USLOSS_Halt(1);
    }
    /*
     * Wait for the clock driver to start. The idea is that ClockDriver
     * will V the semaphore "semRunning" once it is running.
     */

    sempReal(semRunning);

    /*
     * Create the disk device drivers here.  You may need to increase
     * the stack size depending on the complexity of your
     * driver, and perhaps do something with the pid returned.
     */

    for (i = 0; i < USLOSS_DISK_UNITS; i++) {
      sprintf(diskbuf, "%d", i);
      
      pid = fork1("Disk driver", DiskDriver, NULL, USLOSS_MIN_STACK, 2); //Create driver process

      //Check if fork1 succeeded
      if(pid < 0) {
	USLOSS_Console("start3(): Can't create disk driver\n");
	USLOSS_Halt(1);
      }

      //Store the pid of the new disk driver into our array of disk pids
      diskPIDs[i] = pid;
      //Wait for the disk driver to start by calling sempReal
      sempReal(semRunning);
      
      //Get size of the disk of this unit
      int sec;
      int track;
      diskSizeReal(i, &sec, &track, &procTable[pid%MAXPROC].track);
    }

    /*
     * Create terminal device drivers.
     */
    for(i = 0; i < USLOSS_TERM_UNITS; i++) {
      sprintf(termbuf, "%d", i);

      //Create term processes and store them in the termProcs matrix
      termProcs[i][0] = fork1(name, TermWriter, termbuf, USLOSS_MIN_STACK, 2); //Create term process
      termProcs[i][1] = fork1(name, TermReader, termbuf, USLOSS_MIN_STACK, 2); //Create term process
      termProcs[i][2] = fork1(name, TermDriver, termbuf, USLOSS_MIN_STACK, 2); //Create term process

      //Wait for all term drivers to start by calling sempReal
      sempReal(semRunning);
      sempReal(semRunning);
      sempReal(semRunning);      
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

    status = 0;

    /*
     * Zap the device drivers
     */
    zap(clockPID);  // clock driver
    join(&status);

    //Zap the disk drivers
    for(i = 0; i < USLOSS_DISK_UNITS; i++) {
      semvReal(procTable[diskPIDs[i]].blockedSem);
      zap(diskPIDs[i]);
      join(&status);
    }

    //Zap term writers, readers, and drivers
    //Term Writers
    for(i = 0; i < USLOSS_TERM_UNITS; i++) {
      MboxSend(mboxLineWrite[i], NULL, 0);
      zap(termProcs[i][0]);
      join(&status);
    }

    //Term Readers
    for(i = 0; i < USLOSS_TERM_UNITS; i++) {
      MboxSend(mboxCharRec[i], NULL, 0);
      zap(termProcs[i][1]);
      join(&status);
    }

    //Term Drivers
    for(i = 0; i < USLOSS_TERM_UNITS; i++) {
      char file[50];
      int num = 0;
      //Enable recv interrupts
      num = USLOSS_TERM_CTRL_RECV_INT(num);
      USLOSS_DeviceOutput(USLOSS_TERM_DEV, i, (void *)((long)num));

      //Write to the term*.in file
      sprintf(file, "term%d.in", i);
      FILE *fn = fopen(file, "a+");
      fprintf(fn, "last line\n");
      fflush(fn);
      fclose(fn);

      //Zap the term driver
      zap(termProcs[i][2]);
      join(&status);
      
    }
    // eventually, at the end:
    quit(0);
}

int
ClockDriver(char *arg)
{
    int result;
    int status;

    // Let the parent know we are running
    semvReal(semRunning);

    // Infinite loop until we are zap'd
    while(! isZapped()) {
	result = waitDevice(USLOSS_CLOCK_DEV, 0, &status);
	if (result != 0) {
	    return 0;
	}
	/*
	 * Compute the current time and wake up any processes
	 * whose time has come.
	 */
    }
}

int
DiskDriver(char *arg)
{
    return 0;
}








int diskSizeReal(int unit, int *sector, int *track, int *disk){}





//CHECK IF IN KERNEL MODE
int isKernelMode()
{
  union psrValues xRay;
  xRay.integerPart = USLOSS_PsrGet();
  return xRay.bits.curMode;
}

void procInit(int index)
{
  int i = index % MAXPROC;
  procTable[i].pid = index;
  procTable[i].mboxId = MboxCreate(0,0);
  procTable[i].blockedSem = semCreateReal(0);
  procTable[i].wakeTime = -1;
  procTable[i].prevDisk = NULL;
  procTable[i].nextDisk = NULL;
  procTable[i].track = -1;
}

/*
 *  Routine:  emptyProc
 *
 *  Description: empties/nullifies the specified process in the Process Table
 *
 *  Arguments: sysargs struct 
 *
 */
void emptyProc(int index)
{
  int i = index % MAXPROC;
  procTable[i].pid = -1;
  procTable[i].mboxId = -1;
  procTable[i].blockedSem = -1;
  procTable[i].wakeTime = -1;
  procTable[i].prevDisk = NULL;
  procTable[i].nextDisk = NULL;
}

/************ REQUIRE KERNEL MODE ***********/
/* ------------------------------------------------------------------------
   Name - requireKernelMode
   Purpose - Checks if we are in kernel mode and prints an error messages
              and halts USLOSS if not.
   Parameters - The name of the function calling it, for the error message.
   Side Effects - Prints and halts if we are not in kernel mode
   ------------------------------------------------------------------------ */

/*
 *  Routine:  requireKernelMode
 *
 *  Description: Checks if we are in kernel mode and prints an error messages
                 and halts USLOSS if not.
 *
 *  Arguments:   Name of the function that needs to check if in kernel mode
 *
 *  Return Value: void
 *
 */
void requireKernelMode(char *name)
{
    if( (USLOSS_PSR_CURRENT_MODE & USLOSS_PsrGet()) == 0 ) {
        USLOSS_Console("%s: called while in user mode, by process %d. Halting...\n", 
             name, getpid());
        USLOSS_Halt(1); 
    }
} 

/************ SLEEPS *************/

/*
 *  Routine:  sleep
 *
 *  Description: Extract the value needed for sleepReal()
 *
 *  Arguments:    USLOSS_Sysargs *sysArgs - system arguments passed to the sleep
                  function
 *
 *  Return Value: void
 *
 */

void sleep(USLOSS_Sysargs *sysArgs) {
  requireKernelMode("sleep()");
  int sec = (long) sysArgs->arg1;
  int val = sleepReal(sec);
  sysArgs->arg4 = (void *) ((long) val);
  setUserMode();
}


/*
 *  Routine:  sleepReal()
 *
 *  Description: Extract the value needed for sleepReal()
 *
 *  Arguments:    int procSeconds - number of seconds associated with the current process
 *
 *  Return Value: -1 if the processes time slice is < 0, 0 if successful
 *
 */

int sleepReal(int procSeconds) {
  requireKernelMode("sleepReal()");
  if (procSeconds < 0) return -1;

  //get the current process
  if (ProcTable[getpid() % MAXPROC].pid == -1) {
      //initialize ProcTable entry if the process does not exist yet
      initProc(getpid());
  }
  procPtr currProc = &ProcTable[getpid() % MAXPROC];
  
  //set wake time for the current process
  currProc->wakeTime = USLOSS_Clock() + procSeconds*1000000;

  //add the current process to the sleep heap
  heapAdd(&sleepHeap, currProc);

  //block the current process
  sempReal(currProc->blockSem);
  return 0;
}


/* extract values from sysargs and call diskReadReal */
void diskRead(USLOSS_Sysargs * sysArgs) {
    requireKernelMode("diskRead()");

    int sectors = (long) sysArgs->arg2;
    int track = (long) sysArgs->arg3;
    int first = (long) sysArgs->arg4;
    int unit = (long) sysArgs->arg5;

    int val = diskReadReal(unit, track, first, sectors, sysArgs->arg1);

    sysArgs->arg1 = (void *) ((long) val);
    if (val == -1) {
      sysArgs->arg4 = (void *) ((long) -1)
    } else {
      sysArgs->arg4 = (void *) ((long) 0);
    }
    setUserMode();
}


/* extract values from sysargs and call diskWriteReal */
void diskWrite(USLOSS_Sysargs * sysArgs) {
    requireKernelMode("diskWrite()");

    int sectors = (long) sysArgs->arg2;
    int track = (long) sysArgs->arg3;
    int first = (long) sysArgs->arg4;
    int unit = (long) sysArgs->arg5;

    int val = diskWriteReal(unit, track, first, sectors, sysArgs->arg1);

    sysArgs->arg1 = (void *) ((long) val);
    if (val == -1) {
      sysArgs->arg4 = (void *) ((long) -1)
    } else {
      sysArgs->arg4 = (void *) ((long) 0);
    }
    setUserMode();
}

int diskWriteReal(int unit, int track, int first, int sectors, void *buffer) {
    requireKernelMode("diskWriteReal()");
    //pass 1 to diskReadOrWriteReal() to indicate it's a write
    return diskReadOrWriteReal(unit, track, first, sectors, buffer, 1);
}



int diskReadReal(int unit, int track, int first, int sectors, void *buffer) {
    requireKernelMode("diskWriteReal");
    //pass 0 to diskReadOrWriteReal() to indicate it's a read
    return diskReadOrWriteReal(unit, track, first, sectors, buffer, 0);
}


/*------------------------------------------------------------------------
    diskReadOrWriteReal: Reads or writes to the desk depending on the 
                        value of write; write if write == 1, else read.
    Returns: -1 if given illegal input, 0 otherwise
 ------------------------------------------------------------------------*/
int diskReadOrWriteReal(int unit, int track, int first, int sectors, void *buffer, int write) {
    //return -1 if any arguments are invalid
    if (unit > 1  || unit < 0 || 
        track > ProcTable[diskPids[unit]].diskTrack || track < 0 ||
        first > USLOSS_DISK_TRACK_SIZE || first < 0 || 
        buffer == NULL  ||
        (first + sectors)/USLOSS_DISK_TRACK_SIZE + track > ProcTable[diskPids[unit]].diskTrack) {
        return -1;
    }

    procPtr driver = &ProcTable[diskPids[unit]];

    //Retrieve the process

    //If the process doesn't exist, initialize
    if (ProcTable[getpid() % MAXPROC].pid == -1) {
        initProc(getpid());
    }
    procPtr currProcess = &currProcessTable[getpid() % MAXPROC];

    if (!write) {
      currProcess->diskRequest.opr = USLOSS_DISK_READ;
      currProcess->diskRequest.reg2 = buffer;
      currProcess->diskTrack = track;
      currProcess->diskFirstSec = first;
      currProcess->diskSectors = sectors;
      currProcess->diskBuffer = buffer;
    } else {
      currProcess->diskRequest.opr = USLOSS_DISK_WRITE;
    }
        
    addDiskQ(&diskQs[unit], currProcess); // add to disk queue 
    semvReal(driver->blockSem);  // wake up disk driver
    sempReal(currProcess->blockSem); // block

    int status;
    int result = USLOSS_DeviceInput(USLOSS_DISK_DEV, unit, &status);

    return result;
}


void diskSize(USLOSS_Sysargs * sysArgs) {

    requireKernelMode("diskSize()");

    int unit = (long) sysArgs->arg1;
    int sector, track, disk;
    int val = diskSizeReal(unit, &sector, &track, &disk);

    sysArgs->arg1 = (void *) ((long) sector);
    sysArgs->arg2 = (void *) ((long) track);
    sysArgs->arg3 = (void *) ((long) disk);
    sysArgs->arg4 = (void *) ((long) val);

    setUserMode();
}

/*------------------------------------------------------------------------
    diskSizeReal: Puts values into pointers for the size of a sector, 
    number of sectors per track, and number of tracks on the disk for the 
    given unit. 

    Routine: diskSizeReal()

    Purpose: takes the pointers given as parameters and assigns them to
             values specifying the number of tracks on the disk as well as the 
             number of sector per track

    Returns: 0 if successful, -1 if invalid arguments are passed in
 ------------------------------------------------------------------------*/
int diskSizeReal(int unit, int *sector, int *track, int *disk) {

    requireKernelMode("diskSizeReal()");

  
    //Illegal argument check
    if (unit < 0 || unit > 1 ||
        sector == NULL||
        track == NULL || 
        disk == NULL) {
        
        //return -1 if illegal arguments are passed in
        return -1;
    }

    procPtr drivePtr = &ProcTable[diskPids[unit]];

    // get the number of tracks for the first time
    if (drivePtr->diskTrack == -1) {
    
        //initialize the process if the proc table entry is empty
        if (ProcTable[getpid() % MAXPROC].pid == -1) {
            initProc(getpid());
        }

        //store current proc
        procPtr currProc = &ProcTable[getpid() % MAXPROC];

        //populate the values of the current proc
        currProc->diskTrack = 0;

        //set request values for the current proc
        USLOSS_DeviceRequest request;
        request.opr = USLOSS_DISK_TRACKS;
        request.reg1 = &drivePtr->diskTrack;
        currProc->diskRequest = request;

        //add to disk queue
        addDiskQ(&diskQs[unit], currProc);
        //unblock the drive ptr
        semvReal(drivePtr->blockSem); 
        //block the current proc
        sempReal(currProc->blockSem);
    }

    *sector = USLOSS_DISK_SECTOR_SIZE;
    *track = USLOSS_DISK_TRACK_SIZE;
    *disk = drivePtr->diskTrack;

    //return 0 if successful
    return 0;
}










