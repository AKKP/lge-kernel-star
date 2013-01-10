
/*
 * DHD Protocol Module for CDC and BDC.
 *
 * Copyright (C) 1999-2010, Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 * $Id: dhd_cdc.c,v 1.22.4.2.4.7.2.12.4.35.2.14 2010/11/09 02:06:54 Exp $
 *
 * BDC is like CDC, except it includes a header for data packets to convey
 * packet priority over the bus, and flags (e.g. to indicate checksum status
 * for dongle offload).
 */

#include <typedefs.h>
#include <osl.h>

#include <bcmutils.h>
#include <bcmcdc.h>
#include <bcmendian.h>

#include <dngl_stats.h>
#include <dhd.h>
#include <dhd_proto.h>
#include <dhd_bus.h>
#include <dhd_dbg.h>


#ifdef PROP_TXSTATUS
#include <wlfc_proto.h>
#include <dhd_wlfc.h>
#endif

/* Packet alignment for most efficient SDIO (can change based on platform) */
#ifndef DHD_SDALIGN
#define DHD_SDALIGN	32
#endif
#if !ISPOWEROF2(DHD_SDALIGN)
#error DHD_SDALIGN is not a power of 2!
#endif

#define RETRIES 2		/* # of retries to retrieve matching ioctl response */
#define BUS_HEADER_LEN	(16+DHD_SDALIGN)	/* Must be atleast SDPCM_RESERVE
				 * defined in dhd_sdio.c (amount of header tha might be added)
				 * plus any space that might be needed for alignment padding.
				 */
#define ROUND_UP_MARGIN	2048 	/* Biggest SDIO block size possible for
				 * round off at the end of buffer
				 */

typedef struct dhd_prot {
	uint16 reqid;
	uint8 pending;
	uint32 lastcmd;
	uint8 bus_header[BUS_HEADER_LEN];
	cdc_ioctl_t msg;
	unsigned char buf[WLC_IOCTL_MAXLEN + ROUND_UP_MARGIN];
} dhd_prot_t;

static int
dhdcdc_msg(dhd_pub_t *dhd)
{
	dhd_prot_t *prot = dhd->prot;
	int len = ltoh32(prot->msg.len) + sizeof(cdc_ioctl_t);
	int ret;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	DHD_OS_WAKE_LOCK(dhd);

	/* NOTE : cdc->msg.len holds the desired length of the buffer to be
	 *        returned. Only up to CDC_MAX_MSG_SIZE of this buffer area
	 *	  is actually sent to the dongle
	 */
	if (len > CDC_MAX_MSG_SIZE)
		len = CDC_MAX_MSG_SIZE;

	/* Send request */
	ret = dhd_bus_txctl(dhd->bus, (uchar*)&prot->msg, len);
	DHD_OS_WAKE_UNLOCK(dhd);
	return ret;
}

static int
dhdcdc_cmplt(dhd_pub_t *dhd, uint32 id, uint32 len)
{
	int ret;
	dhd_prot_t *prot = dhd->prot;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	do {
		ret = dhd_bus_rxctl(dhd->bus, (uchar*)&prot->msg, len+sizeof(cdc_ioctl_t));
		if (ret < 0)
			break;
	} while (CDC_IOC_ID(ltoh32(prot->msg.flags)) != id);

	return ret;
}

static int
dhdcdc_query_ioctl(dhd_pub_t *dhd, int ifidx, uint cmd, void *buf, uint len, uint8 action)
{
	dhd_prot_t *prot = dhd->prot;
	cdc_ioctl_t *msg = &prot->msg;
	void *info;
	int ret = 0, retries = 0;
	uint32 id, flags = 0;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));
	DHD_CTL(("%s: cmd %d len %d\n", __FUNCTION__, cmd, len));


	/* Respond "bcmerror" and "bcmerrorstr" with local cache */
	if (cmd == WLC_GET_VAR && buf)
	{
		if (!strcmp((char *)buf, "bcmerrorstr"))
		{
			strncpy((char *)buf, bcmerrorstr(dhd->dongle_error), BCME_STRLEN);
			goto done;
		}
		else if (!strcmp((char *)buf, "bcmerror"))
		{
			*(int *)buf = dhd->dongle_error;
			goto done;
		}
	}

	memset(msg, 0, sizeof(cdc_ioctl_t));

	msg->cmd = htol32(cmd);
	msg->len = htol32(len);
	msg->flags = (++prot->reqid << CDCF_IOC_ID_SHIFT);
	CDC_SET_IF_IDX(msg, ifidx);
	/* add additional action bits */
	action &= WL_IOCTL_ACTION_MASK;
	msg->flags |= (action << CDCF_IOC_ACTION_SHIFT);
	msg->flags = htol32(msg->flags);

	if (buf)
		memcpy(prot->buf, buf, len);

	if ((ret = dhdcdc_msg(dhd)) < 0) {
		DHD_ERROR(("dhdcdc_query_ioctl: dhdcdc_msg failed w/status %d\n", ret));
		goto done;
	}

retry:
	/* wait for interrupt and get first fragment */
	if ((ret = dhdcdc_cmplt(dhd, prot->reqid, len)) < 0)
		goto done;

	flags = ltoh32(msg->flags);
	id = (flags & CDCF_IOC_ID_MASK) >> CDCF_IOC_ID_SHIFT;

	if ((id < prot->reqid) && (++retries < RETRIES))
		goto retry;
	if (id != prot->reqid) {
		DHD_ERROR(("%s: %s: unexpected request id %d (expected %d)\n",
		           dhd_ifname(dhd, ifidx), __FUNCTION__, id, prot->reqid));
		ret = -EINVAL;
		goto done;
	}

	/* Check info buffer */
	info = (void*)&msg[1];

	/* Copy info buffer */
	if (buf)
	{
		if (ret < (int)len)
			len = ret;
		memcpy(buf, info, len);
	}

	/* Check the ERROR flag */
	if (flags & CDCF_IOC_ERROR)
	{
		ret = ltoh32(msg->status);
		/* Cache error from dongle */
		dhd->dongle_error = ret;
	}

done:
	return ret;
}

static int
dhdcdc_set_ioctl(dhd_pub_t *dhd, int ifidx, uint cmd, void *buf, uint len, uint8 action)
{
	dhd_prot_t *prot = dhd->prot;
	cdc_ioctl_t *msg = &prot->msg;
	int ret = 0;
	uint32 flags, id;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));
	DHD_CTL(("%s: cmd %d len %d\n", __FUNCTION__, cmd, len));

	memset(msg, 0, sizeof(cdc_ioctl_t));

	msg->cmd = htol32(cmd);
	msg->len = htol32(len);
	msg->flags = (++prot->reqid << CDCF_IOC_ID_SHIFT);
	CDC_SET_IF_IDX(msg, ifidx);
	/* add additional action bits */
	action &= WL_IOCTL_ACTION_MASK;
	msg->flags |= (action << CDCF_IOC_ACTION_SHIFT) | CDCF_IOC_SET;
	msg->flags = htol32(msg->flags);

	if (buf)
		memcpy(prot->buf, buf, len);

	if ((ret = dhdcdc_msg(dhd)) < 0)
		goto done;

	if ((ret = dhdcdc_cmplt(dhd, prot->reqid, len)) < 0)
		goto done;

	flags = ltoh32(msg->flags);
	id = (flags & CDCF_IOC_ID_MASK) >> CDCF_IOC_ID_SHIFT;

	if (id != prot->reqid) {
		DHD_ERROR(("%s: %s: unexpected request id %d (expected %d)\n",
		           dhd_ifname(dhd, ifidx), __FUNCTION__, id, prot->reqid));
		ret = -EINVAL;
		goto done;
	}

	/* Check the ERROR flag */
	if (flags & CDCF_IOC_ERROR)
	{
		ret = ltoh32(msg->status);
		/* Cache error from dongle */
		dhd->dongle_error = ret;
	}

done:
	return ret;
}

extern int dhd_bus_interface(struct dhd_bus *bus, uint arg, void* arg2);

int
dhd_prot_ioctl(dhd_pub_t *dhd, int ifidx, wl_ioctl_t * ioc, void * buf, int len)
{
	dhd_prot_t *prot = dhd->prot;
	int ret = -1;
	uint8 action;
#ifndef DONGLEOVERLAYS
	bool acquired = FALSE;
#endif
	if (dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s : bus is down. we have nothing to do\n", __FUNCTION__));
		return ret;
	}
#ifndef DONGLEOVERLAYS
	if (dhd_os_proto_block(dhd))
		acquired = TRUE;
#endif

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ASSERT(len <= WLC_IOCTL_MAXLEN);

	if (len > WLC_IOCTL_MAXLEN)
		goto done;

	if (prot->pending == TRUE) {
		DHD_ERROR(("CDC packet is pending!!!! cmd=0x%x (%lu) lastcmd=0x%x (%lu)\n",
			ioc->cmd, (unsigned long)ioc->cmd, prot->lastcmd,
			(unsigned long)prot->lastcmd));
		if ((ioc->cmd == WLC_SET_VAR) || (ioc->cmd == WLC_GET_VAR)) {
			DHD_TRACE(("iovar cmd=%s\n", (char*)buf));
		}
		goto done;
	}

	prot->pending = TRUE;
	prot->lastcmd = ioc->cmd;
#ifdef DONGLEOVERLAYS
	action = ioc->action;
#else
	action = ioc->set;
#endif
	if (action & WL_IOCTL_ACTION_SET)
		ret = dhdcdc_set_ioctl(dhd, ifidx, ioc->cmd, buf, len, action);
	else {
		ret = dhdcdc_query_ioctl(dhd, ifidx, ioc->cmd, buf, len, action);
		if (ret > 0)
			ioc->used = ret - sizeof(cdc_ioctl_t);
	}

	/* Too many programs assume ioctl() returns 0 on success */
	if (ret >= 0)
		ret = 0;
	else {
		cdc_ioctl_t *msg = &prot->msg;
		ioc->needed = ltoh32(msg->len); /* len == needed when set/query fails from dongle */
	}

	/* Intercept the wme_dp ioctl here */
	if ((!ret) && (ioc->cmd == WLC_SET_VAR) && (!strcmp(buf, "wme_dp"))) {
		int slen, val = 0;

		slen = strlen("wme_dp") + 1;
		if (len >= (int)(slen + sizeof(int)))
			bcopy(((char *)buf + slen), &val, sizeof(int));
		dhd->wme_dp = (uint8) ltoh32(val);
	}

	prot->pending = FALSE;

done:
#ifndef DONGLEOVERLAYS
	if (acquired)
	    dhd_os_proto_unblock(dhd);
#endif /* DONGLEOVERLAYS */
	return ret;
}

int
dhd_prot_iovar_op(dhd_pub_t *dhdp, const char *name,
                  void *params, int plen, void *arg, int len, bool set)
{
	return BCME_UNSUPPORTED;
}

