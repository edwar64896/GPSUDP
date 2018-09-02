#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <opus/opus.h>
#include <ogg/ogg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>

#include "timef.h"
#include "log.h"
#include "portaudio.h"

#define MC_PORT 49876 //audio RX port
#define CONTROL_PORT MC_PORT+1 //we send packets to this port to control TX and RX's
#define APP_CONTROL_PORT MC_PORT+2 //TX and RX send packets to this port to respond to APP Control
#define MC_GROUP "172.23.1.120"

#define SAMPLE_RATE 48000
#define ENCODED_BUFFER_SIZE 16384
#define FRAMES_PER_BUFFER 480
#define TXBUFFER_SIZE 16384
#define NUM_CHANNELS 2
#define PA_SAMPLE_TYPE paFloat32
#define PA_DEVICE 2
#define OGG_PAGESIZE 2
#define OPUS_BITRATE 128000
#define OPUS_PACKETLOSS 10

struct sigaction sa;

void cleanup();
void printOptions();

//OpusEncoder *oe;
struct txDest * txDestList=NULL;

//ogg_stream_state os;
ogg_packet op;
ogg_page og;
ogg_int64_t packetno=0;
ogg_int64_t gpos=0;

uint8_t oerr;
uint8_t pagesize=0;

typedef void sigfunc(int);

int arg_pagesize=OGG_PAGESIZE;
int arg_device=PA_DEVICE;
int arg_bitrate=OPUS_BITRATE;
int arg_packetloss=OPUS_PACKETLOSS;
int arg_verbose=0;
int arg_rtp=0;
int arg_port=MC_PORT;
int arg_streamSerial=12345;
int c;

unsigned char * encodedBuffer=0;
unsigned char * txBuffer=0;

//struct sockaddr_in addr;
//int addrlen, sock, cnt;
struct ip_mreq mreq;

PaStream *stream;
pthread_t control_thread_id;


void sig_handler(int signo)
{
	log_info("Terminating...");
	cleanup();
	system ("/bin/stty cooked");
	exit(0);
}

typedef struct
{
	float sLeft;
	float sRight;
}   
paSample;

typedef int PaStreamCallback( const void *input,
		void *output,
		unsigned long frameCount,
		const PaStreamCallbackTimeInfo* timeInfo,
		PaStreamCallbackFlags statusFlags,
		void *userData ) ;


int getAudioDevicesCount(int * nDevices) {
	int numDevices;
	PaError err;
	numDevices = Pa_GetDeviceCount();
	if( numDevices < 0 )
	{
		log_error( "ERROR: Pa_CountDevices returned 0x%x", numDevices );
		err = numDevices;
		return err;
	} else {
		(*nDevices)=numDevices;
		return 0;
	}
}

struct sockaddr_in app_addr;
int app_addrlen, app_sock;
struct in_addr app_in_addr;

void setupAppControlSocket() {
	/* set up socket */
	app_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (app_sock < 0) {
		log_error("failed to create app_socket");
		exit(1);
	}
	bzero((char *)&app_addr, sizeof(app_addr));
	app_addr.sin_family = AF_INET;
	app_addr.sin_port = htons(APP_CONTROL_PORT);
	app_addrlen = sizeof(app_addr);
}

