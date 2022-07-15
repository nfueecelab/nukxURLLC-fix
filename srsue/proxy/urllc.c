/******************************************************************************
 * File:        urllc.c
 * Description: Creating Dual Connectivity environment.
 *              UE receive packet from eNB.
 *              Then it aggregate in srsue process.
 *****************************************************************************/
#include "urllc.h"

void* input_loop()
{
	char key;
	while (scanf("%c",&key) != EOF) {}
	pthread_exit(NULL);
}

void sig_int_handler(int signo)
{
	pthread_cancel(enb_thread);
	pthread_cancel(ue_thread);
	close(enb_fd);
	close(ue_fd);
	printf("\nStopping Process...\n");
	sleep(2);
	exit(-1);
}

void *recv_enb();
void *send_ue();
void *test_ue();
int main(int argc, char **argv)
{
	signal(SIGINT, sig_int_handler);
	pthread_create(&input_thread, NULL, input_loop, NULL);
	printf("Please enter Ctrl + C to terminate process\n");

	// set udp server sockaddr info
	bzero(&ser_in, sizeof(struct sockaddr_in));
	ser_in.sin_family = AF_INET; // Only for IPv4
	inet_aton(ue_ip, &ser_in.sin_addr);
	ser_in.sin_port = htons(ENB_PORT);

	if((enb_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	{
		perror("create enb socket failed\n");
	}

	if(bind(enb_fd, (struct sockaddr*)&ser_in, sizeof(struct sockaddr_in)) == -1)
	{
		perror("bind enb_fd failed\n");
	}

	// set unix sockaddr info
	memset(&ser_un, 0, sizeof(struct sockaddr_un));
	ser_un.sun_family = AF_UNIX;
	strncpy(ser_un.sun_path+1, ser_name, strlen(ser_name));

	memset(&cli_un, 0, sizeof(struct sockaddr_un));
	cli_un.sun_family = AF_UNIX;
	strncpy(cli_un.sun_path+1, cli_name, strlen(cli_name));

	if((ue_fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
	{
		perror("create ue socket failed\n");
	}

	if(bind(ue_fd, (struct sockaddr*)&cli_un, len_un) < 0)
	{
		perror("bind ue_fd failed\n");
	}
	
	while(1)
	{
		fd_set readfds,writefds;
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		FD_SET(enb_fd, &readfds);
		FD_SET(ue_fd, &writefds);
		tv.tv_sec = 0;
		tv.tv_usec = 50000;

		int r = select((max(enb_fd, ue_fd) + 1), &readfds, &writefds, NULL, &tv);
		switch(r)
		{
			case -1:
				perror("select error!\n");
				break;
			case 0:
				// timeout
				break;
			default:
				// one or both descriptors have data
				if(FD_ISSET(enb_fd, &readfds))
				{	
					struct byte_buffer *recvbuf = malloc(sizeof(struct byte_buffer));
					memset(recvbuf, 0, sizeof(struct byte_buffer));
					pthread_create(&enb_thread, NULL, recv_enb, recvbuf);
					pthread_join(enb_thread,NULL);
					
					if(FD_ISSET(ue_fd, &writefds))
					{
						pthread_create(&ue_thread, NULL, send_ue, recvbuf);
						pthread_join(ue_thread, NULL);
					}
					free(recvbuf);
				}
				break;
		}

	}
	return 0;
}

void *recv_enb(void *buffer)
{
	struct byte_buffer *b = (struct byte_buffer*) buffer;
	socklen_t addr_len;
	b->N_bytes = recvfrom(enb_fd, b->buffer, buffer_size, 0, (struct sockaddr *)&cli_in, &addr_len);
	#if(DEBUG)
	printf("\nreceive data len: %u\n", b->N_bytes);
	#endif
}

void *send_ue(void *buffer)
{
	struct byte_buffer *pdu = (struct byte_buffer*)buffer;
	if(sendto(ue_fd, pdu->buffer, pdu->N_bytes, 0, (struct sockaddr *)&ser_un, len_un) < 0)
	{
		perror("sendto ue unix socket failed\n");
	}else
	{
		#if(DEBUG)
		printf("OK\n");
		#endif
	}
}

void *test_ue()
{
	// // Test example 1
	// struct byte_buffer p;
	// char *c = "test";
	// strncpy(p.buffer, c , strlen(c));
	// p.N_bytes = strlen(c);

	// if(sendto(ue_fd, p.buffer, p.N_bytes, 0, (struct sockaddr *)&ser_un, len_un) < 0)
	// {
	// 	perror("sendto ue unix socket failed\n");
	// }


	// // Test example 2
	// int ii = 1;
	// if(sendto(ue_fd, &ii, sizeof(ii), 0, (struct sockaddr *)&ser_un, len_un) < 0)
	// {
	// 	perror("sendto ue unix socket failed\n");
	// }


	// // Test example 3
	// struct byte_buffer *pdu = malloc(sizeof(struct byte_buffer));
	// char *s = "test example3";
	// pdu->N_bytes = strlen(s);
	// strncpy(pdu->buffer, s, strlen(s));

	// if(sendto(ue_fd, pdu->buffer, pdu->N_bytes, 0, (struct sockaddr *)&ser_un, len_un) < 0)
	// {
	// 	perror("sendto ue unix socket failed\n");
	// }

	sleep(1);
	printf("Exit job\n");
}
