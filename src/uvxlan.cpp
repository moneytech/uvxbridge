/*
 * Copyright (C) 2017 Joyent Inc.
 * Copyright (C) 2017 Matthew Macy <matt.macy@joyent.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

extern "C" {
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mbuf.h>
#include <stdio.h>

#include <net/ethernet.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>

struct rmlock { uint8_t pad; }; /* Needed by pfil.h */
#include <net/pfil.h> /* PFIL_IN */

int ipfw_check_frame(void *arg, struct mbuf **m0, struct ifnet *ifp,
					 int dir, struct inpcb *inp, struct ip_fw_chain *);

}
#include "uvxbridge.h"
#include "uvxlan.h"
#include "proto.h"
#include <nmutil.h>
#include "xxhash.h"
#include <glue.h>


#define AE_REQUEST		0x0100040600080100UL
#define AE_REPLY		0x0200040600080100UL
#define AE_REVREQUEST		0x0300040600080100UL
#define AE_REVREPLY		0x0400040600080100UL

#define AE_REQUEST_ALL		0x0A00040600080100UL
#define AE_REVREQUEST_ALL	0x0B00040600080100UL
#define AE_VM_VXLANID_REQUEST	0x0C00040600080100UL
#define AE_VM_VXLANID_REQUEST_ALL	0x0D00040600080100UL
#define AE_VM_VXLANID_REPLY	0x0E00040600080100UL
#define AE_VM_VLANID_REQUEST	0x0F00040600080100UL
#define AE_VM_VLANID_REQUEST_ALL	0x1000040600080100UL
#define AE_VM_VLANID_REPLY	0x1100040600080100UL

#define A(val) printf("got %s\n", #val)
extern int debug;

/*
 * Send a query to the provisioning agent
 */
void
data_send_arp(uint64_t targetha, uint32_t targetip, uint64_t op, vxstate_t *state)
{
	struct arphdr_ether ae;
	char *txbuf;
	path_state_t ps;

	/* get txbuf */
	if ((txbuf = get_txbuf(&ps, state->vs_nm_ingress)) == NULL)
		return;
	ae.ae_hdr.data = op;
	u64tomac(state->vs_ctrl_mac, ae.ae_sha);
	/* XXX - assume v4 */
	ae.ae_spa = state->vs_dflt_rte.ri_laddr.in4.s_addr;
	u64tomac(targetha, ae.ae_tha);
	ae.ae_tpa = targetip;
	eh_fill((struct ether_header *)txbuf, state->vs_ctrl_mac, state->vs_prov_mac,
			ETHERTYPE_ARP);
	memcpy(txbuf + ETHER_HDR_LEN, &ae, sizeof(struct arphdr_ether));
	txring_next(&ps, 60);
}

void
data_send_arp_phys(char *rxbuf, char *txbuf, path_state_t *ps, vxstate_t *state, int gratuitous)
{
	struct arphdr_ether *dae;
	struct ether_vlan_header *evh, *sevh;
	uint64_t dmac, broadcast = 0xFFFFFFFFFFFF;

	sevh = (struct ether_vlan_header *)(rxbuf);
	evh = (struct ether_vlan_header *)(txbuf);
	/* XXX hardcoding no VLAN */
	dae = (struct arphdr_ether *)(txbuf + ETHER_HDR_LEN);
	if (gratuitous)
		eh_fill((struct ether_header *)evh, state->vs_intf_mac, broadcast, ETHERTYPE_ARP);
	else {
		dmac = mactou64(sevh->evl_shost);
		eh_fill((struct ether_header *)evh, state->vs_intf_mac, dmac, ETHERTYPE_ARP);
	}
	dae->ae_hdr.data = AE_REPLY;
	dae->ae_spa = state->vs_dflt_rte.ri_laddr.in4.s_addr;
	u64tomac(state->vs_intf_mac, dae->ae_sha);
	dae->ae_tpa = state->vs_dflt_rte.ri_laddr.in4.s_addr;
	u64tomac(state->vs_intf_mac, dae->ae_tha);
	*(ps->ps_tx_len) = 60;
}

