/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 by Sergey Fetisov <fsenok@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "dhserver.h"
#include <stddef.h>
#include "lwip/sys.h"

/* DHCP message type */
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_DECLINE        4
#define DHCP_ACK            5
#define DHCP_NAK            6
#define DHCP_RELEASE        7
#define DHCP_INFORM         8
#define DHCP_QUARANTINE_MS  (10U * 60U * 1000U)
#define DHCP_OFFER_TIMEOUT_MS (60U * 1000U)

/* DHCP options */
enum DHCP_OPTIONS
{
	DHCP_PAD                    = 0,
	DHCP_SUBNETMASK             = 1,
	DHCP_ROUTER                 = 3,
	DHCP_DNSSERVER              = 6,
	DHCP_HOSTNAME               = 12,
	DHCP_DNSDOMAIN              = 15,
	DHCP_MTU                    = 26,
	DHCP_BROADCAST              = 28,
	DHCP_PERFORMROUTERDISC      = 31,
	DHCP_STATICROUTE            = 33,
	DHCP_NISDOMAIN              = 40,
	DHCP_NISSERVER              = 41,
	DHCP_NTPSERVER              = 42,
	DHCP_VENDOR                 = 43,
	DHCP_IPADDRESS              = 50,
	DHCP_LEASETIME              = 51,
	DHCP_OPTIONSOVERLOADED      = 52,
	DHCP_MESSAGETYPE            = 53,
	DHCP_SERVERID               = 54,
	DHCP_PARAMETERREQUESTLIST   = 55,
	DHCP_MESSAGE                = 56,
	DHCP_MAXMESSAGESIZE         = 57,
	DHCP_RENEWALTIME            = 58,
	DHCP_REBINDTIME             = 59,
	DHCP_CLASSID                = 60,
	DHCP_CLIENTID               = 61,
	DHCP_USERCLASS              = 77,  /* RFC 3004 */
	DHCP_FQDN                   = 81,
	DHCP_DNSSEARCH              = 119, /* RFC 3397 */
	DHCP_CSR                    = 121, /* RFC 3442 */
	DHCP_MSCSR                  = 249, /* MS code for RFC 3442 */
	DHCP_END                    = 255
};

typedef struct
{
    uint8_t  dp_op;           /* packet opcode type */
    uint8_t  dp_htype;        /* hardware addr type */
    uint8_t  dp_hlen;         /* hardware addr length */
    uint8_t  dp_hops;         /* gateway hops */
    uint32_t dp_xid;          /* transaction ID */
    uint16_t dp_secs;         /* seconds since boot began */
    uint16_t dp_flags;
    uint8_t  dp_ciaddr[4];    /* client IP address */
    uint8_t  dp_yiaddr[4];    /* 'your' IP address */
    uint8_t  dp_siaddr[4];    /* server IP address */
    uint8_t  dp_giaddr[4];    /* gateway IP address */
    uint8_t  dp_chaddr[16];   /* client hardware address */
    uint8_t  dp_legacy[192];
    uint8_t  dp_magic[4];
    uint8_t  dp_options[275]; /* options area */
} DHCP_TYPE;

DHCP_TYPE dhcp_data;
static struct udp_pcb *pcb = NULL;
static const dhcp_config_t *config = NULL;

char magic_cookie[] = {0x63,0x82,0x53,0x63};

static ip4_addr_t get_ip(const uint8_t *pnt)
{
  ip4_addr_t result;
  memcpy(&result, pnt, sizeof(result));
  return result;
}

static void set_ip(uint8_t *pnt, ip4_addr_t value)
{
  memcpy(pnt, &value.addr, sizeof(value.addr));
}

static dhcp_entry_t *entry_by_ip(ip4_addr_t ip)
{
	int i;
	for (i = 0; i < config->num_entry; i++)
		if (config->entries[i].addr.addr == ip.addr)
			return &config->entries[i];
	return NULL;
}

