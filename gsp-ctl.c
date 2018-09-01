#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "log.h"

#define MC_PORT 49876
#define CONTROL_PORT MC_PORT+1
#define APP_CONTROL_PORT MC_PORT+2

struct sockaddr_in addr;
int sock;
pthread_t receive_thread_id;

struct sockaddr_in control_addr;
int control_addrlen, control_sock;

/* incoming packets */
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
	control_addr.sin_port = htons(APP_CONTROL_PORT);
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
void
receive_parseMessage(char * buf,int bufSize, int * pnArgs,char *(*pArgs)[]) {
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

void
receive_processMessage(int nArgs,char *(*pArgs)[],char * sender) {
	if (nArgs==0)
		return;
	if (nArgs==1) {
		if (strcmp((*pArgs)[0],"HELLOFROM")==0) {
			//processHelloFrom(sender);
		}
	}
}
char * control_receive_buffer;
#define CONTROL_RECEIVE_BUFFER_SIZE 65536
void * receive_thread_function(void * args) {
	control_setupSocket();
	control_receive_buffer=calloc(sizeof(char),CONTROL_RECEIVE_BUFFER_SIZE);

	struct timeval to;
	fd_set rfds;
	int rc;
	long cnt;

	int nArgs;
	char * Args[16]; //will end up as pointers within control_receive_buffer

	log_info("Receieve Thread Started");
	while (1) {

		to.tv_sec=1;
		to.tv_usec=0;

		FD_ZERO(&rfds);
		FD_SET(control_sock,&rfds);

		rc=select(control_sock+1,&rfds,NULL,NULL,&to);
		memset(control_receive_buffer,0,CONTROL_RECEIVE_BUFFER_SIZE);
		cnt=recvfrom(control_sock,control_receive_buffer,CONTROL_RECEIVE_BUFFER_SIZE,0,(struct sockaddr *
					) &control_addr, &control_addrlen);
		if (cnt==-1) {
			continue;
		}
		log_info("Packet Received:%s from %s",control_receive_buffer,inet_ntoa(control_addr.sin_addr));
		receive_parseMessage(control_receive_buffer,cnt,&nArgs,&Args);
		log_info("Number of Commands/Parameters in packet: %u",nArgs);
		for (int i=0;i<nArgs;i++) {
			log_info("Command %u is %s len %u",i,Args[i],strlen(Args[i]));
		}
		log_info("inside control_thread_function with %0x",Args);
		receive_processMessage(nArgs,&Args,inet_ntoa(control_addr.sin_addr));
	}
}

/*
 * output Transmisison Socket
 */
void setupSocket() {
	/* set up socket */

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	int broadcastEnable=1;
	int ret=setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
	if (sock < 0) {
		perror("socket");
		exit(1);
	}
	bzero((char *)&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	addr.sin_port = htons(CONTROL_PORT);
}

// Note: This function returns a pointer to a substring of the original string.
// If the given string was allocated dynamically, the caller must not overwrite
// that pointer with the returned value, since the original pointer must be
// deallocated using the same allocator with which it was allocated.  The return
// value must NOT be deallocated using free() etc.
char *trimwhitespace(char *str)
{
	char *end;

	// Trim leading space
	while(isspace((unsigned char)*str)) str++;

	if(*str == 0)  // All spaces?
		return str;

	// Trim trailing space
	end = str + strlen(str) - 1;
	while(end > str && isspace((unsigned char)*end)) end--;

	// Write new null terminator character
	end[1] = '\0';

	return str;
}

int main (int argc,char *argv[]) {

	char data[64];
	char *tdata;
	printf("GSP Control App!\n");
	setupSocket();
	pthread_create(&receive_thread_id,NULL,receive_thread_function,NULL);

	while (1) {
		memset(data,0,sizeof(data));
		if (fgets(data, sizeof(data), stdin)) {
			tdata=trimwhitespace(data);
			if (strlen(tdata))
				sendto(sock,tdata,strlen(tdata),0,(struct sockaddr *) &addr,sizeof(struct sockaddr_in));
		}
	}
}