int
cmd_dispatch_arp(char *rxbuf, char *txbuf, path_state_t *ps, vxstate_t *state)
{
	struct arphdr_ether *sah, *dah;
	struct ether_header *eh;
	int op;
	uint32_t hostip, targetpa = 0;
	uint16_t *rmacp, *lmacp;
	uint64_t len, targetha = 0, reply = 0;
	l2tbl_t &l2tbl = state->vs_l2_phys;
	ftablemap_t &ftablemap = state->vs_ftables;
	intf_info_map_t &intftbl = state->vs_intf_table;

	len = ps->ps_rx_len;
	if (len < ETHER_HDR_LEN + sizeof(struct arphdr_ether) && debug < 2)
		return 0;

	eh = (struct ether_header *)(rxbuf);
	sah = (struct arphdr_ether *)(rxbuf + ETHER_HDR_LEN);
	dah = (struct arphdr_ether *)(txbuf + ETHER_HDR_LEN);
	op = ntohs(sah->ae_hdr.fields.ar_op);
	/* place holder */
	hostip = htobe32(0xDEADBEEF);

	switch (op) {
		case ARPOP_REQUEST: {
			A(ARPOP_REQUEST);
			auto it = l2tbl.l2t_v4.find(sah->ae_tpa);
			if (it != l2tbl.l2t_v4.end()) {
				reply = AE_REPLY;
				targetpa = sah->ae_tpa;
				targetha = it->second;
			}
			break;
		}
		case ARPOP_REPLY:
			A(ARPOP_REPLY);
			memcpy(&targetha, sah->ae_tha, ETHER_ADDR_LEN);
			if (targetha == 0)
				l2tbl.l2t_v4.erase(sah->ae_tpa);
			else
				l2tbl.l2t_v4.insert(pair<uint32_t, uint64_t>(sah->ae_tpa, targetha));
			break;
		case ARPOP_REVREQUEST: {
			/* ipv4 forwarding table lookup */
			A(ARPOP_REVREQUEST);
			targetha = mactou64(eh->ether_shost);
			auto ftable_it = ftablemap.find(targetha);
			if (ftable_it == ftablemap.end()) {
				reply = AE_VM_VXLANID_REQUEST;
				targetpa = 0;
				break;
			}
			targetha = mactou64(sah->ae_tha);
			auto it = ftable_it->second.find(targetha);
			if (it != ftable_it->second.end() && it->second.vfe_v6 == 0) {
				reply = AE_REVREPLY;
				targetpa = it->second.vfe_raddr.in4.s_addr;
			}
			break;
		}
		case ARPOP_REVREPLY: {
			/* ipv4 forwarding table update */
			A(ARPOP_REVREPLY);
			memcpy(&targetha, sah->ae_tha, ETHER_ADDR_LEN);
			targetha = mactou64(eh->ether_shost);
			auto ftable_it = ftablemap.find(targetha);
			if (ftable_it == ftablemap.end()) {
				reply = AE_VM_VXLANID_REQUEST;
				targetpa = 0;
				break;
			}
			if (sah->ae_tpa == 0)
				ftable_it->second.erase(targetha);
			else {
				vfe_t vfe;
				vfe.vfe_raddr.in4.s_addr = sah->ae_tpa;
				ftable_it->second.insert(fwdent(targetha, vfe));
				/* proactively resolve the MAC address for tpa */
				reply = AE_REQUEST;
				targetpa = sah->ae_tpa;
				targetha = 0;
			}
			break;
		}
		case ARPOP_REQUEST_ALL:
			A(ARPOP_REQUEST_ALL);
			break;
		case ARPOP_REVREQUEST_ALL:
			A(ARPOP_REVREQUEST_ALL);
			break;
		case ARPOP_VM_VXLANID_REQUEST: {
			A(ARPOP_VM_VXLANID_REQUEST);
			memcpy(&targetha, sah->ae_tha, ETHER_ADDR_LEN);
			auto it = intftbl.find(targetha);
			if (it != intftbl.end()) {
				reply = AE_VM_VXLANID_REPLY;
				targetpa = it->second->ii_ent.fields.flags << 24;
				targetpa |= it->second->ii_ent.fields.vxlanid;
			}
			break;
		}
		case ARPOP_VM_VXLANID_REQUEST_ALL:
			A(ARPOP_VM_VXLANID_REQUEST_ALL);
			break;
		case ARPOP_VM_VXLANID_REPLY: {

			A(ARPOP_VM_VXLANID_REPLY);
			targetha = mactou64(sah->ae_tha);
			if (sah->ae_tpa == 0) {
				uint32_t vxlanid;
				auto it = intftbl.find(targetha);

				if (it != intftbl.end()) {
					vxlanid = it->second->ii_ent.fields.vxlanid;
					auto it_ftable = ftablemap.find(vxlanid);
					if (it_ftable != ftablemap.end() &&
						it_ftable->second.size() == 0)
						ftablemap.erase(vxlanid);

					delete it->second;
					intftbl.erase(targetha);
				}
			} else {
				ftable_t ftable;
				intf_info_t *ii;
				uint32_t vxlanid = sah->ae_tpa & 0xffffff;
				uint8_t flags = sah->ae_tflags;
				bool insert = false;
				auto it = intftbl.find(targetha);

				if (it != intftbl.end()) {
					ii = it->second;
				} else {
					ii = new intf_info();
					insert = true;
				}
				ii->ii_ent.fields.vxlanid = vxlanid;
				ii->ii_ent.fields.flags = flags;
				if (insert)
					intftbl.insert(pair<uint64_t, intf_info_t*>(targetha, ii));

				auto it_ftable = ftablemap.find(sah->ae_tpa);
				if (it_ftable == ftablemap.end())
					ftablemap.insert(pair<uint32_t, ftable_t>(vxlanid, ftable));
			}
			break;
		}
		case ARPOP_VM_VLANID_REQUEST: {
			A(ARPOP_VM_VLANID_REQUEST);
			memcpy(&targetha, sah->ae_tha, ETHER_ADDR_LEN);
			auto it = intftbl.find(targetha);
			if (it != intftbl.end()) {
				reply = AE_VM_VLANID_REPLY;
				targetpa = it->second->ii_ent.fields.vlanid;
			}
			break;
		}
		case ARPOP_VM_VLANID_REQUEST_ALL:
			A(ARPOP_VM_VLANID_REQUEST_ALL);
			break;
		case ARPOP_VM_VLANID_REPLY: {
			A(ARPOP_VM_VLANID_REPLY);
			memcpy(&targetha, sah->ae_tha, ETHER_ADDR_LEN);
			auto it = intftbl.find(targetha);

			if (it != intftbl.end())
				it->second->ii_ent.fields.vlanid = sah->ae_tpa;
			else if (sah->ae_tpa != 0) {
				intf_info_t *ii = new intf_info();
				ii->ii_ent.fields.vlanid = sah->ae_tpa;
				intftbl.insert(pair<uint64_t, intf_info_t*>(targetha, ii));
			}
			break;
		}
		default:
			printf("unrecognized value data: 0x%016lX op: 0x%02X\n", sah->ae_hdr.data, op);
	}
	/* save potential cache stall to the end */
	if (reply) {
		eh = (struct ether_header *)txbuf;
		lmacp = (uint16_t *)(uintptr_t)&eh->ether_dhost;
		rmacp =  (uint16_t *)&state->vs_prov_mac;
		lmacp[0] = rmacp[0];
		lmacp[1] = rmacp[1];
		lmacp[2] = rmacp[2];
		rmacp =  (uint16_t *)&state->vs_ctrl_mac;
		lmacp[3] = rmacp[0];
		lmacp[4] = rmacp[1];
		lmacp[5] = rmacp[2];
		/* [6] */
		eh->ether_type = ETHERTYPE_ARP;
		/* [7-10] */
		dah->ae_hdr.data = reply;
		/* [11-13] - ar_sha */
		lmacp[11] = rmacp[0];
		lmacp[12] = rmacp[1];
		lmacp[13] = rmacp[2];
		/* [14-15] - ae_spa */
		dah->ae_spa = hostip;

		rmacp = (uint16_t *)&targetha;
		/* [16-18] - ae_tha */
		lmacp[16] = rmacp[0];
		lmacp[17] = rmacp[1];
		lmacp[18] = rmacp[2];
		/* actual value of interest */
		dah->ae_tpa = targetpa;
		*(ps->ps_tx_len) = 60;
		return (1);
	}
	return (0);
}

