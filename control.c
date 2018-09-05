#include <strings.h>
#include <string.h>
#include <opus/opus.h>
#include <ogg/ogg.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include "control.h"
#include "log.h"

struct sockaddr_in control_addr;
int control_addrlen, control_sock;
char * control_receive_buffer;
int control_cmds_size=3;

struct txDest {
	OpusEncoder *oe;
	ogg_stream_state os;

	int serial;
	int bitrate;
	int pagesize;
	char dest[64];	

	unsigned char * encodedBuffer;
	unsigned char * txBuffer;

	struct sockaddr_in addr;
	struct in_addr sender_dst;

	int addrlen;
	int sock; 
	int cnt;
	
	// ok we are doing a linked list.

	struct txDest * next;
	struct txDest * prior;
};

struct control_cmd {
	const char * cmd;
	void (*cmd_fn)(int,char *(*)[],char*);
};

struct control_cmd control_cmds[]={
	{"ANNOUNCE",control_announce},
	{"SEND",control_send},
	{"STOP",control_stop}
};
void control_setupSocket() {
	/* set up receive socket */
	control_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (control_sock < 0) {
		log_error("cannot set up control socket");
		exit(1);
	}

	/* clear ohe structure */
	bzero((char *)&control_addr, sizeof(struct sockaddr_in));

	/* set the address structure */
	control_addr.sin_family = AF_INET;
	control_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	control_addr.sin_port = htons(CONTROL_PORT);
	control_addrlen = sizeof(struct sockaddr_in);

	/* make it non-blocking */
	int flags = fcntl(control_sock, F_GETFL);
	flags |= O_NONBLOCK;
	fcntl(control_sock, F_SETFL, flags);

	/* bind the socket to the address */
	if (bind(control_sock, (struct sockaddr *) &control_addr, sizeof(struct sockaddr)) < 0) {
		log_error("did not bind to control socket");
		exit(1);
	}

}

void control_parseMessage(char * buf,int bufSize, int * pnArgs,char *(*pArgs)[]) {
	int nArgs=0,i=0;
	char *Arg;

	Arg=buf;

	while (1) {
		if (*buf==':') *buf=0;
		if (*buf==0) {
			(*pArgs)[nArgs++]=Arg;
			Arg=buf+1;
		}
		buf++;
		if (i++>=bufSize) break;
		if (nArgs==10) break;
	}
	(*pnArgs)=nArgs;
}


struct txDest * control_getLastTxDest(struct txDest * first) {
	while (first->next!=NULL)
		first++;
	return first;
}

struct txDest * control_getTxDestByDest(struct txDest * first, const char * dest) {
	while (first!=NULL) {
		if (strcmp(first->dest,dest)==0) 
			return first;
		first=first->next;
	}
	return NULL;
}

struct txDest * control_addTxDestToEnd(struct txDest ** llist, char * dest,int bitrate,int serial) {
	uint8_t oerr;
	int oeErr;


	struct txDest * td=calloc(sizeof(struct txDest),1);
	log_debug ("creating Dest element %0x for linked list for serial %u to dest %s at bitrate %u",td,serial,dest,bitrate);

	td->oe=opus_encoder_create(SAMPLE_RATE,2,OPUS_APPLICATION_AUDIO,&oeErr);
	oerr=opus_encoder_init(td->oe,SAMPLE_RATE,2,OPUS_APPLICATION_AUDIO);
	oerr=opus_encoder_ctl(td->oe,OPUS_SET_PACKET_LOSS_PERC(OPUS_PACKETLOSS));
	oerr=opus_encoder_ctl(td->oe,OPUS_SET_BITRATE(bitrate));
	if (ogg_stream_init(&td->os,serial)==-1) {
		log_error("ogg_stream_init failed for serial %u",serial);
	}

	td->encodedBuffer=calloc(sizeof(char),ENCODED_BUFFER_SIZE);
	td->txBuffer=calloc(sizeof(char),TXBUFFER_SIZE);

	td->serial = serial;
	td->bitrate = bitrate;
	td->pagesize = 0;

	strncpy(td->dest,dest,64);
	inet_aton(dest,&td->sender_dst);

	control_setupStreamingSocket(&td->sock, &td->addr, &td->addrlen, &td->sender_dst);

