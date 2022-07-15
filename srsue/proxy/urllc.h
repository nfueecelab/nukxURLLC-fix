#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>

#define ENB_PORT 8888
#define enb_ip "192.168.137.134"
#define ue_ip "192.168.137.133"

#define DEBUG 1

#define max(a,b) \
({ __typeof__ (a) _a = (a); \
   __typeof__ (b) _b = (b); \
 _a > _b ? _a : _b; })


#define buffer_size 12237
struct byte_buffer
{
	uint32_t N_bytes;
	uint8_t buffer[buffer_size];
};

int enb_fd, ue_fd;
struct sockaddr_in ser_in, cli_in;
struct sockaddr_un ser_un, cli_un;
const char* const ser_name = "server";
const char* const cli_name = "client";
socklen_t len_un = sizeof(struct sockaddr_un);

pthread_t input_thread, enb_thread, ue_thread;
struct timeval tv;