static void
dhcp_fill(struct dhcp *bp, vxstate_t *state)
{
	uint16_t *dstp, *srcp;
	bzero(bp, sizeof(*bp));
	bp->bp_op = BOOTREQUEST;
	bp->bp_htype = HTYPE_ETHERNET;
	bp->bp_hlen = ETHER_ADDR_LEN;
	bp->bp_hops = 0;
	bp->bp_xid = htonl(42); /* magic number :) */
	srcp = (uint16_t *)&state->vs_ctrl_mac;
	dstp = (uint16_t *)&bp->bp_chaddr;
	dstp[0] = srcp[0];
	dstp[1] = srcp[1];
	dstp[2] = srcp[2];
	bp->bp_vendid = htonl(BP_FIXED);
}

int
cmd_send_dhcp(char *rxbuf __unused, char *txbuf, path_state_t *ps, vxstate_t *state)
{
	struct ether_header *eh = (struct ether_header *)txbuf;
	struct ip *ip = (struct ip *)(uintptr_t)(eh + 1);
	struct udphdr *uh = (struct udphdr *)(ip + 1);
	struct dhcp *bp = (struct dhcp *)(uintptr_t)(uh + 1);

	eh_fill(eh, state->vs_ctrl_mac, state->vs_prov_mac, ETHERTYPE_IP);
	/* source IP unknown, dest broadcast IP */
	ip_fill(ip, 0, 0xffffffff, sizeof(*bp) + sizeof(*uh) + sizeof(*ip), IPPROTO_UDP);
	udp_fill(uh, IPPORT_BOOTPC, IPPORT_BOOTPS, sizeof(*bp));
	dhcp_fill(bp, state);
	*(ps->ps_tx_len) = BP_MSG_OVERHEAD + sizeof(*bp);
	return (1);
}

