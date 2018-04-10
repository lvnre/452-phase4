#include <usyscall.h>
#include <phase1.h>
#include <phase2.h>
#include <libuser4.h>
#include <usloss.h>

#define CHECKMODE {    \
    if (USLOSS_PsrGet() & USLOSS_PSR_CURRENT_MODE) { \
        USLOSS_Console("Trying to invoke syscall from kernel\n"); \
        USLOSS_Halt(1);  \
    }  \
}

/*
 *  Routine:
 *
 *  Description:
 *
 *  Arguments:
 *
 *  Return Value: 
 *
 */
int Sleep(int seconds) {
     USLOSS_Sysargs sysArg;
    CHECKMODE;
    sysArg.number = SYS_SLEEP;
    sysArg.arg1 = (void *) ((long) seconds);

    USLOSS_Syscall(&sysArg);

    return (long) sysArg.arg4;
}

/*
 *  Routine:
 *
 *  Description:
 *
 *  Arguments:
 *
 *  Return Value: 
 *
 */
int DiskRead(void *diskBuffer, int unit, int track, int first, int sectors, int *status) {
    USLOSS_Sysargs sysArg;
    CHECKMODE;
    sysArg.number = SYS_DISKREAD;
    sysArg.arg1 = diskBuffer;
    sysArg.arg2 = (void *) ((long) sectors);
    sysArg.arg3 = (void *) ((long) track);
    sysArg.arg4 = (void *) ((long) first);
    sysArg.arg5 = (void *) ((long) unit);

    USLOSS_Syscall(&sysArg);

    *status = (long) sysArg.arg1;
    return (long) sysArg.arg4;
}

/*
 *  Routine:
 *
 *  Description:
 *
 *  Arguments:
 *
 *  Return Value: 
 *
 */
int DiskWrite(void *diskBuffer, int unit, int track, int first, int sectors, int *status) {
    USLOSS_Sysargs sysArg;
    CHECKMODE;
    sysArg.number = SYS_DISKWRITE;
    sysArg.arg1 = diskBuffer;
    sysArg.arg2 = (void *) ((long) sectors);
    sysArg.arg3 = (void *) ((long) track);
    sysArg.arg4 = (void *) ((long) first);
    sysArg.arg5 = (void *) ((long) unit);

    USLOSS_Syscall(&sysArg);

    *status = (long) sysArg.arg1;
    return (long) sysArg.arg4;
}

/*
 *  Routine:
 *
 *  Description:
 *
 *  Arguments:
 *
 *  Return Value: 
 *
 */
int DiskSize (int unit, int *sector, int *track, int *disk) {
    USLOSS_Sysargs sysArg;
    CHECKMODE;
    sysArg.number = SYS_DISKSIZE;
    sysArg.arg1 = (void *) ((long) unit);

    USLOSS_Syscall(&sysArg);

    *sector = (long) sysArg.arg1;
    *track = (long) sysArg.arg2;
    *disk = (long) sysArg.arg3;
    return (long) sysArg.arg4;
}

/*
 *  Routine:
 *
 *  Description:
 *
 *  Arguments:
 *
 *  Return Value: 
 *
 */
int TermRead (char *buffer, int bufferSize, int unitID, int *numCharsRead) {
    USLOSS_Sysargs sysArg;
    CHECKMODE;
    sysArg.number = SYS_TERMREAD;
    sysArg.arg1 = buffer;
    sysArg.arg2 = (void *) ((long) bufferSize);
    sysArg.arg3 = (void *) ((long) unitID);

    USLOSS_Syscall(&sysArg);

    *numCharsRead = (long) sysArg.arg2;
    return (long) sysArg.arg4;
}

/*
 *  Routine:
 *
 *  Description:
 *
 *  Arguments:
 *
 *  Return Value: 
 *
 */
int TermWrite(char *buffer, int bufferSize, int unitID, int *numCharsRead) {
    USLOSS_Sysargs sysArg;
    CHECKMODE;
    sysArg.number = SYS_TERMWRITE;
    sysArg.arg1 = buffer;
    sysArg.arg2 = (void *) ((long) bufferSize);
    sysArg.arg3 = (void *) ((long) unitID);

    USLOSS_Syscall(&sysArg);

    *numCharsRead = (long) sysArg.arg2;
    return (long) sysArg.arg4;
}

/* end libuser4.c */