static dhcp_entry_t *entry_by_mac(uint8_t *mac)
{
	int i;
	for (i = 0; i < config->num_entry; i++)
		if (memcmp(config->entries[i].mac, mac, 6) == 0)
			return &config->entries[i];
	return NULL;
}

static dhcp_entry_t *entry_by_offer_mac(const uint8_t *mac)
{
	for (int i = 0; i < config->num_entry; i++)
		if (config->entries[i].offered &&
			memcmp(config->entries[i].offered_mac, mac, 6) == 0)
			return &config->entries[i];

	return NULL;
}

static __inline bool is_vacant(dhcp_entry_t *entry)
{
	return memcmp("\0\0\0\0\0", entry->mac, 6) == 0;
}

static dhcp_entry_t *vacant_address(void) {
	for (int i = 0; i < config->num_entry; i++)
		if (is_vacant(config->entries + i)
			&& !config->entries[i].quarantined
			&&!config->entries[i].offered)
			return config->entries + i;
	return NULL;
}

static __inline void free_entry(dhcp_entry_t *entry)
{
	memset(entry->mac, 0, sizeof(entry->mac));
	entry->lease_started = 0;

	memset(entry->offered_mac, 0, sizeof(entry->offered_mac));
	entry->offer_started = 0;
	entry->offered = false;
}

static void expire_leases(void)
{
	uint32_t now = sys_now();

	for (int i = 0; i < config->num_entry; i++)
	{
		dhcp_entry_t *entry = &config->entries[i];

		/* Expire quarantined addresses */
		if (entry->quarantined && now - entry->quarantine_started >= DHCP_QUARANTINE_MS)
		{
			entry->quarantined = false;
			entry->quarantine_started = 0;
		}

		/* Expire pending DHCP offers */
		if (entry->offered &&
			(uint32_t)(now - entry->offer_started) >= DHCP_OFFER_TIMEOUT_MS)
		{
			entry->offered = false;
			entry->offer_started = 0;
			memset(entry->offered_mac, 0, sizeof(entry->offered_mac));
		}

		/* Expire normal DHCP leases */
		if (!is_vacant(entry) && now - entry->lease_started >= entry->lease * 1000U)
			free_entry(entry);
	}
}

static uint8_t *find_dhcp_option(uint8_t *attrs, int size, uint8_t attr) {
	int i = 0;

	while (i < size) {
		uint8_t option = attrs[i];

		// PAD: one-byte option with no length field
		if (option == DHCP_PAD) {
			i++;
			continue;
		}
		if (option == DHCP_END) // END: no more options
			return NULL;
		if (i + 1 >= size) // Every other option requires a length byte
			return NULL;

		uint8_t option_length = attrs[i + 1];

		// Check that the complete option fits in the buffer
		if (option_length > size - i - 2)
			return NULL;
		if (option == attr)
			return attrs + i;

		i += option_length + 2;
	}
	return NULL;
}

static bool validate_dhcp_options(const uint8_t *attrs, int size)
{
	int i = 0;
	int message_type_count = 0;
	int requested_ip_count = 0;
	int server_id_count = 0;

	while (i < size)
	{
		uint8_t option = attrs[i];

		if (option == DHCP_PAD)
		{
			i++;
			continue;
		}

		if (option == DHCP_END)
			return true;

		if (i + 1 >= size)
			return false;

		uint8_t len = attrs[i + 1];

		if (len > size - i - 2)
			return false;

		switch (option)
		{
			case DHCP_MESSAGETYPE:
				if (++message_type_count > 1 || len != 1)
					return false;
				break;

			case DHCP_IPADDRESS:
				if (++requested_ip_count > 1 || len != 4)
					return false;
				break;

			case DHCP_SERVERID:
				if (++server_id_count > 1 || len != 4)
					return false;
				break;
		}

		i += len + 2;
	}

	/* A valid DHCP option list must terminate with END */
	return false;
}

