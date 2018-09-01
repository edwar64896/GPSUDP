#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct sockaddr_in addr;
int addrlen, sock;
struct ip_mreq mreq;


void setupSocket() {
				/* set up receive socket */
				sock = socket(AF_INET, SOCK_DGRAM, 0);
				if (sock < 0) {
								perror("socket");
								exit(1);
				}

				/* clear the structure */
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


int main(int argc,char * argv[]) {
				printf("hello\n");

				setupSocket();





				exit(0);
}
