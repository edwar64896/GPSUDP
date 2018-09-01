#include <stdio.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <opus/opus.h>
#include <ogg/ogg.h>
#include <samplerate.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdio.h>

#include "portaudio.h"
#include "pa_ringbuffer.h"
#include "log.h"
#include "ma.h"
#include "timef.h"

#define MC_PORT 49876
#define MC_GROUP "127.0.0.1"

#define SAMPLE_RATE 48000
#define RESAMPLER_QUALITY 5
#define ENCODED_BUFFER_SIZE 16384
#define RECEIVE_BUFFER_SIZE 65536
#define FRAMES_PER_BUFFER 480
#define FRAMES_PER_BUFFER_PAOUT 240
#define CHANNELS 2
#define N_STREAM_STRUCTURES 16
#define RING_BUFFER_SIZE	65536 // must be a power of 2
ring_buffer_size_t ringBufSize=RING_BUFFER_SIZE;


int ncurses=1; //provide ncurses display
int rc;
pthread_attr_t attr;
struct sched_param param;

int arg_debug=0;

typedef struct
{
				float sLeft;
				float sRight;
}   paSample;

paSample * mixBuffer;

float * unvec_sLeft;
float * unvec_sRight;

ogg_sync_state oy;
ogg_packet op;
ogg_page og;
ogg_int64_t packetno=0;
ogg_int64_t lastPacketno=0;

pthread_t network_thread_id;
pthread_t stream_thread_id;
pthread_t display_thread_id;

unsigned char * encodedBuffer;
unsigned char * receiveBuffer;

struct sockaddr_in addr;
int addrlen, sock;
struct ip_mreq mreq;

/*
 * Hashable Stream Structure for Ogg
 *
 * ogg_stream_state and OpusDecoder for each new stream
 *
 */

struct hashable_ogg_stream_state {
				int id; //key // ogg serial number (yeah its in os, but...)
				int seq; //sequence of the stream (for display purposes)
				int frames_per_buffer; //number of frames
				int active; // 1 when stream is being read. use this to help discard some of the initial ringbuffer at the start.
				ogg_stream_state os; //stream state (ogg)
				PaUtilRingBuffer rBuf; //ringbuffer structure
				uint32_t rBufSpace; //??
				pthread_mutex_t	page_mutex;
				pthread_mutex_t	packet_mutex;
				OpusDecoder *od; //decoder
				SRC_STATE *src; //sample rate converter structures
				struct timespec lastSeen; //time when we last got a packet from this puppy
				struct timespec tp;
				struct ma ma; //moving average structure
				double ratio; //src ratio
				double timing;
				paSample *resamplerPCM; //buffer for the codec to place decoded audio
				paSample *streamPCM; //buffer for the codec to place decoded audio
				paSample *mixbuf; //buffer for the ringbuffer to use for mixing
				void *pRingBuffer; //ring buffer
};

struct hashable_ogg_stream_state *initStreams () {
				struct hashable_ogg_stream_state *s;
				int oeErr;
				s=calloc(sizeof(struct hashable_ogg_stream_state),N_STREAM_STRUCTURES);
				for (int i=0; i<N_STREAM_STRUCTURES; i++ ){

								(s+i)->id=0; //serial number of the stream (ogg) this will be filled in when 
														// the stream connects from the server
								(s+i)->seq=i; //sequence - display primarily
								(s+i)->ratio=1; 
								(s+i)->active=0;
								(s+i)->frames_per_buffer=FRAMES_PER_BUFFER;

								/* 
								 * establish the Moving Average Structure for each stream
								 */
								ma_init (&(s+i)->ma);

								/*
								 * initialize stream buffers
								 */
								(s+i)->streamPCM=malloc(sizeof(paSample)*FRAMES_PER_BUFFER);
								(s+i)->resamplerPCM=malloc(sizeof(paSample)*FRAMES_PER_BUFFER*2);
								(s+i)->mixbuf=malloc(sizeof(paSample)*FRAMES_PER_BUFFER_PAOUT);

								/* 
								 * initialize ring buffer
								 */
								(s+i)->pRingBuffer=(void*)calloc(sizeof(paSample),ringBufSize);
								if (PaUtil_InitializeRingBuffer(&(s+i)->rBuf,sizeof(paSample),ringBufSize,(s+i)->pRingBuffer)==-1) {
												log_error("problem with ring buffer init");
								}

								/* 
								 * initialize sample rate converter
								 */

								(s+i)->src=src_new(SRC_SINC_MEDIUM_QUALITY, CHANNELS, &oeErr);
								if ((s+i)->src == 0 || (s+i)->src == NULL) {
												log_error("SRC_NEW failed with this..... %s",src_strerror(oeErr));
								}

								/*
								 * initialize thread mutei
								 */
								pthread_mutex_init (&(s+i)->page_mutex,NULL);
								pthread_mutex_init (&(s+i)->packet_mutex,NULL);

								/*
								 * initialize opus decoder
								 */
								(s+i)->od=opus_decoder_create(SAMPLE_RATE,CHANNELS,&oeErr);
								opus_decoder_init((s+i)->od,SAMPLE_RATE,CHANNELS);
				}
				return s;
}

