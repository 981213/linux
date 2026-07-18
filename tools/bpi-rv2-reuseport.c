// SPDX-License-Identifier: GPL-2.0
/* Measure BPI-RV2 RX-hash distribution over SO_REUSEPORT sockets. */

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCKETS 4
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static int attach_rxhash_high16_filter(int fd)
{
	struct sock_filter instructions[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 SKF_AD_OFF + SKF_AD_RXHASH),
		BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
		BPF_STMT(BPF_ALU | BPF_AND | BPF_K, SOCKETS - 1),
		BPF_STMT(BPF_RET | BPF_A, 0),
	};
	struct sock_fprog program = {
		.len = ARRAY_SIZE(instructions),
		.filter = instructions,
	};

	return setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF,
			  &program, sizeof(program));
}

static int open_socket(unsigned int port, unsigned int timeout)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	struct timeval tv = { .tv_sec = timeout };
	int one = 1;
	int fd;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
	    bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		close(fd);
		return -1;
	}
	return fd;
}

static void receive_child(int index, int sockets[SOCKETS], int pipes[SOCKETS][2])
{
	uint64_t count = 0;
	char buffer[2048];
	int i;

	for (i = 0; i < SOCKETS; i++) {
		close(pipes[i][0]);
		if (i != index) {
			close(pipes[i][1]);
			close(sockets[i]);
		}
	}
	while (recv(sockets[index], buffer, sizeof(buffer), 0) >= 0)
		count++;
	if (errno != EAGAIN && errno != EWOULDBLOCK)
		perror("recv");
	if (write(pipes[index][1], &count, sizeof(count)) != sizeof(count))
		perror("write");
	_exit(0);
}

int main(int argc, char **argv)
{
	unsigned int port, timeout;
	uint64_t counts[SOCKETS] = {};
	int pipes[SOCKETS][2];
	int sockets[SOCKETS];
	char *end;
	int i;

	if (argc != 3) {
		fprintf(stderr, "usage: %s PORT TIMEOUT_SECONDS\n", argv[0]);
		return 2;
	}
	port = strtoul(argv[1], &end, 0);
	if (*end || !port || port > UINT16_MAX)
		return 2;
	timeout = strtoul(argv[2], &end, 0);
	if (*end || !timeout)
		return 2;

	for (i = 0; i < SOCKETS; i++) {
		if (pipe(pipes[i])) {
			perror("pipe");
			return 1;
		}
		sockets[i] = open_socket(port, timeout);
		if (sockets[i] < 0) {
			perror("socket");
			return 1;
		}
	}
	if (attach_rxhash_high16_filter(sockets[0])) {
		perror("SO_ATTACH_REUSEPORT_CBPF");
		return 1;
	}
	for (i = 0; i < SOCKETS; i++) {
		pid_t child = fork();

		if (child < 0) {
			perror("fork");
			return 1;
		}
		if (!child)
			receive_child(i, sockets, pipes);
		close(pipes[i][1]);
		close(sockets[i]);
	}
	puts("READY");
	fflush(stdout);
	for (i = 0; i < SOCKETS; i++) {
		if (read(pipes[i][0], &counts[i], sizeof(counts[i])) !=
		    sizeof(counts[i]))
			perror("read");
		close(pipes[i][0]);
	}
	while (wait(NULL) > 0)
		;
	printf("counts:");
	for (i = 0; i < SOCKETS; i++)
		printf(" %llu", (unsigned long long)counts[i]);
	putchar('\n');
	return 0;
}
