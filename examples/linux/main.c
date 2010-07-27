/*
 * Copyright (C) cozybit, Inc. 2007-2010. All Rights Reserved.
 *
 * Use and redistribution subject to licensing terms.
 *
 * mdns on Linux
 *
 * Here we implement the mdns os abstraction and a simple command line front
 * end for mdns.
 */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h>

#include "mdns.h"

#define MDNS_PIDFILE "/var/run/mdns.pid"

#define LOG printf
char *logfile = NULL;

static void linux_mdns_signal(int sig)
{
	switch(sig){
	case SIGHUP:
	case SIGTERM:
		mdns_halt();
		break;
	}
}

/* Our "thread" is really a process.  We only support one thread with this
 * function, so we'll fail if the thread is already running.
 */
void *mdns_thread_create(mdns_thread_entry entry, void *data)
{
	int ret, lock;
	/* str needs to hold a string representing the max pid in decimal */
	char str[sizeof(pid_t)*3 + 1];

	/* check the lock file */
	lock = open(MDNS_PIDFILE, O_RDWR|O_CREAT, 0640);
	if (lock < 0) {
		perror("failed to open lock file");
		return NULL;
	}
	ret = lockf(lock, F_TEST, 0);
	if (ret == -1 && (errno == EACCES || errno == EAGAIN)) {
		fprintf(stderr, "pid file is locked.  mdns already running?\n");
		return NULL;
	}
	if (ret != 0) {
		perror("failed to check lock file");
		return NULL;
	}

	/* looks good.	fork. */
	ret = fork();
	if (ret < 0) {
		perror("failed to fork mdns");
		return NULL;
	} else if (ret > 0) {
		/* parent process.	We don't really need a pointer, so we'll indicate
		 * success with 1
		 */
		return (void *)1;

	} else {
		/* child process */
		setsid();

		/* redirect stdin, out, and err */
		freopen("/dev/null", "r", stdin);
		if (logfile != NULL)
			freopen(logfile, "w", stdout);
		else
			freopen("/dev/null", "w", stdout);
		umask(027);
		ret = lockf(lock, F_TLOCK, 0);
		if (0 > ret) {
			LOG("failed to lock PID file: %s\n", strerror(errno));
			return NULL;
		}
		sprintf(str, "%d\n", getpid());
		ret = write(lock, str, strlen(str));
		if (0 > ret) {
			LOG("failed to write to PID file: %s\n", strerror(errno));
			return NULL;
		}
		signal(SIGCHLD, SIG_IGN);
		signal(SIGTSTP, SIG_IGN);
		signal(SIGTTOU, SIG_IGN);
		signal(SIGTTIN, SIG_IGN);

		/* On an RTOS with a single memory space, we would generally just
		 * communicate with the mdns task by invoking the API.	But on linux it
		 * runs off in its own process, so we have to send signals.	 SIGHUP and
		 * SIGTERM both invoke mdns_halt().
		 */
		signal(SIGHUP, linux_mdns_signal);
		signal(SIGTERM, linux_mdns_signal);

		/* initalize the random number generator */
		srand(time(0));

		/* launch the entry function */
		entry(data);
		return NULL;
	}
}

void mdns_thread_delete(void *t)
{
	/* nothing to free here. */
	return;
}

void mdns_thread_yield(void)
{
	sleep(0);
}

void mdns_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
    fflush(stdout);
}

uint32_t mdns_time_ms(void)
{
    struct timeval t;

    if (gettimeofday(&t, NULL) != 0)
        printf("Warning: Failed to get time.\n");
    return (uint32_t)(t.tv_sec * 1000 + t.tv_usec/1000);
}

int mdns_rand_range(int n)
{
	int r = rand();
	return r / (RAND_MAX / n + 1);
}

int mdns_socket_mcast(uint32_t mcast_addr, uint16_t port)
{
	int sock;
	int yes = 1;
	unsigned char ttl = 255;
	struct sockaddr_in in_addr;
	struct ip_mreq mc;

	memset(&in_addr, 0, sizeof(in_addr));
	in_addr.sin_family = AF_INET;
	in_addr.sin_port = port;
	in_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		LOG("error: could not open multicast socket\n");
		return sock;
	}

#ifdef SO_REUSEPORT
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (char*)&yes,
					sizeof(yes)) < 0) {
		LOG("error: failed to set SO_REUSEPORT option\n");
		return -1;
	}
#endif
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

	if (bind(sock, (struct sockaddr*)&in_addr, sizeof(in_addr))) {
		close(sock);
		return -1;
	}

	/* join multicast group */
	mc.imr_multiaddr.s_addr = mcast_addr;
	mc.imr_interface.s_addr = htonl(INADDR_ANY);
	if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mc,
					sizeof(mc)) < 0) {
		LOG("error: failed to join multicast group\n");
		return -1;
	}
	/* set other IP-level options */
	if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (unsigned char *)&ttl,
					sizeof(ttl)) < 0) {
		LOG("error: failed to set multicast TTL\n");
		return -1;
	}

	if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
		LOG("error: failed to put socket in non-blocking mode\n");
		return -1;
	}
	return sock;
}

