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
#include <time.h>
#include <stdio.h>
#include "gspudp.h"
#include "log.h"
#include "portaudio.h"

#define MC_PORT 49876
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

OpusEncoder *oe;

ogg_stream_state os;
ogg_packet op;
ogg_page og;
ogg_int64_t packetno=0;
ogg_int64_t gpos=0;

uint8_t oerr;
uint8_t pagesize=0;

typedef void sigfunc(int);

int arg_pagesize=OGG_PAGESIZE;
int arg_device=PA_DEVICE;
int arg_framesPerBuffer=FRAMES_PER_BUFFER;
int arg_bitrate=OPUS_BITRATE;
int arg_packetloss=OPUS_PACKETLOSS;
int arg_verbose=0;
int arg_rtp=0;
int arg_port=MC_PORT;
int arg_streamSerial=12345;
int c;
struct in_addr arg_dst;

unsigned char * encodedBuffer=0;
unsigned char * txbuffer=0;

struct sockaddr_in addr;
int addrlen, sock, cnt;
struct ip_mreq mreq;

PaStream *stream;

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


void timespec_diff(struct timespec *start, struct timespec *stop,
		struct timespec *result)
{
	if ((stop->tv_nsec - start->tv_nsec) < 0) {
		result->tv_sec = stop->tv_sec - start->tv_sec - 1;
		result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
	} else {
		result->tv_sec = stop->tv_sec - start->tv_sec;
		result->tv_nsec = stop->tv_nsec - start->tv_nsec;
	}
	return;
}

uint64_t timespec_to_uint64(struct timespec input) {
	return (input.tv_sec*1e09)+input.tv_nsec;
}


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

void setupSocket() {
	/* set up socket */
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket");
		exit(1);
	}
	bzero((char *)&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	//addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_addr = arg_dst;
	addr.sin_port = htons(arg_port);
	addrlen = sizeof(addr);
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

uint64_t getSecondsSinceMidnight(struct tm * tm) {
	uint64_t rc=
		(tm->tm_sec 		+
		 (tm->tm_min * 60)	+
		 (tm->tm_hour * 3600));
	return rc;
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
				ssmSamples=callbackTime*48000;
				log_info("Stream Transmitting....");
			} else
				return 0;
		}
	}

	if (paCallbackRunning) {

		ssmSamples+=framesPerBuffer;

		memset(encodedBuffer,0,ENCODED_BUFFER_SIZE);
		memset(txbuffer,0,TXBUFFER_SIZE);
		memset(&op,0,sizeof(ogg_packet));

		oBufSize=opus_encode_float(oe,(const float *)inputBuffer,framesPerBuffer,encodedBuffer,ENCODED_BUFFER_SIZE);

		//gpos+=framesPerBuffer;

		op.packet=encodedBuffer;
		op.bytes=oBufSize;
		op.b_o_s=(packetno==0?1:0);
		op.e_o_s=0;
		op.granulepos=ssmSamples;
		op.packetno=packetno++;

		ogg_stream_packetin(&os,&op);
		pagesize = ((pagesize+1) % arg_pagesize);

		if (pagesize == 0 && ogg_stream_flush(&os,&og)) {
			memcpy(txbuffer,og.header,og.header_len);
			memcpy(txbuffer+og.header_len,og.body,og.body_len);
			sendto(sock,txbuffer,og.header_len+og.body_len,0,(struct sockaddr *) &addr,addrlen);
		}
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
	printf("-s <%pkt loss>	Percentage Packet Loss\n");
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

	inet_aton(MC_GROUP,&arg_dst); // destination IP. we use strncpy to avoid buffer overrun.

	while ((opt = getopt(argc, argv, "hlf:d:o:e:g:r:s:t:")) != -1) {
		switch (opt) {
			case 'e': //Stream Serial number 
				arg_streamSerial = atoi(optarg);
				break;
			case 'f': //Frames Per Buffer 
				arg_framesPerBuffer = atoi(optarg);
				break;
			case 'd': //device ID
				arg_device = atoi(optarg);
				break;
			case 't': //destination ip
				inet_aton(optarg,&arg_dst);
				break;
			case 'o': //destination port
				arg_port = atoi(optarg);
				break;
			case 'r': //opus bitrate
				arg_bitrate = atoi(optarg);
				break;
			case 's': //Percentage Packetloss
				arg_packetloss = atoi(optarg);
				break;
			case 'g': //packets in page size
				arg_pagesize = atoi(optarg);
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
	txbuffer=(unsigned char *)calloc(sizeof(unsigned char),TXBUFFER_SIZE);

	setupSocket();

	memset(&inputParameters,0,sizeof(PaStreamParameters));
	memset(&outputParameters,0,sizeof(PaStreamParameters));

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
	/* -- setup stream -- */
	err = Pa_OpenStream(
			&stream,
			&inputParameters,
			&outputParameters,
			//	NULL,
			SAMPLE_RATE,
			arg_framesPerBuffer,
			paClipOff,      /* we won't output out of range samples so don't bother clipping them */
			paCallback, /* no callback, use blocking API */
			NULL ); /* no callback, so no callback userData */
	if( err != paNoError ) goto error;
	/* Open an audio I/O stream. */

	//oe=opus_encoder_create(SAMPLE_RATE,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&oeErr);
	oe=opus_encoder_create(SAMPLE_RATE,2,OPUS_APPLICATION_AUDIO,&oeErr);
	oerr=opus_encoder_ctl(oe,OPUS_SET_PACKET_LOSS_PERC(arg_packetloss));
	oerr=opus_encoder_ctl(oe,OPUS_SET_BITRATE(arg_bitrate));

	//opus_encoder_init(oe,SAMPLE_RATE,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY);
	opus_encoder_init(oe,SAMPLE_RATE,2,OPUS_APPLICATION_AUDIO);
	ogg_stream_init(&os,arg_streamSerial);	

	log_info("Preparing to start Stream...");
	err = Pa_StartStream(stream);
	if (err != paNoError) goto error;


	while (1) {
	system("/bin/stty raw");
		c=getchar();
	system("/bin/stty cooked");
		switch (c) {
			case 'q':
				cleanup();
				exit(0);
				break;
			case '_':
			case '-':
				arg_bitrate-=1000;
				log_info("Opus Bitrate:			%u",arg_bitrate) ;
				oerr=opus_encoder_ctl(oe,OPUS_SET_BITRATE(arg_bitrate));
				break;
			case '=':
			case '+':
				arg_bitrate+=1000;
				log_info("Opus Bitrate:			%u",arg_bitrate) ;
				oerr=opus_encoder_ctl(oe,OPUS_SET_BITRATE(arg_bitrate));
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
	ogg_stream_destroy(&os);
	opus_encoder_destroy(oe);
	Pa_CloseStream(stream);
	Pa_Terminate();
	system("/bin/stty cooked");
}

void printOptions() {
	log_info("Frames Per Buffer:	%u",arg_framesPerBuffer) ;
	log_info("Opus Bitrate:		%u",arg_bitrate) ;
	log_info("Packet Loss:		%u",arg_packetloss) ;
	log_info("Page Size:		%u",arg_pagesize) ;
	log_info("Device Name:		%s",Pa_GetDeviceInfo(arg_device)->name) ;
	log_info("Stream Serial:	%u",arg_streamSerial);
	log_info("Destination IP:	%s",inet_ntoa(arg_dst));
	log_info("Destination Port:	%u",arg_port );
}
