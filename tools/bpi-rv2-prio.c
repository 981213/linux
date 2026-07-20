// SPDX-License-Identifier: GPL-2.0
/* Configure and inspect a root PRIO qdisc without iproute2 in the initramfs. */

#include <errno.h>
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
	struct iovec iov = { .iov_base = nlh, .iov_len = nlh->nlmsg_len };
	struct msghdr msg = {
		.msg_name = &kernel,
		.msg_namelen = sizeof(kernel),
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	struct nlmsghdr *answer;
	int fd, len, ret = 0;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	nlh->nlmsg_seq = 1;
	nlh->nlmsg_pid = getpid();
	if (sendmsg(fd, &msg, 0) < 0) {
		ret = -errno;
		goto out;
	}
	len = recv(fd, response, sizeof(response), 0);
	if (len < 0) {
		ret = -errno;
		goto out;
	}
	for (answer = (struct nlmsghdr *)response;
	     NLMSG_OK(answer, (unsigned int)len);
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

static int prio_replace(unsigned int ifindex, int incompatible)
{
	static const char kind[] = "prio";
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
			.tcm_ifindex = ifindex,
		},
	};
	struct tc_prio_qopt qopt = { .bands = 8 };
	int i, ret;

	for (i = 0; i <= TC_PRIO_MAX; i++)
		qopt.priomap[i] = 7 - (i & 7);
	if (incompatible)
		qopt.priomap[0] = 0;

	ret = addattr(&req.nlh, sizeof(req), TCA_KIND, kind, sizeof(kind));
	if (!ret)
		ret = addattr(&req.nlh, sizeof(req), TCA_OPTIONS, &qopt,
			      sizeof(qopt));
	if (ret)
		return ret;

	return transact(&req.nlh);
}

static int ets_replace(unsigned int ifindex, int weighted)
{
	static const char kind[] = "ets";
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
			.tcm_ifindex = ifindex,
		},
	};
	struct rtattr *options, *priomap, *quanta;
	uint8_t nbands = 8, nstrict = weighted ? 0 : 8;
	uint32_t quantum = 1500;
	uint8_t band;
	int i, ret;

	ret = addattr(&req.nlh, sizeof(req), TCA_KIND, kind, sizeof(kind));
	if (ret)
		return ret;
	options = nest_start(&req.nlh, sizeof(req), TCA_OPTIONS | NLA_F_NESTED);
	if (!options)
		return -EMSGSIZE;
	ret = addattr(&req.nlh, sizeof(req), TCA_ETS_NBANDS, &nbands,
		      sizeof(nbands));
	if (!ret)
		ret = addattr(&req.nlh, sizeof(req), TCA_ETS_NSTRICT, &nstrict,
			      sizeof(nstrict));
	if (ret)
		return ret;
	if (weighted) {
		quanta = nest_start(&req.nlh, sizeof(req),
				    TCA_ETS_QUANTA | NLA_F_NESTED);
		if (!quanta)
			return -EMSGSIZE;
		for (i = 0; i < 8; i++) {
			ret = addattr(&req.nlh, sizeof(req), TCA_ETS_QUANTA_BAND,
				      &quantum, sizeof(quantum));
			if (ret)
				return ret;
		}
		nest_end(&req.nlh, quanta);
	}
	priomap = nest_start(&req.nlh, sizeof(req),
			     TCA_ETS_PRIOMAP | NLA_F_NESTED);
	if (!priomap)
		return -EMSGSIZE;
	for (i = 0; i <= TC_PRIO_MAX; i++) {
		band = 7 - (i & 7);
		ret = addattr(&req.nlh, sizeof(req), TCA_ETS_PRIOMAP_BAND,
			      &band, sizeof(band));
		if (ret)
			return ret;
	}
	nest_end(&req.nlh, priomap);
	nest_end(&req.nlh, options);

	return transact(&req.nlh);
}

static int prio_destroy(unsigned int ifindex)
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
			.tcm_ifindex = ifindex,
		},
	};

	return transact(&req.nlh);
}

static int prio_show(unsigned int ifindex)
{
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	struct request req = {
		.nlh = {
			.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
			.nlmsg_type = RTM_GETQDISC,
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
			.nlmsg_seq = 1,
		},
		.tcm = { .tcm_family = AF_UNSPEC },
	};
	char response[8192];
	int fd, len, found = 0;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;
	if (sendto(fd, &req, req.nlh.nlmsg_len, 0,
		   (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
		len = -errno;
		goto out;
	}
	for (;;) {
		struct nlmsghdr *nlh;
		int remaining;

		len = recv(fd, response, sizeof(response), 0);
		if (len < 0) {
			len = -errno;
			goto out;
		}
		remaining = len;
		for (nlh = (struct nlmsghdr *)response;
		     NLMSG_OK(nlh, (unsigned int)remaining);
		     nlh = NLMSG_NEXT(nlh, remaining)) {
			struct tcmsg *tcm;
			struct rtattr *rta;
			int attrs, offloaded = 0;
			const char *kind = NULL;

			if (nlh->nlmsg_type == NLMSG_DONE) {
				len = found ? 0 : -ENOENT;
				goto out;
			}
			if (nlh->nlmsg_type != RTM_NEWQDISC)
				continue;
			tcm = NLMSG_DATA(nlh);
			if (tcm->tcm_ifindex != (int)ifindex)
				continue;
			attrs = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*tcm));
			rta = (struct rtattr *)((char *)tcm + NLMSG_ALIGN(sizeof(*tcm)));
			for (; RTA_OK(rta, attrs); rta = RTA_NEXT(rta, attrs)) {
				if (rta->rta_type == TCA_KIND)
					kind = RTA_DATA(rta);
				else if (rta->rta_type == TCA_HW_OFFLOAD)
					offloaded = *(uint8_t *)RTA_DATA(rta);
			}
			if (kind && (!strcmp(kind, "prio") ||
				     !strcmp(kind, "ets"))) {
				printf("%s handle %x: hw_offload %d\n", kind,
				       tcm->tcm_handle, offloaded);
				found = 1;
			}
		}
	}
out:
	close(fd);
	return len;
}

int main(int argc, char **argv)
{
	unsigned int ifindex;
	int ret;

	if (argc != 3) {
		fprintf(stderr,
			"usage: %s IFACE replace|replace-bad|ets-replace|ets-weighted|clear|show\n",
			argv[0]);
		return 2;
	}
	ifindex = if_nametoindex(argv[1]);
	if (!ifindex) {
		perror(argv[1]);
		return 2;
	}
	if (!strcmp(argv[2], "replace"))
		ret = prio_replace(ifindex, 0);
	else if (!strcmp(argv[2], "replace-bad"))
		ret = prio_replace(ifindex, 1);
	else if (!strcmp(argv[2], "ets-replace"))
		ret = ets_replace(ifindex, 0);
	else if (!strcmp(argv[2], "ets-weighted"))
		ret = ets_replace(ifindex, 1);
	else if (!strcmp(argv[2], "clear"))
		ret = prio_destroy(ifindex);
	else if (!strcmp(argv[2], "show"))
		ret = prio_show(ifindex);
	else
		return 2;
	if (ret) {
		errno = -ret;
		perror("PRIO netlink request");
		return 1;
	}

	return 0;
}