static int parse_service(struct mdns_service *service, char *str)
{
	char *token, *p = str, *e, *k;
	int port, num_keys, i;

	token = strsep(&p, ":");
	if (token == NULL || *token == 0) {
		printf("error: no service name specified: %s\n", str);
		return -1;
	}
	service->servname = token;

	token = strsep(&p, ":");
	if (token == NULL || *token == 0) {
		printf("error: no service type specified: %s\n", str);
		return -2;
	}
	service->servtype = token;

	token = strsep(&p, ":");
	if (token == NULL || *token == 0) {
		printf("error: no service port specified: %s\n", str);
		return -3;
	}
	port = strtol(token, &e, 10);
	if ((errno == ERANGE && (port == LONG_MAX || port == LONG_MIN)) ||
		port > UINT16_MAX || port < 0)
	{
		printf("error: invalid port number.\n");
		return -4;
	}
	service->port = port;

	token = strsep(&p, ":");
	if (token == NULL || *token == 0) {
		printf("error: no service protocol specified: %s\n", str);
		return -5;
	}
	if (strcmp(token, "tcp") == 0) {
		service->proto = MDNS_PROTO_TCP;
	} else if (strcmp(token, "tcp") == 0) {
		service->proto = MDNS_PROTO_UDP;
	} else {
		printf("error: unexpected protocol %s\n", token);
		return -6;
	}

	token = strsep(&p, ":");
	if (token == NULL || *token == 0)
		/* no key/value pairs.  We're done */
		return 0;

	num_keys = 1;
	k = strchr(p, ':');
	while (k != NULL) {
		num_keys++;
		k = strchr(k + 1, ':');
	}
	service->keyvals = malloc(num_keys*sizeof(char *));
	if (service->keyvals == NULL) {
		printf("error: failed to allocate array for key/value pairs\n");
		return -7;
	}
	memset(service->keyvals, 0, num_keys*sizeof(char *) + 1);
	service->keyvals[0] = token;
	for (i = 1; i < num_keys; i++) {
		service->keyvals[i] = strsep(&p, ":");
		if (service->keyvals[i] == NULL || *service->keyvals[i] == 0) {
			printf("error: failed to parse key/value pair\n");
			return -8;
		}
	}
	return 0;
}

#define HELP_TEXT \
"Usage: mdns [options] <command>\n\n" \
"command can be one of the following:\n" \
"launch        start mdns daemon\n" \
"halt          stop mdns daemon\n" \
"test          run internal tests." \
"\n" \
"Options\n" \
"-h             Print this help text\n" \
"-b <ipaddr>    ipaddress to bind to\n" \
"-d <domain>    domain to resolve (default is 'local')\n" \
"-n <hostname>  hostname to resolve (default is 'node')\n" \
"-l <logfile>   logfile for daemon (default is /dev/null)\n" \
"-s <name:type:port:proto[:key1=val1:key2=val2]>\n" \
"               advertise a service with the specified name\n" \
"               (e.g., mywebpage, or \"My Website\"), type (e.g., http),\n" \
"               port (e.g., 80), proto (e.g., tcp), and key/value pairs\n" \
"               (e.g., path=/index.html).  Multiple -s options may be\n" \
"               supplied.  Colons may not appear in any of these fields.\n"

static struct mdns_service *services[MDNS_MAX_SERVICES + 1];

int main(int argc, char **argv)
{
	int ret = 0;
	char opt;
	in_addr_t ipaddr = 0;
	char *cmd;
	char *domain = NULL;
	char *hostname = "node";
	int num_services = 0, i;

	memset(services, 0, sizeof(services));

	while ((opt = getopt(argc, argv, "hb:d:n:l:s:")) != -1) {
		switch (opt) {
		case 'h':
			printf(HELP_TEXT);
			return 0;
		case 'b':
			ipaddr = inet_addr(optarg);
			break;
		case 'd':
			domain = optarg;
			break;
		case 'n':
			hostname = optarg;
			break;
		case 'l':
			logfile = optarg;
			break;
		case 's':
			if (num_services == MDNS_MAX_SERVICES) {
				printf("Warning: Only first %d services will be advertised.\n",
					   MDNS_MAX_SERVICES);
				break;
			}
			services[num_services] = malloc(sizeof(struct mdns_service));
			if (!services[num_services]) {
				printf("Failed to allocate service structure.\n");
				ret = -1;
				goto done;
			}
			memset(services[num_services], 0, sizeof(struct mdns_service));
			ret = parse_service(services[num_services], optarg);
			if (ret != 0)
				goto done;
			num_services++;
			break;
		default:
			printf("Unexpected option %c\n", opt);
			return -1;
		}
	}

	if (optind >= argc) {
		printf("No command specified.\n");
		return -1;
	}

	cmd = argv[optind];
	if (strcmp(cmd, "launch") == 0) {
		ret = mdns_launch(ipaddr, domain, hostname, services);

	} else if (strcmp(cmd, "halt") == 0) {
		mdns_halt();
		return 0;

	} else if (strcmp(cmd, "test") == 0) {
		mdns_tests();
		return 0;

	} else {
		printf("No such command: %s\n", cmd);
		return -1;
	}

done:
	for (i = 0; i < MDNS_MAX_SERVICES; i++) {
		if (services[i] == NULL)
			break;
		if (services[i]->keyvals)
			free(services[i]->keyvals);
		free(services[i]);
	}
	return ret;
}