static int fill_options(void *dest,
	uint8_t msg_type,
	const char *domain,
	ip4_addr_t dns,
	int lease_time,
	ip4_addr_t serverid,
	ip4_addr_t router,
	ip4_addr_t subnet)
{
	uint8_t *ptr = (uint8_t *)dest;
	/* ACK message type */
	*ptr++ = 53;
	*ptr++ = 1;
	*ptr++ = msg_type;

	/* dhcp server identifier */
	*ptr++ = DHCP_SERVERID;
	*ptr++ = 4;
	set_ip(ptr, serverid);
	ptr += 4;

	/* lease time */
	*ptr++ = DHCP_LEASETIME;
	*ptr++ = 4;
	*ptr++ = (lease_time >> 24) & 0xFF;
	*ptr++ = (lease_time >> 16) & 0xFF;
	*ptr++ = (lease_time >> 8) & 0xFF;
	*ptr++ = (lease_time >> 0) & 0xFF;

	/* subnet mask */
	*ptr++ = DHCP_SUBNETMASK;
	*ptr++ = 4;
	set_ip(ptr, subnet);
	ptr += 4;

	/* router */
	if (router.addr != 0)
	{
		*ptr++ = DHCP_ROUTER;
		*ptr++ = 4;
		set_ip(ptr, router);
		ptr += 4;
	}

	/* domain name */
	if (domain != NULL)
	{
		int len = strlen(domain);
		*ptr++ = DHCP_DNSDOMAIN;
		*ptr++ = len;
		memcpy(ptr, domain, len);
		ptr += len;
	}

	/* domain name server (DNS) */
	if (dns.addr != 0)
	{
		*ptr++ = DHCP_DNSSERVER;
		*ptr++ = 4;
		set_ip(ptr, dns);
		ptr += 4;
	}

	/* end */
	*ptr++ = DHCP_END;
	return ptr - (uint8_t *)dest;
}

static int fill_inform_options(
		void *dest,
		const char *domain,
		ip4_addr_t serverid,
		ip4_addr_t router,
		ip4_addr_t subnet,
		ip4_addr_t dns)
{
	uint8_t *ptr = (uint8_t *)dest;

	/* DHCP message type: ACK */
	*ptr++ = DHCP_MESSAGETYPE;
	*ptr++ = 1;
	*ptr++ = DHCP_ACK;

	/* DHCP server identifier */
	*ptr++ = DHCP_SERVERID;
	*ptr++ = 4;
	set_ip(ptr, serverid);
	ptr += 4;

	/* Subnet mask */
	*ptr++ = DHCP_SUBNETMASK;
	*ptr++ = 4;
	set_ip(ptr, subnet);
	ptr += 4;

	/* Router */
	if (router.addr != 0)
	{
		*ptr++ = DHCP_ROUTER;
		*ptr++ = 4;
		set_ip(ptr, router);
		ptr += 4;
	}

	/* Domain name */
	if (domain != NULL)
	{
		int len = strlen(domain);

		if (len > 255)
			len = 255;

		*ptr++ = DHCP_DNSDOMAIN;
		*ptr++ = (uint8_t)len;
		memcpy(ptr, domain, len);
		ptr += len;
	}

	/* DNS server */
	if (dns.addr != 0)
	{
		*ptr++ = DHCP_DNSSERVER;
		*ptr++ = 4;
		set_ip(ptr, dns);
		ptr += 4;
	}

	/* End */
	*ptr++ = DHCP_END;

	return (int)(ptr - (uint8_t *)dest);
}

/*
 * RFC 2131 Section 4.1 compliant destination address selection
 */