#ifdef PROP_TXSTATUS
void dhd_wlfc_dump(dhd_pub_t *dhdp, struct bcmstrbuf *strbuf)
{
	int i;
	uint8* ea;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhdp->wlfc_state;
	wlfc_hanger_t* h;
	wlfc_mac_descriptor_t* mac_table;
	wlfc_mac_descriptor_t* interfaces;
	char* iftypes[] = {"STA", "AP", "WDS", "p2pGO", "p2pCL"};

	if (wlfc == NULL) {
		bcm_bprintf(strbuf, "wlfc not initialized yet\n");
		return;
	}
	h = (wlfc_hanger_t*)wlfc->hanger;
	if (h == NULL) {
		bcm_bprintf(strbuf, "wlfc-hanger not initialized yet\n");
	}

	mac_table = wlfc->destination_entries.nodes;
	interfaces = wlfc->destination_entries.interfaces;
	bcm_bprintf(strbuf, "---- wlfc stats ----\n");
	if (h) {
		bcm_bprintf(strbuf, "wlfc hanger (pushed,popped,f_push,"
			"f_pop,f_slot, pending) = (%d,%d,%d,%d,%d,%d)\n",
			h->pushed,
			h->popped,
			h->failed_to_push,
			h->failed_to_pop,
			h->failed_slotfind,
			(h->pushed - h->popped));
	}

	bcm_bprintf(strbuf, "wlfc fail(tlv,credit_rqst,mac_update,psmode_update), "
		"(dq_full,sendq_full, rollback_fail) = (%d,%d,%d,%d), (%d,%d,%d)\n",
		wlfc->stats.tlv_parse_failed,
		wlfc->stats.credit_request_failed,
		wlfc->stats.mac_update_failed,
		wlfc->stats.psmode_update_failed,
		wlfc->stats.delayq_full_error,
		wlfc->stats.sendq_full_error,
		wlfc->stats.rollback_failed);

	bcm_bprintf(strbuf, "SENDQ (len,credit,sent) "
		"(AC0[%d,%d,%d],AC1[%d,%d,%d],AC2[%d,%d,%d],AC3[%d,%d,%d],BC_MC[%d,%d,%d])\n",
		wlfc->SENDQ.q[0].len, wlfc->FIFO_credit[0], wlfc->stats.sendq_pkts[0],
		wlfc->SENDQ.q[1].len, wlfc->FIFO_credit[1], wlfc->stats.sendq_pkts[1],
		wlfc->SENDQ.q[2].len, wlfc->FIFO_credit[2], wlfc->stats.sendq_pkts[2],
		wlfc->SENDQ.q[3].len, wlfc->FIFO_credit[3], wlfc->stats.sendq_pkts[3],
		wlfc->SENDQ.q[4].len, wlfc->FIFO_credit[4], wlfc->stats.sendq_pkts[4]);

#ifdef PROP_TXSTATUS_DEBUG
	bcm_bprintf(strbuf, "SENDQ dropped: AC[0-3]:(%d,%d,%d,%d), (bcmc,atim):(%d,%d)\n",
		wlfc->stats.dropped_qfull[0], wlfc->stats.dropped_qfull[1],
		wlfc->stats.dropped_qfull[2], wlfc->stats.dropped_qfull[3],
		wlfc->stats.dropped_qfull[4], wlfc->stats.dropped_qfull[5]);
#endif

	bcm_bprintf(strbuf, "\n");
	for (i = 0; i < WLFC_MAX_IFNUM; i++) {
		if (interfaces[i].occupied) {
			char* iftype_desc;

			if (interfaces[i].iftype > WLC_E_IF_ROLE_P2P_CLIENT)
				iftype_desc = "<Unknown";
			else
				iftype_desc = iftypes[interfaces[i].iftype];

			ea = interfaces[i].ea;
			bcm_bprintf(strbuf, "INTERFACE[%d].ea = "
				"[%02x:%02x:%02x:%02x:%02x:%02x], if:%d, type: %s\n", i,
				ea[0], ea[1], ea[2], ea[3], ea[4], ea[5],
				interfaces[i].interface_id,
				iftype_desc);

			bcm_bprintf(strbuf, "INTERFACE[%d].DELAYQ(len,state,credit)"
				"= (%d,%s,%d)\n",
				i,
				interfaces[i].psq.len,
				((interfaces[i].state ==
				WLFC_STATE_OPEN) ? " OPEN":"CLOSE"),
				interfaces[i].requested_credit);

			bcm_bprintf(strbuf, "INTERFACE[%d].DELAYQ"
				"(sup,ac0),(sup,ac1),(sup,ac2),(sup,ac3) = "
				"(%d,%d),(%d,%d),(%d,%d),(%d,%d)\n",
				i,
				interfaces[i].psq.q[0].len,
				interfaces[i].psq.q[1].len,
				interfaces[i].psq.q[2].len,
				interfaces[i].psq.q[3].len,
				interfaces[i].psq.q[4].len,
				interfaces[i].psq.q[5].len,
				interfaces[i].psq.q[6].len,
				interfaces[i].psq.q[7].len);
		}
	}

	bcm_bprintf(strbuf, "\n");
	for (i = 0; i < WLFC_MAC_DESC_TABLE_SIZE; i++) {
		if (mac_table[i].occupied) {
			ea = mac_table[i].ea;
			bcm_bprintf(strbuf, "MAC_table[%d].ea = "
				"[%02x:%02x:%02x:%02x:%02x:%02x], if:%d\n", i,
				ea[0], ea[1], ea[2], ea[3], ea[4], ea[5],
				mac_table[i].interface_id);

			bcm_bprintf(strbuf, "MAC_table[%d].DELAYQ(len,state,credit)"
				"= (%d,%s,%d)\n",
				i,
				mac_table[i].psq.len,
				((mac_table[i].state ==
				WLFC_STATE_OPEN) ? " OPEN":"CLOSE"),
				mac_table[i].requested_credit);
#ifdef PROP_TXSTATUS_DEBUG
			bcm_bprintf(strbuf, "MAC_table[%d]: (opened, closed) = (%d, %d)\n",
				i, mac_table[i].opened_ct, mac_table[i].closed_ct);
#endif
			bcm_bprintf(strbuf, "MAC_table[%d].DELAYQ"
				"(sup,ac0),(sup,ac1),(sup,ac2),(sup,ac3) = "
				"(%d,%d),(%d,%d),(%d,%d),(%d,%d)\n",
				i,
				mac_table[i].psq.q[0].len,
				mac_table[i].psq.q[1].len,
				mac_table[i].psq.q[2].len,
				mac_table[i].psq.q[3].len,
				mac_table[i].psq.q[4].len,
				mac_table[i].psq.q[5].len,
				mac_table[i].psq.q[6].len,
				mac_table[i].psq.q[7].len);
		}
	}

#ifdef PROP_TXSTATUS_DEBUG
	{
		int avg;
		int moving_avg = 0;
		int moving_samples;

		if (wlfc->stats.latency_sample_count) {
			moving_samples = sizeof(wlfc->stats.deltas)/sizeof(uint32);

			for (i = 0; i < moving_samples; i++)
				moving_avg += wlfc->stats.deltas[i];
			moving_avg /= moving_samples;

			avg = (100 * wlfc->stats.total_status_latency) /
				wlfc->stats.latency_sample_count;
			bcm_bprintf(strbuf, "txstatus latency (average, last, moving[%d]) = "
				"(%d.%d, %03d, %03d)\n",
				moving_samples, avg/100, (avg - (avg/100)*100),
				wlfc->stats.latency_most_recent,
				moving_avg);
		}
	}

	bcm_bprintf(strbuf, "wlfc- fifo[0-5] credit stats: sent = (%d,%d,%d,%d,%d,%d), "
		"back = (%d,%d,%d,%d,%d,%d)\n",
		wlfc->stats.fifo_credits_sent[0],
		wlfc->stats.fifo_credits_sent[1],
		wlfc->stats.fifo_credits_sent[2],
		wlfc->stats.fifo_credits_sent[3],
		wlfc->stats.fifo_credits_sent[4],
		wlfc->stats.fifo_credits_sent[5],

		wlfc->stats.fifo_credits_back[0],
		wlfc->stats.fifo_credits_back[1],
		wlfc->stats.fifo_credits_back[2],
		wlfc->stats.fifo_credits_back[3],
		wlfc->stats.fifo_credits_back[4],
		wlfc->stats.fifo_credits_back[5]);
	{
		uint32 fifo_cr_sent = 0;
		uint32 fifo_cr_acked = 0;
		uint32 request_cr_sent = 0;
		uint32 request_cr_ack = 0;
		uint32 bc_mc_cr_ack = 0;

		for (i = 0; i < sizeof(wlfc->stats.fifo_credits_sent)/sizeof(uint32); i++) {
			fifo_cr_sent += wlfc->stats.fifo_credits_sent[i];
		}

		for (i = 0; i < sizeof(wlfc->stats.fifo_credits_back)/sizeof(uint32); i++) {
			fifo_cr_acked += wlfc->stats.fifo_credits_back[i];
		}

		for (i = 0; i < WLFC_MAC_DESC_TABLE_SIZE; i++) {
			if (wlfc->destination_entries.nodes[i].occupied) {
				request_cr_sent +=
					wlfc->destination_entries.nodes[i].dstncredit_sent_packets;
			}
		}
		for (i = 0; i < WLFC_MAX_IFNUM; i++) {
			if (wlfc->destination_entries.interfaces[i].occupied) {
				request_cr_sent +=
				wlfc->destination_entries.interfaces[i].dstncredit_sent_packets;
			}
		}
		for (i = 0; i < WLFC_MAC_DESC_TABLE_SIZE; i++) {
			if (wlfc->destination_entries.nodes[i].occupied) {
				request_cr_ack +=
					wlfc->destination_entries.nodes[i].dstncredit_acks;
			}
		}
		for (i = 0; i < WLFC_MAX_IFNUM; i++) {
			if (wlfc->destination_entries.interfaces[i].occupied) {
				request_cr_ack +=
					wlfc->destination_entries.interfaces[i].dstncredit_acks;
			}
		}
		bcm_bprintf(strbuf, "wlfc- (sent, status) => pq(%d,%d), vq(%d,%d),"
			"other:%d, bc_mc:%d, signal-only, (sent,freed): (%d,%d)",
			fifo_cr_sent, fifo_cr_acked,
			request_cr_sent, request_cr_ack,
			wlfc->destination_entries.other.dstncredit_acks,
			bc_mc_cr_ack,
			wlfc->stats.signal_only_pkts_sent, wlfc->stats.signal_only_pkts_freed);
	}
#endif /* PROP_TXSTATUS_DEBUG */
	bcm_bprintf(strbuf, "\n");
	bcm_bprintf(strbuf, "wlfc- pkt((in,2bus,txstats,hdrpull),(dropped,hdr_only,wlc_tossed)"
		"(freed,free_err,rollback)) = "
		"((%d,%d,%d,%d),(%d,%d,%d),(%d,%d,%d))\n",
		wlfc->stats.pktin,
		wlfc->stats.pkt2bus,
		wlfc->stats.txstatus_in,
		wlfc->stats.dhd_hdrpulls,

		wlfc->stats.pktdropped,
		wlfc->stats.wlfc_header_only_pkt,
		wlfc->stats.wlc_tossed_pkts,

		wlfc->stats.pkt_freed,
		wlfc->stats.pkt_free_err, wlfc->stats.rollback);

	bcm_bprintf(strbuf, "wlfc- suppress((d11,wlc,err),enq(d11,wl,hq,mac?),retx(d11,wlc,hq)) = "
		"((%d,%d,%d),(%d,%d,%d,%d),(%d,%d,%d))\n",

		wlfc->stats.d11_suppress,
		wlfc->stats.wl_suppress,
		wlfc->stats.bad_suppress,

		wlfc->stats.psq_d11sup_enq,
		wlfc->stats.psq_wlsup_enq,
		wlfc->stats.psq_hostq_enq,
		wlfc->stats.mac_handle_notfound,

		wlfc->stats.psq_d11sup_retx,
		wlfc->stats.psq_wlsup_retx,
		wlfc->stats.psq_hostq_retx);
	return;
}

/* Create a place to store all packet pointers submitted to the firmware until 
	a status comes back, suppress or otherwise.

	hang-er: noun, a contrivance on which things are hung, as a hook.
*/
static void*
dhd_wlfc_hanger_create(osl_t *osh, int max_items)
{
	int i;
	wlfc_hanger_t* hanger;

	/* allow only upto a specfic size for now */
	ASSERT(max_items == WLFC_HANGER_MAXITEMS);

	hanger = (wlfc_hanger_t*)MALLOC(osh, WLFC_HANGER_SIZE(max_items));

	memset(hanger, 0, WLFC_HANGER_SIZE(max_items));
	hanger->max_items = max_items;

	for (i = 0; i < hanger->max_items; i++) {
		hanger->items[i].state = WLFC_HANGER_ITEM_STATE_FREE;
	}
	return hanger;
}

static int
dhd_wlfc_hanger_delete(osl_t *osh, void* hanger)
{
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	if (h) {
		MFREE(osh, h, WLFC_HANGER_SIZE(h->max_items));
		return BCME_OK;
	}
	return BCME_BADARG;
}

static uint16
dhd_wlfc_hanger_get_free_slot(void* hanger)
{
	int i;
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	if (h) {
		for (i = 0; i < h->max_items; i++) {
			if (h->items[i].state == WLFC_HANGER_ITEM_STATE_FREE)
				return (uint16)i;
		}
	}
	h->failed_slotfind++;
	return WLFC_HANGER_MAXITEMS;
}

static int
dhd_wlfc_hanger_pushpkt(void* hanger, void* pkt, uint32 slot_id)
{
	int rc = BCME_OK;
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	if (h && (slot_id < WLFC_HANGER_MAXITEMS)) {
		if (h->items[slot_id].state == WLFC_HANGER_ITEM_STATE_FREE) {
			h->items[slot_id].state = WLFC_HANGER_ITEM_STATE_INUSE;
			h->items[slot_id].pkt = pkt;
			h->items[slot_id].identifier = slot_id;
			h->pushed++;
		}
		else {
			h->failed_to_push++;
			rc = BCME_NOTFOUND;
		}
	}
	else
		rc = BCME_BADARG;
	return rc;
}

static int
dhd_wlfc_hanger_poppkt(void* hanger, uint32 slot_id, void** pktout, int remove_from_hanger)
{
	int rc = BCME_OK;
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	/* this packet was not pushed at the time it went to the firmware */
	if (slot_id == WLFC_HANGER_MAXITEMS)
		return BCME_NOTFOUND;

	if (h) {
		if (h->items[slot_id].state == WLFC_HANGER_ITEM_STATE_INUSE) {
			*pktout = h->items[slot_id].pkt;
			if (remove_from_hanger) {
				h->items[slot_id].state =
					WLFC_HANGER_ITEM_STATE_FREE;
				h->items[slot_id].pkt = NULL;
				h->items[slot_id].identifier = 0;
				h->popped++;
			}
		}
		else {
			h->failed_to_pop++;
			rc = BCME_NOTFOUND;
		}
	}
	else
		rc = BCME_BADARG;
	return rc;
}

