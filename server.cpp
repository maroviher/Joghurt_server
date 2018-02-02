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
#include <sys/ioctl.h>

#include <stdbool.h>
#include <inttypes.h>

#include "RCSwitch.h"
#include <pthread.h>
#include <poll.h>

#include <chrono>
#include <sstream>
#include <thread>
#include <mutex>
using namespace std;

const int g_iTemperatureLowerLimit = 35000;
const int g_iTemperatureUpperLimit = 39000;
const int g_iCentralTemperature = 36600;
const int g_iTemperatureDelta = 250;
char* strSystemCode = "10010";
int iUnitCode = 3;
const char w1_path[] = "/sys/bus/w1/devices/28-02162563e0ee/w1_slave";
int g_iTemper = 32;

enum enPowerStates
{
	ON,
	OFF
};

enum enPowerStates g_enCurrentPowerState = OFF;

int sockfd = -1;
char* g_line = NULL;
RCSwitch* pSwitch = NULL;
unsigned short port;
struct in_addr ip = { };

//statistics
chrono::system_clock::time_point time_start, time_last_poweron;
long poweron_duration_sec = 0, cntOn=0, cntOff=0;
std::mutex g_mux_stat;

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
				fprintf(stderr, "Waiting for a TCP connection on %s:%" SCNu16 "...",
						inet_ntoa(saddr.sin_addr), ntohs(saddr.sin_port));
				struct sockaddr_in cli_addr;
				socklen_t clilen = sizeof(cli_addr);
				while ((-1 == (sfd = accept(sockListen, (struct sockaddr *) &cli_addr, &clilen)))
						&& (EINTR == errno))
					;
				if (sfd >= 0) {
					fprintf(stderr, "Client connected from %s:%" SCNu16 "\n",
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
/*
int ReadTemperatur(int* pTemperatur) {

	FILE* fp = fopen("/tmp/w1_path", "r");
	if (fp == NULL) {
		fprintf(stderr, "error %d opening file %s\n", errno, "/tmp/w1_path");
		return -1;
	}

	size_t len = 0;
	ssize_t read;
	char* _buf = (char*)malloc(123);
	if (-1 == (read = ::getline((char**)&_buf, &len, fp))) {//read the first line
		fprintf(stderr, "getline error, line %d\n", __LINE__);
		return -1;
	}

	if (1 != sscanf(_buf, "%d", pTemperatur)) {
		fprintf(stderr, "sscanf error, line %d, buf=%s\n", __LINE__, _buf);
		return -1;
	}
	free(_buf);
	if(fp)
		fclose(fp);
	return 0;
}*/

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
	std::lock_guard<std::mutex> lock(g_mux_stat);
	if((ON != g_enCurrentPowerState)/* || (iTemper < g_iTemperatureLowerLimit)*/)
	{
		cntOn++;
		time_last_poweron = chrono::system_clock::now();

		for(int i=0; i<3; i++)
			pSwitch->switchOn(strSystemCode, iUnitCode);
		//system("for i in $(echo \"1\n2\n3\"); do /home/pi/raspberry-remote/send 10010 3 1; done");
		g_enCurrentPowerState = ON;
	}
}

void TurnPowerOff_inittial()
{
	pSwitch->switchOff(strSystemCode, iUnitCode);
}

void TurnPowerOff(int iTemper)
{
	std::lock_guard<std::mutex> lock(g_mux_stat);
	if((OFF != g_enCurrentPowerState)/* || (iTemper > g_iTemperatureUpperLimit)*/)
	{
		cntOff++;
		long last_poweron_duration_sec = chrono::duration_cast<chrono::seconds>(
				chrono::system_clock::now() - time_last_poweron).count();
		poweron_duration_sec += last_poweron_duration_sec;

		for(int i=0; i<3; i++)
			pSwitch->switchOff(strSystemCode, iUnitCode);
		//system("for i in $(echo \"1\n2\n3\"); do /home/pi/raspberry-remote/send 10010 3 0; done");
		g_enCurrentPowerState = OFF;
	}
}

void GetStatistics(string& strStat)
{
	std::lock_guard<std::mutex> lock(g_mux_stat);
	time_t tt_start = chrono::system_clock::to_time_t(time_start);
	time_t tt_now   = chrono::system_clock::to_time_t(chrono::system_clock::now());
	long l_power_on = poweron_duration_sec;
	if(ON == g_enCurrentPowerState)
	{
		l_power_on += chrono::duration_cast<chrono::seconds>(
						chrono::system_clock::now() - time_last_poweron).count();
	}
	stringstream ss;
	ss<<"started: " << ctime(&tt_start) <<
			"current temperature=" << g_iTemper <<
			"\npower cycles: on=" << cntOn << ", off=" << cntOff <<
	    "\npoweron sec=" << l_power_on <<
	    "\nnow:     "<< ctime(&tt_now)<<"\n";
	strStat = ss.str().c_str();
}

void* thread_start(void *arg)
{
	while(1)
	{
		sockfd = SetupListenSocket(&ip, port, 3, false);
		if (sockfd < 0) {
			fprintf(stderr, "Fatal error on SetupListenSocket\n");
			return NULL;
		}

		int sendbuff = 12;
		if(0 != setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sendbuff, sizeof(sendbuff)))
			fprintf(stderr, "setsockopt failed, errno=%d\n", errno);

		while(1)
		{
			string strStat;
			GetStatistics(strStat);
			if (strStat.length() != send(sockfd, strStat.c_str(), strStat.length(), MSG_NOSIGNAL)) {
				close(sockfd);
				fprintf(stderr, "send failed, errno=%d\n", errno);
				break;
			}

			//the following waits 1000ms or for the connection is closed
			pollfd pfd;
			pfd.fd = sockfd;
			pfd.events = POLLIN;
			pfd.revents = 0;
			if (poll(&pfd, 1, 1000) > 0) {
				char buffer[32];
				if (recv(pfd.fd, buffer, sizeof(buffer), MSG_PEEK | MSG_DONTWAIT) == 0) {
					close(sockfd);
					fprintf(stderr, "connection closed\n");
					break;
				}
				pfd.revents = 0;
			}

			unsigned long size;
			if(0 ==	ioctl(sockfd, TIOCOUTQ, &size))
			{
				if(size > (unsigned long)sendbuff){
					close(sockfd);
					break;
				}
			}
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
		show_usage_and_exit(argv);

	bool bListen = false, bVerbose = false;
	unsigned short recv_timeout = 3;

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

	g_line = (char*)malloc(200);
	if (!g_line)
		exit(EXIT_FAILURE);

	pthread_t thread;
	pthread_create(&thread, NULL, thread_start, NULL);

	TurnPowerOff_inittial();
	time_start = chrono::system_clock::now();
	while (1) {
		if(0 != ReadTemperatur(&g_iTemper))
			g_iTemper = 12345678;//error, send unrealistic temperature, and turn off power

		if(g_iTemper < g_iCentralTemperature - g_iTemperatureDelta )
			TurnPowerOn(g_iTemper);
		else if(g_iTemper > g_iTemperatureDelta + g_iCentralTemperature)
			TurnPowerOff(g_iTemper);
	}

	return 0;
}