static ip_addr_t get_dhcp_destination(struct netif *netif, const DHCP_TYPE *dhcp,
                                const ip4_addr_t *yiaddr, bool is_nak)
{
    ip4_addr_t giaddr = get_ip(dhcp->dp_giaddr);
    ip4_addr_t ciaddr = get_ip(dhcp->dp_ciaddr);
    bool giaddr_zero = ip4_addr_isany_val(giaddr);
    bool ciaddr_zero = ip4_addr_isany_val(ciaddr);
    bool broadcast_flag = (dhcp->dp_flags & htons(0x8000)) != 0;
	ip_addr_t dest_addr;

    if (!giaddr_zero) {
        // If giaddr is not zero, send to giaddr (relay agent)
        ip_addr_set_ip4_u32(&dest_addr, giaddr.addr);
        return dest_addr;
    }

    if (is_nak) {
        //  RFC 2131: "In all cases, when 'giaddr' is zero,
        // the server broadcasts any DHCPNAK messages to 0xffffffff"
        // goto dest_broadcast;

        /* DHCPNAK must use the limited broadcast address */
        ip_addr_set_ip4_u32(&dest_addr, 0xFFFFFFFFUL);
        return dest_addr;
    }

    if (!ciaddr_zero) {
        // RFC 2131: "If the 'giaddr' field is zero and the 'ciaddr' field is nonzero,
        // then the server unicasts DHCPOFFER and DHCPACK messages to the address in 'ciaddr'"
        ip_addr_set_ip4_u32(&dest_addr, ciaddr.addr);
        return dest_addr;
    }

    if (broadcast_flag) {
        // RFC 2131: "If 'giaddr' is zero and 'ciaddr' is zero, and the broadcast bit is set,
        // then the server broadcasts DHCPOFFER and DHCPACK messages to 0xffffffff"
        goto dest_broadcast;
    }

    // RFC 2131: "If the broadcast bit is not set and 'giaddr' is zero and 'ciaddr' is zero,
    // then the server unicasts DHCPOFFER and DHCPACK messages to the client's hardware
    // address and 'yiaddr' address"
    if (yiaddr && !ip4_addr_isany(yiaddr)) {
        ip_addr_set_ip4_u32(&dest_addr, yiaddr->addr);
        // TODO: This requires ARP table manipulation to associate yiaddr with client MAC
        // For now, fall back to broadcast as this is complex to implement correctly
        goto dest_broadcast;
    }

dest_broadcast:
    ip_addr_set_ip4_u32(&dest_addr,
        ip4_addr_get_u32(netif_ip4_addr(netif)) | ~ip4_addr_get_u32(netif_ip4_netmask(netif)));
    return dest_addr;

}

static void send_nak(struct netif *netif, struct udp_pcb *upcb, u16_t port)
{
	struct pbuf *pp;
	ip_addr_t dest_addr;

	/* Prepare BOOTP reply */
	dhcp_data.dp_op = 2;
	dhcp_data.dp_secs = 0;
	memset(dhcp_data.dp_yiaddr, 0, 4);
	memset(dhcp_data.dp_ciaddr, 0, 4);
	memcpy(dhcp_data.dp_magic, magic_cookie, 4);

	/* DHCP message type: NAK */
	memset(dhcp_data.dp_options, 0, sizeof(dhcp_data.dp_options));

	ptrdiff_t pos = 0;
	uint8_t *opt = dhcp_data.dp_options;

	opt[pos++] = DHCP_MESSAGETYPE;
	opt[pos++] = 1;
	opt[pos++] = DHCP_NAK;

	opt[pos++] = DHCP_SERVERID;
	opt[pos++] = 4;
	set_ip(&opt[pos], *netif_ip4_addr(netif));
	pos += 4;

	opt[pos] = DHCP_END;

	pp = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_data), PBUF_POOL);
	if (pp == NULL)
		return;

	memcpy(pp->payload, &dhcp_data, sizeof(dhcp_data));

	dest_addr = get_dhcp_destination(netif, &dhcp_data, NULL, true);

	udp_sendto(upcb, pp, &dest_addr, port);

	pbuf_free(pp);
}

