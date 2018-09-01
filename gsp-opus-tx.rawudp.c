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
#include "portaudio.h"

#define MC_PORT 60000
//#define MC_GROUP "239.0.0.1"
#define MC_GROUP "172.23.1.120"

#define SAMPLE_RATE 48000
#define ENCODED_BUFFER_SIZE 16384
#define FRAMES_PER_BUFFER 480
#define TXBUFFER_SIZE 16384
#define NUM_CHANNELS 2
#define PA_SAMPLE_TYPE paFloat32
#define PA_DEVICE 2
#define OGG_PAGESIZE 4
#define OPUS_BITRATE 256000
#define OPUS_PACKETLOSS 50

struct sigaction sa;

void cleanup();

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

unsigned char * encodedBuffer=0;
unsigned char * txbuffer=0;

struct sockaddr_in addr;
int addrlen, sock, cnt;
struct ip_mreq mreq;
char message[50];

PaStream *stream;

void sig_handler(int signo)
{
	printf("\n\nTerminating...\n");
	cleanup();
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
		printf( "ERROR: Pa_CountDevices returned 0x%x\n", numDevices );
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
	addr.sin_addr.s_addr = inet_addr(MC_GROUP);
	addr.sin_port = htons(MC_PORT);
	addrlen = sizeof(addr);
}

void  printAudioDevices() {
	int nDevices=0;
	PaDeviceInfo *device;
	getAudioDevicesCount(&nDevices);
	for (int i=0;i<nDevices;i++) {
		device=Pa_GetDeviceInfo(i);
		printf("Device No: %i:%s\n",i,device->name);
	}

}

static int paCallback( const void *inputBuffer, void *outputBuffer,
		unsigned long framesPerBuffer,
		const PaStreamCallbackTimeInfo* timeInfo,
		PaStreamCallbackFlags statusFlags,
		void *userData )
{
	unsigned int i;
	opus_int32 oBufSize;

	memset(encodedBuffer,0,ENCODED_BUFFER_SIZE);
	//memset(txbuffer,0,TXBUFFER_SIZE);
	//memset(&op,0,sizeof(ogg_packet));

	oBufSize=opus_encode_float(oe,(const float *) inputBuffer,framesPerBuffer,encodedBuffer,ENCODED_BUFFER_SIZE);

	if (1) {
		sendto(sock,encodedBuffer,oBufSize,0,(struct sockaddr *) &addr,addrlen);
	} else {
		gpos+=framesPerBuffer;

		op.packet=encodedBuffer;
		op.bytes=oBufSize;
		op.b_o_s=(packetno==0?1:0);
		op.e_o_s=0;
		op.granulepos=gpos;
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
	printf("-d <device>	Device ID\n");
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


	while ((opt = getopt(argc, argv, "hlf:d:g:r:s:")) != -1) {
		switch (opt) {
			case 'f': //Frames Per Buffer 
				arg_framesPerBuffer = atoi(optarg);
				break;
			case 'd': //device ID
				arg_device = atoi(optarg);
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


	printf("Stream Settings: \n");
	printf("  Frames Per Buffer:		%u\n",arg_framesPerBuffer) ;
	printf("  Opus Bitrate:			%u\n",arg_bitrate) ;
	printf("  Packet Loss:			%u\n",arg_packetloss) ;
	printf("  Page Size:			%u\n",arg_pagesize) ;
	printf("  Device Name:			%s\n",Pa_GetDeviceInfo(arg_device)->name) ;
	//printf("  Device Latency:		%i\n",Pa_GetDeviceInfo(arg_device)->defaultLowInputLatency) ;
	printf("\n\n\n\n") ;

	encodedBuffer=calloc(sizeof(unsigned char),ENCODED_BUFFER_SIZE);
	txbuffer=calloc(sizeof(unsigned char),TXBUFFER_SIZE);

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

	oe=opus_encoder_create(SAMPLE_RATE,2,OPUS_APPLICATION_AUDIO,&oeErr);

	//opus_encoder_init(oe,SAMPLE_RATE,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY);
	opus_encoder_init(oe,SAMPLE_RATE,2,OPUS_APPLICATION_AUDIO);

	//ogg_stream_init(&os,12345);	
	//oe=opus_encoder_create(SAMPLE_RATE,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&oeErr);
	//
	oerr=opus_encoder_ctl(oe,OPUS_SET_PACKET_LOSS_PERC(arg_packetloss));
	oerr=opus_encoder_ctl(oe,OPUS_SET_BITRATE(arg_bitrate));

	err = Pa_StartStream(stream);
	if (err != paNoError) goto error;
	printf("Stream Started...\n");

	while (1) {
		Pa_Sleep(10000);	
	}

error:

	exit(0);

}

void cleanup() {
	ogg_stream_destroy(&os);
	opus_encoder_destroy(oe);
	Pa_CloseStream(stream);
	Pa_Terminate();
}