void control_setupStreamingSocket(int * sock, struct sockaddr_in * addr, int * addrlen, struct in_addr * sender_dst) {
	/* set up socket */
	*sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (*sock < 0) {
		log_error("socket setup failure (setupSocket)");
		exit(1);
	}
	bzero((char *)addr, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_addr = *sender_dst;
	addr->sin_port = htons(MC_PORT);
	*addrlen = sizeof(*addr);
}

void  printAudioDevices() {
	int nDevices=0;
	const PaDeviceInfo *device;
	getAudioDevicesCount(&nDevices);
	for (int i=0;i<nDevices;i++) {
		device=Pa_GetDeviceInfo(i);
		log_info("Device No: %i:%s",i,device->name);
	}

}

struct sockaddr_in control_addr;
int control_addrlen, control_sock;

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

void
control_parseMessage(char * buf,int bufSize, int * pnArgs,char *(*pArgs)[]) {
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

void
control_removeTxDest(struct txDest ** llist, struct txDest * td) {
	
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

struct control_cmd {
	const char * cmd;
	void (*cmd_fn)(int,char *(*)[],char*);
};

struct control_cmd control_cmds[]={
	{"ANNOUNCE",control_announce},
	{"SEND",control_send},
	{"STOP",control_stop}
};
int control_cmds_size=3;

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

uint64_t ssm=0;
uint64_t ssmSamples=0;
uint8_t paCallbackRunning=0;

static int paCallback( const void *inputBuffer, void *outputBuffer,
		unsigned long framesPerBuffer,
		const PaStreamCallbackTimeInfo* timeInfo,
		PaStreamCallbackFlags statusFlags,
		void *userData )
{
	unsigned int i;
	opus_int32 oBufSize;
	struct timespec tStart;
	struct tm tmStart;
	uint64_t callbackTime;

	// populate the start time 
	//
	// wait for a second boundary.

	if (!paCallbackRunning) {
		if (ssm==0) {

			/*
			 * first time around we grab Seconds Since Midnight
			 * and populate ssm
			 *
			 * Next time around we check to see if we have passed
			 * a second boundary.
			 */
			clock_gettime(CLOCK_REALTIME,&tStart);
			localtime_r(&tStart.tv_sec,&tmStart);
			ssm=getSecondsSinceMidnight(&tmStart);
			return 0;
		} else {
			/*
			 * second time around the callback - 
			 * have we passed a boundary?
			 */
			clock_gettime(CLOCK_REALTIME,&tStart);
			localtime_r(&tStart.tv_sec,&tmStart);
			callbackTime=getSecondsSinceMidnight(&tmStart);
			if (callbackTime>ssm) {
				paCallbackRunning=1;
				ssm=callbackTime;
				ssmSamples=callbackTime*SAMPLE_RATE;
			} else
				return 0;
		}
	}

	if (paCallbackRunning) {

		ssmSamples+=framesPerBuffer;

		struct txDest * txDestList_iterator=txDestList;

		while (txDestList_iterator != NULL) {

			memset(txDestList_iterator->encodedBuffer,0,ENCODED_BUFFER_SIZE);
			memset(txDestList_iterator->txBuffer,0,TXBUFFER_SIZE);
			memset(&op,0,sizeof(ogg_packet));

			oBufSize=opus_encode_float(txDestList_iterator->oe,(const float *)inputBuffer,framesPerBuffer,txDestList_iterator->encodedBuffer,ENCODED_BUFFER_SIZE);

			op.packet=txDestList_iterator->encodedBuffer;
			op.bytes=oBufSize;
			op.b_o_s=(packetno==0?1:0);
			op.e_o_s=0;
			op.granulepos=ssmSamples;
			op.packetno=packetno++;

			ogg_stream_packetin(&txDestList_iterator->os,&op);
			txDestList_iterator->pagesize = ((txDestList_iterator->pagesize+1) % arg_pagesize);

			if (txDestList_iterator->pagesize == 0 && ogg_stream_flush(&txDestList_iterator->os,&og)) {
				memcpy(txDestList_iterator->txBuffer,og.header,og.header_len);
				memcpy(txDestList_iterator->txBuffer+og.header_len,og.body,og.body_len);
				sendto(txDestList_iterator->sock,txDestList_iterator->txBuffer,og.header_len+og.body_len,0,(struct sockaddr *) &txDestList_iterator->addr,txDestList_iterator->addrlen);
			}

			txDestList_iterator=txDestList_iterator->next;
		}


		//gpos+=framesPerBuffer;


	}

	return 0;
}

void displayHelp() {
	printf("GSP Opus Streamer 0.1 2018\n\n");
	printf("-h 		Show Help\n");
	printf("-v		Verbose\n");
	printf("-l		List Devices\n");
	printf("-e <serial>	Stream Serial Number\n");
	printf("-d <device>	Device ID\n");
	printf("-t <dest>	Destination IP\n");
	printf("-o <port>	Destination PORT\n");
	printf("-f <frames>	Frames Per Buffer\n");
	printf("-r <bitrate>	Opus Bitrate\n");
	printf("-s <%%pkt loss>	Percentage Packet Loss\n");
	printf("-g <packets>	Opus Packets Per Ogg Page\n");
}

int main(int argc,char *argv[]) {

	sa.sa_handler=sig_handler;
	sigaction(SIGINT,&sa,NULL);

	PaError err;
	static paSample data;
	PaStreamParameters inputParameters;
	PaStreamParameters outputParameters;
	int oeErr;
	int opt;

	//inet_aton(MC_GROUP,&arg_dst); // destination IP. we use strncpy to avoid buffer overrun.

	while ((opt = getopt(argc, argv, "hld:e:")) != -1) {
		switch (opt) {
			case 'e': //Stream Serial number 
				arg_streamSerial = atoi(optarg);
				break;
			case 'd': //device ID
				arg_device = atoi(optarg);
				break;
			case 'l': //Audio Devices
				Pa_Initialize();
				printAudioDevices();
				Pa_Terminate();
				exit(0);
				break;
			case 'h': //help
				displayHelp();
				exit(0);
				break;
			default:
				break;
		}
	}

	Pa_Initialize();

	printOptions();

	encodedBuffer=(unsigned char *)calloc(sizeof(unsigned char),ENCODED_BUFFER_SIZE);
	txBuffer=(unsigned char *)calloc(sizeof(unsigned char),TXBUFFER_SIZE);

	setupAppControlSocket();


	log_info("App Control Socket Available");
	memset(&inputParameters,0,sizeof(PaStreamParameters));
	memset(&outputParameters,0,sizeof(PaStreamParameters));
	log_info("Pa Params Cleared");

	/* -- setup input and output -- */
	inputParameters.device = arg_device;
	inputParameters.channelCount = NUM_CHANNELS;
	inputParameters.sampleFormat = PA_SAMPLE_TYPE;
	inputParameters.suggestedLatency = Pa_GetDeviceInfo( inputParameters.device )->defaultLowInputLatency ;
	inputParameters.hostApiSpecificStreamInfo = NULL;
	outputParameters.device = arg_device;
	outputParameters.channelCount = NUM_CHANNELS;
	outputParameters.sampleFormat = PA_SAMPLE_TYPE;
	outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowInputLatency ;
	outputParameters.hostApiSpecificStreamInfo = NULL;
	
	log_info("Pa Params Populated");
	/* -- setup stream -- */
	err = Pa_OpenStream(
			&stream,
			&inputParameters,
			NULL,
			SAMPLE_RATE,
			FRAMES_PER_BUFFER,
			paClipOff,      /* we won't output out of range samples so don't bother clipping them */
			paCallback, /* no callback, use blocking API */
			NULL ); /* no callback, so no callback userData */

	log_debug("Pa_OpenStream error %u",err);

	if( err != paNoError ) goto error;
	/* Open an audio I/O stream. */

	log_debug ("Audio Stream Open");

	pthread_create(&control_thread_id,NULL,&control_thread_function,NULL) ;

	log_info("Preparing to start Stream...");
	err = Pa_StartStream(stream);
	if (err != paNoError) goto error;

	while (1) {
		c=getchar();
		switch (c) {
			case 'q':
				cleanup();
				exit(0);
				break;
			case 'o':
				printOptions();
				break;
		}
	}

error:

	exit(0);

}

void cleanup() {
	//ogg_stream_destroy(&os);
	//opus_encoder_destroy(oe);
	Pa_CloseStream(stream);
	Pa_Terminate();
	system("/bin/stty cooked");
}

void printOptions() {
	log_info("Device Name:		%s",Pa_GetDeviceInfo(arg_device)->name) ;
	log_info("Stream Serial:	%u",arg_streamSerial);
}