static int
_dhd_wlfc_pushheader(athost_wl_status_info_t* ctx, void* p, bool tim_signal,
	uint8 tim_bmp, uint8 mac_handle, uint32 htodtag)
{
	uint32 wl_pktinfo = 0;
	uint8* wlh;
	uint8 dataOffset;
	uint8 fillers;
	uint8 tim_signal_len = 0;

	struct bdc_header *h;

	if (tim_signal) {
		tim_signal_len = 1 + 1 + WLFC_CTL_VALUE_LEN_PENDING_TRAFFIC_BMP;
	}

	/* +2 is for Type[1] and Len[1] in TLV, plus TIM signal */
	dataOffset = WLFC_CTL_VALUE_LEN_PKTTAG + 2 + tim_signal_len;
	fillers = ROUNDUP(dataOffset, 4) - dataOffset;
	dataOffset += fillers;

	PKTPUSH(ctx->osh, p, dataOffset);
	wlh = (uint8*) PKTDATA(ctx->osh, p);

	wl_pktinfo = htol32(htodtag);

	wlh[0] = WLFC_CTL_TYPE_PKTTAG;
	wlh[1] = WLFC_CTL_VALUE_LEN_PKTTAG;
	memcpy(&wlh[2], &wl_pktinfo, sizeof(uint32));

	if (tim_signal_len) {
		wlh[dataOffset - fillers - tim_signal_len ] =
			WLFC_CTL_TYPE_PENDING_TRAFFIC_BMP;
		wlh[dataOffset - fillers - tim_signal_len + 1] =
			WLFC_CTL_VALUE_LEN_PENDING_TRAFFIC_BMP;
		wlh[dataOffset - fillers - tim_signal_len + 2] = mac_handle;
		wlh[dataOffset - fillers - tim_signal_len + 3] = tim_bmp;
	}
	if (fillers)
		memset(&wlh[dataOffset - fillers], WLFC_CTL_TYPE_FILLER, fillers);

	PKTPUSH(ctx->osh, p, BDC_HEADER_LEN);
	h = (struct bdc_header *)PKTDATA(ctx->osh, p);
	h->flags = (BDC_PROTO_VER << BDC_FLAG_VER_SHIFT);
	if (PKTSUMNEEDED(p))
		h->flags |= BDC_FLAG_SUM_NEEDED;


	h->priority = (PKTPRIO(p) & BDC_PRIORITY_MASK);
	h->flags2 = 0;
	h->dataOffset = dataOffset >> 2;
	BDC_SET_IF_IDX(h, DHD_PKTTAG_IF(PKTTAG(p)));
	return BCME_OK;
}

static int
_dhd_wlfc_pullheader(athost_wl_status_info_t* ctx, void* pktbuf)
{
	struct bdc_header *h;

	if (PKTLEN(ctx->osh, pktbuf) < BDC_HEADER_LEN) {
		WLFC_DBGMESG(("%s: rx data too short (%d < %d)\n", __FUNCTION__,
		           PKTLEN(ctx->osh, pktbuf), BDC_HEADER_LEN));
		return BCME_ERROR;
	}
	h = (struct bdc_header *)PKTDATA(ctx->osh, pktbuf);

	/* pull BDC header */
	PKTPULL(ctx->osh, pktbuf, BDC_HEADER_LEN);
	/* pull wl-header */
	PKTPULL(ctx->osh, pktbuf, (h->dataOffset << 2));
	return BCME_OK;
}

static wlfc_mac_descriptor_t*
_dhd_wlfc_find_table_entry(athost_wl_status_info_t* ctx, void* p)
{
	int i;
	wlfc_mac_descriptor_t* table = ctx->destination_entries.nodes;
	uint8 ifid = DHD_PKTTAG_IF(PKTTAG(p));
	uint8* dstn = DHD_PKTTAG_DSTN(PKTTAG(p));

	/* no lookup necessary, only if this packet belongs to STA interface */
	if (((ctx->destination_entries.interfaces[ifid].iftype == WLC_E_IF_ROLE_STA) ||
		ETHER_ISMULTI(dstn) ||
		(ctx->destination_entries.interfaces[ifid].iftype == WLC_E_IF_ROLE_P2P_CLIENT)) &&
		(ctx->destination_entries.interfaces[ifid].occupied)) {
			return &ctx->destination_entries.interfaces[ifid];
	}

	for (i = 0; i < WLFC_MAC_DESC_TABLE_SIZE; i++) {
		if (table[i].occupied) {
			if (table[i].interface_id == ifid) {
				if (!memcmp(table[i].ea, dstn, ETHER_ADDR_LEN))
					return &table[i];
			}
		}
	}
	return &ctx->destination_entries.other;
}

static int
_dhd_wlfc_rollback_packet_toq(athost_wl_status_info_t* ctx,
	void* p, ewlfc_packet_state_t pkt_type, uint32 hslot)
{
	/*
	put the packet back to the head of queue

	- a packet from send-q will need to go back to send-q and not delay-q
	since that will change the order of packets.
	- suppressed packet goes back to suppress sub-queue
	- pull out the header, if new or delayed packet

	Note: hslot is used only when header removal is done.
	*/
	wlfc_mac_descriptor_t* entry;
	void* pktout;
	int rc = BCME_OK;
	int prec;

	entry = _dhd_wlfc_find_table_entry(ctx, p);
	prec = DHD_PKTTAG_FIFO(PKTTAG(p));
	if (entry != NULL) {
		if (pkt_type == eWLFC_PKTTYPE_SUPPRESSED) {
			/* wl-header is saved for suppressed packets */
			if (NULL == WLFC_PKTQ_PENQ_HEAD(&entry->psq, ((prec << 1) + 1), p)) {
				WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
				rc = BCME_ERROR;
			}
		}
		else {
			/* remove header first */
			_dhd_wlfc_pullheader(ctx, p);

			if (pkt_type == eWLFC_PKTTYPE_DELAYED) {
				/* delay-q packets are going to delay-q */
				if (NULL == WLFC_PKTQ_PENQ_HEAD(&entry->psq, (prec << 1), p)) {
					WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
					rc = BCME_ERROR;
				}
			}
			else {
				/* these are going to SENDQ */
				if (NULL == WLFC_PKTQ_PENQ_HEAD(&ctx->SENDQ, prec, p)) {
					WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
					rc = BCME_ERROR;
				}
			}
			/* free the hanger slot */
			dhd_wlfc_hanger_poppkt(ctx->hanger, hslot, &pktout, 1);

			/* decrement sequence count */
			WLFC_DECR_SEQCOUNT(entry, prec);
		}
		/*
		if this packet did not count against FIFO credit, it must have
		taken a requested_credit from the firmware (for pspoll etc.)
		*/
		if (!DHD_PKTTAG_CREDITCHECK(PKTTAG(p))) {
			entry->requested_credit++;
		}
	}
	else {
		WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		rc = BCME_ERROR;
	}
	if (rc != BCME_OK)
		ctx->stats.rollback_failed++;
	else
		ctx->stats.rollback++;

	return rc;
}

static void
_dhd_wlfc_flow_control_check(athost_wl_status_info_t* ctx, struct pktq* pq, uint8 if_id)
{
	if ((pq->len <= WLFC_FLOWCONTROL_LOWATER) && (ctx->hostif_flow_state[if_id] == ON)) {
		/* start traffic */
		ctx->hostif_flow_state[if_id] = OFF;
		/*
		WLFC_DBGMESG(("qlen:%02d, if:%02d, ->OFF, start traffic %s()\n",
		pq->len, if_id, __FUNCTION__));
		*/
		WLFC_DBGMESG(("F"));
		/* dhd_txflowcontrol(ctx->dhdp, if_id, OFF); */
		ctx->toggle_host_if = 0;
	}
	if ((pq->len >= WLFC_FLOWCONTROL_HIWATER) && (ctx->hostif_flow_state[if_id] == OFF)) {
		/* stop traffic */
		ctx->hostif_flow_state[if_id] = ON;
		/*
		WLFC_DBGMESG(("qlen:%02d, if:%02d, ->ON, stop traffic   %s()\n",
		pq->len, if_id, __FUNCTION__));
		*/
		WLFC_DBGMESG(("N"));
		/* dhd_txflowcontrol(ctx->dhdp, if_id, ON); */
		ctx->host_ifidx = if_id;
		ctx->toggle_host_if = 1;
	}
	return;
}

static int
_dhd_wlfc_send_signalonly_packet(athost_wl_status_info_t* ctx, wlfc_mac_descriptor_t* entry,
	uint8 ta_bmp)
{
	int rc = BCME_OK;
	void* p = NULL;
	int dummylen = ((dhd_pub_t *)ctx->dhdp)->hdrlen+ 12;

	/* allocate a dummy packet */
	p = PKTGET(ctx->osh, dummylen, TRUE);
	if (p) {
		PKTPULL(ctx->osh, p, dummylen);
		DHD_PKTTAG_SET_H2DTAG(PKTTAG(p), 0);
		_dhd_wlfc_pushheader(ctx, p, TRUE, ta_bmp, entry->mac_handle, 0);
		DHD_PKTTAG_SETSIGNALONLY(PKTTAG(p), 1);
#ifdef PROP_TXSTATUS_DEBUG
		ctx->stats.signal_only_pkts_sent++;
#endif
		dhd_bus_txdata(((dhd_pub_t *)ctx->dhdp)->bus, p);
	}
	else {
		DHD_ERROR(("%s: couldn't allocate new %d-byte packet\n",
		           __FUNCTION__, dummylen));
		rc = BCME_NOMEM;
	}
	return rc;
}

/* Return TRUE if traffic availability changed */
static bool
_dhd_wlfc_traffic_pending_check(athost_wl_status_info_t* ctx, wlfc_mac_descriptor_t* entry,
	int prec)
{
	bool rc = FALSE;

	if (entry->state == WLFC_STATE_CLOSE) {
		if ((pktq_plen(&entry->psq, (prec << 1)) == 0) &&
			(pktq_plen(&entry->psq, ((prec << 1) + 1)) == 0)) {

			if (entry->traffic_pending_bmp & NBITVAL(prec)) {
				rc = TRUE;
				entry->traffic_pending_bmp =
					entry->traffic_pending_bmp & ~ NBITVAL(prec);
			}
		}
		else {
			if (!(entry->traffic_pending_bmp & NBITVAL(prec))) {
				rc = TRUE;
				entry->traffic_pending_bmp =
					entry->traffic_pending_bmp | NBITVAL(prec);
			}
		}
	}
	if (rc) {
		/* request a TIM update to firmware at the next piggyback opportunity */
		if (entry->traffic_lastreported_bmp != entry->traffic_pending_bmp) {
			entry->send_tim_signal = 1;
			_dhd_wlfc_send_signalonly_packet(ctx, entry, entry->traffic_pending_bmp);
			entry->traffic_lastreported_bmp = entry->traffic_pending_bmp;
			entry->send_tim_signal = 0;
		}
		else {
			rc = FALSE;
		}
	}
	return rc;
}

static int
_dhd_wlfc_enque_suppressed(athost_wl_status_info_t* ctx, int prec, void* p)
{
	wlfc_mac_descriptor_t* entry;

	entry = _dhd_wlfc_find_table_entry(ctx, p);
	if (entry == NULL) {
		WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_NOTFOUND;
	}
	/*
	- suppressed packets go to sub_queue[2*prec + 1] AND
	- delayed packets go to sub_queue[2*prec + 0] to ensure
	order of delivery.
	*/
	if (NULL == WLFC_PKTQ_PENQ(&entry->psq, ((prec << 1) + 1), p)) {
		ctx->stats.delayq_full_error++;
		/* WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__)); */
		WLFC_DBGMESG(("s"));
		return BCME_ERROR;
	}
	/* A packet has been pushed, update traffic availability bitmap, if applicable */
	_dhd_wlfc_traffic_pending_check(ctx, entry, prec);
	_dhd_wlfc_flow_control_check(ctx, &entry->psq, DHD_PKTTAG_IF(PKTTAG(p)));
	return BCME_OK;
}

static int
_dhd_wlfc_pretx_pktprocess(athost_wl_status_info_t* ctx,
	wlfc_mac_descriptor_t* entry, void* p, int header_needed, uint32* slot)
{
	int rc = BCME_OK;
	int hslot = WLFC_HANGER_MAXITEMS;
	bool send_tim_update = FALSE;
	uint32 htod = 0;
	uint8 free_ctr;

	*slot = hslot;

	if (entry == NULL) {
		entry = _dhd_wlfc_find_table_entry(ctx, p);
	}

	if (entry == NULL) {
		WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_ERROR;
	}
	if (entry->send_tim_signal) {
		send_tim_update = TRUE;
		entry->send_tim_signal = 0;
		entry->traffic_lastreported_bmp = entry->traffic_pending_bmp;
	}
	if (header_needed) {
		hslot = dhd_wlfc_hanger_get_free_slot(ctx->hanger);
		free_ctr = WLFC_SEQCOUNT(entry, DHD_PKTTAG_FIFO(PKTTAG(p)));
		DHD_PKTTAG_SET_H2DTAG(PKTTAG(p), htod);
	}
	else {
		hslot = WLFC_PKTID_HSLOT_GET(DHD_PKTTAG_H2DTAG(PKTTAG(p)));
		free_ctr = WLFC_PKTID_FREERUNCTR_GET(DHD_PKTTAG_H2DTAG(PKTTAG(p)));
	}
	WLFC_PKTID_HSLOT_SET(htod, hslot);
	WLFC_PKTID_FREERUNCTR_SET(htod, free_ctr);
	DHD_PKTTAG_SETPKTDIR(PKTTAG(p), 1);
	WL_TXSTATUS_SET_FLAGS(htod, WLFC_PKTFLAG_PKTFROMHOST);
	WL_TXSTATUS_SET_FIFO(htod, DHD_PKTTAG_FIFO(PKTTAG(p)));
	WLFC_PKTFLAG_SET_GENERATION(htod, entry->generation);

	if (!DHD_PKTTAG_CREDITCHECK(PKTTAG(p))) {
		/*
		Indicate that this packet is being sent in response to an
		explicit request from the firmware side.
		*/
		WLFC_PKTFLAG_SET_PKTREQUESTED(htod);
	}
	else {
		WLFC_PKTFLAG_CLR_PKTREQUESTED(htod);
	}
	if (header_needed) {
		rc = _dhd_wlfc_pushheader(ctx, p, send_tim_update,
			entry->traffic_lastreported_bmp, entry->mac_handle, htod);
		if (rc == BCME_OK) {
			DHD_PKTTAG_SET_H2DTAG(PKTTAG(p), htod);
			/*
			a new header was created for this packet.
			push to hanger slot and scrub q. Since bus
			send succeeded, increment seq number as well.
			*/
			rc = dhd_wlfc_hanger_pushpkt(ctx->hanger, p, hslot);
			if (rc == BCME_OK) {
				/* increment free running sequence count */
				WLFC_INCR_SEQCOUNT(entry, DHD_PKTTAG_FIFO(PKTTAG(p)));
#ifdef PROP_TXSTATUS_DEBUG
				((wlfc_hanger_t*)(ctx->hanger))->items[hslot].push_time =
					OSL_SYSUPTIME();
#endif
			}
			else {
				WLFC_DBGMESG(("%s() hanger_pushpkt() failed, rc: %d\n",
					__FUNCTION__, rc));
			}
		}
	}
	else {
		/* remove old header */
		_dhd_wlfc_pullheader(ctx, p);

		hslot = WLFC_PKTID_HSLOT_GET(DHD_PKTTAG_H2DTAG(PKTTAG(p)));
		free_ctr = WLFC_PKTID_FREERUNCTR_GET(DHD_PKTTAG_H2DTAG(PKTTAG(p)));
		/* push new header */
		_dhd_wlfc_pushheader(ctx, p, send_tim_update,
			entry->traffic_lastreported_bmp, entry->mac_handle, htod);
	}
	*slot = hslot;
	return rc;
}

