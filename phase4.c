#include <usloss.h>
#include <usyscall.h>
#include <providedPrototypes.h>
#include <phase1.h>
#include <phase4-structs.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define ABS(a,b) (a-b > 0 ? a-b : -(a-b))


int  semRunning;
int  ClockDriver(char *);
int  DiskDriver(char *);
int  TermDriver(char *);
int  TermReader(char *);
int  TermWriter(char *);
void sleep(USLOSS_Sysargs *);
int  sleepReal(int); 
void diskRead(USLOSS_Sysargs *);
int  diskReadReal(int, int, int, int, void *);
void diskWrite(USLOSS_Sysargs *);
int  diskWriteReal(int, int, int, int, void *);
int  diskWriteOrReadReal(int, int, int, int, void *, int);
void diskSize(USLOSS_Sysargs *);
int diskSizeReal(int unit, int *sector, int *track, int *disk);
void termRead(USLOSS_Sysargs *);
int  termReadReal(int, int, char *);
void termWrite(USLOSS_Sysargs *);
int  termWriteReal(int, int, char *);

/*HELPER FUNCTION DECLARATION*/
int isKernelMode();
void userModeOn();
void procInit(int index);
void enableInterrupts();
procStructPtr topSleepingQ(pQueue *q);
void addToSleepingQ(pQueue *, procStructPtr);
procStructPtr removeTopSleepingQ(pQueue *q);
void addToDiskList(diskList *, procStructPtr);
procStructPtr removeFromDiskList(diskList *list);

procStructure ProcStructTable[MAXPROC];

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
  for(i = 0; i < MAXPROC; i++){
    procInit(i);
  }

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

  int num;
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
    diskSizeReal(i, &num, &num, &ProcStructTable[pid%MAXPROC].track);
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
    semvReal(ProcStructTable[diskPIDs[i]].blockedSem);
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
    int x = USLOSS_DeviceOutput(USLOSS_TERM_DEV, i, (void *)((long)num));

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
    //Enable interrupts
    enableInterrupts();

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
	procStructPtr sleepingProc;
	int time;
	gettimeofdayReal(&time);

	//Check if there are procs on the sleeping queue and check it's time
	if(sleeping.size > 0 && time >= topSleepingQ(&sleeping)->time){
	  //Get the proc off the top of the sleeping queue and wake it up 
	  sleepingProc = removeTopSleepingQ(&sleeping);
	  semvReal(sleepingProc->blockedSem);
	}
    }
    return 0;
}

int
DiskDriver(char *arg)
{
  int unit = atoi((char *) arg);
  int status;
  int retVal;

  //Initialize the process in the procTable
  procInit(getpid());

  //Get the proc
  procStructPtr currProc = &ProcStructTable[getpid()%MAXPROC];

  //Initialize the disk list for this unit
  disks[unit].head = NULL;
  disks[unit].tail = NULL;
  disks[unit].length = 0;
  disks[unit].current = NULL;

  semvReal(semRunning);
  //Enable interrupts
  enableInterrupts();

  // Infinite loop until we are zap'd
  while(! isZapped()) {
    //Block on sem to await request
    sempReal(currProc->blockedSem);

    //Check if we were zapped while blocked
    if(isZapped())
      return 0;

    //Take the next request from the Q if there is one
    if(disks[unit].length > 0) {
      
      if(disks[unit].current == NULL)
	disks[unit].current = disks[unit].head;
      
      procStructPtr proc = disks[unit].current;
      int track = proc->track;

      //Read/Write requests
      if(proc->request.opr != USLOSS_DISK_TRACKS) {
	//Loop to get/send data
	while(proc->sectors > 0) {
	  //Find track
	  USLOSS_DeviceRequest req;
	  req.opr = USLOSS_DISK_SEEK;
	  req.reg1 = &track;

	  USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &req);
	  retVal = waitDevice(USLOSS_DISK_DEV, unit, &status);
	  //Check the return value from wait device
	  if(retVal != 0)
	    return 0;

	  //Write and read sectors
	  int sec = proc->firstSector;
	  while(proc->sectors > 0 && sec < USLOSS_DISK_TRACK_SIZE) {
	    proc->request.reg1 = (void *)((long) sec);

	    USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &proc->request);
	    retVal = waitDevice(USLOSS_DISK_DEV, unit, &status);
	    //Check return value from wait device
	    if(retVal != 0)
	      return 0;

	    //Reduce sectors
	    proc->sectors--;
	    //Update request
	    proc->request.reg2 += USLOSS_DISK_SECTOR_SIZE;
	    sec++;
	  }

	  track++;
	  proc->firstSector = 0;
	}
      }

      //Handle tracks request - initiate by using USLOSS_DeviceOutput
      else {
	USLOSS_DeviceOutput(USLOSS_DISK_DEV, unit, &proc->request);
	retVal = waitDevice(USLOSS_DISK_DEV, unit, &status);
	//Check return value of wait device
	if(retVal != 0)
	  return 0;
      }

      //Remove the process from the disk list and unblock
      removeFromDiskList(&disks[unit]);
      semvReal(proc->blockedSem);
    }
  }
  //Unblock parent proc
  semvReal(semRunning);
  return 0;
}

