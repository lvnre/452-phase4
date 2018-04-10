typedef struct procStructure procStructure;
typedef struct procStructure * procStructPtr;
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
  procStructPtr head;
  procStructPtr tail;
  int     length;
  int     type;
  procStructPtr current;
};

struct procStructure {
  int                  pid;
  int 		       mboxID; 
  int                  blockedSem;
  int		       time;
  void 		       *diskBuffer;
  procStructPtr        prevDisk;
  procStructPtr        nextDisk;
  int 		       track;
  int 		       firstSector;
  int 		       sectors;
  USLOSS_DeviceRequest request;
};

struct pQueue {
  procStructPtr processes[MAXPROC];
  int size;
};



