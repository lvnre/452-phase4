typedef struct proc proc;
typedef struct proc * procPtr;
typedef struct diskList diskList;
typedef struct pQueue pQueue;

struct diskList {
  procPtr head;
  procPtr tail;
  int     lenght;
  int     type;
};

struct proc {
  int                  pid;
  int 		       mboxID; 
  int                  blockedSem;
  int		       wakeTime;
  void 		       *diskBuffer;
  procPtr 	       prevDisk;
  procPtr 	       nextDisk;
  int 		       track;
  int 		       firstSector;
  int 		       sectors;
  USLOSS_DeviceRequest diskRequest;
};

struct pQueue {
  procPtr processes[MAXPROC];
  int size;
};



