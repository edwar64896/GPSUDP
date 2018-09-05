



#if !defined __GSPUDP_CONTROL__
#define __GSPUDP_CONTROL__
#define CONTROL_PORT 49877 //we send packets to this port to control TX and RX's

struct txDest;

void control_setupSocket() ;
void control_parseMessage(char * buf,int bufSize, int * pnArgs,char *(*pArgs)[]) ;
void * control_thread_function(void * args) ;
void control_processMessage(int nArgs,char *(*pArgs)[],char * sender) ;
void control_stop(int nArgs, char *(*pArgs)[], char * sender) ;
void control_send(int nArgs, char *(*pArgs)[], char * sender) ;
void control_announce(int nArgs, char *(*pArgs)[],char * sender) ;
void control_removeTxDest(struct txDest ** llist, struct txDest * td) ;
struct txDest * control_addTxDestToEnd(struct txDest ** llist, char * dest,int bitrate,int serial) ;
struct txDest * control_getTxDestByDest(struct txDest * first, const char * dest) ;
struct txDest * control_getLastTxDest(struct txDest * first) ;

#endif //__GSPUDP_CONTROL__
