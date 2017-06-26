#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <stdbool.h>
#include <inttypes.h>

#include "RCSwitch.h"

const int g_iTemperatureLowerLimit = 35000;
const int g_iTemperatureUpperLimit = 39000;
const int g_iCentralTemperature = 37000;
const int g_iTemperatureDelta = 250;
char* strSystemCode = "10010";
int iUnitCode = 3;
const char w1_path[] = "/sys/bus/w1/devices/28-02162563e0ee/w1_slave";

enum enPowerStates
{
	ON,
	OFF
};

enum enPowerStates g_enCurrentPowerState = OFF;

int sockfd = -1;
char* g_line = NULL;
RCSwitch* pSwitch = NULL;

int SetupListenSocket(struct in_addr* ip, unsigned short port,
		unsigned short rcv_timeout, bool bVerbose) {
	int sfd = -1;
	struct sockaddr_in saddr = { };
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(port);
	saddr.sin_addr = *ip;

	int sockListen = socket(AF_INET, SOCK_STREAM, 0);
	if (sockListen >= 0) {
		int iTmp = 1;
		setsockopt(sockListen, SOL_SOCKET, SO_REUSEADDR, &iTmp, sizeof(int)); //no error handling, just go on
		if (bind(sockListen, (struct sockaddr *) &saddr, sizeof(saddr)) >= 0) {
			while ((-1 == (iTmp = listen(sockListen, 0))) && (EINTR == errno))
				;
			if (-1 != iTmp) {
				fprintf(stderr,
						"Waiting for a TCP connection on %s:%" SCNu16 "...",
						inet_ntoa(saddr.sin_addr), ntohs(saddr.sin_port));
				struct sockaddr_in cli_addr;
				socklen_t clilen = sizeof(cli_addr);
				while ((-1
						== (sfd = accept(sockListen,
								(struct sockaddr *) &cli_addr, &clilen)))
						&& (EINTR == errno))
					;
				if (sfd >= 0) {
					struct timeval timeout;
					timeout.tv_sec = rcv_timeout;
					timeout.tv_usec = 0;
					if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO,
							(char *) &timeout, sizeof(timeout)) < 0)
						fprintf(stderr, "setsockopt failed\n");
					fprintf(stderr, "Client connected from %s:%"SCNu16"\n",
							inet_ntoa(cli_addr.sin_addr),
							ntohs(cli_addr.sin_port));
				} else
					fprintf(stderr, "Error on accept: %s\n", strerror(errno));
			} else //if (-1 != iTmp)
			{
				fprintf(stderr, "Error trying to listen on a socket: %s\n",
						strerror(errno));
			}
		} else //if (bind(sockListen, (struct sockaddr *) &saddr, sizeof(saddr)) >= 0)
		{
			fprintf(stderr, "Error on binding socket: %s\n", strerror(errno));
		}
	} else //if (sockListen >= 0)
	{
		fprintf(stderr, "Error creating socket: %s\n", strerror(errno));
	}

	if (sockListen >= 0) //regardless success or error
		close(sockListen); //do not listen on a given port anymore

	return sfd;
}

void error(char *msg) {
	perror(msg);
	exit(1);
}

void show_usage_and_exit(char** argv) {
	char* bname = argv[0];
	fprintf(stderr, "Usage: wait for incoming connection: %s -l -p 1234\n", bname);
	exit(EXIT_FAILURE);
}

int ReadTemperatur(int* pTemperatur) {
	//the file looks like this
	//4b 01 4b 46 7f ff 0c 10 53 : crc=53 YES
	//4b 01 4b 46 7f ff 0c 10 53 t=20687

	int iRet = -1;
	FILE* fp = fopen(w1_path, "r");
	if (fp == NULL) {
		fprintf(stderr, "error %d opening file %s\n", errno, w1_path);
		return -1;
	}
	size_t len = 0;
	ssize_t read;
	if (-1 == (read = getline(&g_line, &len, fp))) {//read the first line
		fprintf(stderr, "getline error, line %d\n", __LINE__);
		goto _error;
	}
	if (read < 5) {
		fprintf(stderr, "read < 5 error, line %d\n", __LINE__);
		goto _error;
	}
	if (0 == memcmp(g_line + read - 3 - 1, "YES", 3)) {	//crc ok
		if (-1 == (read = getline(&g_line, &len, fp)))	//read the second line
				{
			fprintf(stderr, "getline error, line %d\n", __LINE__);
			goto _error;
		}

		char* pTemperPos = strstr(g_line, "t=");
		if (!pTemperPos) {
			fprintf(stderr, "strstr error, line %d\n", __LINE__);
			goto _error;
		}
		if (1 != sscanf(pTemperPos + 2, "%d", pTemperatur)) {
			fprintf(stderr, "sscanf error, line %d\n", __LINE__);
			goto _error;
		}
		//printf("Temperatur=%d\n", *pTemperatur);
		iRet = 0;
	}
_error:
	if(fp)
		fclose(fp);
	return iRet;
}

