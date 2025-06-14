
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/types.h>
#include <linux/netfilter.h>		/* for NF_ACCEPT */
#include <errno.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

#include "libnet.h"
#include <string.h>

char num;
char **host = NULL;
uint32_t verdict = NF_DROP;

void dump(unsigned char* buf, int size) {
	int i;
	for (i = 0; i < size; i++) {
		if (i != 0 && i % 16 == 0)
			printf("\n");
		printf("%02X ", buf[i]);
	}
	printf("\n");
}

char *strnstr(char *s, char *find, int slen) {
	char c, sc;
	int len = strlen(find);

	if (len == 0)
		return s;

	char *new_s = (char *)malloc(slen + 1);
	strncpy(new_s, s, slen);
	new_s[slen] = '\0';

	char *ret = strstr(new_s, find);
	free(new_s);

	return ret;
}


/* returns packet id */
static uint32_t print_pkt (struct nfq_data *tb)
{
	int id = 0;
	struct nfqnl_msg_packet_hdr *ph;
	struct nfqnl_msg_packet_hw *hwph;
	uint32_t mark, ifi, uid, gid;
	int ret;
	unsigned char *data, *secdata;


	ph = nfq_get_msg_packet_hdr(tb);
	if (ph) {
		id = ntohl(ph->packet_id);
		printf("hw_protocol=0x%04x hook=%u id=%u ",
			ntohs(ph->hw_protocol), ph->hook, id);
	}

	hwph = nfq_get_packet_hw(tb);
	if (hwph) {
		int i, hlen = ntohs(hwph->hw_addrlen);

		printf("hw_src_addr=");
		for (i = 0; i < hlen-1; i++)
			printf("%02x:", hwph->hw_addr[i]);
		printf("%02x ", hwph->hw_addr[hlen-1]);
	}

	mark = nfq_get_nfmark(tb);
	if (mark)
		printf("mark=%u ", mark);

	ifi = nfq_get_indev(tb);
	if (ifi)
		printf("indev=%u ", ifi);

	ifi = nfq_get_outdev(tb);
	if (ifi)
		printf("outdev=%u ", ifi);
	ifi = nfq_get_physindev(tb);
	if (ifi)
		printf("physindev=%u ", ifi);

	ifi = nfq_get_physoutdev(tb);
	if (ifi)
		printf("physoutdev=%u ", ifi);

	if (nfq_get_uid(tb, &uid))
		printf("uid=%u ", uid);

	if (nfq_get_gid(tb, &gid))
		printf("gid=%u ", gid);

	ret = nfq_get_secctx(tb, &secdata);
	if (ret > 0)
		printf("secctx=\"%.*s\" ", ret, secdata);

	ret = nfq_get_payload(tb, &data);
	if (ret >= 0){
		printf("payload_len=%d ", ret);
		dump(data, ret);
	}

	fputc('\n', stdout);

	return id;
}

static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
	      struct nfq_data *nfa, void *data)
{
	uint32_t id = print_pkt(nfa);
	printf("entering callback\n");
	unsigned char* pkt;
	uint8_t ipv4_len, tcp_len;
	u_int16_t pay_len;
	int ret = nfq_get_payload(nfa, &pkt);
	char *site = NULL;
	struct libnet_ipv4_hdr* ipv4_hdr = (struct libnet_ipv4_hdr*)pkt;
	if(ipv4_hdr->ip_p != IPTYPE_TCP)
		return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
	struct libnet_tcp_hdr* tcp_hdr= (struct libnet_tcp_hdr*)(ipv4_hdr+1);
	/* http request */
	unsigned char *http = (unsigned char*)(tcp_hdr) + (tcp_hdr->th_off * 4);	
	if(ntohs(tcp_hdr->th_dport) == 80 && (strncmp(http, "GET", 3) == 0 || strncmp(http, "POST", 4) == 0 || strncmp(http, "POST", 4) == 0 || strncmp(http, "PUT", 3) == 0 || strncmp(http, "DELETE", 6) == 0 || strncmp(http, "PATCH", 5) == 0 || strncmp(http, "HEAD", 4) == 0 || strncmp(http, "OPTIONS", 7) == 0 || strncmp(http, "CONNECT", 7) == 0)){
		printf("\n********** HTTP Request Detected **********\n");	
		if((site = strnstr(http, "Host: ", 50)) == NULL) return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
		for(int i = 1; i < num; i++) {
			if (strncmp(site + 6, host[i], strlen(host[i])) == 0 && strncmp(site + 6 + strlen(host[i]), "\r\n", 2) == 0) {
				printf("%s Blocked\n\n", host[i]);
				return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
			}
		}

	}

	return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
}


void usage() {
	printf("syntax: netfilter-test <host> \n");
	printf("sample: netfilter-test test.gilgil.net\n");
}


int main(int argc, char **argv)
{
	struct nfq_handle *h;
	struct nfq_q_handle *qh;
	int fd;
	int rv;
	uint32_t queue = 0;
	char buf[4096] __attribute__ ((aligned));

	if (argc != 2) {
		printf("syntax : netfilter-test <host>\n");
		printf("sample : netfilter-test test.gilgil.net\n");
		return -1;
	}

	num = argc;
	host = argv;
	

	printf("opening library handle\n");
	h = nfq_open();
	if (!h) {
		fprintf(stderr, "error during nfq_open()\n");
		exit(1);
	}

	printf("unbinding existing nf_queue handler for AF_INET (if any)\n");
	if (nfq_unbind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_unbind_pf()\n");
		exit(1);
	}

	printf("binding nfnetlink_queue as nf_queue handler for AF_INET\n");
	if (nfq_bind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_bind_pf()\n");
		exit(1);
	}

	printf("binding this socket to queue '%d'\n", queue);
	qh = nfq_create_queue(h, queue, &cb, NULL);
	if (!qh) {
		fprintf(stderr, "error during nfq_create_queue()\n");
		exit(1);
	}

	printf("setting copy_packet mode\n");
	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "can't set packet_copy mode\n");
		exit(1);
	}

	printf("setting flags to request UID and GID\n");
	if (nfq_set_queue_flags(qh, NFQA_CFG_F_UID_GID, NFQA_CFG_F_UID_GID)) {
		fprintf(stderr, "This kernel version does not allow to "
				"retrieve process UID/GID.\n");
	}

	printf("setting flags to request security context\n");
	if (nfq_set_queue_flags(qh, NFQA_CFG_F_SECCTX, NFQA_CFG_F_SECCTX)) {
		fprintf(stderr, "This kernel version does not allow to "
				"retrieve security context.\n");
	}

	printf("Waiting for packets...\n");

	fd = nfq_fd(h);

	for (;;) {
		if ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
			printf("pkt received\n");
			nfq_handle_packet(h, buf, rv);
			continue;
		}
		/* if your application is too slow to digest the packets that
		 * are sent from kernel-space, the socket buffer that we use
		 * to enqueue packets may fill up returning ENOBUFS. Depending
		 * on your application, this error may be ignored. Please, see
		 * the doxygen documentation of this library on how to improve
		 * this situation.
		 */
		if (rv < 0 && errno == ENOBUFS) {
			printf("losing packets!\n");
			continue;
		}
		perror("recv failed");
		break;
	}

	printf("unbinding from queue 0\n");
	nfq_destroy_queue(qh);

#ifdef INSANE
	/* normally, applications SHOULD NOT issue this command, since
	 * it detaches other programs/sockets from AF_INET, too ! */
	printf("unbinding from AF_INET\n");
	nfq_unbind_pf(h, AF_INET);
#endif

	printf("closing library handle\n");
	nfq_close(h);

	exit(0);
}
