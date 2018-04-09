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
    //Term writers
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