static void
uvxstat_fill(struct uvxstat *stat, vxstate_t *state)
{
	/* XXX -- only supports one datapath */
	if (state->vs_datapath_count)
		memcpy(stat, &state->vs_dp_states[0]->vsd_stats, sizeof(*stat));
}

int
cmd_send_heartbeat(char *rxbuf __unused, char *txbuf, path_state_t *ps,
				   vxstate_t *state)
{
	struct ether_header *eh = (struct ether_header *)txbuf;
	struct ip *ip = (struct ip *)(uintptr_t)(eh + 1);
	struct udphdr *uh = (struct udphdr *)(ip + 1);
	struct uvxstat *stat = (struct uvxstat *)(uintptr_t)(uh + 1);

	eh_fill(eh, state->vs_ctrl_mac, state->vs_prov_mac, ETHERTYPE_IP);
	/* source IP unknown, dest broadcast IP */
	ip_fill(ip, 0, 0xffffffff, sizeof(*stat) + sizeof(*uh) + sizeof(*ip), IPPROTO_UDP);
	udp_fill(uh, IPPORT_STATPC, IPPORT_STATPS, sizeof(*stat));
	uvxstat_fill(stat, state);
	*(ps->ps_tx_len) = BP_MSG_OVERHEAD + sizeof(*stat);
	return (1);
}

int
cmd_dispatch_bp(struct dhcp *bp, vxstate_t *state)
{
	rte_t *rte = &state->vs_dflt_rte;
	/* Validate BOOTP header */
	/* .... */
	/* we only care about our interface address and the gateway address */
	rte->ri_prefixlen = 24;
	rte->ri_mask.in4.s_addr = htobe32(0xffffff00);
	rte->ri_laddr.in4.s_addr = bp->bp_ciaddr.s_addr;
	rte->ri_raddr.in4.s_addr = bp->bp_giaddr.s_addr;
	rte->ri_flags = RI_VALID;
	DBG("route installed\n");
	return (1);
}

/*
 * generic handler for sockopt functions
 */
static int
ctl_handler(struct sockopt *sopt, struct ip_fw_chain *chain)
{
	int error = EINVAL;

	ND("called, level %d", sopt->sopt_level);
	if (sopt->sopt_level != IPPROTO_IP)
		return (EINVAL);
	switch (sopt->sopt_name) {
	default:
		D("command not recognised %d", sopt->sopt_name);
		break;
	case IP_FW3: // XXX untested
	case IP_FW_ADD: /* ADD actually returns the body... */
	case IP_FW_GET:
	case IP_FW_DEL:
	case IP_FW_TABLE_GETSIZE:
	case IP_FW_TABLE_LIST:
	case IP_FW_NAT_GET_CONFIG:
	case IP_FW_NAT_GET_LOG:
	case IP_FW_FLUSH:
	case IP_FW_ZERO:
	case IP_FW_RESETLOG:
	case IP_FW_TABLE_ADD:
	case IP_FW_TABLE_DEL:
	case IP_FW_TABLE_FLUSH:
	case IP_FW_NAT_CFG:
	case IP_FW_NAT_DEL:
		if (ip_fw_ctl_ptr != NULL)
			error = ip_fw_ctl_ptr(sopt, chain);
		else {
			D("ipfw not enabled");
			error = ENOPROTOOPT;
		}
		break;
#ifdef __unused__
	case IP_DUMMYNET_GET:
	case IP_DUMMYNET_CONFIGURE:
	case IP_DUMMYNET_DEL:
	case IP_DUMMYNET_FLUSH:
	case IP_DUMMYNET3:
		if (ip_dn_ctl_ptr != NULL)
			error = ip_dn_ctl_ptr(sopt);
		else
			error = ENOPROTOOPT;
		break ;
#endif
	}
	ND("returning error %d", error);
	return error;
}


int
cmd_udp_fill(char *txbuf, void *payload, int len, uint16_t sport, uint16_t dport, vxstate_t *state)
{
	struct ether_header *eh = (struct ether_header *)txbuf;
	struct ip *ip = (struct ip *)(uintptr_t)(eh + 1);
	struct udphdr *uh = (struct udphdr *)(ip + 1);
	void *data = (void *)(uintptr_t)(uh + 1);

	eh_fill(eh, state->vs_ctrl_mac, state->vs_prov_mac, ETHERTYPE_IP);
	/* source IP unknown, dest broadcast IP */
	ip_fill(ip, state->vs_dflt_rte.ri_laddr.in4.s_addr, 0xffffffff,
			len + sizeof(*uh) + sizeof(*ip), IPPROTO_UDP);
	udp_fill(uh, sport, dport, len);
	memcpy(data, payload, len);
	return (len + sizeof(*uh) + sizeof(*ip) + sizeof(*eh));
}

