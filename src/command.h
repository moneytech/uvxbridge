#ifndef _UVXCOMMAND_H_
#define _UVXCOMMAND_H_

#define	ETHERTYPE_UVXCONF		0xDECA	/* uxvxbridge config type  */

#define UVXMAGIC				0xABADCAFE

struct uvxcmd_header {
	uint32_t uh_magic;
	uint8_t	uh_op;
	uint8_t	uh_rc;
	uint16_t uh_seqno;
};

struct arp_request {
	uint32_t ar_pa;
};

struct arp_reply {
	uint32_t ar_pa;
	uint8_t ar_ha[ETHER_ADDR_LEN];
};

struct fte_request {
	uint32_t fr_vxlanid;
	uint8_t fr_ha[ETHER_ADDR_LEN];
};

struct fte_reply {
	uint32_t fr_pa;
	uint32_t fr_vxlanid;
	uint8_t fr_ha[ETHER_ADDR_LEN];
};

struct vm_intf_request {
	uint8_t vir_ha[ETHER_ADDR_LEN];
};

struct vm_intf_reply {
	uint32_t vir_vxlanid;
	uint16_t vir_vlanid;
	uint8_t	vir_ha[ETHER_ADDR_LEN];
	uint32_t vir_flags;
};

struct dtls_configure {
	struct in_addr dc_pa;
	/* TBD - key state etc */
};

struct dtls_query {
	struct in_addr dc_pa;
	/* TBD - key state etc */
};

struct ipfw_cmd {
	uint32_t ic_len;
	uint32_t ic_pad;
};

	
#define CMD_ARP_REQUEST		0x1
#define CMD_ARP_REPLY		0x2

#define CMD_FTE_REQUEST		0x3
#define CMD_FTE_REPLY		0x4

#define CMD_VM_INTF_REQUEST	0x5
#define CMD_VM_INTF_REPLY	0x6

#define CMD_DTLS_CONFIGURE	0x7
#define CMD_DTLS_QUERY		0x8

#define CMD_IPFW			0x9

void uvxcmd_fill(char *txbuf, uint64_t smac, uint64_t dmac, uint32_t op);



#endif