struct hashable_ogg_stream_state *streams = NULL;

/*
 * matches serial number with stream state structure.
 *
 * finds first available empty structure by seeking for a structure with
 * serial id == 0
 */
struct hashable_ogg_stream_state *find_stream(int serial) {
				for (int i=0; i<N_STREAM_STRUCTURES; i++ ){
								if ((streams+i)->id==serial)
												return (streams+i);
				}
				return NULL;
}

void sig_handler(int signo)
{
				if (signo == SIGINT)
								log_info("received SIGINT");
}

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

void setupSocket() {
				/* set up receive socket */
				sock = socket(AF_INET, SOCK_DGRAM, 0);
				if (sock < 0) {
								perror("socket");
								exit(1);
				}

				/* clear ohe structure */
				bzero((char *)&addr, sizeof(addr));

				/* set the address structure */
				addr.sin_family = AF_INET;
				addr.sin_addr.s_addr = htonl(INADDR_ANY);
				addr.sin_port = htons(MC_PORT);
				addrlen = sizeof(addr);

				/* make it non-blocking */
				int flags = fcntl(sock, F_GETFL);
				flags |= O_NONBLOCK;
				fcntl(sock, F_SETFL, flags);

				/* bind the socket to the address */
				if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {        
								perror("bind");
								exit(1);
				}    
				/*
					 mreq.imr_multiaddr.s_addr = inet_addr(MC_GROUP);         
					 mreq.imr_interface.s_addr = htonl(INADDR_ANY);         
					 if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
					 &mreq, sizeof(mreq)) < 0) {
					 perror("setsockopt mreq");
					 exit(1);
					 }*/  

}

void  printAudioDevices() {
				int nDevices=0;
				PaDeviceInfo *device;
				getAudioDevicesCount(&nDevices);
				for (int i=0;i<nDevices;i++) {
								device=Pa_GetDeviceInfo(i);
								log_info("Device No: %i:%s",i,device->name);
				}

} 


// reset this stream.
void resetStream(struct hashable_ogg_stream_state * s) {
				log_info("resetting stream %u for lost stream %u",s->seq,s->id);
				s->id=0; //set the serial id to zero - stops mixing it.
				s->ratio=1.0;
				s->lastSeen.tv_sec=0;
				s->lastSeen.tv_nsec=0;
				s->tp.tv_sec=0;
				src_reset(s->src);
				memset(s->pRingBuffer,0,sizeof(paSample)*ringBufSize);
				memset(s->streamPCM,0,sizeof(paSample)*FRAMES_PER_BUFFER);
				memset(s->resamplerPCM,0,sizeof(paSample)*FRAMES_PER_BUFFER*2);
				memset(s->mixbuf,0,sizeof(paSample)*FRAMES_PER_BUFFER_PAOUT);
				opus_decoder_ctl(s->od,OPUS_RESET_STATE);	
				PaUtil_FlushRingBuffer(&s->rBuf);
				ma_reset(&s->ma);
}

struct timespec * timespec_diff(struct timespec *start, struct timespec *stop, struct timespec *result);
uint64_t timespec_to_uint64(struct timespec *input) ;

/*
 * check all the streams to see when we last saw a packet from them.
 * if it is beyond 2 seconds, then we kill off the stream
 */