int
cmd_dispatch_ipfw(struct ipfw_wire_hdr *ipfw, char *txbuf, path_state_t *ps, vxstate_t *state)
{
	struct thread dummy;
	intf_info_map_t &intftbl = state->vs_intf_table;
	socklen_t optlen = 0;
	uint64_t mac;
	struct sockopt sopt;
	void *optval  = (void *)(uintptr_t)(ipfw + 1);
	struct ip_fw_chain *chain;

	mac = ((uint64_t)ipfw->mac) & 0xFFFFFFFFFFFF;
	auto it = intftbl.find(mac);
	if (it == intftbl.end()) {
		ipfw->level = htonl(ENOENT);
		ipfw->optlen = htonl(0);
		goto done;
	}
	chain = it->second->ii_chain;
	sopt.sopt_dir = (enum sopt_dir)ntohl(ipfw->dir);
	sopt.sopt_level = ntohl(ipfw->level);
	sopt.sopt_val = optval;
	sopt.sopt_name = ntohl(ipfw->optname);
	sopt.sopt_valsize = ntohl(ipfw->optlen);
	sopt.sopt_td = &dummy;

	ipfw->level = htonl(ctl_handler(&sopt, chain));
	ipfw->optlen = htonl(0);
	ipfw->dir = htonl(sopt.sopt_dir);
	if (sopt.sopt_dir == SOPT_GET) {
		ipfw->optlen = htonl(sopt.sopt_valsize);
		optlen = sopt.sopt_valsize;
	}
  done:
	*(ps->ps_tx_len) = cmd_udp_fill(txbuf, ipfw, sizeof(*ipfw) + optlen,
				   IPPORT_IPFWPS, IPPORT_IPFWPC, state);
	return (1);
}

int
cmd_dispatch_ip(char *rxbuf, char *txbuf, path_state_t *ps, vxstate_t *state)
{
	struct ip *ip = (struct ip *)(uintptr_t)(rxbuf + ETHER_HDR_LEN);
	struct udphdr *uh = (struct udphdr *)(uintptr_t)((caddr_t)ip + (ip->ip_hl << 2));
	struct dhcp *bp = (struct dhcp *)(uintptr_t)(uh + 1);
	struct ipfw_wire_hdr *ipfw = (struct ipfw_wire_hdr *)(uintptr_t)(uh + 1);
	uint16_t dport;
	/* validate ip header */

	/* validate the UDP header */
	dport = ntohs(uh->uh_dport);

	/* we're only supporting a BOOTP response for the moment */
	if (ps->ps_rx_len < sizeof(*bp) + BP_MSG_OVERHEAD)
		return 0;

	if (dport == IPPORT_BOOTPS && cmd_dispatch_bp(bp, state)) {
		char *txbuf_;
		path_state_t psgrat;

		bzero(&psgrat, sizeof(path_state_t));
		if ((txbuf_ = get_txbuf(&psgrat, state->vs_nm_egress)) == NULL)
			return (0);
		/*
		 * we have our address -- now we want to send out a
		 * gratuitous ARP for the switch
		 */
		data_send_arp_phys(rxbuf, txbuf_, &psgrat, state, 1);
		txring_next(&psgrat, 60);
		/* XXX proactively resolve the MAC address for the gateway */
		/* ... */
	} else if (dport == IPPORT_IPFWPS) {
		return cmd_dispatch_ipfw(ipfw, txbuf, ps, state);
	}
	return (0);
}

/*
 * Respond to queries for our encapsulating IP
 */
int
data_dispatch_arp_phys(char *rxbuf, char *txbuf, path_state_t *ps,
				  vxstate_dp_t *dp_state)
{
	vxstate_t *state = dp_state->vsd_state;
	struct arphdr_ether *sah;
	struct ether_vlan_header *evh;
	int etype, hdrlen;

	evh = (struct ether_vlan_header *)(rxbuf);
	if (evh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		etype = ntohs(evh->evl_proto);
		hdrlen = ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN;
	} else {
		etype = ntohs(evh->evl_encap_proto);
		hdrlen = ETHER_HDR_LEN;
	}
	sah = (struct arphdr_ether *)(rxbuf + hdrlen);
	if (sah->ae_hdr.data != AE_REQUEST)
		return (0);
	if (sah->ae_tpa != state->vs_dflt_rte.ri_laddr.in4.s_addr)
		return (0);

	/* we've confirmed that it's bound for us - we need to respond */
	data_send_arp_phys(rxbuf, txbuf, ps, state, 0);
	return (1);
}

