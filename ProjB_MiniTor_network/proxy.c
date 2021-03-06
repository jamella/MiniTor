/*
 * proxy.c
 *
 *  Created on: Sep 17, 2014
 *      Author: Nguyen Tran
 */

#include "proxy.h"
#include "controlMessage.h"
#include "router1.h"
#include "aes.h"
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/if_link.h>
#include <arpa/inet.h>
#include <time.h>

#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>



/*
 * Print error message with error code
 */
void error(char* message)
{
	perror(message);
	exit(1);
}

/*
 * function to call in parent process for communication in Stage 1
 */
void createCommunicationStage1(int numRouter)
{
	int portno, data, noRouter,routerPort, pid;
	int sockfd;
	socklen_t servlen,clilen;
	noRouter = 0;

	struct sockaddr_in serv_addr, cli_addr; //UDP connection

	//Struct to get router info from router
	struct payload {
		int router;
		int pid;
	};
	struct payload *p;
	char* buffer = malloc(sizeof(struct payload));

	//create socket
	sockfd = socket(AF_INET,SOCK_DGRAM,0); //use UDP
	//check for error
	if(sockfd < 0)
	{
		error ("ERROR creating socket in server...");
	}
	//initialize all fields in serv_addr to 0
	memset((char*) &serv_addr, 0 ,sizeof(serv_addr));
	//set fields in struct sockaddr_in
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(0); //Bind to port 0 and let the OS assigns the UDP port
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	//bind the socket to an addr
	if(bind(sockfd,(struct sockaddr*) &serv_addr,sizeof(serv_addr)) < 0)
	{
		error("ERROR binding to socket in server...");
	}
	else
	{
		servlen = sizeof(serv_addr);
		getsockname(sockfd,(struct sockaddr *) &serv_addr, &servlen);
		portno = ntohs(serv_addr.sin_port);
	}


	clilen = sizeof(cli_addr);
	for(int n = 0; n < numRouter; n++)
	{
		pid = fork(); //fork a router based on number of router
		noRouter++; //Update number of router after each forking
		if(pid < 0)
		{
			error ("ERROR on fork");
		}
		else if(pid == 0) //in CHILD process
		{
			communicateToServerStage1(portno,noRouter);
			close(sockfd);
		}
		else //in PARENT process
		{
			printf("Waiting on port %d\n", portno);
			data = recvfrom(sockfd,buffer,sizeof(struct payload),0,(struct sockaddr*) &cli_addr, &clilen);

			if(data < 0)
			{
				printf ("ERROR receiving message in port %d\n",portno);
			}
			else
			{
				p = (struct payload*)buffer;
			}
				routerPort = ntohs(cli_addr.sin_port);

				/*
				 * write to file
				 */
				writeInfoToFileStage1(portno,p->router,p->pid,routerPort);

				free(buffer);
		}
	}
}

/*
 * function to call in parent process for communication in Stage 2 and Stage 3
 */
void createCommunicationStage2and3(int stageNum, int numRouter)
{
	FILE *fp;
	if(stageNum == 2)
	{
		fp = fopen("stage2.proxy.out","w");
		if(fp == NULL)
		{
			error ("Can't open file for write");
		}
	}
	else if (stageNum == 3)
	{
		fp = fopen("stage3.proxy.out","w");
		if(fp == NULL)
		{
			error ("Can't open file for write");
		}
	}

	/*
	 * Need to find out interface IP addr and
	 * I re-used this code
	 * code are from here: http://man7.org/linux/man-pages/man3/getifaddrs.3.html
	 */
	struct ifaddrs *ifaddr, *ifa;
	int family, name_check,n;
	char eth_ip_addr [15];
	char *eth_name;
	char ip_arr[8][15];
	int i = 0;
	if(getifaddrs(&ifaddr) == -1)
	{
		error ("getifaddrs function call failed!");
	}

	n=0;
	for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
	{
		if(ifa->ifa_addr == NULL)
		{
			continue;
		}
		family = ifa->ifa_addr->sa_family;
		if(family == AF_INET)
		{
			eth_name = ifa->ifa_name;
			if(strncmp(eth_name,"lo",2) == 0 || strncmp(eth_name,"tun1",4) == 0)
			{
				continue;
			}

			name_check = getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in),eth_ip_addr,sizeof(eth_ip_addr),NULL,0,NI_NUMERICHOST);
			if(name_check != 0) //fail
			{
				error ("getnameinfo failed!");
			}
			n++;
			if(strncmp (eth_ip_addr,"192.168.20",10) == 0)
			{
				if(i >= 8)
				{
					error ("ERROR ethernet ip address array out of bound! \n");
				}
				strcpy(ip_arr[i], eth_ip_addr);
				i++;
			}
		}
	}


	/*
	 * Necessary struct and constant declaration for sendmsg() - sending raw socket to ETH0
	 */
	struct iovec io;
	struct msghdr msgh;
	memset(&msgh,0,sizeof(msgh));
	memset(&io,0,sizeof(io));


	int portno, data, noRouter, routerPort, pid;
	int sockfd;
	socklen_t servlen,clilen;
	noRouter = 0;

	struct timeval tv; //time interval wait for select()
	tv.tv_sec = 7;
	tv.tv_usec = 0;

	struct icmphdr* icmp;
	struct iphdr* ip;

	char saddr[16];
	char daddr[16];

	struct sockaddr_in serv_addr, cli_addr, dest_addr;

	//Getting router info from router
	struct payload {
		int router;
		int pid;
	};
	struct payload *p;

	char* buffer = malloc(sizeof(struct payload));


	char icmp_reply_buf[2048];

	/*
	 * Set up Tunnel
	 */
    char tun_name[IFNAMSIZ];
    char buffer_tun[2048];

    int maxfds1;


    /* Connect to the tunnel interface (make sure you create the tunnel interface first) */
    strcpy(tun_name, "tun1");
    int tun_fd = tun_alloc(tun_name, IFF_TUN | IFF_NO_PI);

    if(tun_fd < 0)
    {
    	perror("Open tunnel interface");
    	exit(1);
    }



	//create socket
	sockfd = socket(AF_INET,SOCK_DGRAM,0); //use UDP
	//check for error
	if(sockfd < 0)
	{
		error ("ERROR creating socket in server...");
	}
	//initialize all fields in serv_addr to 0
	memset((char*) &serv_addr, 0 ,sizeof(serv_addr));
	//set fields in struct sockaddr_in
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(0); //Bind to port 0 and let the OS assigns the UDP port
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	//bind the socket to an addr
	if(bind(sockfd,(struct sockaddr*) &serv_addr,sizeof(serv_addr)) < 0)
	{
		error("ERROR binding to socket in server...");
	}
	else
	{
		servlen = sizeof(serv_addr);
		getsockname(sockfd,(struct sockaddr *) &serv_addr, &servlen);
		portno = ntohs(serv_addr.sin_port);
	}


	/*
	 * Calculate max fd set
	 */
    if(tun_fd > sockfd)
    {
    	maxfds1 = tun_fd + 1;
    }
    else
    {
    	maxfds1 = sockfd + 1;
    }

	clilen = sizeof(cli_addr);

	/*
	 * Check number of router specified in config file matches the limit
	 */
	if(numRouter > 6)
	{
		error ("ERROR number of router specified in config file is more than 6\n");
	}

	//re-use i - index, set it back to 0
	i = -1;

	/*
	 * FOR LOOP to fork as many routers as specified in the config file
	 */
	for(int n = 0; n < numRouter; n++)
	{
		noRouter++; //Update number of router after each forking
		i++;
		fflush(fp);
		pid = fork(); //fork a router based on number of router
		if(pid < 0)
		{
			error ("ERROR on fork");
		}
		else if(pid == 0) //in CHILD process
		{
			if(stageNum == 2)
			{
				communicateToServerStage2(portno,noRouter);
			}
			else if(stageNum == 3)
			{
				communicateToServerStage3(portno,noRouter,ip_arr[i]);
			}
		}
		else //in PARENT process
		{
			printf("Waiting on port %d\n", portno);
			data = recvfrom(sockfd,buffer,sizeof(struct payload),0,(struct sockaddr*) &cli_addr, &clilen);
			printf ("Data payload size: %d\n",data); // == 8
			if(data < 0)
			{
				printf ("ERROR receiving message in port %d\n",portno);
			}
			else
			{
				p = (struct payload*)buffer;
			}
			routerPort = ntohs(cli_addr.sin_port);
			writeInfoToFileStage2Part1(fp,portno,p->router,p->pid,routerPort);


			//WHILE LOOP to keep reading from tunnel
			while(1)
			{
				int sel_return;
				fd_set readfds;

				FD_ZERO(&readfds); //initialize the set
				FD_SET(tun_fd, &readfds); //add tunnel fd to fd_set
				FD_SET(sockfd, &readfds); //add sockfd to fd_set

				/*
				 * Waiting for connection from tunnel or router. If after 7 seconds with no communication, function exits
				 * and terminates program
				 */
				sel_return = select(maxfds1,&readfds,NULL,NULL,&tv);

				if(sel_return < 0)
				{
					printf("ERROR in selecting \n");
					exit(1);
				}
				else if(sel_return == 0)
				{
					printf("After 7 seconds without any communication. Function exits, program is terminated\n");
					//kill(p->pid,SIGKILL);
					break;
				}
				if(FD_ISSET(tun_fd, &readfds))
				{
					/*
					 * reset timeout
					 */
					tv.tv_sec = 7;
					tv.tv_usec = 0;

					/* Now read data coming from the tunnel */
					int nread = read(tun_fd,buffer_tun,sizeof(buffer_tun));
					if(nread < 0)
					{
						perror("Reading from tunnel interface");
						close(tun_fd);
						exit(-1);
					}
					else
					{
						printf("\nRead a packet from tunnel, packet length:%d\n", nread);

						/*
						 * ip header info
						 */
						ip = (struct iphdr*) buffer_tun;
						inet_ntop(AF_INET,&ip->saddr,saddr,sizeof(saddr));
						inet_ntop(AF_INET,&ip->daddr,daddr,sizeof(daddr));

						/*
						 * icmp header info
						 */
						icmp = (struct icmphdr*) (buffer_tun+sizeof(struct iphdr));
						/*
						 * write to file
						 */
						writeInfoToFileStage2Part2(fp, routerPort,"tunnel", saddr, daddr,icmp->type);

						/*
						 * forward to router via proxy
						 */
						printf("NOW FORWARD ICMP PACKET TO ROUTER NUMBER: %d\n",p->router);
						//send icmp packet to router
						inet_pton(AF_INET,ip_arr[0], &(cli_addr.sin_addr));
						printf ("IP ADDR of router before send: %s\n",inet_ntoa(cli_addr.sin_addr));

						data = sendto(sockfd,buffer_tun,nread,0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
						if(data < 0)
						{
							printf ("ERROR sending icmp echo message from proxy to router\n");
						}

					}
				}
				if(FD_ISSET(sockfd, &readfds))
				{
					/*
					 * reset timeout
					 */
					tv.tv_sec = 7;
					tv.tv_usec = 0;

					/*
					 * data coming to proxy from router for ICMP reply packet
					 */
					printf("Receiving data from router(s)...\n");
					data = recvfrom(sockfd,icmp_reply_buf,160,0,(struct sockaddr*) &cli_addr, &clilen);

					if(data < 0)
					{
						printf ("ERROR receiving message in port %d\n",portno);
					}
					else
					{
						//printf("RECEIVED ICMP REPLY PACKET FROM ROUTER NUMBER %d with pid %d\n",p->router,p->pid);
						printf("RECEIVED ICMP REPLY PACKET FROM ROUTER...processing\n");

						/*
						 * ip header info
						 */
						ip = (struct iphdr*) icmp_reply_buf;
						inet_ntop(AF_INET,&ip->saddr,saddr,sizeof(saddr));
						inet_ntop(AF_INET,&ip->daddr,daddr,sizeof(daddr));

						/*
						 * icmp header info
						 */
						icmp = (struct icmphdr*) (icmp_reply_buf+sizeof(struct iphdr));

						/*
						 * write to file
						 */
						writeInfoToFileStage2Part2(fp, routerPort,"port", saddr, daddr,icmp->type);

						/*
						 * Check destination Ip addr to send back to tunnel or $ETH0
						 */
						if(strncmp ("10.5.51.2",daddr,9) == 0)
						{
							/*
							 * write back to tunnel
							 */
							printf("Writing ICMP REPLY packet back to tunnel interface from proxy\n");
							int nwrite = write(tun_fd,icmp_reply_buf,data);
							if(nwrite < 0)
							{
								error("Writing to tunnel interface");
								close(tun_fd);
								exit(-1);
							}
						}
						else
						{
							int rawfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
							if(rawfd < 0)
							{
								error ("ERROR creating raw socket in proxy\n");
							}

							//initialize all fields in serv_addr to 0
							memset(&dest_addr, 0 ,sizeof(dest_addr));
							//set fields in struct sockaddr_in
							dest_addr.sin_family = AF_INET;
							dest_addr.sin_port = htons(0);
							dest_addr.sin_addr.s_addr = ip->daddr;

							/*
							 * construct struct msghdr to pass in sendmsg()
							 */
							msgh.msg_name = &dest_addr;
							msgh.msg_namelen = sizeof(dest_addr);

							msgh.msg_iovlen = 1;
							msgh.msg_iov = &io;
							msgh.msg_iov->iov_base = icmp;
							msgh.msg_iov->iov_len = ntohs(ip->tot_len) - sizeof(struct iphdr);
							msgh.msg_control = NULL;
							msgh.msg_controllen = 0;
							msgh.msg_flags = 0;

							//send ICMP ECHO to $ETH0
							printf("Sending ICMP ECHO packet to ETH0...\n");
							data = sendmsg(rawfd, &msgh,0);
							if(data < 0)
							{
								printf ("ERROR sending icmp echo message from proxy to ETH0 interface\n");
							}
							close(rawfd);
						}
					}
				}
			}
		}
	}
	fclose(fp);
	freeifaddrs(ifaddr);
	free(buffer);
	close(tun_fd);
	close(sockfd);
}

