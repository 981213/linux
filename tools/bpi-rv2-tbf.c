// SPDX-License-Identifier: GPL-2.0
/* Configure a root TBF qdisc without requiring iproute2 in the initramfs. */

#include <errno.h>
#include <inttypes.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define QDISC_HANDLE TC_H_MAKE(1 << 16, 0)

struct request {
	struct nlmsghdr nlh;
	struct tcmsg tcm;
	char attrs[512];
};

static int addattr(struct nlmsghdr *nlh, size_t maxlen, unsigned short type,
		   const void *data, size_t len)
{
	size_t offset = NLMSG_ALIGN(nlh->nlmsg_len);
	size_t attr_len = RTA_LENGTH(len);
	struct rtattr *rta;

	if (offset + RTA_ALIGN(attr_len) > maxlen)
		return -EMSGSIZE;
	rta = (struct rtattr *)((char *)nlh + offset);
	rta->rta_type = type;
	rta->rta_len = attr_len;
	if (len)
		memcpy(RTA_DATA(rta), data, len);
	nlh->nlmsg_len = offset + RTA_ALIGN(attr_len);
	return 0;
}

static struct rtattr *nest_start(struct nlmsghdr *nlh, size_t maxlen,
				 unsigned short type)
{
	struct rtattr *nest;
	size_t offset = NLMSG_ALIGN(nlh->nlmsg_len);

	if (addattr(nlh, maxlen, type, NULL, 0))
		return NULL;
	nest = (struct rtattr *)((char *)nlh + offset);
	return nest;
}

static void nest_end(struct nlmsghdr *nlh, struct rtattr *nest)
{
	nest->rta_len = (char *)nlh + nlh->nlmsg_len - (char *)nest;
}

static int transact(struct nlmsghdr *nlh)
{
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	char response[4096];
	struct iovec iov;
	struct msghdr msg;
	struct nlmsghdr *answer;
	int fd, len, ret = 0;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	nlh->nlmsg_seq = 1;
	nlh->nlmsg_pid = getpid();
	iov.iov_base = nlh;
	iov.iov_len = nlh->nlmsg_len;
	msg = (struct msghdr) {
		.msg_name = &kernel,
		.msg_namelen = sizeof(kernel),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	if (sendmsg(fd, &msg, 0) < 0) {
		ret = -errno;
		goto out;
	}
	len = recv(fd, response, sizeof(response), 0);
	if (len < 0) {
		ret = -errno;
		goto out;
	}
	for (answer = (struct nlmsghdr *)response; NLMSG_OK(answer, len);
	     answer = NLMSG_NEXT(answer, len)) {
		if (answer->nlmsg_type == NLMSG_ERROR) {
			ret = ((struct nlmsgerr *)NLMSG_DATA(answer))->error;
			break;
		}
	}
out:
	close(fd);
	return ret;
}

static int tbf_replace(unsigned int ifindex, uint64_t rate_bps,
		       uint32_t burst)
{
	struct request req = {
		.nlh = {
			.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
			.nlmsg_type = RTM_NEWQDISC,
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE |
				       NLM_F_REPLACE,
		},
		.tcm = {
			.tcm_family = AF_UNSPEC,
			.tcm_handle = QDISC_HANDLE,
			.tcm_parent = TC_H_ROOT,
		},
	};
	struct tc_tbf_qopt qopt = {};
	struct rtattr *options;
	uint64_t rate_bytes = rate_bps / 8;
	uint64_t limit;
	static const char kind[] = "tbf";
	int ret;

	if (!rate_bytes || !burst)
		return -EINVAL;
	req.tcm.tcm_ifindex = ifindex;
	qopt.rate.rate = rate_bytes > UINT32_MAX ? UINT32_MAX : rate_bytes;
	qopt.rate.linklayer = TC_LINKLAYER_ETHERNET;
	/* A 100 ms software backlog keeps the qdisc from dropping while the
	 * hardware shaper is being exercised.
	 */
	limit = burst + rate_bytes / 10;
	qopt.limit = limit > UINT32_MAX ? UINT32_MAX : limit;

	ret = addattr(&req.nlh, sizeof(req), TCA_KIND, kind, sizeof(kind));
	if (ret)
		return ret;
	options = nest_start(&req.nlh, sizeof(req), TCA_OPTIONS);
	if (!options)
		return -EMSGSIZE;
	ret = addattr(&req.nlh, sizeof(req), TCA_TBF_PARMS, &qopt,
		      sizeof(qopt));
	if (!ret)
		ret = addattr(&req.nlh, sizeof(req), TCA_TBF_RATE64,
			      &rate_bytes, sizeof(rate_bytes));
	if (!ret)
		ret = addattr(&req.nlh, sizeof(req), TCA_TBF_BURST, &burst,
			      sizeof(burst));
	if (ret)
		return ret;
	nest_end(&req.nlh, options);

	return transact(&req.nlh);
}

static int tbf_destroy(unsigned int ifindex)
{
	struct request req = {
		.nlh = {
			.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
			.nlmsg_type = RTM_DELQDISC,
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK,
		},
		.tcm = {
			.tcm_family = AF_UNSPEC,
			.tcm_parent = TC_H_ROOT,
		},
	};

	req.tcm.tcm_ifindex = ifindex;
	return transact(&req.nlh);
}

int main(int argc, char **argv)
{
	unsigned int ifindex;
	uint64_t rate;
	uint32_t burst;
	char *end;
	int ret;

	if (argc != 3 && argc != 4) {
		fprintf(stderr,
			"usage: %s IFACE clear | IFACE RATE_BPS BURST_BYTES\n",
			argv[0]);
		return 2;
	}
	ifindex = if_nametoindex(argv[1]);
	if (!ifindex) {
		perror(argv[1]);
		return 2;
	}
	if (argc == 3 && !strcmp(argv[2], "clear")) {
		ret = tbf_destroy(ifindex);
	} else if (argc == 4) {
		errno = 0;
		rate = strtoull(argv[2], &end, 0);
		if (errno || *end) {
			fprintf(stderr, "invalid rate: %s\n", argv[2]);
			return 2;
		}
		burst = strtoul(argv[3], &end, 0);
		if (errno || *end) {
			fprintf(stderr, "invalid burst: %s\n", argv[3]);
			return 2;
		}
		ret = tbf_replace(ifindex, rate, burst);
	} else {
		fprintf(stderr, "invalid arguments\n");
		return 2;
	}
	if (ret) {
		errno = -ret;
		perror("TBF netlink request");
		return 1;
	}

	return 0;
}