/*
 * Proactively query the provisioning agent
 * - guest MAC -> vxlanid and vlanid when they ARP
 * - destination MAC -> remote IP when they ARP for the vxlan MAC
 * - remote IP -> remote MAC when the remote IP is first learned
 */
int
data_dispatch_arp_vx(char *rxbuf, char *txbuf __unused, path_state_t *ps __unused,
				  vxstate_dp_t *dp_state)
{
	struct ether_vlan_header *evh;
	struct arphdr_ether *sae;
	vxstate_t *state = dp_state->vsd_state;
	intf_info_map_t &intftbl = state->vs_intf_table;
	ftablemap_t *ftablemap = &state->vs_ftables;
	int etype, hdrlen;
	uint64_t mac;
	uint32_t vxlanid;

	evh = (struct ether_vlan_header *)rxbuf;
	if (evh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		hdrlen = ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN;
		etype = ntohs(evh->evl_proto);
	} else {
		hdrlen = ETHER_HDR_LEN;
		etype = ntohs(evh->evl_encap_proto);
	}
	sae = (struct arphdr_ether *)(rxbuf + hdrlen);

	/* a host local VM MAC address -- need to have vxlanid */
	mac = mactou64(evh->evl_shost);
	if (mac != state->vs_prov_mac) {
		auto it_ii = intftbl.find(mac);
		if (it_ii == intftbl.end()) {
			/* request vxlanid */
			data_send_arp(mac, 0, AE_VM_VXLANID_REQUEST, state);
			return (0);
		}
	}

	mac = mactou64(evh->evl_shost);
	/* the provisioning agent responded -- should be a remote IP */
	if (mac == state->vs_prov_mac &&
		sae->ae_hdr.fields.ar_op == ntohs(ARPOP_REPLY)) {
		/* check if we have an IP -> MAC mapping */
		mac = mactou64(evh->evl_dhost);
		auto it_ii = intftbl.find(mac);
		if (it_ii == intftbl.end()) {
			/* request vxlanid */
			data_send_arp(mac, 0, AE_VM_VXLANID_REQUEST, state);
			return (0);
		}
		vxlanid = it_ii->second->ii_ent.fields.vxlanid;
		auto it_ftable = ftablemap->find(vxlanid);
		if (it_ftable == ftablemap->end()) {
			/* send request for VXLANID */
			data_send_arp(mac, 0, AE_VM_VXLANID_REQUEST, state);
			return (0);
		}
		/* XXX -- implement a reverse lookup table for forwarding table entries */
		data_send_arp(0, sae->ae_tpa, AE_REVREQUEST, state);

	}
	return (0);
}

void
netmap_enqueue(struct mbuf *m, int proto __unused)
{
	struct netmap_port *peer = (struct netmap_port *)m->__m_peer;
	path_state_t ps;
	struct nm_desc *d;
	char *txbuf;

	if (peer == NULL) {
		D("error missing peer in %p", m);
		m_freem(m);
	}
	if (peer->np_dir == AtoB)
		d = peer->np_state->vsd_state->vs_nm_egress;
	else
		d = peer->np_state->vsd_state->vs_nm_ingress;
	bzero(&ps, sizeof(path_state_t));
	if ((txbuf = get_txbuf(&ps, d)) == NULL)
		return;
	if (peer->np_dir == AtoB) {
		ps.ps_rx_len = m->m_len;
		if (vxlan_encap_v4((char *)m->m_data, txbuf, &ps, peer->np_state))
			txring_next(&ps, *(ps.ps_tx_len));
	} else {
		/* XXX --- fragmentation */
		nm_pkt_copy(m->m_data, txbuf, m->m_len);
		txring_next(&ps, m->m_len);
	}
	m_freem(m);
}

static int
ipfw_check(char *buf, uint16_t len, struct netmap_port *src, struct netmap_port *dst,
	struct ip_fw_chain *chain)
{
	struct mbuf m0, *mp;

	mp = &m0;
	m0.m_flags = M_STACK;
	m0.__m_extbuf = buf;
	m0.__m_extlen = len;
	m0.__m_peer = dst;
	m0.__m_callback = netmap_enqueue;
	m0.m_pkthdr.rcvif = src->np_ifp;
	m0.m_data = buf;
	m0.m_len = m0.m_pkthdr.len = len;
	ipfw_check_frame(NULL, &mp, NULL, PFIL_IN, NULL, chain);
	if (mp == NULL || m0.__m_peer != dst)
		return (0);
	return (1);
}

/*
 * If valid, encapsulate rxbuf in to txbuf
 *
 */
