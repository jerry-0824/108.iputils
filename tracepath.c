/*
 * tracepath.c
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 */

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <linux/errqueue.h>
#include <linux/types.h>
#include <netdb.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef USE_IDN
# include <locale.h>
# ifndef AI_IDN
#  define AI_IDN 0x0040
# endif
# ifndef NI_IDN
#  define NI_IDN 32
# endif
# define getnameinfo_flags	NI_IDN
#else
# define getnameinfo_flags	0
#endif

#ifndef SOL_IPV6
# define SOL_IPV6 IPPROTO_IPV6
#endif

#ifndef IP_PMTUDISC_DO
# define IP_PMTUDISC_DO		3
#endif
#ifndef IPV6_PMTUDISC_DO
# define IPV6_PMTUDISC_DO	3
#endif

#define MAX_HOPS_LIMIT		255
#define MAX_HOPS_DEFAULT	30

#define HOST_COLUMN_SIZE	52

struct hhistory {
	int hops;
	struct timeval sendtime;
};

struct probehdr {
	uint32_t ttl;
	struct timeval tv;
};

struct run_state {
	struct hhistory his[64];
	int hisptr;
	struct sockaddr_storage target;
	socklen_t targetlen;
	uint16_t base_port;
	int max_hops;
	int overhead;
	int mtu;
	void *pktbuf;
	int hops_to;
	int hops_from;
	unsigned int
		no_resolve:1,
		show_both:1,
		mapped:1;
};

/*
 * All includes, definitions, struct declarations, and global variables are
 * above.  After this comment all you can find is functions.
 */

static void data_wait(int fd)
{
	fd_set fds;
	struct timeval tv = {
		.tv_sec = 1,
		.tv_usec = 0
	};

	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	select(fd + 1, &fds, NULL, NULL, &tv);
}

static void print_host(struct run_state *ctl, const char *a, const char *b)
{
	int plen;

	plen = printf("%s", a);
	if (ctl->show_both)
		plen += printf(" (%s)", b);
	if (plen >= HOST_COLUMN_SIZE)
		plen = HOST_COLUMN_SIZE - 1;
	printf("%*s", HOST_COLUMN_SIZE - plen, "");
}