	td->next=NULL;

	if (txDestList==NULL) {
		(*llist)=td; 
		td->prior=NULL;
	} else {
		td->prior=control_getLastTxDest(*llist); 
		td->prior->next=td;
	}	

	return td;	
}

void control_removeTxDest(struct txDest ** llist, struct txDest * td) {
	
	log_debug("removing element %0x from the Dest linked list",td);

	struct txDest * next=td->next;
	struct txDest * prior=td->prior;

	// first one
	if (td->prior==NULL) {
		if (td->next == NULL)
			(*llist)=NULL;
		else {
			td->next->prior=NULL;
			(*llist)=td->next;
		}
	// not the first one
	} else {
		if (td->next==NULL)  {
			td->prior->next=NULL;
		} else {
			td->prior->next=td->next;
			td->next->prior=td->prior;
		}
	}

	ogg_stream_clear(&td->os);
	opus_encoder_destroy(td->oe);

	free(td->txBuffer);
	free(td->encodedBuffer);

	free(td);
	log_debug("done freeing Dest Structure");
}

void control_announce(int nArgs, char *(*pArgs)[],char * sender) {
	log_info("inside Control:Announce command function. responding to %s",sender);

	inet_aton(sender,&app_in_addr);
	app_addr.sin_addr = app_in_addr;
	
	sendto(app_sock,"hellofromme",11,0,(struct sockaddr *) &app_addr,app_addrlen);
}

void control_send(int nArgs, char *(*pArgs)[], char * sender) {
	log_info("inside Control:Send command function. responding to %s",sender);
	char * dest=(*pArgs)[1];
	int bitrate=atoi((*pArgs)[2]);
	int serial=atoi((*pArgs)[3]);
	control_addTxDestToEnd(&txDestList, dest,bitrate,serial);
}

void control_stop(int nArgs, char *(*pArgs)[], char * sender) {
	log_info("inside Control:Stop command function. responding to %s",sender);
	char * dest=(*pArgs)[1];
	struct txDest * td=control_getTxDestByDest(txDestList,dest);
	if (td)
		control_removeTxDest(&txDestList,td) ;
	else
		log_error("TX Dest Structure not found for %s",dest);
}

void
control_processMessage(int nArgs,char *(*pArgs)[],char * sender) {
	if (nArgs==0)
		return;

	log_debug("processing message from %s",sender);

	for (int i=0;i<control_cmds_size;i++) {
		if (strcmp((*pArgs)[0],control_cmds[i].cmd)==0) {
			(*control_cmds[i].cmd_fn)(nArgs,pArgs,sender);
		}
	}
}

char * control_receive_buffer;
#define CONTROL_RECEIVE_BUFFER_SIZE 65536
void * control_thread_function(void * args) {
	control_setupSocket();
	control_receive_buffer=calloc(sizeof(char),CONTROL_RECEIVE_BUFFER_SIZE);

	struct timeval to;
	fd_set rfds;
	int rc;
	long cnt;

	int nArgs;
	char * Args[16]; //will end up as pointers within control_receive_buffer

	log_info("Control Thread Started");
	
	while (1) {

		to.tv_sec=1;
		to.tv_usec=0;

		FD_ZERO(&rfds);
		FD_SET(control_sock,&rfds);

		rc=select(control_sock+1,&rfds,NULL,NULL,&to);
		memset(control_receive_buffer,0,CONTROL_RECEIVE_BUFFER_SIZE);
		cnt=recvfrom(control_sock,control_receive_buffer,CONTROL_RECEIVE_BUFFER_SIZE,0,(struct sockaddr *) &control_addr, &control_addrlen);
		if (cnt==-1) {
			continue;
		}
		log_info("Packet Received:%s from %s",control_receive_buffer,inet_ntoa(control_addr.sin_addr));
		control_parseMessage(control_receive_buffer,cnt,&nArgs,&Args);
		log_info("Number of Commands/Parameters in packet: %u",nArgs);
		for (int i=0;i<nArgs;i++) {
			log_info("Command %u is %s len %u",i,Args[i],strlen(Args[i]));
		}
		//log_info("inside control_thread_function with %0x",Args);
		control_processMessage(nArgs,&Args,inet_ntoa(control_addr.sin_addr));
	}
}