static int _dhd_wlfc_is_destination_closed(athost_wl_status_info_t* ctx,
	wlfc_mac_descriptor_t* entry, int prec)
{
	if (ctx->destination_entries.interfaces[entry->interface_id].iftype ==
		WLC_E_IF_ROLE_P2P_GO) {
		/* - destination interface is of type p2p GO.
		For a p2pGO interface, if the destination is OPEN but the interface is
		CLOSEd, do not send traffic. But if the dstn is CLOSEd while there is
		destination-specific-credit left send packets. This is because the
		firmware storing the destination-specific-requested packet in queue.
		*/
		if ((entry->state == WLFC_STATE_CLOSE) && (entry->requested_credit == 0) &&
			(entry->requested_packet == 0))
			return 1;
	}
	/* AP, p2p_go -> unicast desc entry, STA/p2p_cl -> interface desc. entry */
	if (((entry->state == WLFC_STATE_CLOSE) && (entry->requested_credit == 0) &&
		(entry->requested_packet == 0)) ||
		(!(entry->ac_bitmap & (1 << prec))))
		return 1;

	return 0;
}

static void*
_dhd_wlfc_deque_delayedq(athost_wl_status_info_t* ctx,
	int prec, uint8* ac_credit_spent, uint8* needs_hdr, wlfc_mac_descriptor_t** entry_out)
{
	wlfc_mac_descriptor_t* entry;
	wlfc_mac_descriptor_t* table;
	uint8 token_pos;
	int total_entries;
	void* p = NULL;
	int pout;
	int i;

	*entry_out = NULL;
	token_pos = ctx->token_pos[prec];
	/* most cases a packet will count against FIFO credit */
	*ac_credit_spent = 1;
	*needs_hdr = 1;

	/* search all entries, include nodes as well as interfaces */
	table = (wlfc_mac_descriptor_t*)&ctx->destination_entries;
	total_entries = sizeof(ctx->destination_entries)/sizeof(wlfc_mac_descriptor_t);

	for (i = 0; i < total_entries; i++) {
		entry = &table[(token_pos + i) % total_entries];
		if (entry->occupied) {
			if (!_dhd_wlfc_is_destination_closed(ctx, entry, prec)) {
				p = pktq_mdeq(&entry->psq,
					/* higher precedence will be picked up first,
					i.e. suppressed packets before delayed ones
					*/
					(NBITVAL((prec << 1) + 1) | NBITVAL((prec << 1))),
					&pout);
				if (p != NULL) {
					/* did the packet come from suppress sub-queue? */
					if (pout == ((prec << 1) + 1)) {
						/*
						this packet was suppressed and was sent on the bus
						previously; this already has a header
						*/
						*needs_hdr = 0;
					}
					if (entry->requested_credit > 0) {
						entry->requested_credit--;
#ifdef PROP_TXSTATUS_DEBUG
						entry->dstncredit_sent_packets++;
#endif
						/*
						if the packet was pulled out while destination is in
						closed state but had a non-zero packets requested,
						then this should not count against the FIFO credit.
						That is due to the fact that the firmware will
						most likely hold onto this packet until a suitable
						time later to push it to the appropriate  AC FIFO.
						*/
						if (entry->state == WLFC_STATE_CLOSE)
							*ac_credit_spent = 0;
					}
					else if (entry->requested_packet > 0) {
						entry->requested_packet--;
						DHD_PKTTAG_SETONETIMEPKTRQST(PKTTAG(p));
						if (entry->state == WLFC_STATE_CLOSE)
							*ac_credit_spent = 0;
					}
					/* move token to ensure fair round-robin */
					ctx->token_pos[prec] =
						(token_pos + i + 1) % total_entries;
					*entry_out = entry;
					_dhd_wlfc_flow_control_check(ctx, &entry->psq,
						DHD_PKTTAG_IF(PKTTAG(p)));
					/*
					A packet has been picked up, update traffic
					availability bitmap, if applicable
					*/
					_dhd_wlfc_traffic_pending_check(ctx, entry, prec);
					return p;
				}
			}
		}
	}
	return NULL;
}

static void*
_dhd_wlfc_deque_sendq(athost_wl_status_info_t* ctx, int prec, uint8* ac_credit_spent)
{
	wlfc_mac_descriptor_t* entry;
	void* p;

	/* most cases a packet will count against FIFO credit */
	*ac_credit_spent = 1;

	p = pktq_pdeq(&ctx->SENDQ, prec);
	if (p != NULL) {
		if (ETHER_ISMULTI(DHD_PKTTAG_DSTN(PKTTAG(p))))
			/* bc/mc packets do not have a delay queue */
			return p;

		entry = _dhd_wlfc_find_table_entry(ctx, p);

		if (entry == NULL) {
			WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
			return p;
		}

		while ((p != NULL) && _dhd_wlfc_is_destination_closed(ctx, entry, prec)) {
			/*
			- suppressed packets go to sub_queue[2*prec + 1] AND
			- delayed packets go to sub_queue[2*prec + 0] to ensure
			order of delivery.
			*/
			if (NULL == WLFC_PKTQ_PENQ(&entry->psq, (prec << 1), p)) {
				WLFC_DBGMESG(("D"));
				/* dhd_txcomplete(ctx->dhdp, p, FALSE); */
				PKTFREE(ctx->osh, p, TRUE);
				ctx->stats.delayq_full_error++;
			}
			/*
			A packet has been pushed, update traffic availability bitmap,
			if applicable
			*/
			_dhd_wlfc_traffic_pending_check(ctx, entry, prec);
			_dhd_wlfc_flow_control_check(ctx, &entry->psq, DHD_PKTTAG_IF(PKTTAG(p)));
			p = pktq_pdeq(&ctx->SENDQ, prec);
			if (p == NULL)
				break;

			entry = _dhd_wlfc_find_table_entry(ctx, p);

			if ((entry == NULL) || (ETHER_ISMULTI(DHD_PKTTAG_DSTN(PKTTAG(p))))) {
				return p;
			}
		}
		if (p) {
			if (entry->requested_packet == 0) {
				if (entry->requested_credit > 0)
					entry->requested_credit--;
			}
			else {
				entry->requested_packet--;
				DHD_PKTTAG_SETONETIMEPKTRQST(PKTTAG(p));
			}
			if (entry->state == WLFC_STATE_CLOSE)
				*ac_credit_spent = 0;
#ifdef PROP_TXSTATUS_DEBUG
			entry->dstncredit_sent_packets++;
#endif
		}
		if (p)
			_dhd_wlfc_flow_control_check(ctx, &ctx->SENDQ, DHD_PKTTAG_IF(PKTTAG(p)));
	}
	return p;
}

static int
_dhd_wlfc_mac_entry_update(athost_wl_status_info_t* ctx, wlfc_mac_descriptor_t* entry,
	ewlfc_mac_entry_action_t action, uint8 ifid, uint8 iftype, uint8* ea)
{
	int rc = BCME_OK;

	if (action == eWLFC_MAC_ENTRY_ACTION_ADD) {
		entry->occupied = 1;
		entry->state = WLFC_STATE_OPEN;
		entry->requested_credit = 0;
		entry->interface_id = ifid;
		entry->iftype = iftype;
		entry->ac_bitmap = 0xff; /* update this when handling APSD */
		/* for an interface entry we may not care about the MAC address */
		if (ea != NULL)
			memcpy(&entry->ea[0], ea, ETHER_ADDR_LEN);
		pktq_init(&entry->psq, WLFC_PSQ_PREC_COUNT, WLFC_PSQ_LEN);
	}
	else if (action == eWLFC_MAC_ENTRY_ACTION_DEL) {
		entry->occupied = 0;
		entry->state = WLFC_STATE_CLOSE;
		entry->requested_credit = 0;
		/* enable after packets are queued-deqeued properly.
		pktq_flush(dhd->osh, &entry->psq, FALSE);
		*/
	}
	return rc;
}

int
dhd_wlfc_interface_entry_update(void* state,
	ewlfc_mac_entry_action_t action, uint8 ifid, uint8 iftype, uint8* ea)
{
	athost_wl_status_info_t* ctx = (athost_wl_status_info_t*)state;
	wlfc_mac_descriptor_t* entry;

	if (ifid >= WLFC_MAX_IFNUM)
		return BCME_BADARG;

	entry = &ctx->destination_entries.interfaces[ifid];
	return _dhd_wlfc_mac_entry_update(ctx, entry, action, ifid, iftype, ea);
}

int
dhd_wlfc_FIFOcreditmap_update(void* state, uint8* credits)
{
	athost_wl_status_info_t* ctx = (athost_wl_status_info_t*)state;

	/* update the AC FIFO credit map */
	ctx->FIFO_credit[0] = credits[0];
	ctx->FIFO_credit[1] = credits[1];
	ctx->FIFO_credit[2] = credits[2];
	ctx->FIFO_credit[3] = credits[3];
	/* credit for bc/mc packets */
	ctx->FIFO_credit[4] = credits[4];
	/* credit for ATIM FIFO is not used yet. */
	return BCME_OK;
}

int
dhd_wlfc_enque_sendq(void* state, int prec, void* p)
{
	athost_wl_status_info_t* ctx = (athost_wl_status_info_t*)state;

	if ((state == NULL) ||
		/* prec = AC_COUNT is used for bc/mc queue */
		(prec > AC_COUNT) ||
		(p == NULL)) {
		WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}
	if (FALSE == dhd_prec_enq(ctx->dhdp, &ctx->SENDQ, p, prec)) {
		ctx->stats.sendq_full_error++;
		/*
		WLFC_DBGMESG(("Error: %s():%d, qlen:%d\n",
		__FUNCTION__, __LINE__, ctx->SENDQ.len));
		*/
		WLFC_HOST_FIFO_DROPPEDCTR_INC(ctx, prec);
		WLFC_DBGMESG(("Q"));
		PKTFREE(ctx->osh, p, TRUE);
		return BCME_ERROR;
	}
	ctx->stats.pktin++;
	/* _dhd_wlfc_flow_control_check(ctx, &ctx->SENDQ, DHD_PKTTAG_IF(PKTTAG(p))); */
	return BCME_OK;
}