void checkStreams() {
				struct timespec ts,td;
				struct hashable_ogg_stream_state * s=streams;

				clock_gettime(CLOCK_REALTIME,&ts);

				for (int j=0;j<N_STREAM_STRUCTURES;j++) {
								if ((s+j)->id == 0)
												continue;
								uint64_t lastSeenSince=timespec_to_uint64(timespec_diff(&(s+j)->lastSeen,&ts,&td));
								if (lastSeenSince>=1e09)
												resetStream(s+j);
				}
}


void
movingAveragePageTime(struct hashable_ogg_stream_state * oss, uint64_t granulepos, uint8_t packetsInPage) {
				struct timespec tp;
				struct timespec tp_result;
				clock_gettime(CLOCK_REALTIME,&tp);
				if (oss->tp.tv_sec==0)
								oss->tp=tp;
				timespec_diff(&oss->tp,&tp,&tp_result);
				oss->tp=tp;

				/*
				 * ans gives us a duration in nano-seconds between pages
				 * which should be 10000000 (20ms).
				 */
				uint64_t ans=timespec_to_uint64(&tp_result);

				/*
				 * we are going to tweak the resampler 
				 * sampling rate to adjust the number of 
				 * samples we generate to compensate
				 */
				if (ma_constrain(&oss->ma,0.1,ans)) {
								oss->timing=ma_push(&oss->ma,ans)/1000000.00;
				} else {
								oss->timing=ma_avg(&oss->ma)/1000000.00;
				}
				if (oss->timing==0 || !oss->ma.established) {
								oss->ratio = 1;
				} else {
								oss->ratio = (10 * packetsInPage)/oss->timing; //20ms because it's 2 packets per page (fixed)
				}
}

void punchPacket(struct hashable_ogg_stream_state *oss) {
				int oerr;
				int opus_err;
				int src_err;

				SRC_DATA srcd;

				memset (&srcd,0,sizeof(SRC_DATA));

				memset(&op,0,sizeof(ogg_packet));

				pthread_mutex_lock(&oss->page_mutex);
				oerr=ogg_stream_packetout(&oss->os,&op);
				pthread_mutex_unlock(&oss->page_mutex);

				switch (oerr) {
								case 1: //good packet.

												opus_err=opus_decode_float(oss->od,op.packet,op.bytes,(float *)oss->streamPCM,FRAMES_PER_BUFFER,0);
												if (opus_err<0) 
																log_error("opus error = %i",opus_err);
												else {

																srcd.data_in=(float *)oss->streamPCM;
																srcd.data_out=(float *)oss->resamplerPCM;
																srcd.input_frames=(long)FRAMES_PER_BUFFER;
																srcd.output_frames=(long)(FRAMES_PER_BUFFER*2);
																srcd.src_ratio=oss->ratio;

																if (src_err=src_process(oss->src,&srcd) != 0) {
																				log_error("src error good packet %s",src_strerror(src_err));
																				log_info("src %0x",oss->src);
																}

																pthread_mutex_lock(&oss->packet_mutex);
																PaUtil_WriteRingBuffer(&oss->rBuf,oss->resamplerPCM,srcd.output_frames_gen);
																pthread_mutex_unlock(&oss->packet_mutex);
												}
												break;
								case 0:
												break;
								default: //we are out of sync and there is a gap.
												opus_err=opus_decode_float(oss->od,NULL,0,(float *)oss->streamPCM,FRAMES_PER_BUFFER,0);
												if (opus_err<0) 
																log_error("opus error = %i",opus_err);
												else {
																srcd.data_in=(float *)oss->streamPCM;
																srcd.data_out=(float *)oss->resamplerPCM;
																srcd.input_frames=(long)FRAMES_PER_BUFFER;
																srcd.output_frames=(long)(FRAMES_PER_BUFFER*2);
																srcd.src_ratio=oss->ratio;

																if (src_err=src_process(oss->src,&srcd) != 0) {
																				log_error("src error %s",src_strerror(src_err));
																}

																pthread_mutex_lock(&oss->packet_mutex);
																PaUtil_WriteRingBuffer(&oss->rBuf,oss->resamplerPCM,srcd.output_frames_gen);
																pthread_mutex_unlock(&oss->packet_mutex);
												}
												break;

				}
}