/*
 * function to call in parent process for communication in Stage 4
 */
void createCommunicationStage4(int stageNum, int numRouter)
{
	FILE *fp;
	fp = fopen("stage4.proxy.out","w");
	if(fp == NULL)
	{
		error ("Can't open file for write");
	}

	/*
	 * Get a list of all available interfaces in order to assign to each router.
	 * I re-use the general idea and code to get all available interfaces using getifaddrs(3)
	 * Source code is from here: http://man7.org/linux/man-pages/man3/getifaddrs.3.html
	 */
	struct ifaddrs *ifaddr, *ifa;
	int family, name_check,n;
	char eth_ip_addr [15];
	char *eth_name;
	char ip_arr[8][15];
	int i = 0;
	if(getifaddrs(&ifaddr) == -1)
	{
		error ("getifaddrs function call failed!");
	}

	n=0;
	for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
	{
		if(ifa->ifa_addr == NULL)
		{
			continue;
		}
		family = ifa->ifa_addr->sa_family;
		if(family == AF_INET)
		{
			eth_name = ifa->ifa_name;
			if(strncmp(eth_name,"lo",2) == 0 || strncmp(eth_name,"tun1",4) == 0)
			{
				continue;
			}

			name_check = getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in),eth_ip_addr,sizeof(eth_ip_addr),NULL,0,NI_NUMERICHOST);
			if(name_check != 0) //fail
			{
				error ("getnameinfo failed!");
			}
			n++;
			if(strncmp (eth_ip_addr,"192.168.20",10) == 0)
			{
				if(i >= 8)
				{
					error ("ERROR ethernet ip address array out of bound! \n");
				}
				strcpy(ip_arr[i], eth_ip_addr);
				i++;
			}
		}
	}
	/*
	 * Necessary struct and constant declaration for sendmsg() - sending raw socket to ETH0
	 */
	struct iovec io;
	struct msghdr msgh;
	memset(&msgh,0,sizeof(msgh));
	memset(&io,0,sizeof(io));


	int portno, data, noRouter, routerPort, pid;
	int sockfd;
	socklen_t servlen,clilen;
	noRouter = 0;

	struct timeval tv; //time interval wait for select()
	tv.tv_sec = 7;
	tv.tv_usec = 0;

	struct icmphdr* icmp;
	struct iphdr* ip;

	char saddr[16];
	char daddr[16];

	struct sockaddr_in serv_addr, cli_addr, dest_addr;
	struct sockaddr_in cliAddr_arr[numRouter];

	//Getting router info from router
	struct payload {
		int router;
		int pid;
	};
	struct payload *p;

	char* buffer = malloc(sizeof(struct payload));


	char icmp_reply_buf[2048];

	/*
	 * Set up variable for hased IP addr
	 */
	int assignedRouter = 0;
	char* token, *ptr;
	unsigned long num=0, value = 0;

	/*
	 * Set up Tunnel
	 */
    char tun_name[IFNAMSIZ];
    char buffer_tun[2048];

    int maxfds1;


    /* Connect to the tunnel interface (make sure you create the tunnel interface first) */
    strcpy(tun_name, "tun1");
    int tun_fd = tun_alloc(tun_name, IFF_TUN | IFF_NO_PI);

    if(tun_fd < 0)
    {
    	perror("Open tunnel interface");
    	exit(1);
    }



	//create socket
	sockfd = socket(AF_INET,SOCK_DGRAM,0); //use UDP
	//check for error
	if(sockfd < 0)
	{
		error ("ERROR creating socket in server...");
	}
	//initialize all fields in serv_addr to 0
	memset((char*) &serv_addr, 0 ,sizeof(serv_addr));
	//set fields in struct sockaddr_in
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(0); //Bind to port 0 and let the OS assigns the UDP port
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	//bind the socket to an addr
	if(bind(sockfd,(struct sockaddr*) &serv_addr,sizeof(serv_addr)) < 0)
	{
		error("ERROR binding to socket in server...");
	}
	else
	{
		servlen = sizeof(serv_addr);
		getsockname(sockfd,(struct sockaddr *) &serv_addr, &servlen);
		portno = ntohs(serv_addr.sin_port);
	}

	/*
	 * write to config in stage 4
	 */
	if(stageNum == 4)
	{
		writeInfoToFileStage4Part1(fp, portno);
	}

	/*
	 * Calculate max fd set
	 */
    if(tun_fd > sockfd)
    {
    	maxfds1 = tun_fd + 1;
    }
    else
    {
    	maxfds1 = sockfd + 1;
    }

	clilen = sizeof(cli_addr);

	/*
	 * Check number of router specified in config file matches the limit
	 */
	if(numRouter > 8)
	{
		error ("ERROR number of router specified in config file is more than 8\n");
	}

	//re-use i - index, set it back to 0
	i = -1;

	int k = 0; //index for client struct array

	/*
	 * FOR LOOP to fork as many routers as specified in the config file
	 */
	for(int n = 0; n < numRouter; n++)
	{
		noRouter++; //Update number of router after each forking
		i++;
		fflush(fp);
		pid = fork(); //fork a router based on number of router
		if(pid < 0)
		{
			error ("ERROR on fork");
		}
		else if(pid == 0) //in CHILD process
		{
			printf( "my i = %d\n", i);
			printf ("IP assgined to router %d = %s\n",noRouter,ip_arr[i]);
			communicateToServerStage4(portno,noRouter,ip_arr[i]);
			break;
			//close(sockfd);
		}
		else //in PARENT process
		{
			if(noRouter < numRouter)
			{
				printf("Waiting on port %d\n", portno);
				data = recvfrom(sockfd,buffer,sizeof(struct payload),0,(struct sockaddr*) &cli_addr, &clilen);
				cliAddr_arr[k] = cli_addr;
				k++;
				printf ("Data payload size: %d\n",data); // == 8
				if(data < 0)
				{
					printf ("ERROR receiving message in port %d\n",portno);
				}
				else
				{
					p = (struct payload*)buffer;
				}
				routerPort = ntohs(cli_addr.sin_port);

				writeInfoToFileStage4Part2 (fp, p->router,p->pid,routerPort);
			}
			else
			{
				/*
				 * read data from the last router where noRouter == numRouter
				 */
				printf("Waiting on port %d\n", portno);
				data = recvfrom(sockfd,buffer,sizeof(struct payload),0,(struct sockaddr*) &cli_addr, &clilen);
				cliAddr_arr[k] = cli_addr;
				printf ("Data payload size: %d\n",data); // == 8
				if(data < 0)
				{
					printf ("ERROR receiving message in port %d\n",portno);
				}
				else
				{
					p = (struct payload*)buffer;
				}
				routerPort = ntohs(cli_addr.sin_port);

				writeInfoToFileStage4Part2 (fp, p->router,p->pid,routerPort);

				//WHILE LOOP to keep reading from tunnel or router(s)
				while(1)
				{
					int sel_return;
					fd_set readfds;

					FD_ZERO(&readfds); //initialize the set
					FD_SET(tun_fd, &readfds); //add tunnel fd to fd_set
					FD_SET(sockfd, &readfds); //add sockfd to fd_set

					/*
					 * Waiting for connection from tunnel or router. If after 7 seconds with no communication, function exits
					 * and terminates program
					 */
					sel_return = select(maxfds1,&readfds,NULL,NULL,&tv);

					if(sel_return < 0)
					{
						printf("ERROR in selecting \n");
						exit(1);
					}
					else if(sel_return == 0)
					{
						printf("After 7 seconds without any communication. Function exits, program is terminated\n");
						//kill(p->pid,SIGKILL);
						break;
					}
					if(FD_ISSET(tun_fd, &readfds))
					{
						/*
						 * reset timeout
						 */
						tv.tv_sec = 7;
						tv.tv_usec = 0;

						/* Now read data coming from the tunnel */
						int nread = read(tun_fd,buffer_tun,sizeof(buffer_tun));
						if(nread < 0)
						{
							perror("Reading from tunnel interface");
							close(tun_fd);
							exit(-1);
						}
						else
						{
							printf("\nRead a packet from tunnel, packet length:%d\n", nread);

							/*
							 * ip header info
							 */
							ip = (struct iphdr*) buffer_tun;
							inet_ntop(AF_INET,&ip->saddr,saddr,sizeof(saddr));
							inet_ntop(AF_INET,&ip->daddr,daddr,sizeof(daddr));

							/*
							 * icmp header info
							 */
							icmp = (struct icmphdr*) (buffer_tun+sizeof(struct iphdr));
							/*
							 * write to file
							 */
							writeInfoToFileStage2Part2(fp, routerPort,"tunnel", saddr, daddr,icmp->type);

							/*
							 * Compare destination IP address to see if it's one of the routers
							 */
							if(strncmp ("192.168.201.2",daddr,sizeof(daddr)) == 0)
							{
								assignedRouter = 1;
							}
							else if(strncmp ("192.168.202.2",daddr,sizeof(daddr)) == 0)
							{
								assignedRouter = 2;
							}
							else if(strncmp ("192.168.203.2",daddr,sizeof(daddr)) == 0)
							{
								assignedRouter = 3;
							}
							else if(strncmp ("192.168.204.2",daddr,sizeof(daddr)) == 0)
							{
								assignedRouter = 4;
							}
							else if(strncmp ("192.168.205.2",daddr,sizeof(daddr)) == 0)
							{
								assignedRouter = 5;
							}
							else if(strncmp ("192.168.206.2",daddr,sizeof(daddr)) == 0)
							{
								assignedRouter = 6;
							}
							else if(strncmp ("192.168.207.2",daddr,sizeof(daddr)) == 0)
							{
								assignedRouter = 7;
							}
							else if(strncmp ("192.168.208.2",daddr,sizeof(daddr)) == 0)
							{
								assignedRouter = 8;
							}
							else
							{
								/*
								 * Hash destination IP addr in order to assign to appropriate router
								 * need to check if the hashed value is valid in the cliAddr_arr
								 * I re-use the following code to convert IP addr from string to unsigned long number
								 * Source code: http://electrofriends.com/source-codes/software-programs/c/number-programs/c-program-to-convert-ip-address-to-32-bit-long-int/
								 */
								token = strtok(daddr,".");
								while(token != NULL)
								{
									value = strtoul(token,&ptr,0);
									num = (num << 8) + value;
									token = strtok(NULL, ".");
								}
								assignedRouter = (num % numRouter) + 1; //hased dest addr to assign to appropriate router
							}
							printf ("My assigned router = %d\n", assignedRouter);

							/*
							 * forward to router via proxy
							 */
							printf("NOW FORWARD ICMP PACKET TO ROUTER NUMBER: %d\n",p->router);
							//send icmp packet to router

						//	inet_pton(AF_INET,ip_arr[0], &(cli_addr.sin_addr));
							cli_addr = cliAddr_arr[assignedRouter-1];
							printf ("IP ADDR of router before send: %s\n",inet_ntoa(cli_addr.sin_addr));



							data = sendto(sockfd,buffer_tun,nread,0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
							if(data < 0)
							{
								printf ("ERROR sending icmp echo message from proxy to router\n");
							}

						}
					}
					if(FD_ISSET(sockfd, &readfds))
					{
						/*
						 * reset timeout
						 */
						tv.tv_sec = 7;
						tv.tv_usec = 0;

						/*
						 * data coming to proxy from router for ICMP reply packet
						 */
						printf("Receiving data from router(s)...\n");
						data = recvfrom(sockfd,icmp_reply_buf,160,0,(struct sockaddr*) &cli_addr, &clilen);

						if(data < 0)
						{
							printf ("ERROR receiving message in port %d\n",portno);
						}
						else
						{
							//printf("RECEIVED ICMP REPLY PACKET FROM ROUTER NUMBER %d with pid %d\n",p->router,p->pid);
							printf("RECEIVED ICMP REPLY PACKET FROM ROUTER...processing\n");

							/*
							 * ip header info
							 */
							ip = (struct iphdr*) icmp_reply_buf;
							inet_ntop(AF_INET,&ip->saddr,saddr,sizeof(saddr));
							inet_ntop(AF_INET,&ip->daddr,daddr,sizeof(daddr));

							/*
							 * icmp header info
							 */
							icmp = (struct icmphdr*) (icmp_reply_buf+sizeof(struct iphdr));

							/*
							 * write to file
							 */
							routerPort = ntohs(cli_addr.sin_port);
							writeInfoToFileStage2Part2(fp, routerPort,"port", saddr, daddr,icmp->type);

							/*
							 * Check destination Ip addr to send back to tunnel or $ETH0
							 */
							if(strncmp ("10.5.51.2",daddr,9) == 0)
							{
								/*
								 * write back to tunnel
								 */
								printf("Writing ICMP REPLY packet back to tunnel interface from proxy\n");
								int nwrite = write(tun_fd,icmp_reply_buf,data);
								if(nwrite < 0)
								{
									error("Writing to tunnel interface");
									close(tun_fd);
									exit(-1);
								}
								printf ("All done, closing socket and exiting program...\n");
								printf ("Please wait patient for program to exit, do not Ctrl+C!\n");
							}
							else
							{
								int rawfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
								if(rawfd < 0)
								{
									error ("ERROR creating raw socket in proxy\n");
								}

								//initialize all fields in serv_addr to 0
								memset(&dest_addr, 0 ,sizeof(dest_addr));
								//set fields in struct sockaddr_in
								dest_addr.sin_family = AF_INET;
								dest_addr.sin_port = htons(0);
								dest_addr.sin_addr.s_addr = ip->daddr;

								/*
								 * construct struct msghdr to pass in sendmsg()
								 */
								msgh.msg_name = &dest_addr;
								msgh.msg_namelen = sizeof(dest_addr);

								msgh.msg_iovlen = 1;
								msgh.msg_iov = &io;
								msgh.msg_iov->iov_base = icmp;
								msgh.msg_iov->iov_len = ntohs(ip->tot_len) - sizeof(struct iphdr);
								msgh.msg_control = NULL;
								msgh.msg_controllen = 0;
								msgh.msg_flags = 0;

								//send ICMP ECHO to $ETH0
								printf("Sending ICMP ECHO packet to ETH0...\n");
								data = sendmsg(rawfd, &msgh,0);
								if(data < 0)
								{
									printf ("ERROR sending icmp echo message from proxy to ETH0 interface\n");
								}
								close(rawfd);
								printf ("All done, closing socket and exiting program...\n");
								printf ("Please wait patient for program to exit, do not Ctrl+C!\n");
							}
						}
					}
				}
				break; //break out of for loop when parent is done
			}
		}
	}
	fclose(fp);
	freeifaddrs(ifaddr);
	free(buffer);
	close(tun_fd);
	close(sockfd);
}