static int recverr(struct run_state *ctl, int fd, struct addrinfo *ai, int ttl)
{
	ssize_t recv_size;
	struct probehdr rcvbuf;
	char cbuf[512];
	struct cmsghdr *cmsg;
	struct sock_extended_err *e;
	struct sockaddr_storage addr;
	struct timeval tv;
	struct timeval *rettv;
	int slot = 0;
	int rethops;
	int sndhops;
	int progress = -1;
	int broken_router;
	char hnamebuf[NI_MAXHOST] = "";
	struct iovec iov = {
		.iov_base = &rcvbuf,
		.iov_len = sizeof(rcvbuf)
	};
	struct msghdr msg;
	const struct msghdr reset = {
		.msg_name = (uint8_t *)&addr,
		.msg_namelen = sizeof(addr),
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = cbuf,
		.msg_controllen = sizeof(cbuf),
		0
	};

 restart:
	memset(&rcvbuf, -1, sizeof(rcvbuf));
	msg = reset;

	gettimeofday(&tv, NULL);
	recv_size = recvmsg(fd, &msg, MSG_ERRQUEUE);
	if (recv_size < 0) {
		if (errno == EAGAIN)
			return progress;
		goto restart;
	}

	progress = ctl->mtu;

	rethops = -1;
	sndhops = -1;
	e = NULL;
	rettv = NULL;
	broken_router = 0;

	slot = -ctl->base_port;
	switch (ai->ai_family) {
	case AF_INET6:
		slot += ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
		break;
	case AF_INET:
		slot += ntohs(((struct sockaddr_in *)&addr)->sin_port);
		break;
	}

	if (slot >= 0 && slot < 63 && ctl->his[slot].hops) {
		sndhops = ctl->his[slot].hops;
		rettv = &ctl->his[slot].sendtime;
		ctl->his[slot].hops = 0;
	}
	if (recv_size == sizeof(rcvbuf)) {
		if (rcvbuf.ttl == 0 || rcvbuf.tv.tv_sec == 0)
			broken_router = 1;
		else {
			sndhops = rcvbuf.ttl;
			rettv = &rcvbuf.tv;
		}
	}

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		switch (cmsg->cmsg_level) {
		case SOL_IPV6:
			switch (cmsg->cmsg_type) {
			case IPV6_RECVERR:
				e = (struct sock_extended_err *)CMSG_DATA(cmsg);
				break;
			case IPV6_HOPLIMIT:
#ifdef IPV6_2292HOPLIMIT
			case IPV6_2292HOPLIMIT:
#endif
				memcpy(&rethops, CMSG_DATA(cmsg), sizeof(rethops));
				break;
			default:
				printf("cmsg6:%d\n ", cmsg->cmsg_type);
			}
			break;
		case SOL_IP:
			switch (cmsg->cmsg_type) {
			case IP_RECVERR:
				e = (struct sock_extended_err *)CMSG_DATA(cmsg);
				break;
			case IP_TTL:
				rethops = *(uint8_t *)CMSG_DATA(cmsg);
				break;
			default:
				printf("cmsg4:%d\n ", cmsg->cmsg_type);
			}
		}
	}
	if (e == NULL) {
		printf("no info\n");
		return 0;
	}
	if (e->ee_origin == SO_EE_ORIGIN_LOCAL)
		printf("%2d?: %-32s ", ttl, "[LOCALHOST]");
	else if (e->ee_origin == SO_EE_ORIGIN_ICMP6 ||
		 e->ee_origin == SO_EE_ORIGIN_ICMP) {
		char abuf[NI_MAXHOST];
		struct sockaddr *sa = (struct sockaddr *)(e + 1);
		socklen_t salen;

		if (sndhops > 0)
			printf("%2d:  ", sndhops);
		else
			printf("%2d?: ", ttl);

		switch (sa->sa_family) {
		case AF_INET6:
			salen = sizeof(struct sockaddr_in6);
			break;
		case AF_INET:
			salen = sizeof(struct sockaddr_in);
			break;
		default:
			salen = 0;
		}

		if (ctl->no_resolve || ctl->show_both) {
			if (getnameinfo(sa, salen, abuf, sizeof(abuf), NULL, 0,
					NI_NUMERICHOST))
				strcpy(abuf, "???");
		} else
			abuf[0] = 0;

		if (!ctl->no_resolve || ctl->show_both) {
			fflush(stdout);
			if (getnameinfo(sa, salen, hnamebuf, sizeof hnamebuf, NULL, 0,
					getnameinfo_flags))
				strcpy(hnamebuf, "???");
		} else
			hnamebuf[0] = 0;

		if (ctl->no_resolve)
			print_host(ctl, abuf, hnamebuf);
		else
			print_host(ctl, hnamebuf, abuf);
	}

	if (rettv) {
		int diff = (tv.tv_sec - rettv->tv_sec) * 1000000 +
			   (tv.tv_usec - rettv->tv_usec);
		printf("%3d.%03dms ", diff / 1000, diff % 1000);
		if (broken_router)
			printf("(This broken router returned corrupted payload) ");
	}

	if (rethops <= 64)
		rethops = 65 - rethops;
	else if (rethops <= 128)
		rethops = 129 - rethops;
	else
		rethops = 256 - rethops;

	switch (e->ee_errno) {
	case ETIMEDOUT:
		printf("\n");
		break;
	case EMSGSIZE:
		printf("pmtu %d\n", e->ee_info);
		ctl->mtu = e->ee_info;
		progress = ctl->mtu;
		break;
	case ECONNREFUSED:
		printf("reached\n");
		ctl->hops_to = sndhops < 0 ? ttl : sndhops;
		ctl->hops_from = rethops;
		return 0;
	case EPROTO:
		printf("!P\n");
		return 0;
	case EHOSTUNREACH:
		if ((e->ee_origin == SO_EE_ORIGIN_ICMP &&
		     e->ee_type == 11 &&
		     e->ee_code == 0) ||
		    (e->ee_origin == SO_EE_ORIGIN_ICMP6 &&
		     e->ee_type == 3 &&
		     e->ee_code == 0)) {
			if (rethops >= 0) {
				if (sndhops >= 0 && rethops != sndhops)
					printf("asymm %2d ", rethops);
				else if (sndhops < 0 && rethops != ttl)
					printf("asymm %2d ", rethops);
			}
			printf("\n");
			break;
		}
		printf("!H\n");
		return 0;
	case ENETUNREACH:
		printf("!N\n");
		return 0;
	case EACCES:
		printf("!A\n");
		return 0;
	default:
		printf("\n");
		errno = e->ee_errno;
		perror("NET ERROR");
		return 0;
	}
	goto restart;
}