int ossi=0;
static void * stream_thread_function(void * args) {
				while (1) {
								struct hashable_ogg_stream_state *s=streams;
								if ((s+ossi)->id!=0) {
												punchPacket(s+ossi);
								}
								ossi=(ossi+1) % N_STREAM_STRUCTURES;
				}
}


static void * network_thread_function(void * args) {
				int opus_err;
				int cnt,rc;

				/*
				 * fuckyou apple for breaking poll on sierra!
				 struct pollfd fds[1];

				 fds[0].fd=sock;
				 fds[0].events|=POLLIN;
				 */

				struct timeval to;
				fd_set rfds;

				log_debug("network thread function started..."); 

				while (1) {

								/* grab some data no matter what */
								receiveBuffer=ogg_sync_buffer(&oy,RECEIVE_BUFFER_SIZE);

								to.tv_sec=1;
								to.tv_usec=0;

								FD_ZERO(&rfds);
								FD_SET(sock,&rfds);

								rc=select (sock+1,&rfds,NULL,NULL,&to);

								cnt = recvfrom(sock, receiveBuffer, RECEIVE_BUFFER_SIZE, 0, (struct sockaddr *) &addr, &addrlen);

								if (cnt==-1) {
												checkStreams(); // check to see if any streams are no longer transmitting. If so, kill them off.
												continue;
								}

								/* 
								 * Got a packet from the network so we're dropping it into 
								 * the synch structures and then we are going to pull
								 * out pages, one by one
								 */
								if (ogg_sync_wrote(&oy,cnt)==0)  {
									;//
								}

								/* every packet it a new page
								 * so pull it straight out again
								 */
								if (ogg_sync_pageout(&oy,&og)==1) {


												/* 
												 * we have a page. lets check the serial number against the hash and see if we already ha
												 * have a stream running for it.
												 */

												uint16_t ops=ogg_page_serialno(&og);
												uint64_t gpos=ogg_page_granulepos(&og);
												uint8_t  nPackets=ogg_page_packets(&og);

												int pages=1;
												int oerr;

												/*
												 * Get the stream structure associated with this serial
												 * or assign a new one.
												 */
												struct hashable_ogg_stream_state *oss = find_stream(ops);

												if (oss==NULL) {
																oss=find_stream(0);
																oss->id=ops;
																ogg_stream_init(&(oss)->os,ops);	
																log_info("assigning new stream %u for serial %u",oss->seq,oss->id);
												}

												/*
												 * we now have a stream. it has been added to the hash
												 * it will either be an existing one or a new one.
												 */

												pthread_mutex_lock(&oss->page_mutex);
												rc=ogg_stream_pagein(&oss->os,&og);
												pthread_mutex_unlock(&oss->page_mutex);

												clock_gettime(CLOCK_REALTIME,&oss->lastSeen);

												movingAveragePageTime(oss,gpos,nPackets);


								}
				}
}

inline void deInterleave (paSample * input, float * left, float * right, int frames) {
				for (int i=0;i<frames;i++) {
								(*left++)=input->sLeft;
								(*right++)=input++->sRight;
				}
}

inline void Interleave (paSample * output, float * left, float * right, int frames) {
				for (int i=0;i<frames;i++) {
								output->sLeft=(*left++);
								output++->sRight=(*right++);
				}
}

struct timespec t_start;
struct timespec t_stop;