static void udp_recv_proc(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
	uint8_t *ptr;
	dhcp_entry_t *entry;
	dhcp_entry_t *old_entry;
	struct pbuf *pp;
	struct netif *netif;
	ip_addr_t dest_addr;
	ip4_addr_t requested_ip;

	u16_t packet_len;
	u16_t header_len = (u16_t)offsetof(DHCP_TYPE, dp_options);
	u16_t options_len;

	(void)arg;
	(void)addr;

	if (p == NULL)
		return;

	packet_len = p->tot_len;

	// A DHCP packet must contain the fixed header and message type option.
	if (packet_len < header_len + 3 || packet_len > sizeof(dhcp_data))
	{
		pbuf_free(p);
		return;
	}

	// Copy the complete packet, including any additional pbuf segments.
	memset(&dhcp_data, 0, sizeof(dhcp_data));

	if (pbuf_copy_partial(p, &dhcp_data, packet_len, 0) != packet_len)
	{
		pbuf_free(p);
		return;
	}

	options_len = packet_len - header_len;

	/* Validate the incoming BOOTP/DHCP request */
	if (dhcp_data.dp_op != 1 ||       /* BOOTREQUEST */
		dhcp_data.dp_htype != 1 ||    /* Ethernet */
		dhcp_data.dp_hlen != 6 ||     /* MAC address length */
		memcmp(dhcp_data.dp_magic, magic_cookie, sizeof(magic_cookie)) != 0)
	{
		pbuf_free(p);
		return;
	}

	netif = netif_get_by_index(p->if_idx);

	if (netif == NULL)
	{
		pbuf_free(p);
		return;
	}

	/* Release expired DHCP leases */
	expire_leases();

	if (!validate_dhcp_options(dhcp_data.dp_options, options_len))
	{
		pbuf_free(p);
		return;
	}
	ptr = find_dhcp_option(
		dhcp_data.dp_options,
		options_len,
		DHCP_MESSAGETYPE
	);

	if (ptr == NULL)
	{
		pbuf_free(p);
		return;
	}

	/* DHCP message type must contain exactly one byte */
	if (ptr[1] != 1)
	{
		pbuf_free(p);
		return;
	}

	switch (ptr[2]) {

		case DHCP_DISCOVER:
			/* Reuse an existing lease or pending offer */
			entry = entry_by_mac(dhcp_data.dp_chaddr);

			if (entry == NULL)
				entry = entry_by_offer_mac(dhcp_data.dp_chaddr);

			/* Otherwise select a new available address */
			if (entry == NULL)
				entry = vacant_address();

			if (entry == NULL)
				break;

			dhcp_data.dp_op = 2;
			dhcp_data.dp_secs = 0;
			set_ip(dhcp_data.dp_yiaddr, entry->addr);
			memcpy(dhcp_data.dp_magic, magic_cookie, 4);

			memset(dhcp_data.dp_options, 0,
			       sizeof(dhcp_data.dp_options));

			fill_options(
				dhcp_data.dp_options,
				DHCP_OFFER,
				config->domain,
				config->dns,
				entry->lease,
				*netif_ip4_addr(netif),
				config->router,
				*netif_ip4_netmask(netif)
			);

			pp = pbuf_alloc(
				PBUF_TRANSPORT,
				sizeof(dhcp_data),
				PBUF_POOL
			);

			if (pp == NULL)
				break;

			memcpy(pp->payload, &dhcp_data, sizeof(dhcp_data));

			dest_addr = get_dhcp_destination(
				netif,
				&dhcp_data,
				&entry->addr,
				false
			);

			/* Reserve the address only if the OFFER was sent */
			if (udp_sendto(upcb, pp, &dest_addr, port) == ERR_OK) {
				if (is_vacant(entry)) {
					memcpy(entry->offered_mac,
					       dhcp_data.dp_chaddr, 6);

					entry->offered = true;
					entry->offer_started = sys_now();
				}
			}

			pbuf_free(pp);
			break;

		case DHCP_REQUEST: {
			/* Ignore DHCPREQUEST messages selecting another DHCP server */
			ptr = find_dhcp_option(
				dhcp_data.dp_options,
				options_len,
				DHCP_SERVERID
			);

			if (ptr != NULL) {
				if (ptr[1] != 4)
					break;

				if (memcmp(ptr + 2, netif_ip4_addr(netif), 4) != 0)
					break;
			}

			/* 1. Find the client's existing lease */
			old_entry = entry_by_mac(dhcp_data.dp_chaddr);

			/* 2. Determine the requested IP address */
			requested_ip = get_ip(dhcp_data.dp_ciaddr);

			if (ip4_addr_isany_val(requested_ip)) {
				/* Initial allocation or INIT-REBOOT: use option 50 */
				ptr = find_dhcp_option(
					dhcp_data.dp_options,
					options_len,
					DHCP_IPADDRESS
				);

				if (ptr == NULL) break;
				if (ptr[1] != 4) break;

				requested_ip = get_ip(ptr + 2);
			} else {
				/* Renewal/rebinding: client uses ciaddr */
				if (old_entry == NULL)
					break;

				if (old_entry->addr.addr != requested_ip.addr)
					break;
			}

			entry = entry_by_ip(requested_ip);

			if (entry == NULL) {
				/* NAK initial requests for addresses outside our pool */
				if (ip4_addr_isany_val(get_ip(dhcp_data.dp_ciaddr)))
					send_nak(netif, upcb, port);

				break;
			}

			/* Don't allow another client to claim a reserved offer */
			if (entry->offered &&
				memcmp(entry->offered_mac, dhcp_data.dp_chaddr, 6) != 0)
			{
				if (ip4_addr_isany_val(get_ip(dhcp_data.dp_ciaddr)))
					send_nak(netif, upcb, port);

				break;
			}

			/* Don't take another client's address */
			if (!is_vacant(entry) && entry != old_entry) {
				/* NAK initial requests for addresses assigned to another client */
				if (ip4_addr_isany_val(get_ip(dhcp_data.dp_ciaddr)))
					send_nak(netif, upcb, port);

				break;
			}

			/* 5. Prepare DHCP ACK */
			set_ip(dhcp_data.dp_yiaddr, requested_ip);
			dhcp_data.dp_op = 2;

			dhcp_data.dp_secs = 0;
			memcpy(dhcp_data.dp_magic, magic_cookie, 4);

			memset(dhcp_data.dp_options, 0, sizeof(dhcp_data.dp_options));

			fill_options(
				dhcp_data.dp_options,
				DHCP_ACK,
				config->domain,
				config->dns,
				entry->lease,
				*netif_ip4_addr(netif),
				config->router,
				*netif_ip4_netmask(netif)
			);

			/* 6. Allocate and send ACK */
			pp = pbuf_alloc(PBUF_TRANSPORT, sizeof(dhcp_data), PBUF_POOL);

			if (pp == NULL)
				break;

			memcpy(pp->payload, &dhcp_data, sizeof(dhcp_data));

			dest_addr = get_dhcp_destination(
				netif,
				&dhcp_data,
				&entry->addr,
				false
			);

			if (udp_sendto(upcb, pp, &dest_addr, port) == ERR_OK) {
				if (old_entry != NULL && old_entry != entry)
					free_entry(old_entry);

				memcpy(entry->mac, dhcp_data.dp_chaddr, 6);
				entry->lease_started = sys_now();

				/* Convert the temporary offer into an active lease */
				entry->offered = false;
				entry->offer_started = 0;
				memset(entry->offered_mac, 0, sizeof(entry->offered_mac));
			}

			pbuf_free(pp);
			break;
		}

		case DHCP_DECLINE: {
			/* DHCPDECLINE must identify this DHCP server */
			ptr = find_dhcp_option(
				dhcp_data.dp_options,
				options_len,
				DHCP_SERVERID
			);

			if (ptr == NULL || ptr[1] != 4)
				break;

			if (memcmp(ptr + 2, netif_ip4_addr(netif), 4) != 0)
				break;

			/* Get the declined IP address */
			ptr = find_dhcp_option(
				dhcp_data.dp_options,
				options_len,
				DHCP_IPADDRESS
			);

			if (ptr == NULL || ptr[1] != 4)
				break;

			requested_ip = get_ip(ptr + 2);

			/* Find the address in our pool */
			entry = entry_by_ip(requested_ip);

			if (entry == NULL)
				break;

			/* Don't quarantine another client's active lease */
			if (!is_vacant(entry) && memcmp(entry->mac, dhcp_data.dp_chaddr, 6) != 0)
				break;

			/* Quarantine the declined address */
			memset(entry->mac, 0, 6);
			entry->lease_started = 0;
			entry->quarantined = true;
			entry->quarantine_started = sys_now();

			break;
		}

		case DHCP_RELEASE: {
			/* Check optional DHCP server identifier */
			ptr = find_dhcp_option(
				dhcp_data.dp_options,
				options_len,
				DHCP_SERVERID
			);

			if (ptr != NULL) {
				if (ptr[1] != 4)
					break;

				if (memcmp(ptr + 2, netif_ip4_addr(netif), 4) != 0)
					break;
			}

			/* RELEASE identifies the client's address through ciaddr */
			requested_ip = get_ip(dhcp_data.dp_ciaddr);

			if (ip4_addr_isany_val(requested_ip))
				break;

			/* Find the client's existing lease */
			entry = entry_by_mac(dhcp_data.dp_chaddr);

			if (entry == NULL)
				break;

			/* Verify that the address matches the client's lease */
			if (entry->addr.addr != requested_ip.addr)
				break;

			/* Release the lease immediately */
			free_entry(entry);

			break;
		}

		case DHCP_INFORM: {
			/* DHCPINFORM requires an existing client address */
			requested_ip = get_ip(dhcp_data.dp_ciaddr);

			if (ip4_addr_isany_val(requested_ip))
				break;

			/* DHCPINFORM must not assign or renew a lease */
			dhcp_data.dp_op = 2;
			dhcp_data.dp_secs = 0;
			memset(dhcp_data.dp_yiaddr, 0, 4);
			memcpy(dhcp_data.dp_magic, magic_cookie, 4);

			memset(dhcp_data.dp_options, 0,
			       sizeof(dhcp_data.dp_options));

			fill_inform_options(
				dhcp_data.dp_options,
				config->domain,
				*netif_ip4_addr(netif),
				config->router,
				*netif_ip4_netmask(netif),
				config->dns
			);

			pp = pbuf_alloc(
				PBUF_TRANSPORT,
				sizeof(dhcp_data),
				PBUF_POOL
			);

			if (pp == NULL)
				break;

			memcpy(pp->payload, &dhcp_data, sizeof(dhcp_data));

			/* ciaddr directs the ACK to the existing client */
			dest_addr = get_dhcp_destination(
				netif,
				&dhcp_data,
				NULL,
				false
			);

			udp_sendto(upcb, pp, &dest_addr, port);
			pbuf_free(pp);

			break;
		}

		default:
			break;
	}
	pbuf_free(p);
}

err_t dhserv_init(const dhcp_config_t *c)
{
	err_t err;
	udp_init();
	dhserv_free();
	pcb = udp_new();
	if (pcb == NULL)
		return ERR_MEM;
	err = udp_bind(pcb, IP_ADDR_ANY, c->port);
	if (err != ERR_OK)
	{
		dhserv_free();
		return err;
	}
	udp_recv(pcb, udp_recv_proc, NULL);
	config = c;
	return ERR_OK;
}

void dhserv_free(void)
{
	if (pcb == NULL) return;
	udp_remove(pcb);
	pcb = NULL;
}