int
vxlan_encap_v4(char *rxbuf, char *txbuf, path_state_t *ps,
			   vxstate_dp_t *dp_state)
{
	struct ether_vlan_header *evh;
	int hdrlen, etype;
	vfe_t *vfe;
	vxstate_t *state = dp_state->vsd_state;
	intf_info_map_t &intftbl = state->vs_intf_table;
	ftablemap_t *ftablemap = &state->vs_ftables;
	rte_t *rte = &state->vs_dflt_rte;
	arp_t *l2tbl = &state->vs_l2_phys.l2t_v4;
	struct egress_cache ec;
	struct ip_fw_chain *chain;
	uint64_t srcmac, dstmac, targetha;
	uint16_t sport, pktsize;
	uint32_t vxlanid, laddr, raddr, maskraddr, maskladdr, range;

	evh = (struct ether_vlan_header *)(rxbuf);
	srcmac = mactou64(evh->evl_shost);
	dstmac = mactou64(evh->evl_dhost);
	/* ignore our own traffic */
	if (__predict_false(srcmac == state->vs_ctrl_mac))
		return (0);
	if (dp_state->vsd_ecache.ec_smac == srcmac &&
		dp_state->vsd_ecache.ec_dmac == dstmac) {
		chain = dp_state->vsd_ecache.ec_chain;
		if (chain != NULL) {
			/* XXX do ipfw_check on packet */
			if (ipfw_check(rxbuf, ps->ps_rx_len, &dp_state->vsd_ingress_port,
						   &dp_state->vsd_egress_port, chain) == 0)
				return (0);
		}
		/* XXX VLAN only */
		memcpy(txbuf, &dp_state->vsd_ecache.ec_hdr.vh, sizeof(struct vxlan_header));
		nm_pkt_copy(rxbuf, txbuf + sizeof(struct vxlan_header), ps->ps_rx_len);
		*(ps->ps_tx_len) = ps->ps_rx_len + sizeof(struct vxlan_header);
		return (1);
	}
	if (evh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		hdrlen = ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN;
		etype = ntohs(evh->evl_proto);
	} else {
		hdrlen = ETHER_HDR_LEN;
		etype = ntohs(evh->evl_encap_proto);
	}
	ec.ec_smac = srcmac;
	ec.ec_dmac = dstmac;
	ec.ec_chain = NULL;
	/* fill out final data -- XXX assume no VLAN */
	*((uint64_t *)(uintptr_t)&ec.ec_hdr.vh.vh_vxlanhdr) = 0;
	ec.ec_hdr.vh.vh_vxlanhdr.v_i = 1;
	laddr = rte->ri_laddr.in4.s_addr;

	/* first map evh->evl_shost -> vxlanid / vlanid  --- vs_vni_table */
	targetha = srcmac;
	auto it_ii = intftbl.find(targetha);
	if (it_ii != intftbl.end()) {
		vxlanid = it_ii->second->ii_ent.fields.vxlanid;
	} else {
		data_send_arp(targetha, 0, AE_VM_VXLANID_REQUEST, state);
		/* send request for VXLANID */
		return (0);
	}
	if (it_ii->second->ii_ent.fields.flags & AE_IPFW_EGRESS) {
		ec.ec_chain = it_ii->second->ii_chain;
		/* XXX pass packet to ipfw_chk */
		if (ipfw_check(rxbuf, ps->ps_rx_len, &dp_state->vsd_ingress_port,
					   &dp_state->vsd_egress_port, ec.ec_chain) == 0)
			return (0);
	}
	ec.ec_hdr.vh.vh_vxlanhdr.v_vxlanid = vxlanid;
	/* calculate source port */
	range = state->vs_max_port - state->vs_min_port + 1;
	sport = XXH32(rxbuf, ETHER_HDR_LEN, state->vs_seed) % range;
	sport += state->vs_min_port;
	pktsize = ps->ps_rx_len + sizeof(struct vxlan_header) -
		sizeof(struct ether_header) - sizeof(struct udphdr) - sizeof(struct ip);
	udp_fill((struct udphdr *)(uintptr_t)&ec.ec_hdr.vh.vh_udphdr, sport, VXLAN_DPORT, pktsize);

	/* next map evh->evl_dhost -> remote ip addr in the
	 * corresponding forwarding table - check vs_ftable
	 *
	 */
	auto it_ftable = ftablemap->find(vxlanid);
	if (it_ftable == ftablemap->end()) {
		data_send_arp(targetha, 0, AE_VM_VXLANID_REQUEST, state);
		/* send request for VXLANID */
		return (0);
	}
	targetha = mactou64(evh->evl_dhost);
	auto it_fte = it_ftable->second.find(targetha);
	if (it_fte != it_ftable->second.end()) {
		vfe = &it_fte->second;
		raddr = vfe->vfe_raddr.in4.s_addr;
	} else {
		/* send RARP for ftable entry */
		data_send_arp(targetha, 0, AE_REVREQUEST, state);
		return (0);
	}
	pktsize = ps->ps_rx_len + sizeof(struct vxlan_header) - sizeof(struct ether_header);
	ip_fill((struct ip *)(uintptr_t)&ec.ec_hdr.vh.vh_iphdr, laddr, raddr,
			pktsize, IPPROTO_UDP);

	/* ..... */
	/* next check if remote ip is on our local subnet
	 * chech address & subnet mask
	 */
	maskraddr = raddr & rte->ri_mask.in4.s_addr;
	maskladdr = laddr & rte->ri_mask.in4.s_addr;
	/* .... */
	/* if yes - lookup MAC address for peer - vs_l2_phys */
	if (maskraddr == maskladdr) {
		auto it = l2tbl->find(raddr);
		if (it == l2tbl->end()) {
			/* call ARP for L2 addr */
			data_send_arp(0, raddr, AE_REQUEST, state);
			return (0);
		}
		dstmac = it->second;
	} else {
		/* .... */
		/* if no - lookup MAC address for corresponding router
		 * vs_dflt_rte -> vs_l2_phys
		 */
		auto it = l2tbl->find(rte->ri_raddr.in4.s_addr);
		if (it == l2tbl->end()) {
			/* call ARP for L2 addr */
			data_send_arp(0, raddr, AE_REQUEST, state);
			return (0);
		}
		dstmac = it->second;
	}
	eh_fill(&ec.ec_hdr.vh.vh_ehdr, state->vs_intf_mac, dstmac, ETHERTYPE_IP);
	memcpy(&dp_state->vsd_ecache, &ec, sizeof(struct egress_cache));
	memcpy(txbuf, &ec.ec_hdr, sizeof(struct vxlan_header));
	nm_pkt_copy(rxbuf, txbuf + sizeof(struct vxlan_header), ps->ps_rx_len);


	if (!vfe->vfe_dtls) {
		*(ps->ps_tx_len) = ps->ps_rx_len + sizeof(struct vxlan_header);
		return (1);
	}
	/* call encrypted channel */
	dtls_channel_transmit(vfe->vfe_channel.get(), txbuf, state->vs_mtu /* PAD */, txbuf);
	*(ps->ps_tx_len) = state->vs_mtu;
	return (1);
}