int
dhd_wlfc_commit_packets(void* state, f_commitpkt_t fcommit, void* commit_ctx)
{
	int ac;
	int credit;
	uint8 ac_fifo_credit_spent;
	uint8 needs_hdr;
	uint32 hslot;
	void* p;
	int rc;
	athost_wl_status_info_t* ctx = (athost_wl_status_info_t*)state;
	wlfc_mac_descriptor_t* mac_entry;

	if ((state == NULL) ||
		(fcommit == NULL)) {
		WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	/* 
	Commit packets for regular AC traffic. Higher priority first.

	-NOTE:
	If the bus between the host and firmware is overwhelmed by the
	traffic from host, it is possible that higher priority traffic
	starves the lower priority queue. If that occurs often, we may
	have to employ weighted round-robin or ucode scheme to avoid
	low priority packet starvation.
	*/
	for (ac = AC_COUNT; ac >= 0; ac--) {
		for (credit = 0; credit < ctx->FIFO_credit[ac];) {
			p = _dhd_wlfc_deque_delayedq(ctx, ac, &ac_fifo_credit_spent, &needs_hdr,
				&mac_entry);
			if (p == NULL)
				break;
			/*
			if ac_fifo_credit_spent = 0

			This packet will not count against the FIFO credit.
			To ensure the txstatus corresponding to this packet
			does not provide an implied credit (default behavior)
			mark the packet accordingly.

			if ac_fifo_credit_spent = 1

			This is a normal packet and it counts against the FIFO
			credit count.
			*/
			DHD_PKTTAG_SETCREDITCHECK(PKTTAG(p), ac_fifo_credit_spent);
			rc = _dhd_wlfc_pretx_pktprocess(ctx, mac_entry, p, needs_hdr, &hslot);

			if (rc == BCME_OK)
				rc = fcommit(commit_ctx, p);
			else
				ctx->stats.generic_error++;

			if (rc == BCME_OK) {
				ctx->stats.pkt2bus++;
				if (ac_fifo_credit_spent) {
					ctx->stats.sendq_pkts[ac]++;
					WLFC_HOST_FIFO_CREDIT_INC_SENTCTRS(ctx, ac);
					/*
					1 FIFO credit has been spent by sending this packet
					to the device.
					*/
					credit++;
				}
			}
			else {
				/* bus commit has failed, rollback. */
				rc = _dhd_wlfc_rollback_packet_toq(ctx,
					p,
					/*
					- remove wl-header for a delayed packet
					- save wl-header header for suppressed packets
					*/
					(needs_hdr ? eWLFC_PKTTYPE_DELAYED :
					eWLFC_PKTTYPE_SUPPRESSED),
					hslot);
				if (rc != BCME_OK)
					ctx->stats.rollback_failed++;
			}
		}
		ctx->FIFO_credit[ac] -= credit;
		/* packets from SENDQ are fresh and they'd need header */
		needs_hdr = 1;
		for (credit = 0; credit < ctx->FIFO_credit[ac];) {
			p = _dhd_wlfc_deque_sendq(ctx, ac, &ac_fifo_credit_spent);
			if (p == NULL)
				break;

			DHD_PKTTAG_SETCREDITCHECK(PKTTAG(p), ac_fifo_credit_spent);
			rc = _dhd_wlfc_pretx_pktprocess(ctx, NULL, p, needs_hdr, &hslot);
			if (rc == BCME_OK)
				rc = fcommit(commit_ctx, p);
			else
				ctx->stats.generic_error++;

			if (rc == BCME_OK) {
				ctx->stats.pkt2bus++;
				if (ac_fifo_credit_spent) {
					WLFC_HOST_FIFO_CREDIT_INC_SENTCTRS(ctx, ac);
					ctx->stats.sendq_pkts[ac]++;
					credit++;
				}
			}
			else {
				/* bus commit has failed, rollback. */
				rc = _dhd_wlfc_rollback_packet_toq(ctx,
					p,
					/* remove wl-header while rolling back */
					eWLFC_PKTTYPE_NEW,
					hslot);
				if (rc != BCME_OK)
					ctx->stats.rollback_failed++;
			}
		}
		ctx->FIFO_credit[ac] -= credit;
	}
	return BCME_OK;
}

static uint8
dhd_wlfc_find_mac_desc_id_from_mac(dhd_pub_t *dhdp, uint8* ea)
{
	wlfc_mac_descriptor_t* table =
		((athost_wl_status_info_t*)dhdp->wlfc_state)->destination_entries.nodes;
	uint8 table_index;

	if (ea != NULL) {
		for (table_index = 0; table_index < WLFC_MAC_DESC_TABLE_SIZE; table_index++) {
			if ((0 == memcmp(ea, &table[table_index].ea[0], ETHER_ADDR_LEN)) &&
				table[table_index].occupied)
				return table_index;
		}
	}
	return WLFC_MAC_DESC_ID_INVALID;
}

void
dhd_wlfc_txcomplete(dhd_pub_t *dhd, void *txp, bool success)
{
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	void* p;

	if (DHD_PKTTAG_SIGNALONLY(PKTTAG(txp))) {
#ifdef PROP_TXSTATUS_DEBUG
		wlfc->stats.signal_only_pkts_freed++;
#endif
		/* is this a signal-only packet? */
		PKTFREE(wlfc->osh, txp, TRUE);
		return;
	}
	if (!success) {
		WLFC_DBGMESG(("At: %s():%d, bus_complete() failure for %p, htod_tag:0x%08x\n",
			__FUNCTION__, __LINE__, txp, DHD_PKTTAG_H2DTAG(PKTTAG(txp))));
		dhd_wlfc_hanger_poppkt(wlfc->hanger, WLFC_PKTID_HSLOT_GET(DHD_PKTTAG_H2DTAG
			(PKTTAG(txp))), &p, 1);

		/* indicate failure and free the packet */
		dhd_txcomplete(dhd, txp, FALSE);
		PKTFREE(wlfc->osh, txp, TRUE);

		/* return the credit, if necessary */
		if (DHD_PKTTAG_CREDITCHECK(PKTTAG(txp)))
			wlfc->FIFO_credit[DHD_PKTTAG_FIFO(PKTTAG(txp))]++;
	}
	return;
}

/* Handle discard or suppress indication */
static int
dhd_wlfc_txstatus_update(dhd_pub_t *dhd, uint8* pkt_info)
{
	uint8 	status_flag;
	uint32	status;
	int		ret;
	int		remove_from_hanger = 1;
	void*	pktbuf;
	uint8	fifo_id;
	wlfc_mac_descriptor_t* entry = NULL;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;

	memcpy(&status, pkt_info, sizeof(uint32));
	status_flag = WL_TXSTATUS_GET_FLAGS(status);
	wlfc->stats.txstatus_in++;

	if (status_flag == WLFC_CTL_PKTFLAG_DISCARD) {
		wlfc->stats.pkt_freed++;
	}

	else if (status_flag == WLFC_CTL_PKTFLAG_D11SUPPRESS) {
		wlfc->stats.d11_suppress++;
		remove_from_hanger = 0;
	}

	else if (status_flag == WLFC_CTL_PKTFLAG_WLSUPPRESS) {
		wlfc->stats.wl_suppress++;
		remove_from_hanger = 0;
	}

	else if (status_flag == WLFC_CTL_PKTFLAG_TOSSED_BYWLC) {
		wlfc->stats.wlc_tossed_pkts++;
	}

	ret = dhd_wlfc_hanger_poppkt(wlfc->hanger,
		WLFC_PKTID_HSLOT_GET(status), &pktbuf, remove_from_hanger);
	if (ret != BCME_OK) {
		/* do something */
		return ret;
	}

	if (!remove_from_hanger) {
		/* this packet was suppressed */

		entry = _dhd_wlfc_find_table_entry(wlfc, pktbuf);
		entry->generation = WLFC_PKTID_GEN(status);
	}

#ifdef PROP_TXSTATUS_DEBUG
	{
		uint32 new_t = OSL_SYSUPTIME();
		uint32 old_t;
		uint32 delta;
		old_t = ((wlfc_hanger_t*)(wlfc->hanger))->items[
			WLFC_PKTID_HSLOT_GET(status)].push_time;


		wlfc->stats.latency_sample_count++;
		if (new_t > old_t)
			delta = new_t - old_t;
		else
			delta = 0xffffffff + new_t - old_t;
		wlfc->stats.total_status_latency += delta;
		wlfc->stats.latency_most_recent = delta;

		wlfc->stats.deltas[wlfc->stats.idx_delta++] = delta;
		if (wlfc->stats.idx_delta == sizeof(wlfc->stats.deltas)/sizeof(uint32))
			wlfc->stats.idx_delta = 0;
	}
#endif /* PROP_TXSTATUS_DEBUG */

	fifo_id = DHD_PKTTAG_FIFO(PKTTAG(pktbuf));

	/* pick up the implicit credit from this packet */
	if (DHD_PKTTAG_CREDITCHECK(PKTTAG(pktbuf))) {
		if (wlfc->proptxstatus_mode == WLFC_FCMODE_IMPLIED_CREDIT) {
			wlfc->FIFO_credit[fifo_id]++;
		}
	}
	else {
		/*
		if this packet did not count against FIFO credit, it must have
		taken a requested_credit from the destination entry (for pspoll etc.)
		*/
		if (!entry) {

			entry = _dhd_wlfc_find_table_entry(wlfc, pktbuf);
		}
		if (!DHD_PKTTAG_ONETIMEPKTRQST(PKTTAG(pktbuf)))
			entry->requested_credit++;
#ifdef PROP_TXSTATUS_DEBUG
		entry->dstncredit_acks++;
#endif
	}
	if ((status_flag == WLFC_CTL_PKTFLAG_D11SUPPRESS) ||
		(status_flag == WLFC_CTL_PKTFLAG_WLSUPPRESS)) {
		ret = _dhd_wlfc_enque_suppressed(wlfc, fifo_id, pktbuf);
		if (ret != BCME_OK) {
			/* delay q is full, drop this packet */
			dhd_wlfc_hanger_poppkt(wlfc->hanger, WLFC_PKTID_HSLOT_GET(status),
			&pktbuf, 1);

			/* indicate failure and free the packet */
			dhd_txcomplete(dhd, pktbuf, FALSE);
			PKTFREE(wlfc->osh, pktbuf, TRUE);
		}
	}
	else {
		dhd_txcomplete(dhd, pktbuf, TRUE);
		/* free the packet */
		PKTFREE(wlfc->osh, pktbuf, TRUE);
	}
	return BCME_OK;
}

static int
dhd_wlfc_fifocreditback_indicate(dhd_pub_t *dhd, uint8* credits)
{
	int i;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	for (i = 0; i < WLFC_CTL_VALUE_LEN_FIFO_CREDITBACK; i++) {
#ifdef PROP_TXSTATUS_DEBUG
		wlfc->stats.fifo_credits_back[i] += credits[i];
#endif
		/* update FIFO credits */
		if (wlfc->proptxstatus_mode == WLFC_FCMODE_EXPLICIT_CREDIT)
			wlfc->FIFO_credit[i] += credits[i];
	}
	return BCME_OK;
}

static int
dhd_wlfc_rssi_indicate(dhd_pub_t *dhd, uint8* rssi)
{
	(void)dhd;
	(void)rssi;
	return BCME_OK;
}

static int
dhd_wlfc_mac_table_update(dhd_pub_t *dhd, uint8* value, uint8 type)
{
	int rc;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	uint8 existing_index;
	uint8 table_index;
	uint8 ifid;
	uint8* ea;

	WLFC_DBGMESG(("%s(), mac [%02x:%02x:%02x:%02x:%02x:%02x],%s,idx:%d,id:0x%02x\n",
		__FUNCTION__, value[2], value[3], value[4], value[5], value[6], value[7],
		((type == WLFC_CTL_TYPE_MACDESC_ADD) ? "ADD":"DEL"),
		WLFC_MAC_DESC_GET_LOOKUP_INDEX(value[0]), value[0]));

	table = wlfc->destination_entries.nodes;
	table_index = WLFC_MAC_DESC_GET_LOOKUP_INDEX(value[0]);
	ifid = value[1];
	ea = &value[2];

	if (type == WLFC_CTL_TYPE_MACDESC_ADD) {
		existing_index = dhd_wlfc_find_mac_desc_id_from_mac(dhd, &value[2]);
		if (existing_index == WLFC_MAC_DESC_ID_INVALID) {
			/* this MAC entry does not exist, create one */
			if (!table[table_index].occupied) {
				table[table_index].mac_handle = value[0];
				rc = _dhd_wlfc_mac_entry_update(wlfc, &table[table_index],
				eWLFC_MAC_ENTRY_ACTION_ADD, ifid,
				wlfc->destination_entries.interfaces[ifid].iftype,
				ea);
			}
			else {
				/* the space should have been empty, but it's not */
				wlfc->stats.mac_update_failed++;
			}
		}
		else {
			/*
			there is an existing entry, move it to new index
			if necessary.
			*/
			if (existing_index != table_index) {
				/* if we already have an entry, free the old one */
				table[existing_index].occupied = 0;
				table[existing_index].state = WLFC_STATE_CLOSE;
				table[existing_index].requested_credit = 0;
				table[existing_index].interface_id = 0;
				/* enable after packets are queued-deqeued properly.
				pktq_flush(dhd->osh, &table[existing_index].psq, FALSE);
				*/
			}
		}
	}
	if (type == WLFC_CTL_TYPE_MACDESC_DEL) {
		if (table[table_index].occupied) {
				rc = _dhd_wlfc_mac_entry_update(wlfc, &table[table_index],
					eWLFC_MAC_ENTRY_ACTION_DEL, ifid,
					wlfc->destination_entries.interfaces[ifid].iftype,
					ea);
		}
		else {
			/* the space should have been occupied, but it's not */
			wlfc->stats.mac_update_failed++;
		}
	}
	return BCME_OK;
}

static int
dhd_wlfc_psmode_update(dhd_pub_t *dhd, uint8* value, uint8 type)
{
	/* Handle PS on/off indication */
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	wlfc_mac_descriptor_t* desc;
	uint8 mac_handle = value[0];
	int i;

	table = wlfc->destination_entries.nodes;
	desc = &table[WLFC_MAC_DESC_GET_LOOKUP_INDEX(mac_handle)];
	if (desc->occupied) {
		/* a fresh PS mode should wipe old ps credits? */
		desc->requested_credit = 0;
		if (type == WLFC_CTL_TYPE_MAC_OPEN) {
			desc->state = WLFC_STATE_OPEN;
			DHD_WLFC_CTRINC_MAC_OPEN(desc);
		}
		else {
			desc->state = WLFC_STATE_CLOSE;
			DHD_WLFC_CTRINC_MAC_CLOSE(desc);
			/*
			Indicate to firmware if there is any traffic pending.
			*/
			for (i = AC_BE; i < AC_COUNT; i++) {
				_dhd_wlfc_traffic_pending_check(wlfc, desc, i);
			}
		}
	}
	else {
		wlfc->stats.psmode_update_failed++;
	}
	return BCME_OK;
}

static int
dhd_wlfc_interface_update(dhd_pub_t *dhd, uint8* value, uint8 type)
{
	/* Handle PS on/off indication */
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	uint8 if_id = value[0];

	if (if_id < WLFC_MAX_IFNUM) {
		table = wlfc->destination_entries.interfaces;
		if (table[if_id].occupied) {
			if (type == WLFC_CTL_TYPE_INTERFACE_OPEN) {
				table[if_id].state = WLFC_STATE_OPEN;
				/* WLFC_DBGMESG(("INTERFACE[%d] OPEN\n", if_id)); */
			}
			else {
				table[if_id].state = WLFC_STATE_CLOSE;
				/* WLFC_DBGMESG(("INTERFACE[%d] CLOSE\n", if_id)); */
			}
			return BCME_OK;
		}
	}
	wlfc->stats.interface_update_failed++;

	return BCME_OK;
}

static int
dhd_wlfc_credit_request(dhd_pub_t *dhd, uint8* value)
{
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	wlfc_mac_descriptor_t* desc;
	uint8 mac_handle;
	uint8 credit;

	table = wlfc->destination_entries.nodes;
	mac_handle = value[1];
	credit = value[0];

	desc = &table[WLFC_MAC_DESC_GET_LOOKUP_INDEX(mac_handle)];
	if (desc->occupied) {
		desc->requested_credit = credit;

		desc->ac_bitmap = value[2];
	}
	else {
		wlfc->stats.credit_request_failed++;
	}
	return BCME_OK;
}

static int
dhd_wlfc_packet_request(dhd_pub_t *dhd, uint8* value)
{
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	wlfc_mac_descriptor_t* desc;
	uint8 mac_handle;
	uint8 packet_count;

	table = wlfc->destination_entries.nodes;
	mac_handle = value[1];
	packet_count = value[0];

	desc = &table[WLFC_MAC_DESC_GET_LOOKUP_INDEX(mac_handle)];
	if (desc->occupied) {
		desc->requested_packet = packet_count;

		desc->ac_bitmap = value[2];
	}
	else {
		wlfc->stats.packet_request_failed++;
	}
	return BCME_OK;
}

static int
dhd_wlfc_parse_header_info(dhd_pub_t *dhd, void* pktbuf, int tlv_hdr_len)
{
	uint8 type, len;
	uint8* value;
	uint8* tmpbuf;
	uint16 remainder = tlv_hdr_len;
	uint16 processed = 0;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	tmpbuf = (uint8*)PKTDATA(dhd->osh, pktbuf);
	if (remainder) {
		while ((processed < (WLFC_MAX_PENDING_DATALEN * 2)) && (remainder > 0)) {
			type = tmpbuf[processed];
			if (type == WLFC_CTL_TYPE_FILLER) {
				remainder -= 1;
				processed += 1;
				continue;
			}

			len  = tmpbuf[processed + 1];
			value = &tmpbuf[processed + 2];

			if (remainder < (2 + len))
				break;

			remainder -= 2 + len;
			processed += 2 + len;
			if (type == WLFC_CTL_TYPE_TXSTATUS)
				dhd_wlfc_txstatus_update(dhd, value);

			else if (type == WLFC_CTL_TYPE_FIFO_CREDITBACK)
				dhd_wlfc_fifocreditback_indicate(dhd, value);

			else if (type == WLFC_CTL_TYPE_RSSI)
				dhd_wlfc_rssi_indicate(dhd, value);

			else if (type == WLFC_CTL_TYPE_MAC_REQUEST_CREDIT)
				dhd_wlfc_credit_request(dhd, value);

			else if (type == WLFC_CTL_TYPE_MAC_REQUEST_PACKET)
				dhd_wlfc_packet_request(dhd, value);

			else if ((type == WLFC_CTL_TYPE_MAC_OPEN) ||
				(type == WLFC_CTL_TYPE_MAC_CLOSE))
				dhd_wlfc_psmode_update(dhd, value, type);

			else if ((type == WLFC_CTL_TYPE_MACDESC_ADD) ||
				(type == WLFC_CTL_TYPE_MACDESC_DEL))
				dhd_wlfc_mac_table_update(dhd, value, type);

			else if ((type == WLFC_CTL_TYPE_INTERFACE_OPEN) ||
				(type == WLFC_CTL_TYPE_INTERFACE_CLOSE)) {
				dhd_wlfc_interface_update(dhd, value, type);
			}
		}
		if (remainder != 0) {
			/* trouble..., something is not right */
			wlfc->stats.tlv_parse_failed++;
		}
	}
	return BCME_OK;
}

int dhd_wlfc_init(dhd_pub_t *dhd)
{
	int i;
	athost_wl_status_info_t* wlfc;

	char iovbuf[12]; /* Room for "tlv" + '\0' + parameter */
	/* enable all signals & indicate host proptxstatus logic is active */
	uint32 tlv = WLFC_FLAGS_RSSI_SIGNALS |
		WLFC_FLAGS_XONXOFF_SIGNALS |
		WLFC_FLAGS_CREDIT_STATUS_SIGNALS |
		WLFC_FLAGS_HOST_PROPTXSTATUS_ACTIVE;

	/*
	try to enable signalling by sending "tlv" iovar. if that fails,
	fallback to no flow control? Print a message for now.
	*/

	/* enable proptxtstatus signalling by default */
	bcm_mkiovar("tlv", (char *)&tlv, 4, iovbuf, sizeof(iovbuf));
	if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0) < 0) {
		DHD_ERROR(("dhd_wlfc_init(): failed to enable bdcv2 tlv signalling\n"));
	}
	else {
		/*
		Leaving the message for now, it should be removed after a while; once
		the tlv situation is stable.
		*/
		DHD_ERROR(("dhd_wlfc_init(): successfully enabled bdcv2 tlv signalling, %d\n",
			tlv));
	}

	/* allocate space to track txstatus propagated from firmware */
	dhd->wlfc_state = MALLOC(dhd->osh, sizeof(athost_wl_status_info_t));
	if (dhd->wlfc_state == NULL)
		return BCME_NOMEM;

	/* initialize state space */
	wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	memset(wlfc, 0, sizeof(athost_wl_status_info_t));

	/* remember osh & dhdp */
	wlfc->osh = dhd->osh;
	wlfc->dhdp = dhd;

	wlfc->hanger =
		dhd_wlfc_hanger_create(dhd->osh, WLFC_HANGER_MAXITEMS);
	if (wlfc->hanger == NULL) {
		MFREE(dhd->osh, dhd->wlfc_state, sizeof(athost_wl_status_info_t));
		dhd->wlfc_state = NULL;
		return BCME_NOMEM;
	}

	/* initialize all interfaces to accept traffic */
	for (i = 0; i < WLFC_MAX_IFNUM; i++) {
		wlfc->hostif_flow_state[i] = OFF;
	}

	/* 
	create the SENDQ containing
	sub-queues for all AC precedences + 1 for bc/mc traffic
	*/
	pktq_init(&wlfc->SENDQ, (AC_COUNT + 1), WLFC_SENDQ_LEN);

	wlfc->destination_entries.other.state = WLFC_STATE_OPEN;
	/* bc/mc FIFO is always open [credit aside], i.e. b[5] */
	wlfc->destination_entries.other.ac_bitmap = 0x1f;
	wlfc->destination_entries.other.interface_id = 0;

	wlfc->proptxstatus_mode = WLFC_FCMODE_EXPLICIT_CREDIT;

	return BCME_OK;
}

