typedef struct proc proc;
typedef struct proc * procPtr;
typedef struct diskList diskList;
typedef struct pQueue pQueue;

struct psrBits {
    unsigned int curMode:1;
    unsigned int curIntEnable:1;
    unsigned int prevMode:1;
    unsigned int prevIntEnable:1;
    unsigned int unused:28;
};

union psrValues {
    struct psrBits bits;
    unsigned int integerPart;
};

struct diskList {
  procPtr head;
  procPtr tail;
  int     length;
  int     type;
  procPtr current;
};

struct proc {
  int                  pid;
  int 		       mboxID; 
  int                  blockedSem;
  int		       time;
  void 		       *diskBuffer;
  procPtr 	       prevDisk;
  procPtr 	       nextDisk;
  int 		       track;
  int 		       firstSector;
  int 		       sectors;
  USLOSS_DeviceRequest request;
};

struct pQueue {
  procPtr processes[MAXPROC];
  int size;
};