int
dtls_decrypt_v4(char *rxbuf, char *txbuf, path_state_t *ps,
				vxstate_dp_t *dp_state)
{
	vxstate_t *state = dp_state->vsd_state;
	struct ether_header *eh = (struct ether_header *)rxbuf;
	struct ip *ip = (struct ip *)(uintptr_t)(eh + 1);
	vre_t *vre;

	auto it = state->vs_rtable.find(ip->ip_src.s_addr);
	if (it == state->vs_rtable.end())
		return (0);
	vre = &it->second;
	/* ip->ip_src -> channel */
	dtls_channel_receive(vre->vre_channel.get(), rxbuf, txbuf, ps, dp_state);
}

int
vxlan_decap_v4(char *rxbuf, char *txbuf, path_state_t *ps,
			   vxstate_dp_t *dp_state)
{
	struct vxlan_header *vh = (struct vxlan_header *)rxbuf;
	struct ip *ip = &vh->vh_iphdr;
	uint32_t rxvxlanid = vh->vh_vxlanhdr.v_vxlanid;
	struct ether_header *eh = (struct ether_header *)(rxbuf + sizeof(*vh));
	uint64_t dmac = mactou64(eh->ether_dhost);
	vxstate_t *state = dp_state->vsd_state;
	intf_info_map_t &intftbl = state->vs_intf_table;
	uint16_t pktlen;

	pktlen = min(ps->ps_rx_len,  ntohs(ip->ip_len));
	if (__predict_false(pktlen <= sizeof(struct vxlan_header)))
		return (0);

	auto it = intftbl.find(dmac);
	/* we have no knowledge of this MAC address */
	if (it == intftbl.end())
		return (0);
	/* this MAC address isn't on the VXLAN that we were addressed with */
	if (it->second->ii_ent.fields.vxlanid != rxvxlanid)
		return (0);
	if (it->second->ii_ent.fields.flags & AE_IPFW_INGRESS) {
		/* XXX call ipfw_check */
		if (ipfw_check(rxbuf, ps->ps_rx_len, &dp_state->vsd_egress_port,
					   &dp_state->vsd_ingress_port,
					   it->second->ii_chain) == 0)
			return (0);

	}
	/* copy encapsulated packet */
	pktlen -= sizeof(struct vxlan_header);
	nm_pkt_copy(rxbuf + sizeof(*vh), txbuf, pktlen);
	*(ps->ps_tx_len) = pktlen;
	return (1);
}