/* release all packet resources */
void dhd_wlfc_cleanup(dhd_pub_t *dhd)
{
	int i;
	int total_entries;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	wlfc_hanger_t* h;

	if (dhd->wlfc_state == NULL)
		return;

	total_entries = sizeof(wlfc->destination_entries)/sizeof(wlfc_mac_descriptor_t);
	/* search all entries, include nodes as well as interfaces */
	table = (wlfc_mac_descriptor_t*)&wlfc->destination_entries;

	for (i = 0; i < total_entries; i++) {
		if (table[i].occupied) {
			if (table[i].psq.len) {
				WLFC_DBGMESG(("%s(): DELAYQ[%d].len = %d\n",
					__FUNCTION__, i, table[i].psq.len));
				/* release packets held in DELAYQ */
				pktq_flush(wlfc->osh, &table[i].psq, TRUE);
			}
			table[i].occupied = 0;
		}
	}
	/* release packets held in SENDQ */
	if (wlfc->SENDQ.len)
		pktq_flush(wlfc->osh, &wlfc->SENDQ, TRUE);
	/* any in the hanger? */
	h = (wlfc_hanger_t*)wlfc->hanger;
	for (i = 0; i < h->max_items; i++) {
		if (h->items[i].state == WLFC_HANGER_ITEM_STATE_INUSE) {
			PKTFREE(wlfc->osh, h->items[i].pkt, TRUE);
		}
	}
	return;
}

void dhd_wlfc_deinit(dhd_pub_t *dhd)
{
	/* cleanup all psq related resources */
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)
		dhd->wlfc_state;

	if (dhd->wlfc_state == NULL)
		return;

#ifdef PROP_TXSTATUS_DEBUG
	{
		int i;
		wlfc_hanger_t* h = (wlfc_hanger_t*)wlfc->hanger;
		for (i = 0; i < h->max_items; i++) {
			if (h->items[i].state == WLFC_HANGER_ITEM_STATE_INUSE) {
				WLFC_DBGMESG(("%s() pkt[%d] = 0x%p, FIFO_credit_used:%d\n",
					__FUNCTION__, i, h->items[i].pkt,
					DHD_PKTTAG_CREDITCHECK(PKTTAG(h->items[i].pkt))));
			}
		}
	}
#endif
	/* delete hanger */
	dhd_wlfc_hanger_delete(dhd->osh, wlfc->hanger);

	/* free top structure */
	MFREE(dhd->osh, dhd->wlfc_state, sizeof(athost_wl_status_info_t));
	dhd->wlfc_state = NULL;
	return;
}
#endif /* PROP_TXSTATUS */

void
dhd_prot_dump(dhd_pub_t *dhdp, struct bcmstrbuf *strbuf)
{
	bcm_bprintf(strbuf, "Protocol CDC: reqid %d\n", dhdp->prot->reqid);
#ifdef PROP_TXSTATUS
	dhd_wlfc_dump(dhdp, strbuf);
#endif
}

void
dhd_prot_hdrpush(dhd_pub_t *dhd, int ifidx, void *pktbuf)
{
#ifdef BDC
	struct bdc_header *h;
	uint8 dataOffset = 0;
#endif /* BDC */
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

#ifdef BDC
	/* Push BDC header used to convey priority for buses that don't */

	PKTPUSH(dhd->osh, pktbuf, BDC_HEADER_LEN);

	h = (struct bdc_header *)PKTDATA(dhd->osh, pktbuf);

	h->flags = (BDC_PROTO_VER << BDC_FLAG_VER_SHIFT);
	if (PKTSUMNEEDED(pktbuf))
		h->flags |= BDC_FLAG_SUM_NEEDED;


	h->priority = (PKTPRIO(pktbuf) & BDC_PRIORITY_MASK);
	h->flags2 = 0;
#endif /* BDC */
	h->dataOffset = dataOffset;
	BDC_SET_IF_IDX(h, ifidx);
}

bool
dhd_proto_fcinfo(dhd_pub_t *dhd, void *pktbuf, uint8 *fcbits)
{
#ifdef BDC
	struct bdc_header *h;

	if (PKTLEN(dhd->osh, pktbuf) < BDC_HEADER_LEN) {
		DHD_ERROR(("%s: rx data too short (%d < %d)\n",
			__FUNCTION__, PKTLEN(dhd->osh, pktbuf), BDC_HEADER_LEN));
		return BCME_ERROR;
	}

	h = (struct bdc_header *)PKTDATA(dhd->osh, pktbuf);

	*fcbits = h->priority >> BDC_PRIORITY_FC_SHIFT;
	if ((h->flags2 & BDC_FLAG2_FC_FLAG) == BDC_FLAG2_FC_FLAG)
		return TRUE;
#endif
	return FALSE;
}