int TermDriver(char *arg)
{
  int unit = atoi((char *) arg);
  int status;
  int retVal;

  // Let the parent know we are running
  semvReal(semRunning);

  while(! isZapped()) {
    retVal = waitDevice(USLOSS_TERM_INT, unit, &status);
    //Check the return value
    if(retVal != 0)
      return 0;

    //Check the receive status for a status of USLOSS_DEV_BUSY meaning a char is ready to be
    //recieved
    int recv = USLOSS_TERM_STAT_RECV(status);
    if(recv == USLOSS_DEV_BUSY){
      MboxCondSend(mboxCharRec[unit], &status, sizeof(int));
    }

    //Check the xmit status for a status of USLOSS_DEV_READY meaning a char is ready to be
    //sent
    int xmit = USLOSS_TERM_STAT_XMIT(status);
    if(xmit == USLOSS_DEV_READY) {
      MboxCondSend(mboxCharRec[unit], &status, sizeof(int));
    }
  }
  return 0;
}

int TermReader(char *arg)
{
  int unit = atoi((char *) arg);
  char lineRead[MAXLINE];
  int charRec;
  int nextIndex = 0;
  int i;

  semvReal(semRunning);

  while(! isZapped()) {

    //Read from the mbox
    MboxReceive(mboxCharRec[unit], &charRec, sizeof(int));

    //Use USLOSS_TERM_STAT_CHAR with charRec to get the char
    char c = USLOSS_TERM_STAT_CHAR(charRec);

    //Store in the char array for the line
    lineRead[nextIndex] = c;
    nextIndex++;

    //Check if we have read the max amount of characters or we found a new line char
    if(nextIndex == MAXLINE || c == '\n') {
      //Store a null char at the end
      lineRead[nextIndex] = '\0';

      //Send the completed line to the line read mbox
      MboxSend(mboxLineRead[unit], lineRead, nextIndex);

      //Reset variables to read another line
      nextIndex = 0;
      for(i = 0; i < MAXLINE; i++)
	lineRead[i] = '\0';
    }  
  }
  return 0;
}