void TurnPowerOn(int iTemper)
{
	if((ON != g_enCurrentPowerState) || (iTemper < g_iTemperatureLowerLimit))
	{
		for(int i=0; i<3; i++)
			pSwitch->switchOn(strSystemCode, iUnitCode);
		//system("for i in $(echo \"1\n2\n3\"); do /home/pi/raspberry-remote/send 10010 3 1; done");
		g_enCurrentPowerState = ON;
	}
}

void TurnPowerOff(int iTemper)
{
	if((OFF != g_enCurrentPowerState) || (iTemper > g_iTemperatureUpperLimit))
	{
		for(int i=0; i<3; i++)
			pSwitch->switchOff(strSystemCode, iUnitCode);
		//system("for i in $(echo \"1\n2\n3\"); do /home/pi/raspberry-remote/send 10010 3 0; done");
		g_enCurrentPowerState = OFF;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
		show_usage_and_exit(argv);

	bool bListen = false, bVerbose = false;
	unsigned short port, recv_timeout = 3;
	struct in_addr ip = { };
	int opt;
	while ((opt = getopt(argc, argv, "t:vlh:p:")) != -1) {
		switch (opt) {
		case 'l':
			bListen = true;
			break;
		case 'v':
			bVerbose = true;
			break;
		case 'h':
			if (0 == inet_aton(optarg, &ip)) {
				fprintf(stderr,
						"inet_aton failed. %s is not a valid IPv4 address\n",
						optarg);
				exit(134);
			}
			break;
		case 'p':
			if (1 != sscanf(optarg, "%hu", &port)) {
				fprintf(stderr, "error port\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 't':
			if (1 != sscanf(optarg, "%hu", &recv_timeout)) {
				fprintf(stderr, "error recv_timeout\n");
				exit(EXIT_FAILURE);
			}
			break;
		default: /* '?' */
			show_usage_and_exit(argv);
		}
	}

	if (wiringPiSetup() == -1)
        {
                fprintf(stderr, "wiringPiSetup failed\n");
                return 1;
        }
	piHiPri(20);
        pSwitch = new RCSwitch();
        pSwitch->setPulseLength(300);
        pSwitch->enableTransmit(0);

/*	if (wiringPiSetup() == -1)
	{
		fprintf(stderr, "wiringPiSetup failed\n");
		return 1;
	}
	piHiPri(20);
	RCSwitch mySwitch = RCSwitch();
	mySwitch.setPulseLength(300);
	mySwitch.enableTransmit(0);
	char* strSystemCode = "10010";
	int iUnitCode = 3;
	mySwitch.switchOn(strSystemCode, iUnitCode);
	mySwitch.switchOff(strSystemCode, iUnitCode);
*/
	g_line = (char*)malloc(200);
	if (!g_line)
		exit(EXIT_FAILURE);

	while (1) {
		if (bListen)
			sockfd = SetupListenSocket(&ip, port, 3, bVerbose);
		else
			exit(1);

		if (sockfd < 0) {
			fprintf(stderr, "connect failed\n");
			continue;
		}

		int iTemper;
		char bGoRead = 1;
		while (bGoRead) {

			if(0 != ReadTemperatur(&iTemper))
				iTemper = 12345678;//error, send unrealistic temperature, and turn off power

			if(iTemper < g_iCentralTemperature - g_iTemperatureDelta )
				TurnPowerOn(iTemper);
			else if(iTemper > g_iTemperatureDelta + g_iCentralTemperature)
				TurnPowerOff(iTemper);

			if (4 != send(sockfd, &iTemper, 4, MSG_NOSIGNAL)) {
				close(sockfd);
				fprintf(stderr, "send failed, errno=%d\n", errno);
				bGoRead = 0;
			}
			//sleep(1);
		}
	}

	return 0;
}