void mixAudio(paSample * outputBuffer, float gain) {
				paSample mixSample;
				struct hashable_ogg_stream_state *s=streams;
				int j=0,i=0;
				//#pragma clang loop vectorize(enable)
				for (j=0;j<N_STREAM_STRUCTURES;j++) {
								if ((s+j)->id==0) continue;
								if (!(s+j)->active) {
									PaUtil_FlushRingBuffer(&(s+j)->rBuf);
									(s+j)->active=1;
								}
								pthread_mutex_lock(&(s+j)->packet_mutex);
								PaUtil_ReadRingBuffer(&(s+j)->rBuf,(s+j)->mixbuf,FRAMES_PER_BUFFER_PAOUT);

								// this value should trend towards zero. Currently too high. Why?
								(s+j)->rBufSpace=PaUtil_GetRingBufferReadAvailable(&(s+j)->rBuf);

								pthread_mutex_unlock(&(s+j)->packet_mutex);
				}
				//#pragma clang loop vectorize(assume_safety)
				for (i=0;i<FRAMES_PER_BUFFER_PAOUT;i++) {
								mixSample.sLeft=0.0f;
								mixSample.sRight=0.0f;
								for (j=0;j<N_STREAM_STRUCTURES;j++) {
												if ((s+j)->id==0) continue; 
												mixSample.sLeft+=((((s+j)->mixbuf+i)->sLeft));
												mixSample.sRight+=((((s+j)->mixbuf+i)->sRight));
								}
								outputBuffer->sLeft=mixSample.sLeft*gain;
								outputBuffer++->sRight=mixSample.sRight*gain;
				}
}

static int paCallback( const void *inputBuffer, void *outputBuffer,
								unsigned long framesPerBuffer,
								const PaStreamCallbackTimeInfo* timeInfo,
								PaStreamCallbackFlags statusFlags,
								void *userData )
{
				memset(outputBuffer,0,framesPerBuffer*sizeof(paSample));
				mixAudio((paSample *)outputBuffer,1.0f);

				return 0;
}

int main(int argc,char *argv[]) {

				PaError err;
				static paSample data;
				PaStream *stream;


				mixBuffer=calloc(sizeof(paSample),FRAMES_PER_BUFFER_PAOUT);
				unvec_sLeft=calloc(sizeof(float), FRAMES_PER_BUFFER_PAOUT);
				unvec_sRight=calloc(sizeof(float), FRAMES_PER_BUFFER_PAOUT);

				log_debug("before init streams"); 
				streams=initStreams();
				log_debug("after init streams"); 

				err=Pa_Initialize();
				if (err !=paNoError) goto error;

				setupSocket();

				/* Open an audio I/O stream. */
				err = Pa_OpenDefaultStream( &stream,
												0,          /* no input channels */
												2,          /* stereo output */
												paFloat32,  /* 32 bit floating point output */
												SAMPLE_RATE,
												FRAMES_PER_BUFFER_PAOUT,        /* frames per buffer, i.e. the number
																													 of sample frames that PortAudio will
																													 request from the callback. Many apps
																													 may want to use
																													 paFramesPerBufferUnspecified, which
																													 tells PortAudio to pick the best,
																													 possibly changing, buffer size.*/
												paCallback, /* this is your callback function */
												&data ); /*This is a pointer that will be passed to
																	 your callback*/
				if( err != paNoError ) goto error;

				ogg_sync_init(&oy);

				/* create the threads */

				rc = pthread_attr_init (&attr);
				rc = pthread_attr_getschedparam (&attr, &param);
				(param.sched_priority)++;
				rc = pthread_attr_setschedparam (&attr, &param);

				/* start the audio up */

				log_info("all allocated...");


				err = Pa_StartStream(stream);
				if (err != paNoError) goto error;
				log_info("Audio Stream Started...");

				pthread_create (&stream_thread_id,&attr,stream_thread_function,NULL);
				pthread_create (&network_thread_id,&attr,network_thread_function,NULL);


				int dropout=1;
				int c;

				while (dropout) {
								c=getchar();
								switch (c) {
												case 'q':
																dropout=0;
																exit(0);
																break;
												case 'o':
																//system("/bin/stty cooked");
																//printOptions();
																//system("/bin/stty raw");
																break;
								}
								usleep(100000);
				}

				/* join the threads once the stream stops */
				void * network_thread_status;
				void * stream_thread_status;
				pthread_join(stream_thread_id,&stream_thread_status);
				pthread_join(network_thread_id,&network_thread_status);

error:

				ogg_sync_destroy(&oy);
				if (encodedBuffer) free(encodedBuffer);
				if (receiveBuffer) free(receiveBuffer);
				if (mixBuffer) free (mixBuffer);
				if (err != paNoError)
								log_error( "Error message: %s", Pa_GetErrorText( err ) );
				Pa_CloseStream(stream);
				Pa_Terminate();

				exit(0);

}