int TermWriter(char *arg)
{
  int unit = atoi((char *) arg);
  char line[MAXLINE];
  int nextIndex;
  int status;
  int num = 0;

  semvReal(semRunning);

  while(! isZapped()) {
    //Receive from the Line Write mbox
    int size = MboxReceive(mboxLineWrite[unit], line, MAXLINE);

    //Check if the proc was zapped while trying to receive
    if(isZapped())
      break;

    //Enable transmit interrupts
    num = USLOSS_TERM_CTRL_XMIT_INT(num);
    //Receive interrupt
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void *)((long)num));

    //Transmit the line
    for(nextIndex = 0; nextIndex < size; nextIndex++) {
      MboxReceive(mboxCharSend[unit], &status, sizeof(int));

      //Check the transmit status for a USLOSS_DEV_READY status
      int xmit = USLOSS_TERM_STAT_XMIT(status);
      if(xmit == USLOSS_DEV_READY) {
	//Char to send
	num = USLOSS_TERM_CTRL_CHAR(num, line[nextIndex]);

	//Transmit the char
	num = USLOSS_TERM_CTRL_XMIT_CHAR(num);

	//Enable xmit interrupts
	num = USLOSS_TERM_CTRL_XMIT_INT(num);

	USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void *)((long)num));
      }
    }

    //Reset num and enable receive interrupts
    num = 0;
    if(termInterrupts[unit] == 1)
      num = USLOSS_TERM_CTRL_RECV_INT(num);
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void *)((long)num));
    termInterrupts[unit] = 0;

    //Receive from pid mbox
    int pid;
    MboxReceive(mboxPIDs[unit], &pid, sizeof(int));
    semvReal(ProcStructTable[pid%MAXPROC].blockedSem);
  }
  return 0;
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
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("sleep(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }
  
  int seconds = (long) sysArgs->arg1;
  int result = sleepReal(seconds);
  sysArgs->arg4 = (void *) ((long) result);

  userModeOn();
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
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("sleepReal(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }

  //Check for invalid argument
  if (procSeconds < 0)
    return -1;

  //Init the process if it is not yet done
  if (ProcStructTable[getpid() % MAXPROC].pid == -1) {
      //initialize ProcTable entry if the process does not exist yet
      procInit(getpid());
  }
  //Get the current process
  procStructPtr currProc;
  currProc = &ProcStructTable[getpid() % MAXPROC];


  int time;
  gettimeofdayReal(&time);
  //set time for the current process

  currProc->time = time + procSeconds*1000;

  //add the current process to the sleeping priority queue
  addToSleepingQ(&sleeping, currProc);

  //block the current process
  sempReal(currProc->blockedSem);
  return 0;
}

void diskRead(USLOSS_Sysargs * sysArgs) {
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("diskRead(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }
  
  int sectors = (long) sysArgs->arg2;
  int track = (long) sysArgs->arg3;
  int first = (long) sysArgs->arg4;
  int unit = (long) sysArgs->arg5;
  
  int result = diskReadReal(unit, track, first, sectors, sysArgs->arg1);
  sysArgs->arg1 = (void *) ((long) result);
  
  if (result == -1) {
    sysArgs->arg4 = (void *) ((long) -1);
  }
  else {
    sysArgs->arg4 = (void *) ((long) 0);
  }

  userModeOn();
}

int diskReadReal(int unit, int track, int first, int sectors, void *buffer) {
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("diskReadReal(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }
  //pass 0 to diskReadOrWriteReal() to indicate it's a read
  return diskWriteOrReadReal(unit, track, first, sectors, buffer, 0);
}


/* extract values from sysargs and call diskWriteReal */
void diskWrite(USLOSS_Sysargs * sysArgs) {
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("diskWrite(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }
  int sectors = (long) sysArgs->arg2;
  int track = (long) sysArgs->arg3;
  int first = (long) sysArgs->arg4;
  int unit = (long) sysArgs->arg5;

  int result = diskWriteReal(unit, track, first, sectors, sysArgs->arg1);
  sysArgs->arg1 = (void *) ((long) result);
  
  if (result == -1) {
    sysArgs->arg4 = (void *) ((long) -1);
  }
  else {
    sysArgs->arg4 = (void *) ((long) 0);
  }
  userModeOn();
}

int diskWriteReal(int unit, int track, int first, int sectors, void *buffer) {
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("diskWriteReal(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }
  //pass 1 to diskReadOrWriteReal() to indicate it's a write
  return diskWriteOrReadReal(unit, track, first, sectors, buffer, 1);
}


int diskWriteOrReadReal(int unit, int track, int first, int sectors, void *buffer, int writeRead) {
  
    //return -1 if any arguments are invalid
    if (unit > 1  || unit < 0 || 
        track > ProcStructTable[diskPIDs[unit]].track || track < 0 ||
        first > USLOSS_DISK_TRACK_SIZE || first < 0 || 
        buffer == NULL  ||
        (first + sectors)/USLOSS_DISK_TRACK_SIZE + track > ProcStructTable[diskPIDs[unit]].track) {
        return -1;
    }

    procStructPtr driver = &ProcStructTable[diskPIDs[unit]];

    //If the process doesn't exist, initialize
    if (ProcStructTable[getpid() % MAXPROC].pid == -1) {
        procInit(getpid());
    }
    procStructPtr currProcess = &ProcStructTable[getpid() % MAXPROC];

    if (!writeRead) {
      currProcess->request.opr = USLOSS_DISK_READ;
      currProcess->request.reg2 = buffer;
      currProcess->track = track;
      currProcess->firstSector = first;
      currProcess->sectors = sectors;
      currProcess->diskBuffer = buffer;
    } else {
      currProcess->request.opr = USLOSS_DISK_WRITE;
    }
        
    addToDiskList(&disks[unit], currProcess); // add to disk list 
    semvReal(driver->blockedSem);         // wake disk driver
    sempReal(currProcess->blockedSem);    // block current

    int status;
    int result = USLOSS_DeviceInput(USLOSS_DISK_DEV, unit, &status);

    return result;
}

void diskSize(USLOSS_Sysargs * sysArgs) {

  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("diskSize(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }
  int unit = (long) sysArgs->arg1;
  int sector;
  int track;
  int disk;
  int val = diskSizeReal(unit, &sector, &track, &disk);

  sysArgs->arg1 = (void *) ((long) sector);
  sysArgs->arg2 = (void *) ((long) track);
  sysArgs->arg3 = (void *) ((long) disk);
  sysArgs->arg4 = (void *) ((long) val);

  userModeOn();
}

int diskSizeReal(int unit, int *sector, int *track, int *disk) {

  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("diskSizeReal(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }
  
  //Check illegal args
  if (unit < 0 || unit > 1 ||
      sector == NULL||
      track == NULL || 
      disk == NULL) {
    
    //return -1 if illegal arguments are passed in
    return -1;
  }

  procStructPtr drivePtr = &ProcStructTable[diskPIDs[unit]];

  // get the number of tracks for the first time
  if (drivePtr->track == -1) {

    //initialize the process if the proc table entry is empty
    if (ProcStructTable[getpid() % MAXPROC].pid == -1) {
      procInit(getpid());
    }

    //store current proc
    procStructPtr currProc = &ProcStructTable[getpid() % MAXPROC];

    //populate the values of the current proc
    currProc->track = 0;

    //set request values for the current proc
    USLOSS_DeviceRequest request;
    request.opr = USLOSS_DISK_TRACKS;
    request.reg1 = &drivePtr->track;
    currProc->request = request;

    //add to disk queue
    addToDiskList(&disks[unit], currProc);

    //unblock the drive ptr
    semvReal(drivePtr->blockedSem);

    //block the current proc
  }

  *sector = USLOSS_DISK_SECTOR_SIZE;
  *track = USLOSS_DISK_TRACK_SIZE;
  *disk = drivePtr->track;

  //return 0 if successful
  return 0;
}


void termRead(USLOSS_Sysargs *sysArgs)
{
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("termRead(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }
  
  char * buf = (char *) sysArgs->arg1;
  int bufSize = (long) sysArgs->arg2;
  int unit = (long) sysArgs->arg3;

  int result = termReadReal(unit, bufSize, buf);
  sysArgs->arg2 = (void *)((long) result);
  if(result != -1)
    sysArgs->arg1 = (void *)((long) 0);
  else
    sysArgs->arg1 = (void *)((long) -1);

  userModeOn();
}

int termReadReal(int unit, int bufSize, char *buff)
{
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("termReadReal(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }

  //Check for invalid arguments
  if(unit > USLOSS_TERM_UNITS - 1 || unit < 0 || bufSize < 0)
    return -1;

  char line[MAXLINE];
  int num = 0;
  int result;

  //Enable interrupts if necessary using USLOSS_TERM_CTRL_RECV_INT and DeviceOutput
  //and set termInterrupts = 1
  if(termInterrupts[unit] == 0) {
    num = USLOSS_TERM_CTRL_RECV_INT(num);
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void *)((long)num));
    termInterrupts[unit] = 1;
  }

  //Receive the line from the read line mbox
  result = MboxReceive(mboxLineRead[unit], &line, MAXLINE);
  //Check if we need to update the result if receive returned a value bigger than the buffer size
  if(result > bufSize)
    result = bufSize;

  //Use memcpy to copy the line read from the mbox into the argument buff
  memcpy(buff, line, result);
  return result;
}

void termWrite(USLOSS_Sysargs *sysArgs)
{
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("termWrite(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }

  char * buf = (char *) sysArgs->arg1;
  int bufSize = (long) sysArgs->arg2;
  int unit = (long) sysArgs->arg3;

  int result = termWriteReal(unit, bufSize, buf);
  sysArgs->arg2 = (void *)((long)result);
  if(result != -1)
    sysArgs->arg1 = (void *)((long) 0);
  else
    sysArgs->arg1 = (void *)((long) -1);

  userModeOn();  
}

int termWriteReal(int unit, int bufSize, char *text)
{
  //Check which mode we are in
  if (!isKernelMode()) {
    USLOSS_Console("termWriteReal(): called while in user mode. Halting...\n");
    USLOSS_Halt(1);
  }

  //Check for invalid arguments
  if(unit > USLOSS_TERM_UNITS - 1 || unit < 0 || bufSize < 0)
    return -1;

  int pid = getpid();
  
  MboxSend(mboxPIDs[unit], &pid, sizeof(int));
  MboxSend(mboxLineWrite[unit], text, bufSize);
  sempReal(ProcStructTable[pid%MAXPROC].blockedSem);
  return bufSize;
}




//CHECK IF IN KERNEL MODE
int isKernelMode()
{
  union psrValues xRay;
  xRay.integerPart = USLOSS_PsrGet();
  return xRay.bits.curMode;
}

/*
 *  Routine:  userModeOn
 *
 *  Description: puts the program in user mode
 *
 *  Arguments: sysargs struct 
 *
 */
void userModeOn()
{
  //Last bit is set to 0
  int res = USLOSS_PsrSet( USLOSS_PsrGet() & ~USLOSS_PSR_CURRENT_MODE );
  if(res < 0)
    USLOSS_Console("userModeOn(): could not change to user mode\n");
}

void procInit(int pid)
{
  int i = pid % MAXPROC;
  ProcStructTable[i].pid = pid;
  ProcStructTable[i].mboxID = MboxCreate(0,0);
  ProcStructTable[i].blockedSem = semcreateReal(0);
  ProcStructTable[i].time = -1;
  ProcStructTable[i].prevDisk = NULL;
  ProcStructTable[i].nextDisk = NULL;
  ProcStructTable[i].track = -1;
}

void enableInterrupts()
{
  USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);
}

procStructPtr topSleepingQ(pQueue *q)
{
  return q->processes[0];
}

void addToSleepingQ(pQueue *q, procStructPtr new)
{
  int pred;
  int i = q->size;

  //Start from the back of the priority queue array
  while(i > 0) {
    pred = (i-1)/2;
    //If we find the correct spot
    if(q->processes[pred]->time <= new->time)
      break;
    //Otherwise update
    q->processes[i] = q->processes[pred];
    i = pred;
  }
  //Increment the length
  q->size++;
  //Store the new proc in the space where i ended
  q->processes[i] = new;
}

procStructPtr removeTopSleepingQ(pQueue *q)
{
  //Make sure there are procs in the queue
  if(q->size <= 0)
    return NULL;

  q->size--;
  procStructPtr removedProc = q->processes[0];
  q->processes[0] = q->processes[q->size-1];

  int i = 0;
  int l;
  int r;
  int smallest = 0;

  while(i*2 <= q->size) {
    l = i*2 +1;
    r = i*2 +2;

    //Find smallest child
    if(r <= q->size && q->processes[r]->time < q->processes[smallest]->time) 
      smallest = r;
    if(l <= q->size && q->processes[l]->time < q->processes[smallest]->time) 
      smallest = l;

    if(smallest != i) {
      procStructPtr temp = q->processes[i];
      q->processes[i] = q->processes[smallest];
      q->processes[smallest] = temp;
      i = smallest;
    }
    else
      break;
  }
  return removedProc;
}


void addToDiskList(diskList *list, procStructPtr new)
{
  //Update the length
  list->length++;
  
  //Check if this is the first process to add
  if(list->head == NULL){
    list->head = new;
    list->tail = new;
    
    list->head->nextDisk = NULL;
    list->tail->nextDisk = NULL;

    list->head->prevDisk = NULL;
    list->tail->prevDisk = NULL;
  }
  else {
    //Find correct location based of track values
    procStructPtr front = list->head;
    procStructPtr back = list->tail;

    //Move front forward until we find a track value that is greater than new's
    //or front is null
    while(front != NULL && front->track <= new->track){
      back = front;
      front = front->nextDisk;
      //Check if we've looped back to the front
      if(front == list->head) break;
    }

    //Check if loop broke because front is null
    if(front == NULL) front = list->head;

    back->nextDisk = new; //Update the previous' proc next pointer to new
    new->prevDisk = back; //Update the new proc prev pointer to back

    front->prevDisk = new; //Update the front proc prev disk pointer to new
    new->nextDisk = front; //Update the new proc next pointer to point to front

    //Update head and tail pointers of the list
    if(new->track >= list->tail->track) list->tail = new;
    if(new->track < list->head->track) list->head = new;
  }
}

procStructPtr removeFromDiskList(diskList *list)
{
  if(list->length == 0)
    return NULL;

  if(list->current == NULL)
    list->current = list->head;

  procStructPtr retProc = list->current;

  //Remove the only disk on this list
  if(list->length == 1) {
    list->current = NULL;
    list->head = NULL;
    list->tail = NULL;
  }
  //If current is the tail
  else if(list->current == list->tail) {
    list->tail = list->tail->prevDisk;   //Move the tail
    list->tail->nextDisk = list->head;   //Update tails next
    list->head->prevDisk = list->tail;   //Update heads's prev
    list->current = list->head;          //Update current
  }

  //If current is the head
  else if(list->current == list->head) {
    list->head = list->head->nextDisk;    //Move the head
    list->head->prevDisk = list->tail;    //Update heads prev
    list->tail->nextDisk = list->head;    //Update tail
    list->current = list->head;           //Update current
  }

  else {
    list->current->prevDisk->nextDisk = list->current->nextDisk;
    list->current->nextDisk->prevDisk = list->current->prevDisk;
    list->current = list->current->nextDisk;
  }  

  //Reduce the length
  list->length--;
  return retProc;
}