static int probe_ttl(struct run_state *ctl, int fd, struct addrinfo *ai, int ttl)
{
	int i;
	struct probehdr *hdr = ctl->pktbuf;

	memset(ctl->pktbuf, 0, ctl->mtu);
 restart:
	for (i = 0; i < 10; i++) {
		int res;

		hdr->ttl = ttl;
		switch (ai->ai_family) {
		case AF_INET6:
			((struct sockaddr_in6 *)&ctl->target)->sin6_port =
			    htons(ctl->base_port + ctl->hisptr);
			break;
		case AF_INET:
			((struct sockaddr_in *)&ctl->target)->sin_port =
			    htons(ctl->base_port + ctl->hisptr);
			break;
		}
		gettimeofday(&hdr->tv, NULL);
		ctl->his[ctl->hisptr].hops = ttl;
		ctl->his[ctl->hisptr].sendtime = hdr->tv;
		if (sendto(fd, ctl->pktbuf, ctl->mtu - ctl->overhead, 0,
			   (struct sockaddr *)&ctl->target, ctl->targetlen) > 0)
			break;
		res = recverr(ctl, fd, ai, ttl);
		ctl->his[ctl->hisptr].hops = 0;
		if (res == 0)
			return 0;
		if (res > 0)
			goto restart;
	}
	ctl->hisptr = (ctl->hisptr + 1) & 63;

	if (i < 10) {
		data_wait(fd);
		if (recv(fd, ctl->pktbuf, ctl->mtu, MSG_DONTWAIT) > 0) {
			printf("%2d?: reply received 8)\n", ttl);
			return 0;
		}
		return recverr(ctl, fd, ai, ttl);
	}

	printf("%2d:  send failed\n", ttl);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"\nUsage\n"
		"  tracepath [options] <destination>\n"
		"\nOptions:\n"
		"  -4             use IPv4\n"
		"  -6             use IPv6\n"
		"  -b             print both name and ip\n"
		"  -l <length>    use packet <length>\n"
		"  -m <hops>      use maximum <hops>\n"
		"  -n             no dns name resolution\n"
		"  -p <port>      use destination <port>\n"
		"  -V             print version and exit\n"
		"  <destination>  dns name or ip address\n"
		"\nFor more details see tracepath(8).\n");
	exit(-1);
}

int main(int argc, char **argv)
{
	struct run_state ctl = {
		.max_hops = MAX_HOPS_DEFAULT,
		.hops_to = -1,
		.hops_from = -1,
		0
	};
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_DGRAM,
		.ai_protocol = IPPROTO_UDP,
#ifdef USE_IDN
		.ai_flags = AI_IDN | AI_CANONNAME,
#endif
	};
	struct addrinfo *ai, *result;
	int ch;
	int status;
	int fd;
	int on;
	int ttl;
	char *p;
	char pbuf[NI_MAXSERV];

#ifdef USE_IDN
	setlocale(LC_ALL, "");
