#include <usloss.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <stdlib.h>
#include <phase4-structs.h>
#include <stdio.h>
#include <string.h>

int  semRunning;

int  ClockDriver(char *);
int  DiskDriver(char *);

int diskSizeReal(int unit, int *sector, int *track, int *disk);

/*HELPER FUNCTION DECLARATION*/
int isKernelMode();
void enableInterrupts();
void procInit(int index);
void emptyProc(int index);
procPtr topSleepingQ(pQueue *q);
procPtr removeTopSleeping(pQueue *q);
procPtr removeFromDiskList(diskList *list);




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
	procPtr sleepingProc;
	//Check if there are procs on the sleeping queue and check it's time
	if(sleeping.size > 0 && USLOSS_Clock() >= topSleepingQ(&sleeping)->time){
	  
	  //Get the proc off the top of the sleeping queue and wake it up 
	  sleepingProc = removeTopSleeping(&sleeping);
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
  initProc(getpid());

  //Get the proc
  procPtr currProc = &procTable[getpid()%MAXPROC];

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
      
      procPtr proc = &disks[unit].current;
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
	  while(proc->sectors > 0 && s < USLOSS_DISK_TRACK_SIZE) {
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
	    s++;
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
    if(next == MAXLINE || c == '\n') {
      //Store a null char at the end
      lineRead[next] = '\0';

      //Send the completed line to the line read mbox
      MboxSend(mboxLineRead[unit], line, next);

      //Reset variables to read another line
      next = 0;
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

  semvReal(semRunning);

  while(! isZapped()) {
    //Receive from the Line Write mbox
    int size = MboxReceive(mboxLineWrite[unit], line, MAXLINE);

    //Check if the proc was zapped while trying to receive
    if(isZapped())
      return;

    //Enable transmit interrupts
    int num = USLOSS_TERM_CTRL_XMIT_INT(num);
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
    semvReal(procTable[pid%MAXPROC].blockedSem);
  }
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
  procTable[i].time = -1;
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
  procTable[i].time = -1;
  procTable[i].prevDisk = NULL;
  procTable[i].nextDisk = NULL;
}

void enableInterrupts()
{
  USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);
}

procPtr topSleepingQ(pQueue *q)
{
  q->processes[0];
}

procPtr removeTopSleeping(pQueue *q)
{
  //Make sure there are procs in the queue
  if(q->size <= 0)
    return NULL;

  q->size--;
  procPtr removedProc = q->processes[0];
  q->processes[0] = q->processes[q->size-1];

  int i = 0;
  int l;
  int r;
  int smallest = 0;

  while(i*2 <= q->size) {
    l = i*2 +1;
    r = i*2 +2;

    //Find smallest child
    if(r <= q->size && && q->processes[r]->time < q->processes[smallest]->time) 
      min = right;
    if(l <= q->size && q->processes[l]->time < q->processes[smallest]->time) 
      min = left;

    if(smallest != i) {
      procPtr temp = q->processes[i];
      q->processes[i] = q->processes[smallest];
      q->processes[smallest] = temp;
      i = min;
    }
    else
      break;
  }
  return removedProc;
}


procPtr removeFromDiskList(diskList *list)
{
  if(list->length == 0)
    return NULL;

  if(list->current == NULL)
    list->current = list->head;

  procPtr retProc = list->current;

  //Remove the only disk on this list
  if(list->size == 1) {
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