int
dhd_prot_hdrpull(dhd_pub_t *dhd, int *ifidx, void *pktbuf)
{
#ifdef BDC
	struct bdc_header *h;
#endif

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

#ifdef BDC
	/* Pop BDC header used to convey priority for buses that don't */

	if (PKTLEN(dhd->osh, pktbuf) < BDC_HEADER_LEN) {
		DHD_ERROR(("%s: rx data too short (%d < %d)\n", __FUNCTION__,
		           PKTLEN(dhd->osh, pktbuf), BDC_HEADER_LEN));
		return BCME_ERROR;
	}

	h = (struct bdc_header *)PKTDATA(dhd->osh, pktbuf);

	if ((*ifidx = BDC_GET_IF_IDX(h)) >= DHD_MAX_IFS) {
		DHD_ERROR(("%s: rx data ifnum out of range (%d)\n",
		           __FUNCTION__, *ifidx));
		return BCME_ERROR;
	}

	if (((h->flags & BDC_FLAG_VER_MASK) >> BDC_FLAG_VER_SHIFT) != BDC_PROTO_VER) {
		DHD_ERROR(("%s: non-BDC packet received, flags = 0x%x\n",
		           dhd_ifname(dhd, *ifidx), h->flags));
		if (((h->flags & BDC_FLAG_VER_MASK) >> BDC_FLAG_VER_SHIFT) == BDC_PROTO_VER_1)
			h->dataOffset = 0;
		else
			return BCME_ERROR;
	}

	if (h->flags & BDC_FLAG_SUM_GOOD) {
		DHD_INFO(("%s: BDC packet received with good rx-csum, flags 0x%x\n",
		          dhd_ifname(dhd, *ifidx), h->flags));
		PKTSETSUMGOOD(pktbuf, TRUE);
	}

	PKTSETPRIO(pktbuf, (h->priority & BDC_PRIORITY_MASK));
	PKTPULL(dhd->osh, pktbuf, BDC_HEADER_LEN);
#endif /* BDC */

#ifdef PROP_TXSTATUS
	if (((athost_wl_status_info_t*)dhd->wlfc_state)->proptxstatus_mode != WLFC_FCMODE_NONE) {
		if (!DHD_PKTTAG_PKTDIR(PKTTAG(pktbuf))) {
			/*
			- parse txstatus only for packets that came from the firmware
			*/
			dhd_os_wlfc_block(dhd);
			dhd_wlfc_parse_header_info(dhd, pktbuf, (h->dataOffset << 2));
			((athost_wl_status_info_t*)dhd->wlfc_state)->stats.dhd_hdrpulls++;
			dhd_wlfc_commit_packets(dhd->wlfc_state,  (f_commitpkt_t)dhd_bus_txdata,
				dhd->bus);
			dhd_os_wlfc_unblock(dhd);
		}
	}
#endif
	PKTPULL(dhd->osh, pktbuf, (h->dataOffset << 2));
	return 0;
}

int
dhd_prot_attach(dhd_pub_t *dhd)
{
	dhd_prot_t *cdc;

#ifndef DHD_USE_STATIC_BUF
	if (!(cdc = (dhd_prot_t *)MALLOC(dhd->osh, sizeof(dhd_prot_t)))) {
		DHD_ERROR(("%s: kmalloc failed\n", __FUNCTION__));
		goto fail;
	}
#else
	if (!(cdc = (dhd_prot_t *)dhd_os_prealloc(DHD_PREALLOC_PROT, sizeof(dhd_prot_t)))) {
		DHD_ERROR(("%s: kmalloc failed\n", __FUNCTION__));
		goto fail;
	}
#endif /* DHD_USE_STATIC_BUF */
	memset(cdc, 0, sizeof(dhd_prot_t));

	/* ensure that the msg buf directly follows the cdc msg struct */
	if ((uintptr)(&cdc->msg + 1) != (uintptr)cdc->buf) {
		DHD_ERROR(("dhd_prot_t is not correctly defined\n"));
		goto fail;
	}

	dhd->prot = cdc;
#ifdef BDC
	dhd->hdrlen += BDC_HEADER_LEN;
#endif
	dhd->maxctl = WLC_IOCTL_MAXLEN + sizeof(cdc_ioctl_t) + ROUND_UP_MARGIN;
	return 0;

fail:
#ifndef DHD_USE_STATIC_BUF
	if (cdc != NULL)
		MFREE(dhd->osh, cdc, sizeof(dhd_prot_t));
#endif
	return BCME_NOMEM;
}

/* ~NOTE~ What if another thread is waiting on the semaphore?  Holding it? */
void
dhd_prot_detach(dhd_pub_t *dhd)
{
#ifdef PROP_TXSTATUS
	dhd_wlfc_deinit(dhd);
#endif
#ifndef DHD_USE_STATIC_BUF
	MFREE(dhd->osh, dhd->prot, sizeof(dhd_prot_t));
#endif
	dhd->prot = NULL;
}

void
dhd_prot_dstats(dhd_pub_t *dhd)
{
	/* No stats from dongle added yet, copy bus stats */
	dhd->dstats.tx_packets = dhd->tx_packets;
	dhd->dstats.tx_errors = dhd->tx_errors;
	dhd->dstats.rx_packets = dhd->rx_packets;
	dhd->dstats.rx_errors = dhd->rx_errors;
	dhd->dstats.rx_dropped = dhd->rx_dropped;
	dhd->dstats.multicast = dhd->rx_multicast;
	return;
}

#define htod32(i) i

#define strtoul(nptr, endptr, base) bcm_strtoul((nptr), (endptr), (base))

/* Convert user's input in hex pattern to byte-size mask */
static int
wl_pattern_atoh(char *src, char *dst)
{
	int i;
	if (strncmp(src, "0x", 2) != 0 &&
	    strncmp(src, "0X", 2) != 0) {
		printf("Mask invalid format. Needs to start with 0x\n");
		return -1;
	}
	src = src + 2; /* Skip past 0x */
	if (strlen(src) % 2 != 0) {
		printf("Mask invalid format. Needs to be of even length\n");
		return -1;
	}
	for (i = 0; *src != '\0'; i++) {
		char num[3];
		strncpy(num, src, 2);
		num[2] = '\0';
		dst[i] = (uint8)strtoul(num, NULL, 16);
		src += 2;
	}
	return i;
}

/* LGE_CHANGE_S [yoohoo@lge.com] 2009-04-03, configs */
#if defined(CONFIG_LGE_BCM432X_PATCH)
#include <linux/fs.h>
#include <linux/ctype.h>

#if 0

CONFIG FILE FORMAT
==================

AVAILABLE PARAMETERS
~~~~~~~~~~~~~~~~~~~~
+====================+=========================================================+
| VARIABLE NAME      | DESCRIPTION                                             |
+====================+=========================================================+
| btc_mode           | BTCoexist                                               |
|                    | 0: disable, 1: enable                                   |
+--------------------+---------------------------------------------------------+
| country            | Country Code                                            |
|                    | KR, EU, US or AU ...                                    |
+--------------------+---------------------------------------------------------+
| vlan_mode          | Specifies the use of 802.1Q Tags (ON, OFF, AUTO).       |
|                    | 0: off, 1: on, -1: auto                                 |
+--------------------+---------------------------------------------------------+
| mpc                | Minimum Power Consumption                               |
|                    | 0: disable, 1: enable                                   |
+--------------------+---------------------------------------------------------+
| wme                | WME QoS                                                 |
|                    | 0: disable, 1: enable                                   |
+--------------------+---------------------------------------------------------+
| wme_apsd           | WME APSD (Advanced Power Save Delivery)                 |
|                    | 0: disable, 1: enable                                   |
+--------------------+---------------------------------------------------------+
| wme_qosinfo        | Set APSD parameters on STA.                             |
|                    | - max_sp_len = number of frames per USP: 0 (all), 2, 4, |
|                    |   or 6                                                  |
|                    | - be, bk, vi, and vo = 0 to disable, 1 to enable U-APSD |
|                    |   per AC                                                |
|                    |        <max_sp_len> <be> <bk> <vi> <vo>                 |
|                    | 0x0f =      0         1    1    1    1                  |
|                    | 0x2f =      2         1    1    1    1                  |
|                    | 0x4f =      4         1    1    1    1                  |
|                    | 0x6f =      6         1    1    1    1                  |
|                    | 0x03 =      0         0    0    1    1                  |
+--------------------+---------------------------------------------------------+
| wme_auto_trigger   | 0: disable, 1: enable                                   |
+--------------------+---------------------------------------------------------+
| wme_apsd_trigger   | in msec, 0: disable                                     |
+--------------------+---------------------------------------------------------+
| roam_off           | 0: roaming on, 1: roaming off                           |
+--------------------+---------------------------------------------------------+
| roam_scan_period   | in sec                                                  |
+--------------------+---------------------------------------------------------+
| roam_delta         | in dB                                                   |
+--------------------+---------------------------------------------------------+
| roam_trigger       | in dBm                                                  |
+--------------------+---------------------------------------------------------+
| PM                 | Power Saving Mode                                       |
|                    | 0: off, 1: max, 2: fast                                 |
+--------------------+---------------------------------------------------------+
| assoc_listen       | The Listen Interval sent in association requests        |
|                    | number of beacon                                        |
+--------------------+---------------------------------------------------------+

EXAMPLE
~~~~~~~
btc_mode=1
country=AU
vlan_mode=0
mpc=1
wme=1
wme_apsd=0
wme_qosinfo=0x00
wme_auto_trigger=1
wme_apsd_trigger=0
roam_off=0
roam_scan_period=10
roam_delta=20
roam_trigger=-70
PM=2
assoc_listen=1

#endif

static int dhd_preinit_proc(dhd_pub_t *dhd, int ifidx, char *name, char *value)
{
	int var_int;

	if (!strcmp(name, "country")) {
		return dhdcdc_set_ioctl(dhd, ifidx, WLC_SET_COUNTRY,
				value, WLC_CNTRY_BUF_SZ, TRUE);
	} else if (!strcmp(name, "roam_scan_period")) {
		var_int = (int)simple_strtol(value, NULL, 0);
		return dhdcdc_set_ioctl(dhd, ifidx, WLC_SET_ROAM_SCAN_PERIOD,
				&var_int, sizeof(var_int), TRUE);
	} else if (!strcmp(name, "roam_delta") || !strcmp(name, "roam_trigger")) {
		struct {
			int val;
			int band;
		} x;
		x.val = (int)simple_strtol(value, NULL, 0);
		x.band = WLC_BAND_AUTO;
		return dhdcdc_set_ioctl(dhd, ifidx, strcmp(name, "roam_delta") ?
				WLC_SET_ROAM_TRIGGER : WLC_SET_ROAM_DELTA, &x, sizeof(x), TRUE);
	} else if (!strcmp(name, "PM")) {
		var_int = (int)simple_strtol(value, NULL, 0);
		return dhdcdc_set_ioctl(dhd, ifidx, WLC_SET_PM,
				&var_int, sizeof(var_int), TRUE);
	} else if(!strcmp(name,"cur_etheraddr")) {
		struct ether_addr ea;
		char buf[32];
		uint iovlen;
		int ret;

		bcm_ether_atoe(value, &ea);

		ret = memcmp( &ea.octet, dhd->mac.octet, ETHER_ADDR_LEN);
		if(ret == 0){
			DHD_ERROR(("%s: Same Macaddr\n",__FUNCTION__));
			return 0;
		}

		DHD_ERROR(("%s: Change Macaddr = %02X:%02X:%02X:%02X:%02X:%02X\n",__FUNCTION__,
					ea.octet[0], ea.octet[1], ea.octet[2],
					ea.octet[3], ea.octet[4], ea.octet[5]));

		iovlen = bcm_mkiovar("cur_etheraddr", (char*)&ea, ETHER_ADDR_LEN, buf, 32);

		ret = dhdcdc_set_ioctl(dhd, ifidx, WLC_SET_VAR, buf, iovlen, TRUE);
		if (ret < 0) {
			DHD_ERROR(("%s: can't set MAC address , error=%d\n", __FUNCTION__, ret));
			return ret;
		}
		else{
			memcpy(dhd->mac.octet, (void *)&ea, ETHER_ADDR_LEN);
			return ret;
		}
	} else {
		uint iovlen;
		char iovbuf[WLC_IOCTL_SMLEN];

		/* wlu_iovar_setint */
		var_int = (int)simple_strtol(value, NULL, 0);

		/* Setup timeout bcn_timeout from dhd driver 4.217.48 */
		if(!strcmp(name, "roam_off")) {
			/* Setup timeout if Beacons are lost to report link down */
			if (var_int) {
				uint bcn_timeout = 2;
				bcm_mkiovar("bcn_timeout", (char *)&bcn_timeout, 4, iovbuf, sizeof(iovbuf));
				dhdcdc_set_ioctl(dhd, 0, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE);
			}
		}
		/* Setup timeout bcm_timeout from dhd driver 4.217.48 */

		iovlen = bcm_mkiovar(name, (char *)&var_int, sizeof(var_int),
				iovbuf, sizeof(iovbuf));
		return dhdcdc_set_ioctl(dhd, ifidx, WLC_SET_VAR,
				iovbuf, iovlen, TRUE);
	}

	return 0;
}

static int dhd_preinit_config(dhd_pub_t *dhd, int ifidx)
{
	mm_segment_t old_fs;
	struct kstat stat;
	struct file *fp = NULL;
	unsigned int len;
	char *buf = NULL, *p, *name, *value;
	int ret = 0;

	if (!*config_path)
		return 0;

	old_fs = get_fs();
	set_fs(get_ds());
	if ((ret = vfs_stat(config_path, &stat))) {
		set_fs(old_fs);
		printk(KERN_ERR "%s: Failed to get information (%d)\n",
				config_path, ret);
		return ret;
	}
	set_fs(old_fs);

	if (!(buf = MALLOC(dhd->osh, stat.size + 1))) {
		printk(KERN_ERR "Failed to allocate memory %llu bytes\n", stat.size);
		return -ENOMEM;
	}

	if (!(fp = dhd_os_open_image(config_path)) ||
		(len = dhd_os_get_image_block(buf, stat.size, fp)) < 0)
		goto err;

	buf[stat.size] = '\0';
	for (p = buf; *p; p++) {
		if (isspace(*p))
			continue;
		for (name = p++; *p && !isspace(*p); p++) {
			if (*p == '=') {
				*p = '\0';
				p++;
				for (value = p; *p && !isspace(*p); p++);
				*p = '\0';
				if ((ret = dhd_preinit_proc(dhd, ifidx, name, value)) < 0)
					printk(KERN_ERR "%s: %s=%s\n",
							bcmerrorstr(ret), name, value);
				break;
			}
		}
	}
	ret = 0;

out:
	if (fp)
		dhd_os_close_image(fp);
	if (buf)
		MFREE(dhd->osh, buf, stat.size+1);
	return ret;

err:
	ret = -1;
	goto out;
}
#endif /* CONFIG_LGE_BCM432X_PATCH */
/* LGE_CHANGE_E [yoohoo@lge.com] 2009-04-03, configs */