/*
 * function to call in parent process for communication in Stage 5
 */
void createCommunicationStage5(int stageNum, int numRouter, int numHop)
{
	FILE *fp;
	fp = fopen("stage5.proxy.out","w");
	if(fp == NULL)
	{
		error ("Can't open file for write");
	}

	/*
	 * Get a list of all available interfaces in order to assign to each router.
	 * I re-use the general idea and code to get all available interfaces using getifaddrs(3)
	 * Source code is from here: http://man7.org/linux/man-pages/man3/getifaddrs.3.html
	 */
	struct ifaddrs *ifaddr, *ifa;
	int family, name_check,n;
	char eth_ip_addr [15];
	char *eth_name;
	char ip_arr[8][15];
	int i = 0;
	if(getifaddrs(&ifaddr) == -1)
	{
		error ("getifaddrs function call failed!");
	}

	n=0;
	for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
	{
		if(ifa->ifa_addr == NULL)
		{
			continue;
		}
		family = ifa->ifa_addr->sa_family;
		if(family == AF_INET)
		{
			eth_name = ifa->ifa_name;
			if(strncmp(eth_name,"lo",2) == 0 || strncmp(eth_name,"tun1",4) == 0)
			{
				continue;
			}

			name_check = getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in),eth_ip_addr,sizeof(eth_ip_addr),NULL,0,NI_NUMERICHOST);
			if(name_check != 0) //fail
			{
				error ("getnameinfo failed!");
			}
			n++;
			if(strncmp (eth_ip_addr,"192.168.20",10) == 0)
			{
				if(i >= 8)
				{
					error ("ERROR ethernet ip address array out of bound! \n");
				}
				strcpy(ip_arr[i], eth_ip_addr);
				i++;
			}
		}
	}
	printf ("My i initial = %d\n",i);
	for (int k = 0; k < 6; k++)
	{
		printf("IP addr in ip arr outside the LOOP: %s\n", ip_arr[k]);
	}
	//printf(" n = %d\n", n);

	/*
	 * Necessary struct and constant declaration for sendmsg() - sending raw socket to ETH0
	 */
	struct iovec io;
	struct msghdr msgh;
	memset(&msgh,0,sizeof(msgh));
	memset(&io,0,sizeof(io));


	int portno, data, noRouter, routerPort, pid;
	int routerPortArr [numRouter];

	int sockfd;
	socklen_t servlen,clilen;
	noRouter = 0;

	struct timeval tv; //time interval wait for select()
	tv.tv_sec = 7;
	tv.tv_usec = 0;

	struct icmphdr* icmp;
	struct iphdr* ip;

	char saddr[16];
	char daddr[16];

	struct sockaddr_in serv_addr, cli_addr, dest_addr;
	struct sockaddr_in cliAddr_arr[numRouter];

	//Getting router info from router
	struct payload {
		int router;
		int pid;
		char* routerIP;
	};
	struct payload *p;

	char* buffer = malloc(sizeof(struct payload));


	/*
	 * Create struct for control message in Minitor
	 */

	struct controlMessage *cMsg = (struct controlMessage*) malloc(sizeof(struct controlMessage));

	inet_pton(AF_INET,"127.0.0.1", &cMsg->cmIphdr.saddr);
	inet_pton(AF_INET,"127.0.0.1", &cMsg->cmIphdr.daddr);

	cMsg->cmIphdr.protocol = 253;
	cMsg->cmIphdr.check = 0;
	cMsg->cmIphdr.frag_off = 0;
	cMsg->cmIphdr.id = 0;
	cMsg->cmIphdr.ihl = 0;
	cMsg->cmIphdr.tos = 0;
	cMsg->cmIphdr.tot_len = 0;
	cMsg->cmIphdr.ttl = 0;
	cMsg->cmIphdr.version = 0;

	/*
	 * calculate CID at proxy where i = 0. Func: i*256+s
	 * Don't set type and payload until later
	 */
	uint16_t cid = 0 * 256 +1;
	cMsg->cmIpPayload.cid = htons(cid);

	/*
	 * Struct for payload
	 */
	struct controlMsg_payload {
		uint16_t rPort;
	}; //this struct is for CID in payload portion
	struct controlMsg_payload *payload = (struct controlMsg_payload*) malloc(sizeof(struct controlMsg_payload));

	struct controlMsg_payload_long {
		char icmp_pkt[2048];
	}; //this struct is for ICMP packet payload
	struct controlMsg_payload_long *payload_long = (struct controlMsg_payload_long *) malloc(sizeof(struct controlMsg_payload_long ));


	char* buf_cm = malloc(sizeof(struct controlMessage));
	char* buf_payload_long = malloc(sizeof(struct controlMsg_payload_long));
	/*
	 * Getting info from received minitor control message
	 */
	struct iphdr* ip_hdr;
	struct ipPayload* ip_payload;

	int pktSize = sizeof(ip_payload->cid) + sizeof(ip_payload->mType);
	//int logPort;
	uint16_t myCid = 0;


	int isNewFlow = 0;
	char store_daddr[16];
	char store_saddr[16];
	memset(&store_daddr, 0 ,sizeof(store_daddr));
	memset(&store_saddr, 0 ,sizeof(store_saddr));
	/*
	 * Generate random router in the circuit
	 */
	int routerArr[numHop];
	//initialize all elements to 0
	memset(&routerArr, 0 ,sizeof(numHop));
	int routerArr_index = 0;

	/*
	 * Set up Tunnel
	 */
    char tun_name[IFNAMSIZ];
    char buffer_tun[2048];

    int maxfds1;


    /* Connect to the tunnel interface (make sure you create the tunnel interface first) */
    strcpy(tun_name, "tun1");
    int tun_fd = tun_alloc(tun_name, IFF_TUN | IFF_NO_PI);

    if(tun_fd < 0)
    {
    	perror("Open tunnel interface");
    	exit(1);
    }



	//create socket
	sockfd = socket(AF_INET,SOCK_DGRAM,0); //use UDP
	//check for error
	if(sockfd < 0)
	{
		error ("ERROR creating socket in server...");
	}
	//initialize all fields in serv_addr to 0
	memset((char*) &serv_addr, 0 ,sizeof(serv_addr));
	//set fields in struct sockaddr_in
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(0); //Bind to port 0 and let the OS assigns the UDP port
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	//bind the socket to an addr
	if(bind(sockfd,(struct sockaddr*) &serv_addr,sizeof(serv_addr)) < 0)
	{
		error("ERROR binding to socket in server...");
	}
	else
	{
		servlen = sizeof(serv_addr);
		getsockname(sockfd,(struct sockaddr *) &serv_addr, &servlen);
		portno = ntohs(serv_addr.sin_port);
	}

	/*
	 * write to config in stage 4
	 */
	if(stageNum == 4 || stageNum == 5)
	{
		writeInfoToFileStage4Part1(fp, portno);
	}

	/*
	 * Calculate max fd set
	 */
    if(tun_fd > sockfd)
    {
    	maxfds1 = tun_fd + 1;
    }
    else
    {
    	maxfds1 = sockfd + 1;
    }

	clilen = sizeof(cli_addr);

	/*
	 * Check number of hop specified in config file matches the limit
	 */
	if(numHop > numRouter)
	{
		error ("FATAL ERROR number of hops are more than number of routers, exiting...\n");
	}

	//re-use i - index, set it back to 0
	i = -1;

	int k = 0; //index for client struct array
	int saved_numHop = numHop;
	/*
	 * FOR LOOP to fork as many routers as specified in the config file
	 */
	for(int n = 0; n < numRouter; n++)
	{
		noRouter++; //Update number of router after each forking
		i++;
		fflush(fp);
		pid = fork(); //fork a router based on number of router
		if(pid < 0)
		{
			error ("ERROR on fork");
		}
		else if(pid == 0) //in CHILD process
		{
			printf( "my i = %d\n", i);
			printf ("IP assgined to router %d = %s\n",noRouter,ip_arr[i]);
			communicateToServerStage5(portno,noRouter,ip_arr[i]);
			break;
			//close(sockfd);
		}
		else //in PARENT process
		{
			if(noRouter < numRouter)
			{
				printf("Waiting on port %d\n", portno);
				data = recvfrom(sockfd,buffer,sizeof(struct payload),0,(struct sockaddr*) &cli_addr, &clilen);
				cliAddr_arr[k] = cli_addr;

				printf ("Data payload size: %d\n",data); // == 8
				if(data < 0)
				{
					printf ("ERROR receiving message in port %d\n",portno);
				}
				else
				{
					p = (struct payload*)buffer;
				}
				routerPort = ntohs(cli_addr.sin_port);
				routerPortArr[k] = routerPort;
				//routerArr[k] = p->router;
				k++;
				writeInfoToFileStage5Part1 (fp, p->router,p->pid,routerPort,p->routerIP);
			}
			else
			{
				/*
				 * read data from the last router where noRouter == numRouter
				 */
				printf("Waiting on port %d\n", portno);
				data = recvfrom(sockfd,buffer,sizeof(struct payload),0,(struct sockaddr*) &cli_addr, &clilen);
				cliAddr_arr[k] = cli_addr;
				printf ("Data payload size: %d\n",data); // == 8
				if(data < 0)
				{
					printf ("ERROR receiving message in port %d\n",portno);
				}
				else
				{
					p = (struct payload*)buffer;
				}
				routerPort = ntohs(cli_addr.sin_port);
				routerPortArr[k] = routerPort;
				writeInfoToFileStage5Part1 (fp, p->router,p->pid,routerPort,p->routerIP);

				printf("\nDone gathering info from all router. Waiting for first traffic from tunnel...\n");

				//WHILE LOOP to keep reading from tunnel or router(s)
				while(1)
				{
					int sel_return;
					fd_set readfds;

					FD_ZERO(&readfds); //initialize the set
					FD_SET(tun_fd, &readfds); //add tunnel fd to fd_set
					FD_SET(sockfd, &readfds); //add sockfd to fd_set

					/*
					 * Waiting for connection from tunnel or router. If after 7 seconds with no communication, function exits
					 * and terminates program
					 */
					sel_return = select(maxfds1,&readfds,NULL,NULL,&tv);

					if(sel_return < 0)
					{
						printf("ERROR in selecting \n");
						exit(1);
					}
					else if(sel_return == 0)
					{
						printf("After 7 seconds without any communication. Function exits, program is terminated\n");
						//kill(p->pid,SIGKILL);
						break;
					}
					if(FD_ISSET(tun_fd, &readfds))
					{
						/*
						 * reset timeout
						 */
						tv.tv_sec = 7;
						tv.tv_usec = 0;

						/* Now read data coming from the tunnel */
						int nread = read(tun_fd,buffer_tun,sizeof(buffer_tun));
						if(nread < 0)
						{
							perror("Reading from tunnel interface");
							close(tun_fd);
							exit(-1);
						}
						else
						{
							printf("\nRead a packet from tunnel, packet length:%d\n", nread);

							/*
							 * ip header info
							 */
							ip = (struct iphdr*) buffer_tun;
							inet_ntop(AF_INET,&ip->saddr,saddr,sizeof(saddr));
							inet_ntop(AF_INET,&ip->daddr,daddr,sizeof(daddr));

							/*
							 * icmp header info
							 */
							icmp = (struct icmphdr*) (buffer_tun+sizeof(struct iphdr));
							/*
							 * check if it is a new flow
							 */
							if(strncmp (store_saddr,saddr,sizeof(store_saddr)) == 0 && strncmp (store_daddr,daddr,sizeof(store_daddr)) == 0)
							{
								isNewFlow = 0;
							}
							else
							{
								isNewFlow = 1;
								numHop = saved_numHop;
							}

							if(isNewFlow)
							{
								printf("\nIt's a new flow, creating circuit...\n");
								printf("\nReceived traffic from tunnel. Setting up circuits with %d hop(s)...\n",numHop);
								for(int m=0; m < 16; m++)
								{
									store_daddr[m] = daddr[m];
									store_saddr[m] = saddr[m];
								}

								k = 0; //reset k index
								srand(time(NULL));
								/*
								 * Generate random routers to create circuit
								 */
								int my_random = 0;
								int isSame = 0;
								while(routerArr_index < numHop)
								{
									my_random = (rand() % numRouter) + 1;
									for(int m = 0; m < numHop; m++)
									{
										if(my_random == routerArr[m])
										{
											isSame = 1;
											break;
										}
									}
									if(isSame == 0)
									{
										//add to routerArr
										routerArr[routerArr_index] = my_random;
										routerArr_index++;
									}
									else
									{
										isSame = 0; //set back to zero if it's set to 1
									}
								}
								/*
								 * Log hop: N, router: M
								 */
								for(int i = 0; i < numHop; i++)
								{
									fprintf(fp,"Hop: %d, router: %d\n",i+1,routerArr[i]);
								}
							}

							/*
							 * Building circuit hop by hop until hop count = 0
							 */
							if(isNewFlow)
							{
								while(numHop > 0)
								{
									if(numHop == 1)
									{
										payload->rPort = htons(65535); //convert to network byte order, last router 0xffff
										printf("\nSending last circuit-extend request with port %#06x\n",payload->rPort);
									}
									else
									{
										payload->rPort = htons(routerPortArr[routerArr[k+1]-1]); //convert to network byte order
										printf ("Real router port: %d\n", routerPortArr[routerArr[k+1]-1]);
										printf  ("router port from struct: %d\n", payload->rPort);
										k++;
									}
									cMsg->cmIpPayload.mType = 82; //It is 0x52 in hex. Type = Circuit-extend
									cli_addr = cliAddr_arr[routerArr[0]-1];

									cMsg->cmIpPayload.payload = (char*) payload;

									printf ("SENDING MINITOR CONTROL MESSAGE TO ROUTER\n");
									data = sendto(sockfd,cMsg,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
									if(data < 0)
									{
										printf ("ERROR sending minitor control message to router\n");
									}
									//printf("Size of data sent: %d\n",data);

									printf ("SENDING MINITOR PAYLOAD TO ROUTER\n");
									data = sendto(sockfd,payload,sizeof(struct controlMsg_payload),0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
									if(data < 0)
									{
										printf ("ERROR sending minitor payload to router\n");
									}
									//printf("Size of data sent: %d\n",data);

									/*
									 * Waiting to receive circuit-extend-done before sending other circuit-extend request
									 */
									data = recvfrom(sockfd,buf_cm,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr, &clilen);
									if(data < 0)
									{
										printf ("ERROR receiving circuit-extend-done from router\n");
										exit(1);
									}
									printf ("RECEVING MINITOR CONTROL MESSAGE CIRCUIT-EXTEND-DONE FROM ROUTER\n");
									//printf ("Size of data received: %d\n",data);

									ip_hdr = (struct iphdr*) buf_cm;
									ip_payload = (struct ipPayload*) (buf_cm+sizeof(struct iphdr));

									printf ("My received protocol from router = %d\n", ip_hdr->protocol);
									printf ("My received message type from router = %#04x\n", ip_payload->mType);

									myCid = ntohs(ip_payload->cid);
									/*
									 * additional logging all received packet for verification
									 */
									fprintf(fp,"pkt from port: %d, length: %d, contents: 0x%x%04x\n",routerPortArr[routerArr[0]-1],pktSize,ip_payload->mType,myCid);

									/*
									 * Logging info as proxy receives circuit-extend-done
									 */
									fprintf(fp,"incoming extend-done circuit, incoming: 0x%x from port: %d\n",myCid, routerPortArr[routerArr[0]-1]);

									numHop--;
								}
							}
							/*
							 * write to file
							 */
							writeInfoToFileStage2Part2(fp, routerPort,"tunnel", saddr, daddr,icmp->type);

							/*
							 * Done building circuit, now send actual data from tunnel interface through circuit to outside world
							 */
							printf("\nDone building circuit. Now proxy forward actual data from tunnel interface through existing circuit to outside world with relay-data control message\n");

							//change type of control message
							cMsg->cmIpPayload.mType = 81; //It is 0x51 in hex. Type = relay-data
							cli_addr = cliAddr_arr[routerArr[0]-1]; //addr of router to send

							for (int m = 0; m < (int) sizeof(buffer_tun); m++)
							{
								payload_long->icmp_pkt[m] = buffer_tun[m];
							}

							cMsg->cmIpPayload.payload = (char*) payload_long;

							printf ("SENDING MINITOR CONTROL MESSAGE RELAY-DATA TO ROUTER\n");
							data = sendto(sockfd,cMsg,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
							if(data < 0)
							{
								printf ("ERROR SENDING MINITOR CONTROL MESSAGE RELAY-DATA TO ROUTER\n");
							}
							//printf("Size of data sent: %d\n",data);

							printf ("SENDING MINITOR PAYLOAD LONG TO ROUTER\n");
							data = sendto(sockfd,payload_long->icmp_pkt,nread,0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
							if(data < 0)
							{
								printf ("ERROR SENDING MINITOR PAYLOAD LONG TO ROUTER\n");
							}
							//printf("Size of data sent: %d\n",data);
						}
					}
					if(FD_ISSET(sockfd, &readfds))
					{
						/*
						 * reset timeout
						 */
						tv.tv_sec = 7;
						tv.tv_usec = 0;

						/*
						 * Waiting to receive back the ICMP ECHO request from the circuit chain
						 */
						printf ("\nProxy is receiving ICMP ECHO request and processing it...\n");
						data = recvfrom(sockfd,buf_cm,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr, &clilen);
						if(data < 0)
						{
							printf ("ERROR receiving icmp reply message from router\n");
							exit(1);
						}
						//printf ("Size of data received: %d\n",data);

						ip_hdr = (struct iphdr*) buf_cm;
						ip_payload = (struct ipPayload*) (buf_cm+sizeof(struct iphdr));

						printf ("My received protocol from router = %d\n", ip_hdr->protocol);
						printf ("My received message type from router = %#04x\n", ip_payload->mType);

						/*
						 * Now getting payload back
						 */
						printf ("Proxy is processing ICMP ECHO reply packet from router\n");
						data = recvfrom(sockfd,payload_long->icmp_pkt,84,0,(struct sockaddr*) &cli_addr, &clilen);
						if(data < 0)
						{
							printf ("ERROR receiving and processing ICMP ECHO reply packet from router\n");
							exit(1);
						}
						//printf ("Size of data received: %d\n",data);

						/*
						 * Getting info from received minitor payload
						 */
						ip = (struct iphdr*) payload_long->icmp_pkt;
						inet_ntop(AF_INET,&ip->saddr,saddr,sizeof(saddr));
						inet_ntop(AF_INET,&ip->daddr,daddr,sizeof(daddr));

						/*
						 * icmp header info
						 */
						icmp = (struct icmphdr*) (payload_long->icmp_pkt+sizeof(struct iphdr));

						pktSize = data + sizeof(ip_payload->mType) + sizeof(ip_payload->cid);
						myCid = ntohs(ip_payload->cid);
						/*
						 * Additional logging
						 */
						fprintf(fp,"pkt from port: %d, length: %d, contents: 0x%x%04x",routerPortArr[routerArr[0]-1],pktSize,ip_payload->mType,myCid);
						writeInfoToFileStage5Part2 (fp, (unsigned char*) payload_long->icmp_pkt, 84);

						/*
						 * Logging
						 */
						fprintf(fp,"incoming packet, circuit incoming: 0x%x, src: %s, dst: %s\n",myCid,saddr,daddr);

						/*
						 * Check destination Ip addr to send back to tunnel or $ETH0
						 */
						if(strncmp ("10.5.51.2",daddr,9) == 0)
						{
							/*
							 * write back to tunnel
							 */
							printf("Writing ICMP REPLY packet back to tunnel interface from proxy\n");
							int nwrite = write(tun_fd,payload_long->icmp_pkt,data);
							if(nwrite < 0)
							{
								error("Writing to tunnel interface");
								close(tun_fd);
								exit(-1);
							}
						}
						else
						{
							int rawfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
							if(rawfd < 0)
							{
								error ("ERROR creating raw socket in proxy\n");
							}

							//initialize all fields in serv_addr to 0
							memset(&dest_addr, 0 ,sizeof(dest_addr));
							//set fields in struct sockaddr_in
							dest_addr.sin_family = AF_INET;
							dest_addr.sin_port = htons(0);
							dest_addr.sin_addr.s_addr = ip->daddr;

							/*
							 * construct struct msghdr to pass in sendmsg()
							 */
							msgh.msg_name = &dest_addr;
							msgh.msg_namelen = sizeof(dest_addr);

							msgh.msg_iovlen = 1;
							msgh.msg_iov = &io;
							msgh.msg_iov->iov_base = icmp;
							msgh.msg_iov->iov_len = ntohs(ip->tot_len) - sizeof(struct iphdr);
							msgh.msg_control = NULL;
							msgh.msg_controllen = 0;
							msgh.msg_flags = 0;

							//send ICMP ECHO to $ETH0
							printf("Sending ICMP REPLY packet to ETH0...\n");
							data = sendmsg(rawfd, &msgh,0);
							if(data < 0)
							{
								printf ("ERROR sending icmp echo message from proxy to ETH0 interface\n");
							}
							printf ("Sent successfully! Closing sockets and exiting programs.\n");
							printf ("Please be patient for program to exit gracefully, do not Ctrl+C\n");
							close(rawfd);
						}
					}
				}
				break; //break out of for loop when parent is done
			}
		}
	}
	fclose(fp);
	freeifaddrs(ifaddr);
	free(buffer);
	free(cMsg);
	free(payload);
	free(payload_long);
	free(buf_payload_long);
	free(buf_cm);
	close(tun_fd);
	close(sockfd);
}

/*
 * Create communication for stage 6
 */
void createCommunicationStage6(int stageNum, int numRouter, int numHop)
{
	FILE *fp;
	fp = fopen("stage6.proxy.out","w");
	if(fp == NULL)
	{
		error ("Can't open file for write");
	}

	/*
	 * Get a list of all available interfaces in order to assign to each router.
	 * I re-use the general idea and code to get all available interfaces using getifaddrs(3)
	 * Source code is from here: http://man7.org/linux/man-pages/man3/getifaddrs.3.html
	 */
	struct ifaddrs *ifaddr, *ifa;
	int family, name_check,n;
	char eth_ip_addr [15];
	char *eth_name;
	char ip_arr[8][15];
	int i = 0;
	if(getifaddrs(&ifaddr) == -1)
	{
		error ("getifaddrs function call failed!");
	}

	n=0;
	for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
	{
		if(ifa->ifa_addr == NULL)
		{
			continue;
		}
		family = ifa->ifa_addr->sa_family;
		if(family == AF_INET)
		{
			eth_name = ifa->ifa_name;
			if(strncmp(eth_name,"lo",2) == 0 || strncmp(eth_name,"tun1",4) == 0)
			{
				continue;
			}

			name_check = getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in),eth_ip_addr,sizeof(eth_ip_addr),NULL,0,NI_NUMERICHOST);
			if(name_check != 0) //fail
			{
				error ("getnameinfo failed!");
			}
			n++;
			if(strncmp (eth_ip_addr,"192.168.20",10) == 0)
			{
				if(i >= 8)
				{
					error ("ERROR ethernet ip address array out of bound! \n");
				}
				strcpy(ip_arr[i], eth_ip_addr);
				i++;
			}
		}
	}
	printf ("My i initial = %d\n",i);

	for (int k = 0; k < 6; k++)
	{
		printf("IP addr in ip arr outside the LOOP: %s\n", ip_arr[k]);
	}
	//printf(" n = %d\n", n);

	/*
	 * Necessary struct and constant declaration for sendmsg() - sending raw socket to ETH0
	 */
	struct iovec io;
	struct msghdr msgh;
	memset(&msgh,0,sizeof(msgh));
	memset(&io,0,sizeof(io));


	int portno, data, noRouter, routerPort, pid;
	int routerPortArr [numRouter];

	int sockfd;
	socklen_t servlen,clilen;
	noRouter = 0;

	struct timeval tv; //time interval wait for select()
	tv.tv_sec = 7;
	tv.tv_usec = 0;

	struct icmphdr* icmp;
	struct iphdr* ip;

	char saddr[16];
	char daddr[16];

	struct sockaddr_in serv_addr, cli_addr, dest_addr;
	struct sockaddr_in cliAddr_arr[numRouter];

	//Getting router info from router
	struct payload {
		int router;
		int pid;
		char* routerIP;
	};
	struct payload *p;

	char* buffer = malloc(sizeof(struct payload));


	/*
	 * Create struct for control message in Minitor
	 */

	struct controlMessage *cMsg = (struct controlMessage*) malloc(sizeof(struct controlMessage));

	inet_pton(AF_INET,"127.0.0.1", &cMsg->cmIphdr.saddr);
	inet_pton(AF_INET,"127.0.0.1", &cMsg->cmIphdr.daddr);

	cMsg->cmIphdr.protocol = 253;
	cMsg->cmIphdr.check = 0;
	cMsg->cmIphdr.frag_off = 0;
	cMsg->cmIphdr.id = 0;
	cMsg->cmIphdr.ihl = 0;
	cMsg->cmIphdr.tos = 0;
	cMsg->cmIphdr.tot_len = 0;
	cMsg->cmIphdr.ttl = 0;
	cMsg->cmIphdr.version = 0;

	/*
	 * calculate CID at proxy where i = 0. Func: i*256+s
	 * Don't set type and payload until later
	 */
	uint16_t cid = 0 * 256 +1;
	cMsg->cmIpPayload.cid = htons(cid);

	/*
	 * Struct for payload
	 */
	struct controlMsg_payload {
		uint16_t rPort;
	}; //this struct is for CID in payload portion
	struct controlMsg_payload *payload = (struct controlMsg_payload*) malloc(sizeof(struct controlMsg_payload));

	struct controlMsg_payload_long {
		char icmp_pkt[2048];
	}; //this struct is for ICMP packet payload
	struct controlMsg_payload_long *payload_long = (struct controlMsg_payload_long *) malloc(sizeof(struct controlMsg_payload_long ));

	struct controlMsg_key {
		unsigned char sKey[16];
	};
	struct controlMsg_key *mKey = (struct controlMsg_key *)  malloc (sizeof(struct controlMsg_key));

	char* buf_cm = malloc(sizeof(struct controlMessage));
	char* buf_payload_long = malloc(sizeof(struct controlMsg_payload_long));
	/*
	 * Getting info from received minitor control message
	 */
	struct iphdr* ip_hdr;
	struct ipPayload* ip_payload;

	int pktSize = sizeof(ip_payload->cid) + sizeof(ip_payload->mType);
	//int logPort;
	uint16_t myCid = 0;

	int hopCount = 1;

	/*
	 * 64-bit number array of size 2
	 * Used to generate 128-bit random number
	 */
	unsigned char random_key[16];
	unsigned char key_lastRouter[16];
	//unsigned char session_key[16];
	unsigned char split_portNum[2];
	unsigned char *myKeyList[numHop];
	for(int m = 0; m < numHop; m++)
	{
		myKeyList[m] = NULL;
	}
	unsigned char *mySessionKey;
	unsigned char *buf_payload = malloc (2048);
	int buf_payload_size = 2048;
	int sessionKey_index = 0;

	/*
	 * Generate random router in the circuit
	 */
	int routerArr[numHop];
	//initialize all elements to 0
	memset(&routerArr, 0 ,sizeof(numHop));
	int routerArr_index = 0;

	/*
	 * variable needed to check if there is a new flow
	 */
	int isNewFlow = 0;
	char store_daddr[16];
	char store_saddr[16];
	memset(&store_daddr, 0 ,sizeof(store_daddr));
	memset(&store_saddr, 0 ,sizeof(store_saddr));

	/*
	 * Variable needed for AES encryption
	 */
	unsigned char *crypt_text;
	int crypt_text_len;
	unsigned char *clear_crypt_text;
	int clear_crypt_text_len;

	AES_KEY enc_key;
	AES_KEY dec_key;

	unsigned char *crypt_buffer = malloc (2048);
	int crypt_buf_size = 2048;

	/*
	 * Set up Tunnel
	 */
    char tun_name[IFNAMSIZ];
    char buffer_tun[2048];

    int maxfds1;


    /* Connect to the tunnel interface (make sure you create the tunnel interface first) */
    strcpy(tun_name, "tun1");
    int tun_fd = tun_alloc(tun_name, IFF_TUN | IFF_NO_PI);

    if(tun_fd < 0)
    {
    	perror("Open tunnel interface");
    	exit(1);
    }



	//create socket
	sockfd = socket(AF_INET,SOCK_DGRAM,0); //use UDP
	//check for error
	if(sockfd < 0)
	{
		error ("ERROR creating socket in server...");
	}
	//initialize all fields in serv_addr to 0
	memset((char*) &serv_addr, 0 ,sizeof(serv_addr));
	//set fields in struct sockaddr_in
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(0); //Bind to port 0 and let the OS assigns the UDP port
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	//bind the socket to an addr
	if(bind(sockfd,(struct sockaddr*) &serv_addr,sizeof(serv_addr)) < 0)
	{
		error("ERROR binding to socket in server...");
	}
	else
	{
		servlen = sizeof(serv_addr);
		getsockname(sockfd,(struct sockaddr *) &serv_addr, &servlen);
		portno = ntohs(serv_addr.sin_port);
	}

	if(stageNum == 6)
	{
		writeInfoToFileStage4Part1(fp, portno);
	}

	/*
	 * Calculate max fd set
	 */
    if(tun_fd > sockfd)
    {
    	maxfds1 = tun_fd + 1;
    }
    else
    {
    	maxfds1 = sockfd + 1;
    }

	clilen = sizeof(cli_addr);

	/*
	 * Check number of hop specified in config file matches the limit
	 */
	if(numHop > numRouter)
	{
		error ("FATAL ERROR number of hops are more than number of routers, exiting...\n");
	}

	//re-use i - index, set it back to 0
	i = -1;

	int k = 0; //index for client struct array
	int saved_numHop = numHop;
	/*
	 * FOR LOOP to fork as many routers as specified in the config file
	 */
	for(int n = 0; n < numRouter; n++)
	{
		noRouter++; //Update number of router after each forking
		i++;
		fflush(fp);
		pid = fork(); //fork a router based on number of router
		if(pid < 0)
		{
			error ("ERROR on fork");
		}
		else if(pid == 0) //in CHILD process
		{
			printf( "my i = %d\n", i);
			printf ("IP assgined to router %d = %s\n",noRouter,ip_arr[i]);
			communicateToServerStage6(portno,noRouter,ip_arr[i]);
			break;
		}
		else //in PARENT process
		{
			if(noRouter < numRouter)
			{
				printf("Waiting on port %d\n", portno);
				data = recvfrom(sockfd,buffer,sizeof(struct payload),0,(struct sockaddr*) &cli_addr, &clilen);
				cliAddr_arr[k] = cli_addr;

				printf ("Data payload size: %d\n",data); // == 8
				if(data < 0)
				{
					printf ("ERROR receiving message in port %d\n",portno);
				}
				else
				{
					p = (struct payload*)buffer;
				}
				routerPort = ntohs(cli_addr.sin_port);
				routerPortArr[k] = routerPort;
				k++;
				writeInfoToFileStage5Part1 (fp, p->router,p->pid,routerPort,p->routerIP);
			}
			else
			{
				/*
				 * read data from the last router where noRouter == numRouter
				 */
				printf("Waiting on port %d\n", portno);
				data = recvfrom(sockfd,buffer,sizeof(struct payload),0,(struct sockaddr*) &cli_addr, &clilen);
				cliAddr_arr[k] = cli_addr;
				printf ("Data payload size: %d\n",data); // == 8
				if(data < 0)
				{
					printf ("ERROR receiving message in port %d\n",portno);
				}
				else
				{
					p = (struct payload*)buffer;
				}
				routerPort = ntohs(cli_addr.sin_port);
				routerPortArr[k] = routerPort;
				writeInfoToFileStage5Part1 (fp, p->router,p->pid,routerPort,p->routerIP);


				/*
				 *
				 */
				printf("\nDone gathering info from all router. Waiting for first traffic from tunnel...\n");

				//WHILE LOOP to keep reading from tunnel or router(s)
				while(1)
				{
					int sel_return;
					fd_set readfds;

					FD_ZERO(&readfds); //initialize the set
					FD_SET(tun_fd, &readfds); //add tunnel fd to fd_set
					FD_SET(sockfd, &readfds); //add sockfd to fd_set

					/*
					 * Waiting for connection from tunnel or router. If after 7 seconds with no communication, function exits
					 * and terminates program
					 */
					sel_return = select(maxfds1,&readfds,NULL,NULL,&tv);

					if(sel_return < 0)
					{
						printf("ERROR in selecting \n");
						exit(1);
					}
					else if(sel_return == 0)
					{
						printf("After 7 seconds without any communication. Function exits, program is terminated\n");
						//kill(p->pid,SIGKILL);
						break;
					}
					if(FD_ISSET(tun_fd, &readfds))
					{
						/*
						 * reset timeout
						 */
						tv.tv_sec = 7;
						tv.tv_usec = 0;

						/* Now read data coming from the tunnel */
						int nread = read(tun_fd,buffer_tun,sizeof(buffer_tun));
						if(nread < 0)
						{
							perror("Reading from tunnel interface");
							close(tun_fd);
							exit(-1);
						}
						else
						{
							printf("\nRead a packet from tunnel, packet length:%d\n", nread);
							/*
							 * ip header info
							 */
							ip = (struct iphdr*) buffer_tun;
							inet_ntop(AF_INET,&ip->saddr,saddr,sizeof(saddr));
							inet_ntop(AF_INET,&ip->daddr,daddr,sizeof(daddr));


							/*
							 * icmp header info
							 */
							icmp = (struct icmphdr*) (buffer_tun+sizeof(struct iphdr));
							/*
							 * check if it is a new flow
							 */
							if(strncmp (store_saddr,saddr,sizeof(store_saddr)) == 0 && strncmp (store_daddr,daddr,sizeof(store_daddr)) == 0)
							{
								isNewFlow = 0;
							}
							else
							{
								isNewFlow = 1;
								numHop = saved_numHop;
							}

							if(isNewFlow)
							{
								printf("\nIt's a new flow, creating circuit...\n");
								printf("\nReceived traffic from tunnel. Setting up circuits with %d hop(s)...\n",numHop);
								for(int m=0; m < 16; m++)
								{
									store_daddr[m] = daddr[m];
									store_saddr[m] = saddr[m];
								}

								srand(time(NULL));

								/*
								 * Generate random routers to create circuit
								 */
								int my_random = 0;
								int isSame = 0;
								while(routerArr_index < numHop)
								{
									my_random = (rand() % numRouter) + 1;
									for(int m = 0; m < numHop; m++)
									{
										if(my_random == routerArr[m])
										{
											isSame = 1;
											break;
										}
									}
									if(isSame == 0)
									{
										//add to routerArr
										routerArr[routerArr_index] = my_random;
										routerArr_index++;
									}
									else
									{
										isSame = 0; //set back to zero if it's set to 1
									}
								}
								/*
								 * Log hop: N, router: M
								 */
								for(int i = 0; i < numHop; i++)
								{
									fprintf(fp,"Hop: %d, router: %d\n",i+1,routerArr[i]);
								}
							}
							int print_index = 0;
							k = 0; //reset index k
							if(isNewFlow)
							{
								/*
								 * Building circuit hop by hop until hop count = 0
								 */
								while(numHop > 0)
								{
									/*
									 * Generate 16 copies of last router
									 * generating pseudo-random 128-bit AES key
									 * XOR them to get session key
									 */
									printf ("\nGenerating session key...\n");
									printf("My number = 0x");
									mySessionKey = malloc (16);
									for(int m = 0; m < (int) sizeof(key_lastRouter); m++)
									{
										key_lastRouter[m] = routerArr[numHop-1]; //XOR with last hop router number
										random_key[m] = rand();
										//XOR them
										*(mySessionKey+m) = random_key[m] ^ key_lastRouter[m];
										printf("%02x",*(mySessionKey+m));
									}
									printf ("\n");

									/*
									 * Store session for future encryption
									 */
									myKeyList[sessionKey_index] = mySessionKey;

									printf ("MY SESSION KEY INDEX = %d\n",sessionKey_index);
									printf("MY session key at index 0 = 0x");
									for(int m = 0; m < 16; m++)
									{
										printf("%02x",*(myKeyList[0]+m));
									}
									printf ("\n");

									cMsg->cmIpPayload.mType = 101; //It is 0x65 in hex. Type = fake-diffie-hellman
									cli_addr = cliAddr_arr[routerArr[0]-1];

									printf ("SENDING FAKE-DIFFIE-HELLMAN TO ROUTER\n");
									data = sendto(sockfd,cMsg,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
									if(data < 0)
									{
										printf ("ERROR sending fake-diffie-hellman from proxy to router\n");
									}
									//printf("Size of data sent: %d\n",data);


									if(hopCount == 1) //This is first hop, don't need to encrypt the session key
									{
										printf ("THIS IS FIRST HOP, DON'T NEED TO ENCRYPT SESSION KEY\n");
										for (int m = 0; m < (int) sizeof(struct controlMsg_key); m++)
										{
											*(crypt_buffer+m) = *(mySessionKey+m);
										}

										for(int m = 0; m < (int) sizeof(key_lastRouter); m++)
										{
											printf("%02x",*(crypt_buffer+m));
										}
										printf ("\n");
									}
									else
									{
										/*
										 * Re-use functions to encrypt/decrypt provided by Professor and TA
										 */
										for(int i = sessionKey_index-1; i >= 0; i--)
										{
											class_AES_set_encrypt_key(myKeyList[i], &enc_key);
											if(i == sessionKey_index - 1)
											{
												class_AES_encrypt_with_padding(mySessionKey, 16, &crypt_text, &crypt_text_len, &enc_key);
												if(crypt_text_len > crypt_buf_size)
												{
													crypt_buf_size = crypt_text_len*2;
													crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
												}
												for(int m = 0; m < crypt_text_len; m++)
												{
													*(crypt_buffer+ m) = *(crypt_text+m);
												}
												free(crypt_text);
											}
											else
											{
												class_AES_encrypt_with_padding(crypt_buffer, crypt_text_len, &crypt_text, &crypt_text_len, &enc_key);
												if(crypt_text_len > crypt_buf_size)
												{
													crypt_buf_size = crypt_text_len*2;
													crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
												}
												for(int m = 0; m < crypt_text_len; m++)
												{
													*(crypt_buffer+ m) = *(crypt_text+m);
												}
												free(crypt_text);
											}
										}
										printf ("crypt text length after encrypt SESSION KEY = %d\n", crypt_text_len);
									}


									printf ("SENDING MINITOR PAYLOAD ENCRYPTED SESSION KEY TO ROUTER\n");
									/*
									 * Have to separate since the first seesion key sent to first hop is not encrypted. The others are encrypted
									 */
									if(hopCount == 1)
									{
										data = sendto(sockfd,crypt_buffer,sizeof(struct controlMsg_key),0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
										if(data < 0)
										{
											printf ("ERROR sending minitor payload encrypted session key from proxy to router\n");
										}
										//printf("Size of data sent: %d\n",data);
									}
									else
									{
										data = sendto(sockfd,crypt_buffer,crypt_text_len,0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
										if(data < 0)
										{
											printf ("ERROR sending minitor payload encrypted session key from proxy to router\n");
										}
										//printf("Size of data sent: %d\n",data);
									}

									/*
									 * NOW SENDING PORT NEXT NAME
									 */
									if(numHop == 1)
									{
										payload->rPort = htons(65535); //convert to network byte order, last router 0xffff
										printf("\nSending last circuit-extend request with port %#06x\n",payload->rPort);
									}
									else
									{
										payload->rPort = htons(routerPortArr[routerArr[k+1]-1]); //convert to network byte order
										printf ("Real router port: %d\n", routerPortArr[routerArr[k+1]-1]);
										printf  ("router port from struct: %d\n", payload->rPort);
										k++;
									}
									printf ("My port before: %d\n", ntohs(payload->rPort));
									split_portNum[0] = (payload->rPort) & 0xFF;
									split_portNum[1] =  (payload->rPort) >> 8;
									/*
									 * Encrypted port
									 * Re-use functions to encrypt/decrypt provided by Professor and TA
									 */
									if(hopCount == 1)
									{
										class_AES_set_encrypt_key(myKeyList[sessionKey_index], &enc_key);
										class_AES_encrypt_with_padding(split_portNum, sizeof(split_portNum), &crypt_text, &crypt_text_len, &enc_key);
										if(crypt_text_len > crypt_buf_size)
										{
											crypt_buf_size = crypt_text_len*2;
											crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
										}
										for(int m = 0; m < crypt_text_len; m++)
										{
											*(crypt_buffer+ m) = *(crypt_text+m);
										}
										free(crypt_text);
									}
									else
									{
										for(int i = sessionKey_index; i >= 0; i--)
										{
											class_AES_set_encrypt_key(myKeyList[i], &enc_key);

											if(i == sessionKey_index)
											{
												class_AES_encrypt_with_padding(split_portNum, sizeof(split_portNum), &crypt_text, &crypt_text_len, &enc_key);
												if(crypt_text_len > crypt_buf_size)
												{
													crypt_buf_size = crypt_text_len*2;
													crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
												}
												for(int m = 0; m < crypt_text_len; m++)
												{
													*(crypt_buffer+ m) = *(crypt_text+m);
												}
												free(crypt_text);
											}
											else
											{
												//might need to count how many times encrypt so that can call free()
												class_AES_encrypt_with_padding(crypt_buffer, crypt_text_len, &crypt_text, &crypt_text_len, &enc_key);
												if(crypt_text_len > crypt_buf_size)
												{
													crypt_buf_size = crypt_text_len*2;
													crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
												}
												for(int m = 0; m < crypt_text_len; m++)
												{
													*(crypt_buffer+ m) = *(crypt_text+m);
												}
												free(crypt_text);
											}
										}
									}
									printf ("crypt text length after encrypt PORT NEXT NAME = %d\n", crypt_text_len);
								//	class_AES_set_encrypt_key(session_key, &enc_key);

									/*
									 * Now send the encrypted port
									 */
									cMsg->cmIpPayload.mType = 98; //It is 0x62 in hex. Type = encrypted-circuit-extend

									cMsg->cmIpPayload.payload = (char*) payload;

									printf ("SENDING ENCRYPTED CIRCUIT EXTEND TO ROUTER\n");
									data = sendto(sockfd,cMsg,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
									if(data < 0)
									{
										printf ("ERROR sending encrypted circuit extend to router\n");
									}
									//printf("Size of data sent: %d\n",data);

									printf ("SENDING ENCRYPTED PORT TO ROUTER\n");
									data = sendto(sockfd,crypt_buffer,crypt_text_len,0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
									if(data < 0)
									{
										printf ("ERROR sending encrypted port to router\n");
									}
									//printf("Size of data sent: %d\n",data);


									/*
									 * log fake-diffie-hellman message sent
									 */
									fprintf(fp,"new-fake-diffie-hellman, router index: %d, circuit outgoing: 0x%x, key: 0x",routerArr[print_index],cid);
									writeInfoToFileStage5Part2(fp,myKeyList[sessionKey_index],16);
									print_index++;
									hopCount++;

									sessionKey_index++;

									/*
									 * Waiting to receive circuit-extend-done before sending other circuit-extend request
									 */
									data = recvfrom(sockfd,buf_cm,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr, &clilen);
									if(data < 0)
									{
										printf ("ERROR receiving circuit-extend-done from router\n");
										exit(1);
									}
									printf ("RECEVING MINITOR CONTROL MESSAGE CIRCUIT-EXTEND-DONE FROM ROUTER\n");
								//	printf ("Size of data received: %d\n",data);

									ip_hdr = (struct iphdr*) buf_cm;
									ip_payload = (struct ipPayload*) (buf_cm+sizeof(struct iphdr));

									printf ("My received protocol from router = %d\n", ip_hdr->protocol);
									printf ("My received message type from router = %#04x\n", ip_payload->mType);

									myCid = ntohs(ip_payload->cid);
									/*
									 * additional logging all received packet for verification
									 */
									fprintf(fp,"pkt from port: %d, length: %d, contents: 0x%x%04x\n",routerPortArr[routerArr[0]-1],pktSize,ip_payload->mType,myCid);

									/*
									 * Logging info as proxy receives circuit-extend-done
									 */
									fprintf(fp,"incoming extend-done circuit, incoming: 0x%x from port: %d\n",myCid, routerPortArr[routerArr[0]-1]);

									numHop--;
								}
							}

							/*
							 * Done building encrypted circuit, now encrypt data and send through this circuit
							 */
							printf ("\nDONE BUILDING ENCRYPTED CIRCUIT. BEGIN TO SEND ENCRYPTED DATA DOWN THE CHAIN\n");

							/*
							 * write to file
							 */
							writeInfoToFileStage2Part2(fp, routerPort,"tunnel", saddr, daddr,icmp->type);

							//change type of control message
							cMsg->cmIpPayload.mType = 97; //It is 0x61 in hex. Type = relay-data with encryption

							cli_addr = cliAddr_arr[routerArr[0]-1];

							//change src addr = "0.0.0.0" to protect sender identity
							inet_pton(AF_INET,"0.0.0.0", &ip->saddr);

							printf ("MY session key index = %d\n",sessionKey_index);

							/*
							 * Re-use functions to encrypt/decrypt provided by Professor and TA
							 */
							//now Onion Encrypt the content before send
							for(int i = sessionKey_index-1; i >= 0; i--)
							{
								class_AES_set_encrypt_key(myKeyList[i], &enc_key);
								if(i == sessionKey_index-1)
								{
									printf("\nRead a packet from tunnel, packet length:%d\n", nread);
									class_AES_encrypt_with_padding((unsigned char*)buffer_tun, nread, &crypt_text, &crypt_text_len, &enc_key);
									if(crypt_text_len > crypt_buf_size)
									{
										crypt_buf_size = crypt_text_len*2;
										crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
									}
									for(int m = 0; m < crypt_text_len; m++)
									{
										*(crypt_buffer+ m) = *(crypt_text+m);
									}
									free(crypt_text);
								}
								else
								{
									class_AES_encrypt_with_padding(crypt_buffer, crypt_text_len, &crypt_text, &crypt_text_len, &enc_key);
									if(crypt_text_len > crypt_buf_size)
									{
										crypt_buf_size = crypt_text_len*2;
										crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
									}
									for(int m = 0; m < crypt_text_len; m++)
									{
										*(crypt_buffer+ m) = *(crypt_text+m);
									}
									free(crypt_text);
								}
							}
							printf ("crypt text length after encrypt ICMP payload = %d\n", crypt_text_len);


							printf ("SENDING MINITOR CONTROL MESSAGE RELAY-DATA WITH ENCRYPTION TO ROUTER\n");
							data = sendto(sockfd,cMsg,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
							if(data < 0)
							{
								printf ("ERROR SENDING MINITOR CONTROL MESSAGE RELAY-DATA WITH ENCRYPTION TO ROUTER\n");
							}
							//printf("Size of data sent: %d\n",data);

							printf ("SENDING MINITOR PAYLOAD LONG WITH ENCRYPTION TO ROUTER\n");
							data = sendto(sockfd,crypt_buffer,crypt_text_len,0,(struct sockaddr*) &cli_addr,sizeof(cli_addr));
							if(data < 0)
							{
								printf ("ERROR SENDING MINITOR PAYLOAD LONG TO ROUTER\n");
							}
							//printf("Size of data sent: %d\n",data);
						}
					}
					if(FD_ISSET(sockfd, &readfds))
					{
						/*
						 * reset timeout
						 */
						tv.tv_sec = 7;
						tv.tv_usec = 0;

						/*
						 * Waiting to receive back the ICMP ECHO request from the circuit chain
						 */
						printf ("\nProxy is receiving ICMP ECHO request and processing it...\n");
						data = recvfrom(sockfd,buf_cm,sizeof(struct controlMessage),0,(struct sockaddr*) &cli_addr, &clilen);
						if(data < 0)
						{
							printf ("ERROR receiving icmp echo message from router\n");
							exit(1);
						}
						//printf ("Size of data received: %d\n",data);

						ip_hdr = (struct iphdr*) buf_cm;
						ip_payload = (struct ipPayload*) (buf_cm+sizeof(struct iphdr));

						printf ("My received protocol from router = %d\n", ip_hdr->protocol);
						printf ("My received message type from router = %#04x\n", ip_payload->mType);

						/*
						 * Now getting payload back
						 */
						printf ("Proxy is processing ICMP ECHO reply packet from router\n");
						data = recvfrom(sockfd,buf_payload,buf_payload_size,0,(struct sockaddr*) &cli_addr, &clilen);
						if(data < 0)
						{
							printf ("ERROR receiving and processing ICMP ECHO reply packet from router\n");
							exit(1);
						}
						//printf ("Size of data received: %d\n",data);
						/*
						 * Decapsulate the packet
						 * Re-use functions to encrypt/decrypt provided by Professor and TA
						 */
						//now Onion decrypt the content before send
						for(int i = 0; i < sessionKey_index; i++)
						{
							class_AES_set_decrypt_key(myKeyList[i], &dec_key);

							if(i == 0)
							{
								class_AES_decrypt_with_padding(buf_payload, data, &clear_crypt_text, &clear_crypt_text_len, &dec_key);
								if(clear_crypt_text_len > crypt_buf_size)
								{
									crypt_buf_size = clear_crypt_text_len*2;
									crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
								}
								for(int m = 0; m < clear_crypt_text_len; m++)
								{
									*(crypt_buffer+ m) = *(clear_crypt_text+m);
								}
								free(clear_crypt_text);
							}
							else
							{
								class_AES_decrypt_with_padding(crypt_buffer, clear_crypt_text_len, &clear_crypt_text, &clear_crypt_text_len, &dec_key);
								if(clear_crypt_text_len > crypt_buf_size)
								{
									crypt_buf_size = clear_crypt_text_len*2;
									crypt_buffer = realloc(crypt_buffer,crypt_buf_size);
								}
								for(int m = 0; m < clear_crypt_text_len; m++)
								{
									*(crypt_buffer+ m) = *(clear_crypt_text+m);
								}
								free(clear_crypt_text);
							}
						}
						printf ("clear crypt text length after decrypt payload from router at proxy = %d\n", clear_crypt_text_len);

						/*
						 * Getting info from received minitor payload
						 */

						ip = (struct iphdr*) crypt_buffer;
						inet_ntop(AF_INET,&ip->saddr,saddr,sizeof(saddr));
						inet_ntop(AF_INET,&ip->daddr,daddr,sizeof(daddr));

						/*
						 * Fill back the correct destination addr
						 */
						inet_pton(AF_INET,store_saddr, &ip->daddr);

						/*
						 * icmp header info
						 */
						icmp = (struct icmphdr*) (crypt_buffer+sizeof(struct iphdr));

						pktSize = data + sizeof(ip_payload->mType) + sizeof(ip_payload->cid);
						myCid = ntohs(ip_payload->cid);
						/*
						 * Additional logging
						 */
						fprintf(fp,"pkt from port: %d, length: %d, contents: 0x%x%04x",routerPortArr[routerArr[0]-1],pktSize,ip_payload->mType,myCid);
						writeInfoToFileStage5Part2 (fp, buf_payload, data);

						/*
						 * Logging
						 */
						fprintf(fp,"incoming packet, circuit incoming: 0x%x, src: %s, dst: %s\n",myCid,saddr,store_saddr);

						/*
						 * Check destination Ip addr to send back to tunnel or $ETH0
						 */
						if(strncmp ("10.5.51.2",daddr,9) == 0)
						{
							/*
							 * write back to tunnel
							 */
							printf("Writing ICMP REPLY packet back to tunnel interface from proxy\n");
							int nwrite = write(tun_fd,crypt_buffer,data);
							if(nwrite < 0)
							{
								error("Writing to tunnel interface");
								close(tun_fd);
								exit(-1);
							}
						}
						else
						{
							int rawfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
							if(rawfd < 0)
							{
								error ("ERROR creating raw socket in proxy\n");
							}

							//initialize all fields in serv_addr to 0
							memset(&dest_addr, 0 ,sizeof(dest_addr));
							//set fields in struct sockaddr_in
							dest_addr.sin_family = AF_INET;
							dest_addr.sin_port = htons(0);
							dest_addr.sin_addr.s_addr = ip->daddr;

							/*
							 * construct struct msghdr to pass in sendmsg()
							 */
							msgh.msg_name = &dest_addr;
							msgh.msg_namelen = sizeof(dest_addr);

							msgh.msg_iovlen = 1;
							msgh.msg_iov = &io;
							msgh.msg_iov->iov_base = icmp;
							msgh.msg_iov->iov_len = ntohs(ip->tot_len) - sizeof(struct iphdr);
							msgh.msg_control = NULL;
							msgh.msg_controllen = 0;
							msgh.msg_flags = 0;

							//send ICMP ECHO to $ETH0
							printf("Sending ICMP REPLY packet to ETH0...\n");
							data = sendmsg(rawfd, &msgh,0);
							if(data < 0)
							{
								printf ("ERROR sending icmp echo message from proxy to ETH0 interface\n");
							}
							printf ("Sent successfully! Closing sockets and exiting programs.\n");
							printf ("Please be patient for program to exit gracefully, do not Ctrl+C\n");
							close(rawfd);
						}
					}
				}
				break; //break out of for loop when parent is done
			}
		}
	}
	/*
	 * free all keys
	 */
	if(myKeyList[0] != NULL)
	{
		for(int m = 0; m < hopCount-1; m++)
		{
			if(myKeyList[m] != NULL)
			{
				free(myKeyList[m]);
			}
		}
	}
	fclose(fp);
	freeifaddrs(ifaddr);
	free(buffer);
	free(cMsg);
	free(payload);
	free(payload_long);
	free(buf_payload_long);
	free(buf_cm);
	free(mKey);
	free(crypt_buffer);
	free(buf_payload);
	close(tun_fd);
	close(sockfd);
}

/*
 * write info to config file for stage 1
 */
void writeInfoToFileStage1(int portNum, int routerNum,int rPid,int rPort)
{
	FILE *fp = fopen("stage1.proxy.out","w");
	if(fp == NULL)
	{
		error ("Can't open file for write");
	}
	fprintf(fp,"proxy port: %d\nrouter: %d, pid: %d, port: %d\n",portNum,routerNum,rPid,rPort);
	fclose(fp);
}

/*
 * write info to config file for stage 2 part 1
 */
void writeInfoToFileStage2Part1(FILE *fp1, int portNum, int routerNum,int rPid,int rPort)
{
	fprintf(fp1,"proxy port: %d\nrouter: %d, pid: %d, port: %d\n",portNum,routerNum,rPid,rPort);
}

/*
 * write info to config file for stage 2 part 2
 */
void writeInfoToFileStage2Part2(FILE *fp1, int rPort,char *from, char *src,char *dest,int type)
{
	if(strncmp (from, "port",4) == 0)
	{
		fprintf(fp1,"ICMP from %s: %d, src: %s, dst: %s, type: %d\n",from,rPort,src,dest,type);
	}
	else
	{
		fprintf(fp1,"ICMP from %s, src: %s, dst: %s, type: %d\n",from,src,dest,type);
	}
}

/*
 * Write info to config file for stage 4 part 1
 */
void writeInfoToFileStage4Part1(FILE *fp1, int portNum)
{
	fprintf(fp1,"proxy port: %d\n",portNum);
}

/*
 * write info to config file for stage 4 part 2
 */
void writeInfoToFileStage4Part2(FILE *fp1, int routerNum,int rPid,int rPort)
{
	fprintf(fp1,"router: %d, pid: %d, port: %d\n",routerNum,rPid,rPort);
}

/*
 * Write info to config file for Stage 5 part 1
 */
void writeInfoToFileStage5Part1(FILE *fp1, int routerNum,int rPid,int rPort, char* rIP)
{
	fprintf(fp1,"router: %d, pid: %d, port: %d, IP: %s\n",routerNum,rPid,rPort, rIP);
}

/*
 * Write info to config file for Stage 5 part 2
 */
void writeInfoToFileStage5Part2 (FILE *fp1, unsigned char *content_arr, int arr_size)
{
	for(int i = 0; i < arr_size; i++)
	{
		fprintf(fp1,"%02x",content_arr[i]);
	}
	fprintf(fp1,"\n");
}
/*
 * Set up tunnel
 * Code re-use provided by Professor and TA on Moodle
 */
int tun_alloc(char *dev, int flags)
{
    struct ifreq ifr;
    int fd, err;
    char *clonedev = (char*)"/dev/net/tun";

    if( (fd = open(clonedev , O_RDWR)) < 0 )
    {
    	perror("Opening /dev/net/tun");
    	return fd;
    }

    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_flags = flags;

    if (*dev)
    {
    	strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    if( (err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0 )
    {
    	perror("ioctl(TUNSETIFF)");
    	close(fd);
    	return err;
    }

    strcpy(dev, ifr.ifr_name);
    return fd;
}

