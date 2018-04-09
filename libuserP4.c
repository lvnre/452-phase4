#include <usloss.h>
#include <usyscall.h>
#include <phase1.h>
#include <phase2.h>


void checkMode(){
  if (USLOSS_PsrGet() & USLOSS_PSR_CURRENT_MODE) {
    USLOSS_Console("Attempting to invoke syscall from kernel!\n");
    USLOSS_Halt(1);
  }
}

int Sleep(int seconds) {
  USLOSS_Sysargs sysArgs;
  checkMode();
  sysArgs.number 	= SYS_SLEEP;
  sysArgs.arg1	= (void *) ((long) seconds);
  
  USLOSS_Syscall(&sysArgs);

  return (long) sysArgs.arg4;
}

int DiskRead(void *diskBuffer, int unit, int track, int first, 
	     int sectors, int *status) {

  USLOSS_Sysargs sysArgs;
  checkMode();
  sysArgs.number = SYS_DISKREAD;
  sysArgs.arg1 = diskBuffer;
  sysArgs.arg2 = (void *) ((long) sectors);
  sysArgs.arg3 = (void *) ((long) track);
  sysArgs.arg4 = (void *) ((long) first);
  sysArgs.arg5 = (void *) ((long) unit);

  USLOSS_Syscall(&sysArgs);

  *status = (long) sysArgs.arg1;

  return (long) sysArgs.arg4;
}

int DiskWrite(void *diskBuffer, int unit, int track, int first, 
	      int sectors, int *status) {

  USLOSS_Sysargs sysArgs;
  checkMode();
  sysArgs.number = SYS_DISKWRITE;
  sysArgs.arg1 = diskBuffer;
  sysArgs.arg2 = (void *) ((long) sectors);
  sysArgs.arg3 = (void *) ((long) track);
  sysArgs.arg4 = (void *) ((long) first);
  sysArgs.arg5 = (void *) ((long) unit);

  USLOSS_Syscall(&sysArgs);
  
  *status = (long) sysArgs.arg1;
    
  return (long) sysArgs.arg4;
}

int DiskSize(int unit, int *sector, int *track, int *disk) {
  USLOSS_Sysargs sysArgs;
  checkMode();
  
  sysArgs.number = SYS_DISKSIZE;
  sysArgs.arg1 = (void *) ((long) unit);

  USLOSS_Syscall(&sysArgs);

  *sector = (long) sysArgs.arg1;
  *track = (long) sysArgs.arg2;
  *disk = (long) sysArgs.arg3;
  return (long) sysArgs.arg4;
}

int TermRead(char *buffer, int bufferSize, int unitID, 
	     int *numCharsRead) {
  USLOSS_Sysargs sysArgs;
  checkMode();
  sysArgs.number 	= SYS_TERMREAD;
  sysArgs.arg1 	= buffer;
  sysArgs.arg2 	= (void *) ((long) bufferSize);
  sysArgs.arg3 	= (void *) ((long) unitID);
  
  USLOSS_Syscall(&sysArgs);
  
  *numCharsRead = (long) sysArgs.arg2;

  return (long) sysArgs.arg4;
}

int TermWrite(char *buffer, int bufferSize, int unitID, 
	      int *numCharsRead) {
  USLOSS_Sysargs sysArgs;
  checkMode();
  sysArgs.number 	= SYS_TERMWRITE;
  sysArgs.arg1 	= buffer;
  sysArgs.arg2 	= (void *) ((long) bufferSize);
  sysArgs.arg3 	= (void *) ((long) unitID);

  USLOSS_Syscall(&sysArgs);

  *numCharsRead = (long) sysArgs.arg2;
  
  return (long) sysArgs.arg4;
}






















