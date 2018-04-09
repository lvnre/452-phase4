typedef struct proc proc;
typedef struct proc * procPtr;
typedef struct diskList diskList;
typedef struct pQueue pQueue;

struct diskList {
  procPtr head;
  procPtr tail;
  int     lenght;
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