#endif

	/* Support being called using `tracepath4` or `tracepath6` symlinks */
	if (argv[0][strlen(argv[0]) - 1] == '4')
		hints.ai_family = AF_INET;
	else if (argv[0][strlen(argv[0]) - 1] == '6')
		hints.ai_family = AF_INET6;

	while ((ch = getopt(argc, argv, "46nbh?l:m:p:V")) != EOF) {
		switch (ch) {
		case '4':
			if (hints.ai_family == AF_INET6) {
				fprintf(stderr,
					"tracepath: Only one -4 or -6 option may be specified\n");
				exit(2);
			}
			hints.ai_family = AF_INET;
			break;
		case '6':
			if (hints.ai_family == AF_INET) {
				fprintf(stderr,
					"tracepath: Only one -4 or -6 option may be specified\n");
				exit(2);
			}
			hints.ai_family = AF_INET6;
			break;
		case 'n':
			ctl.no_resolve = 1;
			break;
		case 'b':
			ctl.show_both = 1;
			break;
		case 'l':
			if ((ctl.mtu = atoi(optarg)) <= ctl.overhead) {
				fprintf(stderr,
					"Error: pktlen must be > %d and <= %d.\n",
					ctl.overhead, INT_MAX);
				exit(1);
			}
			break;
		case 'm':
			ctl.max_hops = atoi(optarg);
			if (ctl.max_hops < 0 || ctl.max_hops > MAX_HOPS_LIMIT) {
				fprintf(stderr,
					"Error: max hops must be 0 .. %d (inclusive).\n",
					MAX_HOPS_LIMIT);
			}
			break;
		case 'p':
			ctl.base_port = atoi(optarg);
			break;
		case 'V':
			printf(IPUTILS_VERSION("tracepath"));
			return 0;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	/* Backward compatibility */
	if (!ctl.base_port) {
		p = strchr(argv[0], '/');
		if (p) {
			*p = 0;
			ctl.base_port = atoi(p + 1);
		} else
			ctl.base_port = 44444;
	}
	sprintf(pbuf, "%u", ctl.base_port);

	status = getaddrinfo(argv[0], pbuf, &hints, &result);
	if (status) {
		fprintf(stderr, "tracepath: %s: %s\n", argv[0],
			gai_strerror(status));
		exit(1);
	}

	fd = -1;
	for (ai = result; ai; ai = ai->ai_next) {
		if (ai->ai_family != AF_INET6 && ai->ai_family != AF_INET)
			continue;
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		memcpy(&ctl.target, ai->ai_addr, ai->ai_addrlen);
		ctl.targetlen = ai->ai_addrlen;
		break;
	}
	if (fd < 0) {
		perror("socket/connect");
		exit(1);
	}

	switch (ai->ai_family) {
	case AF_INET6:
		ctl.overhead = 48;
		if (!ctl.mtu)
			ctl.mtu = 128000;
		if (ctl.mtu <= ctl.overhead)
			goto pktlen_error;

		on = IPV6_PMTUDISC_DO;
		if (setsockopt(fd, SOL_IPV6, IPV6_MTU_DISCOVER, &on, sizeof(on)) &&
		    (on = IPV6_PMTUDISC_DO, setsockopt(fd, SOL_IPV6,
		     IPV6_MTU_DISCOVER, &on, sizeof(on)))) {
			perror("IPV6_MTU_DISCOVER");
			exit(1);
		}
		on = 1;
		if (setsockopt(fd, SOL_IPV6, IPV6_RECVERR, &on, sizeof(on))) {
			perror("IPV6_RECVERR");
			exit(1);
		}
		if (setsockopt(fd, SOL_IPV6, IPV6_HOPLIMIT, &on, sizeof(on))
#ifdef IPV6_RECVHOPLIMIT
		    && setsockopt(fd, SOL_IPV6, IPV6_2292HOPLIMIT, &on, sizeof(on))
#endif
		    ) {
			perror("IPV6_HOPLIMIT");
			exit(1);
		}
		if (!IN6_IS_ADDR_V4MAPPED(&(((struct sockaddr_in6 *)&ctl.target)->sin6_addr)))
			break;
		ctl.mapped = 1;
		/*FALLTHROUGH*/
	case AF_INET:
		ctl.overhead = 28;
		if (!ctl.mtu)
			ctl.mtu = 65535;
		if (ctl.mtu <= ctl.overhead)
			goto pktlen_error;

		on = IP_PMTUDISC_DO;
		if (setsockopt(fd, SOL_IP, IP_MTU_DISCOVER, &on, sizeof(on))) {
			perror("IP_MTU_DISCOVER");
			exit(1);
		}
		on = 1;
		if (setsockopt(fd, SOL_IP, IP_RECVERR, &on, sizeof(on))) {
			perror("IP_RECVERR");
			exit(1);
		}
		if (setsockopt(fd, SOL_IP, IP_RECVTTL, &on, sizeof(on))) {
			perror("IP_RECVTTL");
			exit(1);
		}
	}

	ctl.pktbuf = malloc(ctl.mtu);
	if (!ctl.pktbuf) {
		perror("malloc");
		exit(1);
	}

	for (ttl = 1; ttl <= ctl.max_hops; ttl++) {
		int res;
		int i;

		on = ttl;
		switch (ai->ai_family) {
		case AF_INET6:
			if (setsockopt(fd, SOL_IPV6, IPV6_UNICAST_HOPS, &on, sizeof(on))) {
				perror("IPV6_UNICAST_HOPS");
				exit(1);
			}
			if (!ctl.mapped)
				break;
			/*FALLTHROUGH*/
		case AF_INET:
			if (setsockopt(fd, SOL_IP, IP_TTL, &on, sizeof(on))) {
				perror("IP_TTL");
				exit(1);
			}
		}

 restart:
		for (i = 0; i < 3; i++) {
			int old_mtu;

			old_mtu = ctl.mtu;
			res = probe_ttl(&ctl, fd, ai, ttl);
			if (ctl.mtu != old_mtu)
				goto restart;
			if (res == 0)
				goto done;
			if (res > 0)
				break;
		}

		if (res < 0)
			printf("%2d:  no reply\n", ttl);
	}
	printf("     Too many hops: pmtu %d\n", ctl.mtu);

 done:
	freeaddrinfo(result);

	printf("     Resume: pmtu %d ", ctl.mtu);
	if (ctl.hops_to >= 0)
		printf("hops %d ", ctl.hops_to);
	if (ctl.hops_from >= 0)
		printf("back %d ", ctl.hops_from);
	printf("\n");
	exit(0);

 pktlen_error:
	fprintf(stderr, "Error: pktlen must be > %d and <= %d\n",
		ctl.overhead, INT_MAX);
	exit(1);
}