int
dhd_preinit_ioctls(dhd_pub_t *dhd)
{
	char eventmask[WL_EVENTING_MASK_LEN];
	char iovbuf[WL_EVENTING_MASK_LEN + 12];	/*  Room for "event_msgs" + '\0' + bitvec  */
	int ret;
	uint up = 0;
	
/* LGE_CHANGE_S [yoohoo@lge.com] 2009-08-27, roam_off, PM */
#if !defined(CONFIG_LGE_BCM432X_PATCH)
#ifdef CUSTOMER_HW2
	uint roamvar = 0;
#else
	uint roamvar = 1;
#endif
	uint power_mode = PM_FAST;
#endif /* CONFIG_LGE_BCM432X_PATCH */
/* LGE_CHANGE_E [yoohoo@lge.com] 2009-08-27, roam_off, PM */
	uint32 dongle_align = DHD_SDALIGN;
	uint32 glom = 0;
 /* LGE_CHANGE_S [hayun.kim@lge.com] 2012-04-09,bcn_timeout 4->1 */
	uint bcn_timeout = 1; 
 /* LGE_CHANGE_E [hayun.kim@lge.com] 2012-04-09,bcn_timeout 4->1 */
	int arpoe = 1;
	int arp_ol = 0xb; //0xf -> 0xb
	int scan_assoc_time = 40;
	int scan_unassoc_time = 80;
	const char 				*str;
	wl_pkt_filter_t		pkt_filter;
	wl_pkt_filter_t		*pkt_filterp;
	int						buf_len;
	int						str_len;
	uint32					mask_size;
	uint32					pattern_size;
	char buf[256];
	uint filter_mode = 1;
#ifdef AP
	uint32 mpc = 0; /* Turn MPC off for AP/APSTA mode */
	uint32 apsta = 1; /* Enable APSTA mode */
#endif

	/* Get the device MAC address */
	strcpy(iovbuf, "cur_etheraddr");
	if ((ret = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, iovbuf, sizeof(iovbuf), FALSE, 0)) < 0) {
		DHD_ERROR(("%s: can't get MAC address , error=%d\n", __FUNCTION__, ret));
		return BCME_NOTUP;
	}
	memcpy(dhd->mac.octet, iovbuf, ETHER_ADDR_LEN);
	/* Set Country code */
	if (dhd->country_code[0] != 0) {
		if (dhd_wl_ioctl_cmd(dhd, WLC_SET_COUNTRY,
			dhd->country_code, sizeof(dhd->country_code), TRUE, 0) < 0) {
			DHD_ERROR(("%s: country code setting failed\n", __FUNCTION__));
		}
	}

/* LGE_CHANGE_S [yoohoo@lge.com] 2009-08-27, already PM setup is configured */
#if !defined(CONFIG_LGE_BCM432X_PATCH)
	/* Set PowerSave mode */
	dhd_wl_ioctl_cmd(dhd, WLC_SET_PM, (char *)&power_mode, sizeof(power_mode), TRUE, 0);
#endif /* CONFIG_LGE_BCM432X_PATCH */
/* LGE_CHANGE_E [yoohoo@lge.com] 2009-08-27, already PM setup is configured */

	/* Match Host and Dongle rx alignment */
	bcm_mkiovar("bus:txglomalign", (char *)&dongle_align, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

	/* disable glom option per default */
	bcm_mkiovar("bus:txglom", (char *)&glom, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
	/* Setup timeout if Beacons are lost to report link down */
	
 /* LGE_CHANGE_S [hayun.kim@lge.com] 2012-04-09,bcn_timeout 4->1 */
	bcm_mkiovar("bcn_timeout", (char *)&bcn_timeout, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
 /* LGE_CHANGE_E [hayun.kim@lge.com] 2012-04-09,bcn_timeout 4->1 */

/* LGE_CHANGE_S [yoohoo@lge.com] 2009-04-08, roam_off */
#if !defined(CONFIG_LGE_BCM432X_PATCH)
	/* Disable build-in roaming to allowed ext supplicant to take of romaing */
	bcm_mkiovar("roam_off", (char *)&roamvar, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif /* CONFIG_LGE_BCM432X_PATCH */
/* LGE_CHANGE_E [yoohoo@lge.com] 2009-04-08, roam_off */

#ifdef AP
	/* Disable build-in roaming to allowed ext supplicant to take of romaing */
	bcm_mkiovar("mpc", (char *)&mpc, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

	/* Disable build-in roaming to allowed ext supplicant to take of romaing */
	bcm_mkiovar("apsta", (char *)&apsta, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
#endif

/* LGE_CHANGE_S [yoohoo@lge.com] 2009-04-03, configs */
#if defined(CONFIG_LGE_BCM432X_PATCH)
	dhd_preinit_config(dhd, 0);
#endif /* CONFIG_LGE_BCM432X_PATCH */
/* LGE_CHANGE_E [yoohoo@lge.com] 2009-04-03, configs */
	
	/* Force STA UP */
	dhd_wl_ioctl_cmd(dhd, WLC_UP, (char *)&up, sizeof(up), TRUE, 0);

//debug
printk("%s: WLC_UP  %d\n", __FUNCTION__, up );

	/* Setup event_msgs */
	bcm_mkiovar("event_msgs", eventmask, WL_EVENTING_MASK_LEN, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, iovbuf, sizeof(iovbuf), FALSE, 0);
	bcopy(iovbuf, eventmask, WL_EVENTING_MASK_LEN);

	/* Setup event_msgs */
	setbit(eventmask, WLC_E_SET_SSID);
	setbit(eventmask, WLC_E_PRUNE);
	setbit(eventmask, WLC_E_AUTH);
	setbit(eventmask, WLC_E_REASSOC);
	setbit(eventmask, WLC_E_REASSOC_IND);
	setbit(eventmask, WLC_E_DEAUTH_IND);
	setbit(eventmask, WLC_E_DISASSOC_IND);
	setbit(eventmask, WLC_E_DISASSOC);
	setbit(eventmask, WLC_E_JOIN);
	setbit(eventmask, WLC_E_ASSOC_IND);
	setbit(eventmask, WLC_E_PSK_SUP);
	setbit(eventmask, WLC_E_LINK);
	setbit(eventmask, WLC_E_NDIS_LINK);
	setbit(eventmask, WLC_E_MIC_ERROR);
	setbit(eventmask, WLC_E_PMKID_CACHE);
	setbit(eventmask, WLC_E_TXFAIL);
	setbit(eventmask, WLC_E_JOIN_START);
	setbit(eventmask, WLC_E_SCAN_COMPLETE);
#if defined(DONGLEOVERLAYS)
	setbit(eventmask, WLC_E_OVERLAY_REQ);
#endif
	bcm_mkiovar("event_msgs", eventmask, WL_EVENTING_MASK_LEN, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

	dhd_wl_ioctl_cmd(dhd, WLC_SET_SCAN_CHANNEL_TIME, (char *)&scan_assoc_time,
		sizeof(scan_assoc_time), TRUE, 0);
	dhd_wl_ioctl_cmd(dhd, WLC_SET_SCAN_UNASSOC_TIME, (char *)&scan_unassoc_time,
		sizeof(scan_unassoc_time), TRUE, 0);

	/* Set ARP offload */
	bcm_mkiovar("arpoe", (char *)&arpoe, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
	bcm_mkiovar("arp_ol", (char *)&arp_ol, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

	/* add a default packet filter pattern */
	str = "pkt_filter_add";
	str_len = strlen(str);
	strncpy(buf, str, str_len);
	buf[ str_len ] = '\0';
	buf_len = str_len + 1;

	pkt_filterp = (wl_pkt_filter_t *) (buf + str_len + 1);

	/* Parse packet filter id. */
	pkt_filter.id = htod32(100);

	/* Parse filter polarity. */
	pkt_filter.negate_match = htod32(0);

	/* Parse filter type. */
	pkt_filter.type = htod32(0);

	/* Parse pattern filter offset. */
	pkt_filter.u.pattern.offset = htod32(0);

#if 0 // louislee 
	/* Parse pattern filter mask. */
	mask_size =	htod32(wl_pattern_atoh("0x01",
		(char *) pkt_filterp->u.pattern.mask_and_pattern));

	/* Parse pattern filter pattern. */
	pattern_size = htod32(wl_pattern_atoh("0x00",
		(char *) &pkt_filterp->u.pattern.mask_and_pattern[mask_size]));

	if (mask_size != pattern_size) {
		DHD_ERROR(("Mask and pattern not the same size\n"));
		return -EINVAL;
	}
#else
	/* Parse pattern filter mask. */
	mask_size =	htod32(wl_pattern_atoh("0xffffffffffff",
		(char *) pkt_filterp->u.pattern.mask_and_pattern));

	/* Parse pattern filter pattern. */
	pattern_size = htod32(wl_pattern_atoh("0xffffffffffff",
		(char *) &pkt_filterp->u.pattern.mask_and_pattern[mask_size]));

	if (mask_size != pattern_size) {
		DHD_ERROR(("Mask and pattern not the same size\n"));
		return -EINVAL;
	}
	
	pkt_filter.negate_match = htod32(1);
#endif
	pkt_filter.u.pattern.size_bytes = mask_size;
	buf_len += WL_PKT_FILTER_FIXED_LEN;
	buf_len += (WL_PKT_FILTER_PATTERN_FIXED_LEN + 2 * mask_size);

	/* Keep-alive attributes are set in local	variable (keep_alive_pkt), and
	** then memcpy'ed into buffer (keep_alive_pktp) since there is no
	** guarantee that the buffer is properly aligned.
	*/
	memcpy((char *)pkt_filterp, &pkt_filter,
		WL_PKT_FILTER_FIXED_LEN + WL_PKT_FILTER_PATTERN_FIXED_LEN);

	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, buf, buf_len, TRUE, 0);

	/* set mode to allow pattern */
	bcm_mkiovar("pkt_filter_mode", (char *)&filter_mode, 4, iovbuf, sizeof(iovbuf));
	dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

	return 0;
}

int
dhd_prot_init(dhd_pub_t *dhd)
{
	int ret = 0;
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));


	ret = dhd_preinit_ioctls(dhd);

	/* Always assumes wl for now */
	dhd->iswl = TRUE;

#ifdef PROP_TXSTATUS
	ret = dhd_wlfc_init(dhd);
#endif

	return ret;
}

void
dhd_prot_stop(dhd_pub_t *dhd)
{
	/* Nothing to do for CDC */
}

/* LGE_CHANGE_S, [yoohoo@lge.com], 2009-11-19, Use deepsleep instead of dhd_dev_reset when driver start or stop */
#if defined(CONFIG_LGE_BCM432X_PATCH) && defined(CONFIG_BRCM_USE_DEEPSLEEP)
extern dhd_pub_t * get_dhd_pub_from_dev(struct net_device *dev);
int dhd_deep_sleep(struct net_device *dev, int flag)
{
	dhd_pub_t *dhd_pub = get_dhd_pub_from_dev(dev);
    char iovbuf[20] = {0};
    uint powervar   = 0;

    DHD_TRACE(("%s: Enter Flag -> %d \n", __FUNCTION__, flag));
	if(dhd_pub == NULL)
		return 0;

    switch(flag) {
	case 1: /* DEEPSLEEP ON*/
		   printk(KERN_INFO "===== [WiFi] DEEP SLEEP ON =====\n");
	
		   /* Disable MPC */	
		   powervar = 0;
		   bcm_mkiovar("mpc", (char *)&powervar, 4, iovbuf, sizeof(iovbuf));
		   dhdcdc_set_ioctl(dhd_pub, 0, WLC_SET_VAR, iovbuf, sizeof(iovbuf));

		   /* Enable Deep Sleep */
		   powervar = 1;
		   bcm_mkiovar("deepsleep", (char *)&powervar, 4, iovbuf, sizeof(iovbuf));
		   dhdcdc_set_ioctl(dhd_pub, 0, WLC_SET_VAR, iovbuf, sizeof(iovbuf));
		   break;

	case 0: /*DEEPSLEEP OFF*/
		   printk(KERN_INFO "===== [WiFi] DEEP SLEEP OFF =====\n");

		   /* Disable Deep Sleep */	
		   powervar = 0;
		   bcm_mkiovar("deepsleep", (char *)&powervar, 4, iovbuf, sizeof(iovbuf));
		   dhdcdc_set_ioctl(dhd_pub, 0, WLC_SET_VAR, iovbuf, sizeof(iovbuf));

		   /* Enable MPC */
		   powervar = 1;
		   bcm_mkiovar("mpc", (char *)&powervar, 4, iovbuf, sizeof(iovbuf));
		   dhdcdc_set_ioctl(dhd_pub, 0, WLC_SET_VAR, iovbuf, sizeof(iovbuf));
		   break;

	default: 
		   printk(KERN_ERR "[%s] Invalid Input Flag (%d)",__FUNCTION__, flag);

    }

    return 0;

}
#endif /* CONFIG_LGE_BCM432X_PATCH && CONFIG_BRCM_USE_DEEPSLEEP */
/* LGE_CHANGE_E, [yoohoo@lge.com], 2009-11-19, Use deepsleep instead of dhd_dev_reset when driver start or stop */
