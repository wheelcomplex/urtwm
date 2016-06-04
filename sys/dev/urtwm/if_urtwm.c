/*	$OpenBSD: if_urtwn.c,v 1.16 2011/02/10 17:26:40 jakemsr Exp $	*/

/*-
 * Copyright (c) 2010 Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2014 Kevin Lo <kevlo@FreeBSD.org>
 * Copyright (c) 2015-2016 Andriy Voskoboinyk <avos@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * Driver for Realtek RTL8821AU.
 */
#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/linker.h>
#include <sys/firmware.h>
#include <sys/kdb.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/rman.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_regdomain.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_ratectl.h>
#ifdef	IEEE80211_SUPPORT_SUPERG
#include <net80211/ieee80211_superg.h>
#endif

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_device.h>
#include "usbdevs.h"

#include <dev/usb/usb_debug.h>

#include "if_urtwmreg.h"
#include "if_urtwmvar.h"

#ifdef USB_DEBUG
enum {
	URTWM_DEBUG_XMIT	= 0x00000001,	/* basic xmit operation */
	URTWM_DEBUG_RECV	= 0x00000002,	/* basic recv operation */
	URTWM_DEBUG_STATE	= 0x00000004,	/* 802.11 state transitions */
	URTWM_DEBUG_RA		= 0x00000008,	/* f/w rate adaptation setup */
	URTWM_DEBUG_USB		= 0x00000010,	/* usb requests */
	URTWM_DEBUG_FIRMWARE	= 0x00000020,	/* firmware(9) loading debug */
	URTWM_DEBUG_BEACON	= 0x00000040,	/* beacon handling */
	URTWM_DEBUG_INTR	= 0x00000080,	/* ISR */
	URTWM_DEBUG_TEMP	= 0x00000100,	/* temperature calibration */
	URTWM_DEBUG_ROM		= 0x00000200,	/* various ROM info */
	URTWM_DEBUG_KEY		= 0x00000400,	/* crypto keys management */
	URTWM_DEBUG_TXPWR	= 0x00000800,	/* dump Tx power values */
	URTWM_DEBUG_RSSI	= 0x00001000,	/* dump RSSI lookups */
	URTWM_DEBUG_ANY		= 0xffffffff
};

#define URTWM_DPRINTF(_sc, _m, ...) do {			\
	if ((_sc)->sc_debug & (_m))				\
		device_printf((_sc)->sc_dev, __VA_ARGS__);	\
} while(0)

#else
#define URTWM_DPRINTF(_sc, _m, ...)	do { (void) sc; } while (0)
#endif

#ifdef URTWM_TODO
static int urtwm_enable_11n = 0;
TUNABLE_INT("hw.usb.urtwm.enable_11n", &urtwm_enable_11n);
#endif

/* various supported device vendors/products */
static const STRUCT_USB_HOST_ID urtwm_devs[] = {
#define URTWM_DEV(v,p)  { USB_VP(USB_VENDOR_##v, USB_PRODUCT_##v##_##p) }
	URTWM_DEV(DLINK,	DWA171A1)
#undef URTWM_DEV
};

static device_probe_t	urtwm_match;
static device_attach_t	urtwm_attach;
static device_detach_t	urtwm_detach;

static usb_callback_t   urtwm_bulk_tx_callback;
static usb_callback_t	urtwm_bulk_rx_callback;
static usb_callback_t	urtwm_intr_rx_callback;

static void		urtwm_radiotap_attach(struct urtwm_softc *);
static void		urtwm_sysctlattach(struct urtwm_softc *);
static void		urtwm_drain_mbufq(struct urtwm_softc *);
static usb_error_t	urtwm_do_request(struct urtwm_softc *,
			    struct usb_device_request *, void *);
static struct ieee80211vap *urtwm_vap_create(struct ieee80211com *,
		    const char [IFNAMSIZ], int, enum ieee80211_opmode, int,
                    const uint8_t [IEEE80211_ADDR_LEN],
                    const uint8_t [IEEE80211_ADDR_LEN]);
static void		urtwm_vap_delete(struct ieee80211vap *);
static struct mbuf *	urtwm_rx_copy_to_mbuf(struct urtwm_softc *,
			    struct r92c_rx_stat *, int);
static struct mbuf *	urtwm_report_intr(struct urtwm_softc *,
			    struct usb_xfer *, struct urtwm_data *);
static struct mbuf *	urtwm_rxeof(struct urtwm_softc *, uint8_t *, int);
#ifdef URTWM_TODO
static void		urtwm_r88e_ratectl_tx_complete(struct urtwm_softc *,
			    void *);
#endif
static struct ieee80211_node *urtwm_rx_frame(struct urtwm_softc *,
			    struct mbuf *, int8_t *);
static void		urtwm_txeof(struct urtwm_softc *, struct urtwm_data *,
			    int);
static int		urtwm_alloc_list(struct urtwm_softc *,
			    struct urtwm_data[], int, int);
static int		urtwm_alloc_rx_list(struct urtwm_softc *);
static int		urtwm_alloc_tx_list(struct urtwm_softc *);
static void		urtwm_free_list(struct urtwm_softc *,
			    struct urtwm_data data[], int);
static void		urtwm_free_rx_list(struct urtwm_softc *);
static void		urtwm_free_tx_list(struct urtwm_softc *);
static struct urtwm_data *	_urtwm_getbuf(struct urtwm_softc *);
static struct urtwm_data *	urtwm_getbuf(struct urtwm_softc *);
static usb_error_t	urtwm_write_region_1(struct urtwm_softc *, uint16_t,
			    uint8_t *, int);
static usb_error_t	urtwm_write_1(struct urtwm_softc *, uint16_t, uint8_t);
static usb_error_t	urtwm_write_2(struct urtwm_softc *, uint16_t, uint16_t);
static usb_error_t	urtwm_write_4(struct urtwm_softc *, uint16_t, uint32_t);
static usb_error_t	urtwm_read_region_1(struct urtwm_softc *, uint16_t,
			    uint8_t *, int);
static uint8_t		urtwm_read_1(struct urtwm_softc *, uint16_t);
static uint16_t		urtwm_read_2(struct urtwm_softc *, uint16_t);
static uint32_t		urtwm_read_4(struct urtwm_softc *, uint16_t);
static usb_error_t	urtwm_setbits_1(struct urtwm_softc *, uint16_t,
			    uint8_t, uint8_t);
static usb_error_t	urtwm_setbits_1_shift(struct urtwm_softc *, uint16_t,
			    uint32_t, uint32_t, int);
static usb_error_t	urtwm_setbits_2(struct urtwm_softc *, uint16_t,
			    uint16_t, uint16_t);
static usb_error_t	urtwm_setbits_4(struct urtwm_softc *, uint16_t,
			    uint32_t, uint32_t);
#ifdef URTWM_TODO
static int		urtwm_fw_cmd(struct urtwm_softc *, uint8_t,
			    const void *, int);
#endif
static void		urtwm_cmdq_cb(void *, int);
static int		urtwm_cmd_sleepable(struct urtwm_softc *, const void *,
			    size_t, CMD_FUNC_PROTO);
static void		urtwm_rf_write(struct urtwm_softc *, int,
			    uint8_t, uint32_t);
static uint32_t		urtwm_rf_read(struct urtwm_softc *, int, uint8_t);
static void		urtwm_rf_setbits(struct urtwm_softc *, int, uint8_t,
			    uint32_t, uint32_t);
static int		urtwm_llt_write(struct urtwm_softc *, uint32_t,
			    uint32_t);
static int		urtwm_efuse_read_next(struct urtwm_softc *, uint8_t *);
static int		urtwm_efuse_read_data(struct urtwm_softc *, uint8_t *,
			    uint8_t, uint8_t);
#ifdef USB_DEBUG
static void		urtwm_dump_rom_contents(struct urtwm_softc *,
			    uint8_t *, uint16_t);
#endif
static int		urtwm_efuse_read(struct urtwm_softc *, uint8_t *,
			    uint16_t);
static int		urtwm_efuse_switch_power(struct urtwm_softc *);
static int		urtwm_setup_endpoints(struct urtwm_softc *);
static int		urtwm_read_chipid(struct urtwm_softc *);
static int		urtwm_read_rom(struct urtwm_softc *);
static void		urtwm_parse_rom(struct urtwm_softc *,
			    struct r88a_rom *);
#ifdef URTWM_TODO
static int		urtwm_ra_init(struct urtwm_softc *);
#endif
static void		urtwm_init_beacon(struct urtwm_softc *,
			    struct urtwm_vap *);
static int		urtwm_setup_beacon(struct urtwm_softc *,
			    struct ieee80211_node *);
static void		urtwm_update_beacon(struct ieee80211vap *, int);
static int		urtwm_tx_beacon(struct urtwm_softc *sc,
			    struct urtwm_vap *);
static int		urtwm_key_alloc(struct ieee80211vap *,
			    struct ieee80211_key *, ieee80211_keyix *,
			    ieee80211_keyix *);
static void		urtwm_key_set_cb(struct urtwm_softc *,
			    union sec_param *);
static void		urtwm_key_del_cb(struct urtwm_softc *,
			    union sec_param *);
static int		urtwm_process_key(struct ieee80211vap *,
			    const struct ieee80211_key *, int);
static int		urtwm_key_set(struct ieee80211vap *,
			    const struct ieee80211_key *);
static int		urtwm_key_delete(struct ieee80211vap *,
			    const struct ieee80211_key *);
static void		urtwm_tsf_sync_adhoc(void *);
static void		urtwm_tsf_sync_adhoc_task(void *, int);
static void		urtwm_tsf_sync_enable(struct urtwm_softc *,
			    struct ieee80211vap *);
static uint32_t		urtwm_get_tsf_low(struct urtwm_softc *, int);
static uint32_t		urtwm_get_tsf_high(struct urtwm_softc *, int);
static void		urtwm_get_tsf(struct urtwm_softc *, uint64_t *, int);
static void		urtwm_set_led(struct urtwm_softc *, int, int);
static void		urtwm_set_mode(struct urtwm_softc *, uint8_t, int);
static void		urtwm_adhoc_recv_mgmt(struct ieee80211_node *,
			    struct mbuf *, int,
			    const struct ieee80211_rx_stats *, int, int);
static int		urtwm_newstate(struct ieee80211vap *,
			    enum ieee80211_state, int);
#ifdef URTWM_TODO
static void		urtwm_calib_to(void *);
static void		urtwm_calib_cb(struct urtwm_softc *,
			    union sec_param *);
#endif
static int8_t		urtwm_get_rssi_cck(struct urtwm_softc *, void *);
static int8_t		urtwm_get_rssi_ofdm(struct urtwm_softc *, void *);
static int8_t		urtwm_get_rssi(struct urtwm_softc *, int, void *);
static void		urtwm_tx_protection(struct urtwm_softc *,
			    struct r88a_tx_desc *, enum ieee80211_protmode);
static void		urtwm_tx_raid(struct urtwm_softc *,
			    struct r88a_tx_desc *, struct ieee80211_node *,
			    int);
static int		urtwm_tx_data(struct urtwm_softc *,
			    struct ieee80211_node *, struct mbuf *,
			    struct urtwm_data *);
static int		urtwm_tx_raw(struct urtwm_softc *,
			    struct ieee80211_node *, struct mbuf *,
			    struct urtwm_data *,
			    const struct ieee80211_bpf_params *);
static void		urtwm_tx_start(struct urtwm_softc *, struct mbuf *,
			    uint8_t, struct urtwm_data *);
static void		urtwm_tx_checksum(struct r88a_tx_desc *);
static int		urtwm_transmit(struct ieee80211com *, struct mbuf *);
static void		urtwm_start(struct urtwm_softc *);
static void		urtwm_parent(struct ieee80211com *);
static int		urtwm_power_on(struct urtwm_softc *);
static void		urtwm_power_off(struct urtwm_softc *);
static int		urtwm_llt_init(struct urtwm_softc *);
#ifdef URTWM_TODO
#ifndef URTWM_WITHOUT_UCODE
static void		urtwm_fw_reset(struct urtwm_softc *);
static void		urtwm_r88e_fw_reset(struct urtwm_softc *);
static int		urtwm_fw_loadpage(struct urtwm_softc *, int,
			    const uint8_t *, int);
static int		urtwm_load_firmware(struct urtwm_softc *);
#endif
#endif
static int		urtwm_dma_init(struct urtwm_softc *);
static int		urtwm_mac_init(struct urtwm_softc *);
static void		urtwm_bb_init(struct urtwm_softc *);
static void		urtwm_rf_init(struct urtwm_softc *);
static void		urtwm_arfb_init(struct urtwm_softc *);
static void		urtwm_band_change(struct urtwm_softc *,
			    struct ieee80211_channel *, int);
static void		urtwm_cam_init(struct urtwm_softc *);
static int		urtwm_cam_write(struct urtwm_softc *, uint32_t,
			    uint32_t);
static void		urtwm_rxfilter_init(struct urtwm_softc *);
static void		urtwm_edca_init(struct urtwm_softc *);
static void		urtwm_mrr_init(struct urtwm_softc *);
static void		urtwm_write_txpower(struct urtwm_softc *, int,
			    struct ieee80211_channel *, uint16_t[]);
static int		urtwm_get_power_group(struct urtwm_softc *,
			    struct ieee80211_channel *);
static void		urtwm_get_txpower(struct urtwm_softc *, int,
		      	    struct ieee80211_channel *, uint16_t[]);
static void		urtwm_set_txpower(struct urtwm_softc *,
		    	    struct ieee80211_channel *);
static void		urtwm_set_rx_bssid_all(struct urtwm_softc *, int);
static void		urtwm_scan_start(struct ieee80211com *);
static void		urtwm_scan_curchan(struct ieee80211_scan_state *,
			    unsigned long);
static void		urtwm_scan_end(struct ieee80211com *);
static void		urtwm_getradiocaps(struct ieee80211com *, int, int *,
			    struct ieee80211_channel[]);
static void		urtwm_set_channel(struct ieee80211com *);
static int		urtwm_wme_update(struct ieee80211com *);
static void		urtwm_update_slot(struct ieee80211com *);
static void		urtwm_update_slot_cb(struct urtwm_softc *,
			    union sec_param *);
static void		urtwm_update_aifs(struct urtwm_softc *, uint8_t);
static uint8_t		urtwm_get_multi_pos(const uint8_t[]);
static void		urtwm_set_multi(struct urtwm_softc *);
static void		urtwm_set_promisc(struct urtwm_softc *);
static void		urtwm_update_promisc(struct ieee80211com *);
static void		urtwm_update_mcast(struct ieee80211com *);
#ifdef URTWM_TODO
static struct ieee80211_node *urtwm_node_alloc(struct ieee80211vap *,
			    const uint8_t mac[IEEE80211_ADDR_LEN]);
static void		urtwm_newassoc(struct ieee80211_node *, int);
static void		urtwm_node_free(struct ieee80211_node *);
#endif
static void		urtwm_set_chan(struct urtwm_softc *,
		    	    struct ieee80211_channel *);
static void		urtwm_antsel_init(struct urtwm_softc *);
#ifdef URTWM_TODO
static void		urtwm_iq_calib(struct urtwm_softc *);
static void		urtwm_lc_calib(struct urtwm_softc *);
static void		urtwm_temp_calib(struct urtwm_softc *);
#endif
static int		urtwm_init(struct urtwm_softc *);
static void		urtwm_stop(struct urtwm_softc *);
static void		urtwm_abort_xfers(struct urtwm_softc *);
static int		urtwm_raw_xmit(struct ieee80211_node *, struct mbuf *,
			    const struct ieee80211_bpf_params *);
static void		urtwm_delay(struct urtwm_softc *, int);

/* Aliases. */
#define	urtwm_bb_write		urtwm_write_4
#define urtwm_bb_read		urtwm_read_4
#define urtwm_bb_setbits	urtwm_setbits_4

static struct usb_config urtwm_config[URTWM_N_TRANSFER] = {
	[URTWM_BULK_RX] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.bufsize = URTWM_RXBUFSZ,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1
		},
		.callback = urtwm_bulk_rx_callback,
	},
	[URTWM_BULK_TX_BE] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.bufsize = URTWM_TXBUFSZ,
		.flags = {
			.ext_buffer = 1,
			.pipe_bof = 1,
			.force_short_xfer = 1,
		},
		.callback = urtwm_bulk_tx_callback,
		.timeout = URTWM_TX_TIMEOUT,	/* ms */
	},
	[URTWM_BULK_TX_BK] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.bufsize = URTWM_TXBUFSZ,
		.flags = {
			.ext_buffer = 1,
			.pipe_bof = 1,
			.force_short_xfer = 1,
		},
		.callback = urtwm_bulk_tx_callback,
		.timeout = URTWM_TX_TIMEOUT,	/* ms */
	},
	[URTWM_BULK_TX_VI] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.bufsize = URTWM_TXBUFSZ,
		.flags = {
			.ext_buffer = 1,
			.pipe_bof = 1,
			.force_short_xfer = 1
		},
		.callback = urtwm_bulk_tx_callback,
		.timeout = URTWM_TX_TIMEOUT,	/* ms */
	},
	[URTWM_BULK_TX_VO] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.bufsize = URTWM_TXBUFSZ,
		.flags = {
			.ext_buffer = 1,
			.pipe_bof = 1,
			.force_short_xfer = 1
		},
		.callback = urtwm_bulk_tx_callback,
		.timeout = URTWM_TX_TIMEOUT,	/* ms */
	},
	[URTWM_INTR_RD] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.bufsize = R88A_INTR_MSG_LEN,
		.flags = {
			.pipe_bof = 1,
			.short_xfer_ok = 1,
		},
		.callback = urtwm_intr_rx_callback,
	},
};

static const struct wme_to_queue {
	uint16_t reg;
	uint8_t qid;
} wme2queue[WME_NUM_AC] = {
	{ R92C_EDCA_BE_PARAM, URTWM_BULK_TX_BE},
	{ R92C_EDCA_BK_PARAM, URTWM_BULK_TX_BK},
	{ R92C_EDCA_VI_PARAM, URTWM_BULK_TX_VI},
	{ R92C_EDCA_VO_PARAM, URTWM_BULK_TX_VO}
};

static const uint8_t urtwm_chan_2ghz[] =
	{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };

static const uint8_t urtwm_chan_5ghz[] =
	{ 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64,
	  100, 102, 104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124,
	  126, 128, 130, 132, 134, 136, 138, 140, 142, 144,
	  149, 151, 153, 155, 157, 159, 161, 163, 165, 167, 168, 169, 171,
	  173, 175, 177 };

static int
urtwm_match(device_t self)
{
	struct usb_attach_arg *uaa = device_get_ivars(self);

	if (uaa->usb_mode != USB_MODE_HOST)
		return (ENXIO);
	if (uaa->info.bConfigIndex != URTWM_CONFIG_INDEX)
		return (ENXIO);
	if (uaa->info.bIfaceIndex != URTWM_IFACE_INDEX)
		return (ENXIO);

	return (usbd_lookup_id_by_uaa(urtwm_devs, sizeof(urtwm_devs), uaa));
}

static int
urtwm_attach(device_t self)
{
	struct usb_attach_arg *uaa = device_get_ivars(self);
	struct urtwm_softc *sc = device_get_softc(self);
	struct ieee80211com *ic = &sc->sc_ic;
	int error;

	device_set_usb_desc(self);
	sc->sc_udev = uaa->device;
	sc->sc_dev = self;

#ifdef USB_DEBUG
	int debug;
	if (resource_int_value(device_get_name(sc->sc_dev),
	    device_get_unit(sc->sc_dev), "debug", &debug) == 0)
		sc->sc_debug = debug;
#endif

	mtx_init(&sc->sc_mtx, device_get_nameunit(self),
	    MTX_NETWORK_LOCK, MTX_DEF);
	URTWM_CMDQ_LOCK_INIT(sc);
	mbufq_init(&sc->sc_snd, ifqmaxlen);

	error = urtwm_setup_endpoints(sc);
	if (error != 0)
		goto detach;

	URTWM_LOCK(sc);
	error = urtwm_read_chipid(sc);
	URTWM_UNLOCK(sc);
	if (error) {
		device_printf(sc->sc_dev, "unsupported test chip\n");
		goto detach;
	}

	sc->ntxchains = 1;
	sc->nrxchains = 1;

	error = urtwm_read_rom(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: cannot read rom, error %d\n",
		    __func__, error);
		goto detach;
	}

	device_printf(sc->sc_dev, "MAC/BB RTL8821AU, RF 6052 %dT%dR\n",
	    sc->ntxchains, sc->nrxchains);

	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(self);
	ic->ic_phytype = IEEE80211_T_OFDM;	/* not only, but not used */
	ic->ic_opmode = IEEE80211_M_STA;	/* default to BSS mode */

	/* set device capabilities */
	ic->ic_caps =
		  IEEE80211_C_STA		/* station mode */
		| IEEE80211_C_MONITOR		/* monitor mode */
		| IEEE80211_C_IBSS		/* adhoc mode */
		| IEEE80211_C_HOSTAP		/* hostap mode */
		| IEEE80211_C_SHPREAMBLE	/* short preamble supported */
		| IEEE80211_C_SHSLOT		/* short slot time supported */
#if 0
		| IEEE80211_C_BGSCAN		/* capable of bg scanning */
#endif
		| IEEE80211_C_WPA		/* 802.11i */
		| IEEE80211_C_WME		/* 802.11e */
#ifdef URTWM_TODO
		| IEEE80211_C_SWAMSDUTX		/* Do software A-MSDU TX */
		| IEEE80211_C_FF		/* Atheros fast-frames */
#endif
		;

	ic->ic_cryptocaps =
	    IEEE80211_CRYPTO_WEP |
	    IEEE80211_CRYPTO_TKIP |
	    IEEE80211_CRYPTO_AES_CCM;

#ifdef URTWM_TODO
	/* Assume they're all 11n capable for now */
	if (urtwm_enable_11n) {
		device_printf(self, "enabling 11n\n");
		ic->ic_htcaps = IEEE80211_HTC_HT |
#if 0
		    IEEE80211_HTC_AMPDU |
#endif
		    IEEE80211_HTC_AMSDU |
		    IEEE80211_HTCAP_MAXAMSDU_3839 |
		    IEEE80211_HTCAP_SMPS_OFF;
		/* no HT40 just yet */
		// ic->ic_htcaps |= IEEE80211_HTCAP_CHWIDTH40;

		/* XXX TODO: verify chains versus streams for urtwn */
		ic->ic_txstream = sc->ntxchains;
		ic->ic_rxstream = sc->nrxchains;
	}
#endif

	/* Enable TX watchdog */
#ifdef D4054
	ic->ic_flags_ext |= IEEE80211_FEXT_WATCHDOG;
#endif

	urtwm_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans,
	    ic->ic_channels);

	ieee80211_ifattach(ic);
	ic->ic_raw_xmit = urtwm_raw_xmit;
	ic->ic_scan_start = urtwm_scan_start;
	sc->sc_scan_curchan = ic->ic_scan_curchan;
	ic->ic_scan_curchan = urtwm_scan_curchan;
	ic->ic_scan_end = urtwm_scan_end;
	ic->ic_getradiocaps = urtwm_getradiocaps;
	ic->ic_set_channel = urtwm_set_channel;
	ic->ic_transmit = urtwm_transmit;
	ic->ic_parent = urtwm_parent;
	ic->ic_vap_create = urtwm_vap_create;
	ic->ic_vap_delete = urtwm_vap_delete;
	ic->ic_wme.wme_update = urtwm_wme_update;
	ic->ic_updateslot = urtwm_update_slot;
	ic->ic_update_promisc = urtwm_update_promisc;
	ic->ic_update_mcast = urtwm_update_mcast;
#ifdef URTWM_TODO
	ic->ic_node_alloc = urtwm_node_alloc;
	ic->ic_newassoc = urtwm_newassoc;
	sc->sc_node_free = ic->ic_node_free;
	ic->ic_node_free = urtwm_node_free;
#endif

	TASK_INIT(&sc->cmdq_task, 0, urtwm_cmdq_cb, sc);

	urtwm_radiotap_attach(sc);
	urtwm_sysctlattach(sc);

	if (bootverbose)
		ieee80211_announce(ic);

	return (0);

detach:
	urtwm_detach(self);
	return (ENXIO);			/* failure */
}

static void
urtwm_radiotap_attach(struct urtwm_softc *sc)
{
	struct urtwm_rx_radiotap_header *rxtap = &sc->sc_rxtap;
	struct urtwm_tx_radiotap_header *txtap = &sc->sc_txtap;

	ieee80211_radiotap_attach(&sc->sc_ic,
	    &txtap->wt_ihdr, sizeof(*txtap), URTWM_TX_RADIOTAP_PRESENT,
	    &rxtap->wr_ihdr, sizeof(*rxtap), URTWM_RX_RADIOTAP_PRESENT);
}

static void
urtwm_sysctlattach(struct urtwm_softc *sc)
{
#ifdef USB_DEBUG
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->sc_dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(sc->sc_dev);

	SYSCTL_ADD_U32(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
	    "debug", CTLFLAG_RW, &sc->sc_debug, sc->sc_debug,
	    "control debugging printfs");
#endif
}

static int
urtwm_detach(device_t self)
{
	struct urtwm_softc *sc = device_get_softc(self);
	struct ieee80211com *ic = &sc->sc_ic;
	unsigned int x;

	/* Prevent further ioctls. */
	URTWM_LOCK(sc);
	sc->sc_flags |= URTWM_DETACHED;
	URTWM_UNLOCK(sc);

	urtwm_stop(sc);

	/* stop all USB transfers */
	usbd_transfer_unsetup(sc->sc_xfer, URTWM_N_TRANSFER);

	/* Prevent further allocations from RX/TX data lists. */
	URTWM_LOCK(sc);
	STAILQ_INIT(&sc->sc_tx_active);
	STAILQ_INIT(&sc->sc_tx_inactive);
	STAILQ_INIT(&sc->sc_tx_pending);

	STAILQ_INIT(&sc->sc_rx_active);
	STAILQ_INIT(&sc->sc_rx_inactive);
	URTWM_UNLOCK(sc);

	/* drain USB transfers */
	for (x = 0; x != URTWM_N_TRANSFER; x++)
		usbd_transfer_drain(sc->sc_xfer[x]);

	/* Free data buffers. */
	URTWM_LOCK(sc);
	urtwm_free_tx_list(sc);
	urtwm_free_rx_list(sc);
	URTWM_UNLOCK(sc);

	if (ic->ic_softc == sc) {
		ieee80211_draintask(ic, &sc->cmdq_task);
		ieee80211_ifdetach(ic);
	}

	URTWM_CMDQ_LOCK_DESTROY(sc);
	mtx_destroy(&sc->sc_mtx);

	return (0);
}

static void
urtwm_drain_mbufq(struct urtwm_softc *sc)
{
	struct mbuf *m;
	struct ieee80211_node *ni;
	URTWM_ASSERT_LOCKED(sc);
	while ((m = mbufq_dequeue(&sc->sc_snd)) != NULL) {
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		m->m_pkthdr.rcvif = NULL;
		ieee80211_free_node(ni);
		m_freem(m);
	}
}

static usb_error_t
urtwm_do_request(struct urtwm_softc *sc, struct usb_device_request *req,
    void *data)
{
	usb_error_t err;
	int ntries = 10;

	URTWM_ASSERT_LOCKED(sc);

	while (ntries--) {
		err = usbd_do_request_flags(sc->sc_udev, &sc->sc_mtx,
		    req, data, 0, NULL, 250 /* ms */);
		if (err == 0)
			break;

		URTWM_DPRINTF(sc, URTWM_DEBUG_USB,
		    "%s: control request failed, %s (retries left: %d)\n",
		    __func__, usbd_errstr(err), ntries);
		usb_pause_mtx(&sc->sc_mtx, hz / 100);
	}
	return (err);
}

static struct ieee80211vap *
urtwm_vap_create(struct ieee80211com *ic, const char name[IFNAMSIZ], int unit,
    enum ieee80211_opmode opmode, int flags,
    const uint8_t bssid[IEEE80211_ADDR_LEN],
    const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct urtwm_softc *sc = ic->ic_softc;
	struct urtwm_vap *uvp;
	struct ieee80211vap *vap;

	if (!TAILQ_EMPTY(&ic->ic_vaps))		/* only one at a time */
		return (NULL);

	uvp = malloc(sizeof(struct urtwm_vap), M_80211_VAP, M_WAITOK | M_ZERO);
	vap = &uvp->vap;
	/* enable s/w bmiss handling for sta mode */

	if (ieee80211_vap_setup(ic, vap, name, unit, opmode,
	    flags | IEEE80211_CLONE_NOBEACONS, bssid) != 0) {
		/* out of memory */
		free(uvp, M_80211_VAP);
		return (NULL);
	}

	if (opmode == IEEE80211_M_HOSTAP || opmode == IEEE80211_M_IBSS)
		urtwm_init_beacon(sc, uvp);

	/* override state transition machine */
	uvp->newstate = vap->iv_newstate;
	vap->iv_newstate = urtwm_newstate;
	vap->iv_update_beacon = urtwm_update_beacon;
	vap->iv_key_alloc = urtwm_key_alloc;
	vap->iv_key_set = urtwm_key_set;
	vap->iv_key_delete = urtwm_key_delete;

	if (opmode == IEEE80211_M_IBSS) {
		uvp->recv_mgmt = vap->iv_recv_mgmt;
		vap->iv_recv_mgmt = urtwm_adhoc_recv_mgmt;
		TASK_INIT(&uvp->tsf_sync_adhoc_task, 0,
		    urtwm_tsf_sync_adhoc_task, vap);
		callout_init(&uvp->tsf_sync_adhoc, 0);
	}

	/* complete setup */
	ieee80211_vap_attach(vap, ieee80211_media_change,
	    ieee80211_media_status, mac);
	ic->ic_opmode = opmode;
	return (vap);
}

static void
urtwm_vap_delete(struct ieee80211vap *vap)
{
	struct ieee80211com *ic = vap->iv_ic;
	struct urtwm_vap *uvp = URTWM_VAP(vap);

	if (uvp->bcn_mbuf != NULL)
		m_freem(uvp->bcn_mbuf);
	if (vap->iv_opmode == IEEE80211_M_IBSS) {
		ieee80211_draintask(ic, &uvp->tsf_sync_adhoc_task);
		callout_drain(&uvp->tsf_sync_adhoc);
	}
	ieee80211_vap_detach(vap);
	free(uvp, M_80211_VAP);
}

static struct mbuf *
urtwm_rx_copy_to_mbuf(struct urtwm_softc *sc, struct r92c_rx_stat *stat,
    int totlen)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mbuf *m;
	uint32_t rxdw0;
	int pktlen;

	URTWM_ASSERT_LOCKED(sc);

	/*
	 * don't pass packets to the ieee80211 framework if the driver isn't
	 * RUNNING.
	 */
	if (!(sc->sc_flags & URTWM_RUNNING))
		return (NULL);

	rxdw0 = le32toh(stat->rxdw0);
	if (rxdw0 & (R92C_RXDW0_CRCERR | R92C_RXDW0_ICVERR)) {
		/*
		 * This should not happen since we setup our Rx filter
		 * to not receive these frames.
		 */
		URTWM_DPRINTF(sc, URTWM_DEBUG_RECV,
		    "%s: RX flags error (%s)\n", __func__,
		    rxdw0 & R92C_RXDW0_CRCERR ? "CRC" : "ICV");
		goto fail;
	}

	pktlen = MS(rxdw0, R92C_RXDW0_PKTLEN);
	if (pktlen < sizeof(struct ieee80211_frame_ack)) {
		URTWM_DPRINTF(sc, URTWM_DEBUG_RECV,
		    "%s: frame is too short: %d\n", __func__, pktlen);
		goto fail;
	}

	m = m_get2(totlen, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (__predict_false(m == NULL)) {
		device_printf(sc->sc_dev, "%s: could not allocate RX mbuf\n",
		    __func__);
		goto fail;
	}

	/* Finalize mbuf. */
	memcpy(mtod(m, uint8_t *), (uint8_t *)stat, totlen);
	m->m_pkthdr.len = m->m_len = totlen;

	return (m);
fail:
	counter_u64_add(ic->ic_ierrors, 1);
	return (NULL);
}

static struct mbuf *
urtwm_report_intr(struct urtwm_softc *sc, struct usb_xfer *xfer,
    struct urtwm_data *data)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct r92c_rx_stat *stat;
	uint8_t *buf;
	int len;

	usbd_xfer_status(xfer, &len, NULL, NULL, NULL);

	if (len < sizeof(*stat)) {
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}

	buf = data->buf;
	stat = (struct r92c_rx_stat *)buf;

	/*
	 * XXX in case when rate adaptation will work,
	 * XXX you will see some number of 'too short'
	 * XXX or 'incorrect' Rx frames via wlanstats.
	 */
#ifdef URTWM_TODO
	/*
	 * For 88E chips we can tie the FF flushing here;
	 * this is where we do know exactly how deep the
	 * transmit queue is.
	 *
	 * But it won't work for R92 chips, so we can't
	 * take the easy way out.
	 */

	int report_sel = MS(le32toh(stat->rxdw3), R88E_RXDW3_RPT);	

	switch (report_sel) {
	case R88E_RXDW3_RPT_RX:
#endif
		return (urtwm_rxeof(sc, buf, len));
#ifdef URTWM_TODO
	case R88E_RXDW3_RPT_TX1:
		urtwm_r88e_ratectl_tx_complete(sc, &stat[1]);
		break;
	default:
		URTWM_DPRINTF(sc, URTWM_DEBUG_INTR,
		    "%s: case %d was not handled\n", __func__,
		    report_sel);
		break;
	}
#endif

	return (NULL);
}

static struct mbuf *
urtwm_rxeof(struct urtwm_softc *sc, uint8_t *buf, int len)
{
	struct r92c_rx_stat *stat;
	struct mbuf *m, *m0 = NULL;
	uint32_t rxdw0;
	int totlen, pktlen, infosz;

	/* Process packets. */
	while (len >= sizeof(*stat)) {
		stat = (struct r92c_rx_stat *)buf;
		rxdw0 = le32toh(stat->rxdw0);

		pktlen = MS(rxdw0, R92C_RXDW0_PKTLEN);
		if (pktlen == 0)
			break;

		infosz = MS(rxdw0, R92C_RXDW0_INFOSZ) * 8;

		/* Make sure everything fits in xfer. */
		totlen = sizeof(*stat) + infosz + pktlen;
		if (totlen > len)
			break;

		if (m0 == NULL)
			m0 = m = urtwm_rx_copy_to_mbuf(sc, stat, totlen);
		else {
			m->m_next = urtwm_rx_copy_to_mbuf(sc, stat, totlen);
			if (m->m_next != NULL)
				m = m->m_next;
		}

		/* Next chunk is 8-byte aligned. */
		if (totlen < len)
			totlen = roundup2(totlen, 8);
		buf += totlen;
		len -= totlen;
	}

	return (m0);
}

#ifdef URTWM_TODO
static void
urtwn_r88e_ratectl_tx_complete(struct urtwm_softc *sc, void *arg)
{
	struct r88e_tx_rpt_ccx *rpt = arg;
	struct ieee80211vap *vap;
	struct ieee80211_node *ni;
	uint8_t macid;
	int ntries;

	macid = MS(rpt->rptb1, R88E_RPTB1_MACID);
	ntries = MS(rpt->rptb2, R88E_RPTB2_RETRY_CNT);

	URTWN_NT_LOCK(sc);
	ni = sc->node_list[macid];
	if (ni != NULL) {
		vap = ni->ni_vap;
		URTWN_DPRINTF(sc, URTWN_DEBUG_INTR, "%s: frame for macid %d was"
		    "%s sent (%d retries)\n", __func__, macid,
		    (rpt->rptb1 & R88E_RPTB1_PKT_OK) ? "" : " not",
		    ntries);

		if (rpt->rptb1 & R88E_RPTB1_PKT_OK) {
			ieee80211_ratectl_tx_complete(vap, ni,
			    IEEE80211_RATECTL_TX_SUCCESS, &ntries, NULL);
		} else {
			ieee80211_ratectl_tx_complete(vap, ni,
			    IEEE80211_RATECTL_TX_FAILURE, &ntries, NULL);
		}
	} else {
		URTWN_DPRINTF(sc, URTWN_DEBUG_INTR, "%s: macid %d, ni is NULL\n",
		    __func__, macid);
	}
	URTWN_NT_UNLOCK(sc);
}
#endif	/* URTWM_TODO */

static struct ieee80211_node *
urtwm_rx_frame(struct urtwm_softc *sc, struct mbuf *m, int8_t *rssi_p)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_frame_min *wh;
	struct r92c_rx_stat *stat;
	uint32_t rxdw0, rxdw3;
	uint8_t rate, cipher;
	int8_t rssi = -127;
	int infosz;

	stat = mtod(m, struct r92c_rx_stat *);
	rxdw0 = le32toh(stat->rxdw0);
	rxdw3 = le32toh(stat->rxdw3);

	rate = MS(rxdw3, R92C_RXDW3_RATE);
	cipher = MS(rxdw0, R92C_RXDW0_CIPHER);
	infosz = MS(rxdw0, R92C_RXDW0_INFOSZ) * 8;

	/* Get RSSI from PHY status descriptor if present. */
	if (infosz != 0 && (rxdw0 & R92C_RXDW0_PHYST)) {
		rssi = urtwm_get_rssi(sc, rate, &stat[1]);
		URTWM_DPRINTF(sc, URTWM_DEBUG_RSSI, "%s: rssi=%d\n", __func__, rssi);
	}

	if (ieee80211_radiotap_active(ic)) {
		struct urtwm_rx_radiotap_header *tap = &sc->sc_rxtap;

		tap->wr_flags = 0;

		/* XXX TODO: multi-vap */
		tap->wr_tsft = urtwm_get_tsf_high(sc, 0);
		if (le32toh(stat->rxdw5) > urtwm_get_tsf_low(sc, 0))
			tap->wr_tsft--;
		tap->wr_tsft = (uint64_t)htole32(tap->wr_tsft) << 32;
		tap->wr_tsft += stat->rxdw5;

		/* XXX 20/40? */
		/* XXX shortgi? */

		/* Map HW rate index to 802.11 rate. */
		/* XXX HT check does not work. */
		if (!(rxdw3 & R92C_RXDW3_HT)) {
			tap->wr_rate = ridx2rate[rate];
		} else if (rate >= 12) {	/* MCS0~15. */
			/* Bit 7 set means HT MCS instead of rate. */
			tap->wr_rate = 0x80 | (rate - 12);
		}

		/* XXX TODO: this isn't right; should use the last good RSSI */
		tap->wr_dbm_antsignal = rssi;
		tap->wr_dbm_antnoise = URTWM_NOISE_FLOOR;
	}

	*rssi_p = rssi;

	/* Drop descriptor. */
	m_adj(m, sizeof(*stat) + infosz);
	wh = mtod(m, struct ieee80211_frame_min *);

	if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) &&
	    cipher != R92C_CAM_ALGO_NONE) {
		m->m_flags |= M_WEP;
	}

	if (m->m_len >= sizeof(*wh))
		return (ieee80211_find_rxnode(ic, wh));

	return (NULL);
}

static void
urtwm_bulk_rx_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct urtwm_softc *sc = usbd_xfer_softc(xfer);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_node *ni;
	struct mbuf *m = NULL, *next;
	struct urtwm_data *data;
	int8_t nf, rssi;

	URTWM_ASSERT_LOCKED(sc);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		data = STAILQ_FIRST(&sc->sc_rx_active);
		if (data == NULL)
			goto tr_setup;
		STAILQ_REMOVE_HEAD(&sc->sc_rx_active, next);
		m = urtwm_report_intr(sc, xfer, data);
		STAILQ_INSERT_TAIL(&sc->sc_rx_inactive, data, next);
		/* FALLTHROUGH */
	case USB_ST_SETUP:
tr_setup:
		data = STAILQ_FIRST(&sc->sc_rx_inactive);
		if (data == NULL) {
			KASSERT(m == NULL, ("mbuf isn't NULL"));
			goto finish;
		}
		STAILQ_REMOVE_HEAD(&sc->sc_rx_inactive, next);
		STAILQ_INSERT_TAIL(&sc->sc_rx_active, data, next);
		usbd_xfer_set_frame_data(xfer, 0, data->buf,
		    usbd_xfer_max_len(xfer));
		usbd_transfer_submit(xfer);

		/*
		 * To avoid LOR we should unlock our private mutex here to call
		 * ieee80211_input() because here is at the end of a USB
		 * callback and safe to unlock.
		 */
		while (m != NULL) {
			next = m->m_next;
			m->m_next = NULL;

			ni = urtwm_rx_frame(sc, m, &rssi);

#ifdef URTWM_TODO
			/* Store a global last-good RSSI */
			if (rssi != -127)
				sc->last_rssi = rssi;
#endif

			URTWM_UNLOCK(sc);

			nf = URTWM_NOISE_FLOOR;
			if (ni != NULL) {
#ifdef URTWM_TODO
				if (rssi != -127)
					URTWN_NODE(ni)->last_rssi = rssi;
				if (ni->ni_flags & IEEE80211_NODE_HT)
					m->m_flags |= M_AMPDU;
				(void)ieee80211_input(ni, m,
				    URTWN_NODE(ni)->last_rssi - nf, nf);
#else
				(void)ieee80211_input(ni, m, rssi - nf, nf);
#endif
				ieee80211_free_node(ni);
			} else {
#ifdef URTWM_TODO
				/* Use last good global RSSI */
				(void)ieee80211_input_all(ic, m,
				    sc->last_rssi - nf, nf);
#else
				(void)ieee80211_input_all(ic, m,
				    rssi - nf, nf);
#endif
			}
			URTWM_LOCK(sc);
			m = next;
		}
		break;
	default:
		/* needs it to the inactive queue due to a error. */
		data = STAILQ_FIRST(&sc->sc_rx_active);
		if (data != NULL) {
			STAILQ_REMOVE_HEAD(&sc->sc_rx_active, next);
			STAILQ_INSERT_TAIL(&sc->sc_rx_inactive, data, next);
		}
		if (error != USB_ERR_CANCELLED) {
			usbd_xfer_set_stall(xfer);
			counter_u64_add(ic->ic_ierrors, 1);
			goto tr_setup;
		}
		break;
	}
finish:
#ifdef URTWM_TODO
	/* Finished receive; age anything left on the FF queue by a little bump */
	/*
	 * XXX TODO: just make this a callout timer schedule so we can
	 * flush the FF staging queue if we're approaching idle.
	 */
#ifdef	IEEE80211_SUPPORT_SUPERG
	URTWN_UNLOCK(sc);
	ieee80211_ff_age_all(ic, 1);
	URTWN_LOCK(sc);
#endif
#endif	/* URTWM_TODO */

	/* Kick-start more transmit in case we stalled */
	urtwm_start(sc);
}

/*
 * XXX can we get something useful from it?
 */
static void
urtwm_intr_rx_callback(struct usb_xfer *xfer, usb_error_t error)
{
	uint8_t input[R88A_INTR_MSG_LEN];
	struct usb_page_cache *pc;
	int actlen;

	usbd_xfer_status(xfer, &actlen, NULL, NULL, NULL);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		pc = usbd_xfer_get_frame(xfer, 0);
		usbd_copy_out(pc, 0, &input, sizeof(input));
		/* FALLTHROUGH */
	case USB_ST_SETUP:
tr_setup:
		usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
		usbd_transfer_submit(xfer);
		break;

	default:		/* Error */
		if (error != USB_ERR_CANCELLED) {
			/* try to clear stall first */
			usbd_xfer_set_stall(xfer);
			goto tr_setup;
		}
		break;
	}
}

static void
urtwm_txeof(struct urtwm_softc *sc, struct urtwm_data *data, int status)
{

	URTWM_ASSERT_LOCKED(sc);

	if (data->ni != NULL)	/* not a beacon frame */
		ieee80211_tx_complete(data->ni, data->m, status);

	if (sc->sc_tx_n_active > 0)
		sc->sc_tx_n_active--;

	data->ni = NULL;
	data->m = NULL;

	STAILQ_INSERT_TAIL(&sc->sc_tx_inactive, data, next);
}

static int
urtwm_alloc_list(struct urtwm_softc *sc, struct urtwm_data data[],
    int ndata, int maxsz)
{
	int i, error;

	for (i = 0; i < ndata; i++) {
		struct urtwm_data *dp = &data[i];
		dp->m = NULL;
		dp->buf = malloc(maxsz, M_USBDEV, M_NOWAIT);
		if (dp->buf == NULL) {
			device_printf(sc->sc_dev,
			    "could not allocate buffer\n");
			error = ENOMEM;
			goto fail;
		}
		dp->ni = NULL;
	}

	return (0);
fail:
	urtwm_free_list(sc, data, ndata);
	return (error);
}

static int
urtwm_alloc_rx_list(struct urtwm_softc *sc)
{
        int error, i;

	error = urtwm_alloc_list(sc, sc->sc_rx, URTWM_RX_LIST_COUNT,
	    URTWM_RXBUFSZ);
	if (error != 0)
		return (error);

	STAILQ_INIT(&sc->sc_rx_active);
	STAILQ_INIT(&sc->sc_rx_inactive);

	for (i = 0; i < URTWM_RX_LIST_COUNT; i++)
		STAILQ_INSERT_HEAD(&sc->sc_rx_inactive, &sc->sc_rx[i], next);

	return (0);
}

static int
urtwm_alloc_tx_list(struct urtwm_softc *sc)
{
	int error, i;

	error = urtwm_alloc_list(sc, sc->sc_tx, URTWM_TX_LIST_COUNT,
	    URTWM_TXBUFSZ);
	if (error != 0)
		return (error);

	STAILQ_INIT(&sc->sc_tx_active);
	STAILQ_INIT(&sc->sc_tx_inactive);
	STAILQ_INIT(&sc->sc_tx_pending);

	for (i = 0; i < URTWM_TX_LIST_COUNT; i++)
		STAILQ_INSERT_HEAD(&sc->sc_tx_inactive, &sc->sc_tx[i], next);

	return (0);
}

static void
urtwm_free_list(struct urtwm_softc *sc, struct urtwm_data data[], int ndata)
{
	int i;

	for (i = 0; i < ndata; i++) {
		struct urtwm_data *dp = &data[i];

		if (dp->buf != NULL) {
			free(dp->buf, M_USBDEV);
			dp->buf = NULL;
		}
		if (dp->ni != NULL) {
			ieee80211_free_node(dp->ni);
			dp->ni = NULL;
		}
		if (dp->m != NULL) {
			m_freem(dp->m);
			dp->m = NULL;
		}
	}
}

static void
urtwm_free_rx_list(struct urtwm_softc *sc)
{
	urtwm_free_list(sc, sc->sc_rx, URTWM_RX_LIST_COUNT);
}

static void
urtwm_free_tx_list(struct urtwm_softc *sc)
{
	urtwm_free_list(sc, sc->sc_tx, URTWM_TX_LIST_COUNT);
}

static void
urtwm_bulk_tx_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct urtwm_softc *sc = usbd_xfer_softc(xfer);
#ifdef	IEEE80211_SUPPORT_SUPERG
	struct ieee80211com *ic = &sc->sc_ic;
#endif
	struct urtwm_data *data;

	URTWM_ASSERT_LOCKED(sc);

	switch (USB_GET_STATE(xfer)){
	case USB_ST_TRANSFERRED:
		data = STAILQ_FIRST(&sc->sc_tx_active);
		if (data == NULL)
			goto tr_setup;
		STAILQ_REMOVE_HEAD(&sc->sc_tx_active, next);
		urtwm_txeof(sc, data, 0);
		/* FALLTHROUGH */
	case USB_ST_SETUP:
tr_setup:
		data = STAILQ_FIRST(&sc->sc_tx_pending);
		if (data == NULL) {
			URTWM_DPRINTF(sc, URTWM_DEBUG_XMIT,
			    "%s: empty pending queue\n", __func__);
			sc->sc_tx_n_active = 0;
			goto finish;
		}
		STAILQ_REMOVE_HEAD(&sc->sc_tx_pending, next);
		STAILQ_INSERT_TAIL(&sc->sc_tx_active, data, next);
		usbd_xfer_set_frame_data(xfer, 0, data->buf, data->buflen);
		usbd_transfer_submit(xfer);
		sc->sc_tx_n_active++;
		break;
	default:
		data = STAILQ_FIRST(&sc->sc_tx_active);
		if (data == NULL)
			goto tr_setup;
		STAILQ_REMOVE_HEAD(&sc->sc_tx_active, next);
		urtwm_txeof(sc, data, 1);
		if (error != USB_ERR_CANCELLED) {
			usbd_xfer_set_stall(xfer);
			goto tr_setup;
		}
		break;
	}
finish:
#ifdef URTWM_TODO
#ifdef	IEEE80211_SUPPORT_SUPERG
	/*
	 * If the TX active queue drops below a certain
	 * threshold, ensure we age fast-frames out so they're
	 * transmitted.
	 */
	if (sc->sc_tx_n_active <= 1) {
		/* XXX ew - net80211 should defer this for us! */

		/*
		 * Note: this sc_tx_n_active currently tracks
		 * the number of pending transmit submissions
		 * and not the actual depth of the TX frames
		 * pending to the hardware.  That means that
		 * we're going to end up with some sub-optimal
		 * aggregation behaviour.
		 */
		/*
		 * XXX TODO: just make this a callout timer schedule so we can
		 * flush the FF staging queue if we're approaching idle.
		 */
		URTWN_UNLOCK(sc);
		ieee80211_ff_flush(ic, WME_AC_VO);
		ieee80211_ff_flush(ic, WME_AC_VI);
		ieee80211_ff_flush(ic, WME_AC_BE);
		ieee80211_ff_flush(ic, WME_AC_BK);
		URTWN_LOCK(sc);
	}
#endif
#endif	/* URTWM_TODO */
	/* Kick-start more transmit */
	urtwm_start(sc);
}

static struct urtwm_data *
_urtwm_getbuf(struct urtwm_softc *sc)
{
	struct urtwm_data *bf;

	bf = STAILQ_FIRST(&sc->sc_tx_inactive);
	if (bf != NULL)
		STAILQ_REMOVE_HEAD(&sc->sc_tx_inactive, next);
	else {
		URTWM_DPRINTF(sc, URTWM_DEBUG_XMIT,
		    "%s: out of xmit buffers\n", __func__);
	}
	return (bf);
}

static struct urtwm_data *
urtwm_getbuf(struct urtwm_softc *sc)
{
	struct urtwm_data *bf;

	URTWM_ASSERT_LOCKED(sc);

	bf = _urtwm_getbuf(sc);
	if (bf == NULL) {
		URTWM_DPRINTF(sc, URTWM_DEBUG_XMIT, "%s: stop queue\n",
		    __func__);
	}
	return (bf);
}

static usb_error_t
urtwm_write_region_1(struct urtwm_softc *sc, uint16_t addr, uint8_t *buf,
    int len)
{
	usb_device_request_t req;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = R92C_REQ_REGS;
	USETW(req.wValue, addr);
	USETW(req.wIndex, 0);
	USETW(req.wLength, len);
	return (urtwm_do_request(sc, &req, buf));
}

static usb_error_t
urtwm_write_1(struct urtwm_softc *sc, uint16_t addr, uint8_t val)
{
	return (urtwm_write_region_1(sc, addr, &val, sizeof(val)));
}

static usb_error_t
urtwm_write_2(struct urtwm_softc *sc, uint16_t addr, uint16_t val)
{
	val = htole16(val);
	return (urtwm_write_region_1(sc, addr, (uint8_t *)&val, sizeof(val)));
}

static usb_error_t
urtwm_write_4(struct urtwm_softc *sc, uint16_t addr, uint32_t val)
{
	val = htole32(val);
	return (urtwm_write_region_1(sc, addr, (uint8_t *)&val, sizeof(val)));
}

static usb_error_t
urtwm_read_region_1(struct urtwm_softc *sc, uint16_t addr, uint8_t *buf,
    int len)
{
	usb_device_request_t req;

	req.bmRequestType = UT_READ_VENDOR_DEVICE;
	req.bRequest = R92C_REQ_REGS;
	USETW(req.wValue, addr);
	USETW(req.wIndex, 0);
	USETW(req.wLength, len);
	return (urtwm_do_request(sc, &req, buf));
}

static uint8_t
urtwm_read_1(struct urtwm_softc *sc, uint16_t addr)
{
	uint8_t val;

	if (urtwm_read_region_1(sc, addr, &val, 1) != 0)
		return (0xff);
	return (val);
}

static uint16_t
urtwm_read_2(struct urtwm_softc *sc, uint16_t addr)
{
	uint16_t val;

	if (urtwm_read_region_1(sc, addr, (uint8_t *)&val, 2) != 0)
		return (0xffff);
	return (le16toh(val));
}

static uint32_t
urtwm_read_4(struct urtwm_softc *sc, uint16_t addr)
{
	uint32_t val;

	if (urtwm_read_region_1(sc, addr, (uint8_t *)&val, 4) != 0)
		return (0xffffffff);
	return (le32toh(val));
}

static usb_error_t
urtwm_setbits_1(struct urtwm_softc *sc, uint16_t addr, uint8_t clr,
    uint8_t set)
{
	return (urtwm_write_1(sc, addr,
	    (urtwm_read_1(sc, addr) & ~clr) | set));
}

static usb_error_t
urtwm_setbits_1_shift(struct urtwm_softc *sc, uint16_t addr, uint32_t clr,
    uint32_t set, int shift)
{
	return (urtwm_setbits_1(sc, addr + shift, clr >> shift * NBBY,
	    set >> shift * NBBY));
}

static usb_error_t
urtwm_setbits_2(struct urtwm_softc *sc, uint16_t addr, uint16_t clr,
    uint16_t set)
{
	return (urtwm_write_2(sc, addr,
	    (urtwm_read_2(sc, addr) & ~clr) | set));
}

static usb_error_t
urtwm_setbits_4(struct urtwm_softc *sc, uint16_t addr, uint32_t clr,
    uint32_t set)
{
	return (urtwm_write_4(sc, addr,
	    (urtwm_read_4(sc, addr) & ~clr) | set));
}

#ifdef URTWM_TODO
static int
urtwn_fw_cmd(struct urtwm_softc *sc, uint8_t id, const void *buf, int len)
{
	struct r92c_fw_cmd cmd;
	usb_error_t error;
	int ntries;

	if (!(sc->sc_flags & URTWN_FW_LOADED)) {
		URTWN_DPRINTF(sc, URTWN_DEBUG_FIRMWARE, "%s: firmware "
		    "was not loaded; command (id %d) will be discarded\n",
		    __func__, id);
		return (0);
	}

	/* Wait for current FW box to be empty. */
	for (ntries = 0; ntries < 100; ntries++) {
		if (!(urtwn_read_1(sc, R92C_HMETFR) & (1 << sc->fwcur)))
			break;
		urtwn_ms_delay(sc);
	}
	if (ntries == 100) {
		device_printf(sc->sc_dev,
		    "could not send firmware command\n");
		return (ETIMEDOUT);
	}
	memset(&cmd, 0, sizeof(cmd));
	cmd.id = id;
	if (len > 3)
		cmd.id |= R92C_CMD_FLAG_EXT;
	KASSERT(len <= sizeof(cmd.msg), ("urtwn_fw_cmd\n"));
	memcpy(cmd.msg, buf, len);

	/* Write the first word last since that will trigger the FW. */
	if (len > 3) {
		error = urtwn_write_2(sc, R92C_HMEBOX_EXT(sc->fwcur),
		    *(uint16_t *)((uint8_t *)&cmd + 4));
		if (error != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
	}
	error = urtwn_write_4(sc, R92C_HMEBOX(sc->fwcur),
	    *(uint32_t *)&cmd);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	sc->fwcur = (sc->fwcur + 1) % R92C_H2C_NBOX;
	return (0);
}
#endif	/* URTWM_TODO */

static void
urtwm_cmdq_cb(void *arg, int pending)
{
	struct urtwm_softc *sc = arg;
	struct urtwm_cmdq *item;

	/*
	 * Device must be powered on (via urtwn_power_on())
	 * before any command may be sent.
	 */
	URTWM_LOCK(sc);
	if (!(sc->sc_flags & URTWM_RUNNING)) {
		URTWM_UNLOCK(sc);
		return;
	}

	URTWM_CMDQ_LOCK(sc);
	while (sc->cmdq[sc->cmdq_first].func != NULL) {
		item = &sc->cmdq[sc->cmdq_first];
		sc->cmdq_first = (sc->cmdq_first + 1) % URTWM_CMDQ_SIZE;
		URTWM_CMDQ_UNLOCK(sc);

		item->func(sc, &item->data);

		URTWM_CMDQ_LOCK(sc);
		memset(item, 0, sizeof (*item));
	}
	URTWM_CMDQ_UNLOCK(sc);
	URTWM_UNLOCK(sc);
}

static int
urtwm_cmd_sleepable(struct urtwm_softc *sc, const void *ptr, size_t len,
    CMD_FUNC_PROTO)
{
	struct ieee80211com *ic = &sc->sc_ic;

	KASSERT(len <= sizeof(union sec_param), ("buffer overflow"));

	URTWM_CMDQ_LOCK(sc);
	if (sc->cmdq[sc->cmdq_last].func != NULL) {
		device_printf(sc->sc_dev, "%s: cmdq overflow\n", __func__);
		URTWM_CMDQ_UNLOCK(sc);

		return (EAGAIN);
	}

	if (ptr != NULL)
		memcpy(&sc->cmdq[sc->cmdq_last].data, ptr, len);
	sc->cmdq[sc->cmdq_last].func = func;
	sc->cmdq_last = (sc->cmdq_last + 1) % URTWM_CMDQ_SIZE;
	URTWM_CMDQ_UNLOCK(sc);

	ieee80211_runtask(ic, &sc->cmdq_task);

	return (0);
}

static void
urtwm_rf_write(struct urtwm_softc *sc, int chain, uint8_t addr,
    uint32_t val)
{
	urtwm_bb_write(sc, R88A_LSSI_PARAM(chain),
	    SM(R88E_LSSI_PARAM_ADDR, addr) |
	    SM(R92C_LSSI_PARAM_DATA, val));
}

static uint32_t
urtwm_rf_read(struct urtwm_softc *sc, int chain, uint8_t addr)
{
	uint32_t pi_mode, val;

	val = urtwm_bb_read(sc, R88A_HSSI_PARAM1(chain));
	pi_mode = (val & R88A_HSSI_PARAM1_PI) ? 1 : 0;

	urtwm_bb_setbits(sc, R88A_HSSI_PARAM2,
	    R88A_HSSI_PARAM2_READ_ADDR_MASK, addr);
	urtwm_delay(sc, 20);

	val = urtwm_bb_read(sc, pi_mode ? R88A_HSPI_READBACK(chain) :
	    R88A_LSSI_READBACK(chain));

	return (MS(val, R92C_LSSI_READBACK_DATA));
}

static void
urtwm_rf_setbits(struct urtwm_softc *sc, int chain, uint8_t addr,
    uint32_t clr, uint32_t set)
{
	urtwm_rf_write(sc, chain, addr,
	    (urtwm_rf_read(sc, chain, addr) & ~clr) | set);
}

static int
urtwm_llt_write(struct urtwm_softc *sc, uint32_t addr, uint32_t data)
{
	usb_error_t error;
	int ntries;

	error = urtwm_write_4(sc, R92C_LLT_INIT,
	    SM(R92C_LLT_INIT_OP, R92C_LLT_INIT_OP_WRITE) |
	    SM(R92C_LLT_INIT_ADDR, addr) |
	    SM(R92C_LLT_INIT_DATA, data));
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	/* Wait for write operation to complete. */
	for (ntries = 0; ntries < 20; ntries++) {
		if (MS(urtwm_read_4(sc, R92C_LLT_INIT), R92C_LLT_INIT_OP) ==
		    R92C_LLT_INIT_OP_NO_ACTIVE)
			return (0);
		urtwm_delay(sc, 10);
	}
	return (ETIMEDOUT);
}

static int
urtwm_efuse_read_next(struct urtwm_softc *sc, uint8_t *val)
{
	uint32_t reg;
	usb_error_t error;
	int ntries;

	if (sc->next_rom_addr >= URTWM_EFUSE_MAX_LEN)
		return (EFAULT);

	reg = urtwm_read_4(sc, R92C_EFUSE_CTRL);
	reg = RW(reg, R92C_EFUSE_CTRL_ADDR, sc->next_rom_addr);
	reg &= ~R92C_EFUSE_CTRL_VALID;

	error = urtwm_write_4(sc, R92C_EFUSE_CTRL, reg);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	/* Wait for read operation to complete. */
	for (ntries = 0; ntries < 100; ntries++) {
		reg = urtwm_read_4(sc, R92C_EFUSE_CTRL);
		if (reg & R92C_EFUSE_CTRL_VALID)
			break;
		urtwm_delay(sc, 1000);	/* XXX */
	}
	if (ntries == 100) {
		device_printf(sc->sc_dev,
		    "could not read efuse byte at address 0x%x\n",
		    sc->next_rom_addr);
		return (ETIMEDOUT);
	}

	*val = MS(reg, R92C_EFUSE_CTRL_DATA);
	sc->next_rom_addr++;

	return (0);
}

static int
urtwm_efuse_read_data(struct urtwm_softc *sc, uint8_t *rom, uint8_t off,
    uint8_t msk)
{
	uint8_t reg;
	int i, error;

	for (i = 0; i < 4; i++) {
		if (msk & (1 << i))
			continue;
		error = urtwm_efuse_read_next(sc, &reg);
		if (error != 0)
			return (error);
		URTWM_DPRINTF(sc, URTWM_DEBUG_ROM, "rom[0x%03X] == 0x%02X\n",
		    off * 8 + i * 2, reg);
		rom[off * 8 + i * 2 + 0] = reg;

		error = urtwm_efuse_read_next(sc, &reg);
		if (error != 0)
			return (error);
		URTWM_DPRINTF(sc, URTWM_DEBUG_ROM, "rom[0x%03X] == 0x%02X\n",
		    off * 8 + i * 2 + 1, reg);
		rom[off * 8 + i * 2 + 1] = reg;
	}

	return (0);
}

#ifdef USB_DEBUG
static void
urtwm_dump_rom_contents(struct urtwm_softc *sc, uint8_t *rom, uint16_t size)
{
	int i;

	/* Dump ROM contents. */
	device_printf(sc->sc_dev, "%s:", __func__);
	for (i = 0; i < size; i++) {
		if (i % 32 == 0)
			printf("\n%03X: ", i);
		else if (i % 4 == 0)
			printf(" ");

		printf("%02X", rom[i]);
	}
	printf("\n");
}
#endif

static int
urtwm_efuse_read(struct urtwm_softc *sc, uint8_t *rom, uint16_t size)
{
#define URTWM_CHK(res) do {	\
	if ((error = res) != 0)	\
		goto end;	\
} while(0)
	uint8_t msk, off, reg;
	int error;

	URTWM_CHK(urtwm_efuse_switch_power(sc));

	/* Read full ROM image. */
	sc->next_rom_addr = 0;
	memset(rom, 0xff, size);

	URTWM_CHK(urtwm_efuse_read_next(sc, &reg));
	while (reg != 0xff) {
		/* check for extended header */
		if ((reg & 0x1f) == 0x0f) {
			off = reg >> 5;
			URTWM_CHK(urtwm_efuse_read_next(sc, &reg));

			if ((reg & 0x0f) != 0x0f)
				off = ((reg & 0xf0) >> 1) | off;
			else
				continue;
		} else
			off = reg >> 4;
		msk = reg & 0xf;

		URTWM_CHK(urtwm_efuse_read_data(sc, rom, off, msk));
		URTWM_CHK(urtwm_efuse_read_next(sc, &reg));
	}

end:

#ifdef USB_DEBUG
	if (sc->sc_debug & URTWM_DEBUG_ROM)
		urtwm_dump_rom_contents(sc, rom, size);
#endif

	urtwm_write_1(sc, R92C_EFUSE_ACCESS, R92C_EFUSE_ACCESS_OFF);

	if (error != 0) {
		device_printf(sc->sc_dev, "%s: error while reading ROM\n",
		    __func__);
	}

	return (error);
#undef URTWM_CHK
}

static int
urtwm_efuse_switch_power(struct urtwm_softc *sc)
{
	usb_error_t error;
	uint32_t reg;

	error = urtwm_write_1(sc, R92C_EFUSE_ACCESS, R92C_EFUSE_ACCESS_ON);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	reg = urtwm_read_2(sc, R92C_SYS_FUNC_EN);
	if (!(reg & R92C_SYS_FUNC_EN_ELDR)) {
		error = urtwm_write_2(sc, R92C_SYS_FUNC_EN,
		    reg | R92C_SYS_FUNC_EN_ELDR);
		if (error != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
	}
	reg = urtwm_read_2(sc, R92C_SYS_CLKR);
	if ((reg & (R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M)) !=
	    (R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M)) {
		error = urtwm_write_2(sc, R92C_SYS_CLKR,
		    reg | R92C_SYS_CLKR_LOADER_EN | R92C_SYS_CLKR_ANA8M);
		if (error != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
	}

	return (0);
}

static int
urtwm_setup_endpoints(struct urtwm_softc *sc)
{
	struct usb_endpoint *ep, *ep_end;
	uint8_t addr[R88A_MAX_EPOUT];
	int error;

	/* Determine the number of bulk-out pipes. */
	sc->ntx = 0;
	sc->sc_iface_index = URTWM_IFACE_INDEX;
	ep = sc->sc_udev->endpoints;
	ep_end = sc->sc_udev->endpoints + sc->sc_udev->endpoints_max;
	for (; ep != ep_end; ep++) {
		uint8_t eaddr;

		if ((ep->edesc == NULL) ||
		    (ep->iface_index != sc->sc_iface_index))
			continue;

		eaddr = ep->edesc->bEndpointAddress;
		URTWM_DPRINTF(sc, URTWM_DEBUG_USB,
		    "%s: endpoint: addr %d, direction %s\n", __func__,
		    UE_GET_ADDR(eaddr), UE_GET_DIR(eaddr) == UE_DIR_OUT ?
		    "output" : "input");

		if (UE_GET_DIR(eaddr) == UE_DIR_OUT) {
			if (sc->ntx == R88A_MAX_EPOUT)
				break;

			addr[sc->ntx++] = UE_GET_ADDR(eaddr);
		}
	}
	if (sc->ntx == 0 || sc->ntx > R88A_MAX_EPOUT) {
		device_printf(sc->sc_dev,
		    "%s: invalid number of Tx bulk pipes (%d)\n", __func__,
		    sc->ntx);
		return (EINVAL);
	}

	/* NB: keep in sync with urtwm_dma_init(). */
	urtwm_config[URTWM_BULK_TX_VO].endpoint = addr[0];
	switch (sc->ntx) {
	case 4:
	case 3:
		urtwm_config[URTWM_BULK_TX_BE].endpoint = addr[2];
		urtwm_config[URTWM_BULK_TX_BK].endpoint = addr[2];
		urtwm_config[URTWM_BULK_TX_VI].endpoint = addr[1];
	case 2:
		urtwm_config[URTWM_BULK_TX_BE].endpoint = addr[1];
		urtwm_config[URTWM_BULK_TX_BK].endpoint = addr[1];
		urtwm_config[URTWM_BULK_TX_VI].endpoint = addr[0];
	case 1:
		urtwm_config[URTWM_BULK_TX_BE].endpoint = addr[0];
		urtwm_config[URTWM_BULK_TX_BK].endpoint = addr[0];
		urtwm_config[URTWM_BULK_TX_VI].endpoint = addr[0];
	default:
		/* NOTREACHED */
		break;
	}

	error = usbd_transfer_setup(sc->sc_udev, &sc->sc_iface_index,
	    sc->sc_xfer, urtwm_config, URTWM_N_TRANSFER, sc, &sc->sc_mtx);
	if (error) {
		device_printf(sc->sc_dev, "could not allocate USB transfers, "
		    "err=%s\n", usbd_errstr(error));
		return (error);
	}

	return (0);
}

static int
urtwm_read_chipid(struct urtwm_softc *sc)
{
	uint32_t reg;

	reg = urtwm_read_4(sc, R92C_SYS_CFG);
	if (reg & R92C_SYS_CFG_TRP_VAUX_EN)	/* test chip */
		return (EIO);

	/* XXX TODO: RTL8812AU. */

	return (0);
}

static int
urtwm_read_rom(struct urtwm_softc *sc)
{
	struct r88a_rom *rom;
	int error;

	rom = malloc(URTWM_EFUSE_MAX_LEN, M_TEMP, M_WAITOK);

	/* Read full ROM image. */
	URTWM_LOCK(sc);
	error = urtwm_efuse_read(sc, (uint8_t *)rom, sizeof(*rom));
	URTWM_UNLOCK(sc);
	if (error != 0)
		goto fail;

	/* Parse & save data in softc. */
	urtwm_parse_rom(sc, rom);

fail:
	free(rom, M_TEMP);

	return (error);
}

static void
urtwm_parse_rom(struct urtwm_softc *sc, struct r88a_rom *rom)
{
#define URTWM_GET_ROM_VAR(var, def)	(((var) != 0xff) ? (var) : (def))
#define URTWM_SIGN4TO8(val) 		(((val) & 0x08) ? (val) | 0xf0 : (val))
	int i, j;

	sc->tx_bbswing_2g = URTWM_GET_ROM_VAR(rom->tx_bbswing_2g, 0);
	sc->tx_bbswing_5g = URTWM_GET_ROM_VAR(rom->tx_bbswing_5g, 0);

	/* Read PA/LNA types. */
	sc->pa_type = URTWM_GET_ROM_VAR(rom->pa_type, 0);
	sc->lna_type = URTWM_GET_ROM_VAR(rom->lna_type, 0);

	for (i = 0; i < sc->ntxchains; i++) {
		struct r88a_tx_pwr_2g *pwr_2g = &rom->tx_pwr[i].pwr_2g;
		struct r88a_tx_pwr_5g *pwr_5g = &rom->tx_pwr[i].pwr_5g;
		struct r88a_tx_pwr_diff_2g *pwr_diff_2g =
		    &rom->tx_pwr[i].pwr_diff_2g;
		struct r88a_tx_pwr_diff_5g *pwr_diff_5g =
		    &rom->tx_pwr[i].pwr_diff_5g;

		for (j = 0; j < URTWM_MAX_GROUP_2G - 1; j++) {
			sc->cck_tx_pwr[i][j] =
			    URTWM_GET_ROM_VAR(pwr_2g->cck[j],
				URTWM_DEF_TX_PWR_2G);
			sc->ht40_tx_pwr_2g[i][j] =
			    URTWM_GET_ROM_VAR(pwr_2g->ht40[j],
				URTWM_DEF_TX_PWR_2G);
		}
		sc->cck_tx_pwr[i][j] = URTWM_GET_ROM_VAR(pwr_2g->cck[j],
		    URTWM_DEF_TX_PWR_2G);

		sc->cck_tx_pwr_diff_2g[i][0] = 0;
		sc->ofdm_tx_pwr_diff_2g[i][0] = URTWM_SIGN4TO8(
		    MS(pwr_diff_2g->ht20_ofdm, LOW_PART));
		sc->bw20_tx_pwr_diff_2g[i][0] = URTWM_SIGN4TO8(
		    MS(pwr_diff_2g->ht20_ofdm, HIGH_PART));
		sc->bw40_tx_pwr_diff_2g[i][0] = 0;

		for (j = 1; j < nitems(pwr_diff_2g->diff123); j++) {
			sc->cck_tx_pwr_diff_2g[i][j] = URTWM_SIGN4TO8(
			    MS(pwr_diff_2g->diff123[j].ofdm_cck, LOW_PART));
			sc->ofdm_tx_pwr_diff_2g[i][j] = URTWM_SIGN4TO8(
			    MS(pwr_diff_2g->diff123[j].ofdm_cck, HIGH_PART));
			sc->bw20_tx_pwr_diff_2g[i][j] = URTWM_SIGN4TO8(
			    MS(pwr_diff_2g->diff123[j].ht40_ht20, LOW_PART));
			sc->bw40_tx_pwr_diff_2g[i][j] = URTWM_SIGN4TO8(
			    MS(pwr_diff_2g->diff123[j].ht40_ht20, HIGH_PART));
		}

		for (j = 0; j < URTWM_MAX_GROUP_5G; j++) {
			sc->ht40_tx_pwr_5g[i][j] =
			    URTWM_GET_ROM_VAR(pwr_5g->ht40[j],
				URTWM_DEF_TX_PWR_5G);
		}

		sc->ofdm_tx_pwr_diff_5g[i][0] = URTWM_SIGN4TO8(
		    MS(pwr_diff_5g->ht20_ofdm, LOW_PART));
		sc->ofdm_tx_pwr_diff_5g[i][1] = URTWM_SIGN4TO8(
		    MS(pwr_diff_5g->ofdm_ofdm[0], HIGH_PART));
		sc->ofdm_tx_pwr_diff_5g[i][2] = URTWM_SIGN4TO8(
		    MS(pwr_diff_5g->ofdm_ofdm[0], LOW_PART));
		sc->ofdm_tx_pwr_diff_5g[i][3] = URTWM_SIGN4TO8(
		    MS(pwr_diff_5g->ofdm_ofdm[1], LOW_PART));

		sc->bw20_tx_pwr_diff_5g[i][0] = URTWM_SIGN4TO8(
		    MS(pwr_diff_5g->ht20_ofdm, HIGH_PART));
		sc->bw40_tx_pwr_diff_5g[i][0] = 0;
		for (j = 1; j < nitems(pwr_diff_5g->ht40_ht20); j++) {
			sc->bw20_tx_pwr_diff_5g[i][j] = URTWM_SIGN4TO8(
			    MS(pwr_diff_5g->ht40_ht20[j], LOW_PART));
			sc->bw40_tx_pwr_diff_5g[i][j] = URTWM_SIGN4TO8(
			    MS(pwr_diff_5g->ht40_ht20[j], HIGH_PART));
		}

		for (j = 0; j < nitems(pwr_diff_5g->ht80_ht160); j++) {
			sc->bw80_tx_pwr_diff_5g[i][j] = URTWM_SIGN4TO8(
			    MS(pwr_diff_5g->ht80_ht160[j], HIGH_PART));
			sc->bw160_tx_pwr_diff_5g[i][j] = URTWM_SIGN4TO8(
			    MS(pwr_diff_5g->ht80_ht160[j], LOW_PART));
		}
	}

	sc->regulatory = MS(rom->rf_board_opt, R92C_ROM_RF1_REGULATORY);
	URTWM_DPRINTF(sc, URTWM_DEBUG_ROM, "%s: regulatory type=%d\n",
	    __func__, sc->regulatory);
	IEEE80211_ADDR_COPY(sc->sc_ic.ic_macaddr, rom->macaddr);
#undef URTWM_SIGN4TO8
#undef URTWM_GET_ROM_VAR
}

static __inline uint8_t
rate2ridx(uint8_t rate)
{
	if (rate & IEEE80211_RATE_MCS) {
		/* 11n rates start at idx 12 */
		return ((rate & 0xf) + 12);
	}
	switch (rate) {
	/* 11g */
	case 12:	return 4;
	case 18:	return 5;
	case 24:	return 6;
	case 36:	return 7;
	case 48:	return 8;
	case 72:	return 9;
	case 96:	return 10;
	case 108:	return 11;
	/* 11b */
	case 2:		return 0;
	case 4:		return 1;
	case 11:	return 2;
	case 22:	return 3;
	default:	return URTWM_RIDX_UNKNOWN;
	}
}

#ifdef URTWM_TODO
/*
 * Initialize rate adaptation in firmware.
 */
static int
urtwn_ra_init(struct urtwm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
	struct ieee80211_node *ni;
	struct ieee80211_rateset *rs, *rs_ht;
	struct r92c_fw_cmd_macid_cfg cmd;
	uint32_t rates, basicrates;
	uint8_t mode, ridx;
	int maxrate, maxbasicrate, error = 0, i;

	ni = ieee80211_ref_node(vap->iv_bss);
	rs = &ni->ni_rates;
	rs_ht = (struct ieee80211_rateset *) &ni->ni_htrates;

	/* Get normal and basic rates mask. */
	rates = basicrates = 0;
	maxrate = maxbasicrate = 0;

	/* This is for 11bg */
	for (i = 0; i < rs->rs_nrates; i++) {
		/* Convert 802.11 rate to HW rate index. */
		ridx = rate2ridx(IEEE80211_RV(rs->rs_rates[i]));
		if (ridx == URTWN_RIDX_UNKNOWN)	/* Unknown rate, skip. */
			continue;
		rates |= 1 << ridx;
		if (ridx > maxrate)
			maxrate = ridx;
		if (rs->rs_rates[i] & IEEE80211_RATE_BASIC) {
			basicrates |= 1 << ridx;
			if (ridx > maxbasicrate)
				maxbasicrate = ridx;
		}
	}

	/* If we're doing 11n, enable 11n rates */
	if (ni->ni_flags & IEEE80211_NODE_HT) {
		for (i = 0; i < rs_ht->rs_nrates; i++) {
			if ((rs_ht->rs_rates[i] & 0x7f) > 0xf)
				continue;
			/* 11n rates start at index 12 */
			ridx = ((rs_ht->rs_rates[i]) & 0xf) + 12;
			rates |= (1 << ridx);

			/* Guard against the rate table being oddly ordered */
			if (ridx > maxrate)
				maxrate = ridx;
		}
	}

	/* NB: group addressed frames are done at 11bg rates for now */
	if (ic->ic_curmode == IEEE80211_MODE_11B)
		mode = R92C_RAID_11B;
	else
		mode = R92C_RAID_11BG;
	/* XXX misleading 'mode' value here for unicast frames */
	URTWN_DPRINTF(sc, URTWN_DEBUG_RA,
	    "%s: mode 0x%x, rates 0x%08x, basicrates 0x%08x\n", __func__,
	    mode, rates, basicrates);

	/* Set rates mask for group addressed frames. */
	cmd.macid = URTWN_MACID_BC | URTWN_MACID_VALID;
	cmd.mask = htole32(mode << 28 | basicrates);
	error = urtwn_fw_cmd(sc, R92C_CMD_MACID_CONFIG, &cmd, sizeof(cmd));
	if (error != 0) {
		ieee80211_free_node(ni);
		device_printf(sc->sc_dev,
		    "could not add broadcast station\n");
		return (error);
	}

	/* Set initial MRR rate. */
	URTWN_DPRINTF(sc, URTWN_DEBUG_RA, "%s: maxbasicrate %d\n", __func__,
	    maxbasicrate);
	urtwn_write_1(sc, R92C_INIDATA_RATE_SEL(URTWN_MACID_BC),
	    maxbasicrate);

	/* Set rates mask for unicast frames. */
	if (ni->ni_flags & IEEE80211_NODE_HT)
		mode = R92C_RAID_11GN;
	else if (ic->ic_curmode == IEEE80211_MODE_11B)
		mode = R92C_RAID_11B;
	else
		mode = R92C_RAID_11BG;
	cmd.macid = URTWN_MACID_BSS | URTWN_MACID_VALID;
	cmd.mask = htole32(mode << 28 | rates);
	error = urtwn_fw_cmd(sc, R92C_CMD_MACID_CONFIG, &cmd, sizeof(cmd));
	if (error != 0) {
		ieee80211_free_node(ni);
		device_printf(sc->sc_dev, "could not add BSS station\n");
		return (error);
	}
	/* Set initial MRR rate. */
	URTWN_DPRINTF(sc, URTWN_DEBUG_RA, "%s: maxrate %d\n", __func__,
	    maxrate);
	urtwn_write_1(sc, R92C_INIDATA_RATE_SEL(URTWN_MACID_BSS),
	    maxrate);

	/* Indicate highest supported rate. */
	if (ni->ni_flags & IEEE80211_NODE_HT)
		ni->ni_txrate = rs_ht->rs_rates[rs_ht->rs_nrates - 1]
		    | IEEE80211_RATE_MCS;
	else
		ni->ni_txrate = rs->rs_rates[rs->rs_nrates - 1];
	ieee80211_free_node(ni);

	URTWN_DPRINTF(sc, URTWN_DEBUG_BEACON, "%s: beacon was %srecognized\n",
	    __func__, urtwn_read_1(sc, R92C_TDECTRL + 2) &
	    (R92C_TDECTRL_BCN_VALID >> 16) ? "" : "not ");

	return (0);
}
#endif	/* URTWM_TODO */

static void
urtwm_init_beacon(struct urtwm_softc *sc, struct urtwm_vap *uvp)
{
	struct r88a_tx_desc *txd = &uvp->bcn_desc;

	txd->offset = sizeof(*txd);
	txd->flags0 = R88A_FLAGS0_LSG | R88A_FLAGS0_FSG | R88A_FLAGS0_OWN |
	    R88A_FLAGS0_BMCAST;

	/*
	 * NB: there is no need to setup HWSEQ_EN bit;
	 * QSEL_BEACON already implies it.
	 */
	txd->txdw1 = htole32(SM(R88A_TXDW1_QSEL, R88A_TXDW1_QSEL_BEACON));
	txd->txdw1 |= htole32(SM(R88A_TXDW1_MACID, URTWM_MACID_BC));

	txd->txdw3 = htole32(R88A_TXDW3_DRVRATE);
	txd->txdw4 = htole32(SM(R88A_TXDW4_DATARATE, URTWM_RIDX_CCK1));
}

static int
urtwm_setup_beacon(struct urtwm_softc *sc, struct ieee80211_node *ni)
{
 	struct ieee80211vap *vap = ni->ni_vap;
	struct urtwm_vap *uvp = URTWM_VAP(vap);
	struct mbuf *m;
	int error;

	URTWM_ASSERT_LOCKED(sc);

	if (ni->ni_chan == IEEE80211_CHAN_ANYC)
		return (EINVAL);

	m = ieee80211_beacon_alloc(ni);
	if (m == NULL) {
		device_printf(sc->sc_dev,
		    "%s: could not allocate beacon frame\n", __func__);
		return (ENOMEM);
	}

	if (uvp->bcn_mbuf != NULL)
		m_freem(uvp->bcn_mbuf);

	uvp->bcn_mbuf = m;

	if ((error = urtwm_tx_beacon(sc, uvp)) != 0)
		return (error);

	/* XXX bcnq stuck workaround */
	if ((error = urtwm_tx_beacon(sc, uvp)) != 0)
		return (error);

	return (0);
}

static void
urtwm_update_beacon(struct ieee80211vap *vap, int item)
{
	struct urtwm_softc *sc = vap->iv_ic->ic_softc;
	struct urtwm_vap *uvp = URTWM_VAP(vap);
	struct ieee80211_beacon_offsets *bo = &vap->iv_bcn_off;
	struct ieee80211_node *ni = vap->iv_bss;
	int mcast = 0;

	URTWM_LOCK(sc);
	if (uvp->bcn_mbuf == NULL) {
		uvp->bcn_mbuf = ieee80211_beacon_alloc(ni);
		if (uvp->bcn_mbuf == NULL) {
			device_printf(sc->sc_dev,
			    "%s: could not allocate beacon frame\n", __func__);
			URTWM_UNLOCK(sc);
			return;
		}
	}
	URTWM_UNLOCK(sc);

	if (item == IEEE80211_BEACON_TIM)
		mcast = 1;	/* XXX */

	setbit(bo->bo_flags, item);
	ieee80211_beacon_update(ni, uvp->bcn_mbuf, mcast);

	URTWM_LOCK(sc);
	urtwm_tx_beacon(sc, uvp);
	URTWM_UNLOCK(sc);
}

/*
 * Push a beacon frame into the chip. Beacon will
 * be repeated by the chip every R92C_BCN_INTERVAL.
 */
static int
urtwm_tx_beacon(struct urtwm_softc *sc, struct urtwm_vap *uvp)
{
	struct r88a_tx_desc *desc = &uvp->bcn_desc;
	struct urtwm_data *bf;

	URTWM_ASSERT_LOCKED(sc);

	bf = urtwm_getbuf(sc);
	if (bf == NULL)
		return (ENOMEM);

	memcpy(bf->buf, desc, sizeof(*desc));
	urtwm_tx_start(sc, uvp->bcn_mbuf, IEEE80211_FC0_TYPE_MGT, bf);

	return (0);
}

static int
urtwm_key_alloc(struct ieee80211vap *vap, struct ieee80211_key *k,
    ieee80211_keyix *keyix, ieee80211_keyix *rxkeyix)
{
	struct urtwm_softc *sc = vap->iv_ic->ic_softc;
	uint8_t i;

	if (!(&vap->iv_nw_keys[0] <= k &&
	     k < &vap->iv_nw_keys[IEEE80211_WEP_NKID])) {
		if (!(k->wk_flags & IEEE80211_KEY_SWCRYPT)) {
			URTWM_LOCK(sc);
			/*
			 * First 4 slots for group keys,
			 * what is left - for pairwise.
			 * XXX incompatible with IBSS RSN.
			 */
			for (i = IEEE80211_WEP_NKID;
			     i < R92C_CAM_ENTRY_COUNT; i++) {
				if ((sc->keys_bmap & (1 << i)) == 0) {
					sc->keys_bmap |= 1 << i;
					*keyix = i;
					break;
				}
			}
			URTWM_UNLOCK(sc);
			if (i == R92C_CAM_ENTRY_COUNT) {
				device_printf(sc->sc_dev,
				    "%s: no free space in the key table\n",
				    __func__);
				return 0;
			}
		} else
			*keyix = 0;
	} else {
		*keyix = k - vap->iv_nw_keys;
	}
	*rxkeyix = *keyix;
	return 1;
}

static void
urtwm_key_set_cb(struct urtwm_softc *sc, union sec_param *data)
{
	struct ieee80211_key *k = &data->key;
	uint8_t algo, keyid;
	int i, error;

	if (k->wk_keyix < IEEE80211_WEP_NKID)
		keyid = k->wk_keyix;
	else
		keyid = 0;

	/* Map net80211 cipher to HW crypto algorithm. */
	switch (k->wk_cipher->ic_cipher) {
	case IEEE80211_CIPHER_WEP:
		if (k->wk_keylen < 8)
			algo = R92C_CAM_ALGO_WEP40;
		else
			algo = R92C_CAM_ALGO_WEP104;
		break;
	case IEEE80211_CIPHER_TKIP:
		algo = R92C_CAM_ALGO_TKIP;
		break;
	case IEEE80211_CIPHER_AES_CCM:
		algo = R92C_CAM_ALGO_AES;
		break;
	default:
		device_printf(sc->sc_dev, "%s: unknown cipher %d\n",
		    __func__, k->wk_cipher->ic_cipher);
		return;
	}

	URTWM_DPRINTF(sc, URTWM_DEBUG_KEY,
	    "%s: keyix %d, keyid %d, algo %d/%d, flags %04X, len %d, "
	    "macaddr %s\n", __func__, k->wk_keyix, keyid,
	    k->wk_cipher->ic_cipher, algo, k->wk_flags, k->wk_keylen,
	    ether_sprintf(k->wk_macaddr));

	/* Write key. */
	for (i = 0; i < 4; i++) {
		error = urtwm_cam_write(sc, R92C_CAM_KEY(k->wk_keyix, i),
		    le32dec(&k->wk_key[i * 4]));
		if (error != 0)
			goto fail;
	}

	/* Write CTL0 last since that will validate the CAM entry. */
	error = urtwm_cam_write(sc, R92C_CAM_CTL1(k->wk_keyix),
	    le32dec(&k->wk_macaddr[2]));
	if (error != 0)
		goto fail;
	error = urtwm_cam_write(sc, R92C_CAM_CTL0(k->wk_keyix),
	    SM(R92C_CAM_ALGO, algo) |
	    SM(R92C_CAM_KEYID, keyid) |
	    SM(R92C_CAM_MACLO, le16dec(&k->wk_macaddr[0])) |
	    R92C_CAM_VALID);
	if (error != 0)
		goto fail;

	return;

fail:
	device_printf(sc->sc_dev, "%s fails, error %d\n", __func__, error);
}

static void
urtwm_key_del_cb(struct urtwm_softc *sc, union sec_param *data)
{
	struct ieee80211_key *k = &data->key;
	int i;

	URTWM_DPRINTF(sc, URTWM_DEBUG_KEY,
	    "%s: keyix %d, flags %04X, macaddr %s\n", __func__,
	    k->wk_keyix, k->wk_flags, ether_sprintf(k->wk_macaddr));

	urtwm_cam_write(sc, R92C_CAM_CTL0(k->wk_keyix), 0);
	urtwm_cam_write(sc, R92C_CAM_CTL1(k->wk_keyix), 0);

	/* Clear key. */
	for (i = 0; i < 4; i++)
		urtwm_cam_write(sc, R92C_CAM_KEY(k->wk_keyix, i), 0);
	sc->keys_bmap &= ~(1 << k->wk_keyix);
}

static int
urtwm_process_key(struct ieee80211vap *vap, const struct ieee80211_key *k,
    int set)
{
	struct urtwm_softc *sc = vap->iv_ic->ic_softc;
	struct urtwm_vap *uvp = URTWM_VAP(vap);

	if (k->wk_flags & IEEE80211_KEY_SWCRYPT) {
		/* Not for us. */
		return (1);
	}

	if (&vap->iv_nw_keys[0] <= k &&
	    k < &vap->iv_nw_keys[IEEE80211_WEP_NKID]) {
		URTWM_LOCK(sc);		/* XXX */
		if ((sc->sc_flags & URTWM_RUNNING) == 0) {
			/*
			 * The device was not started;
			 * the key will be installed later.
			 */
			uvp->keys[k->wk_keyix] = set ? k : NULL;
			URTWM_UNLOCK(sc);
			return (1);
		}
		URTWM_UNLOCK(sc);
	}

	return (!urtwm_cmd_sleepable(sc, k, sizeof(*k),
	    set ? urtwm_key_set_cb : urtwm_key_del_cb));
}

static int
urtwm_key_set(struct ieee80211vap *vap, const struct ieee80211_key *k)
{
	return (urtwm_process_key(vap, k, 1));
}

static int
urtwm_key_delete(struct ieee80211vap *vap, const struct ieee80211_key *k)
{
	return (urtwm_process_key(vap, k, 0));
}

static void
urtwm_tsf_sync_adhoc(void *arg)
{
	struct ieee80211vap *vap = arg;
	struct ieee80211com *ic = vap->iv_ic;
	struct urtwm_vap *uvp = URTWM_VAP(vap);

	if (vap->iv_state == IEEE80211_S_RUN) {
		/* Do it in process context. */
		ieee80211_runtask(ic, &uvp->tsf_sync_adhoc_task);
	}
}

/*
 * Workaround for TSF synchronization:
 * when BSSID filter in IBSS mode is not set
 * (and TSF synchronization is enabled), then any beacon may update it.
 * This routine synchronizes it when BSSID matching is enabled (IBSS merge
 * is not possible during this period).
 */
static void
urtwm_tsf_sync_adhoc_task(void *arg, int pending)
{
	struct ieee80211vap *vap = arg;
	struct urtwm_vap *uvp = URTWM_VAP(vap);
	struct urtwm_softc *sc = vap->iv_ic->ic_softc;
	struct ieee80211_node *ni;

	URTWM_LOCK(sc);
	ni = ieee80211_ref_node(vap->iv_bss);

	/* Accept beacons with the same BSSID. */
	urtwm_set_rx_bssid_all(sc, 0);

        /* Enable synchronization. */
	urtwm_setbits_1(sc, R92C_BCN_CTRL, R92C_BCN_CTRL_DIS_TSF_UDT0, 0);

	/* Synchronize. */
	usb_pause_mtx(&sc->sc_mtx, hz * ni->ni_intval * 5 / 1000);

	/* Disable synchronization. */
	urtwm_setbits_1(sc, R92C_BCN_CTRL, 0, R92C_BCN_CTRL_DIS_TSF_UDT0);

	/* Accept all beacons. */
	urtwm_set_rx_bssid_all(sc, 1);

	/* Schedule next TSF synchronization. */
	callout_reset(&uvp->tsf_sync_adhoc, 60*hz, urtwm_tsf_sync_adhoc, vap);

	ieee80211_free_node(ni);
	URTWM_UNLOCK(sc);
}

static void
urtwm_tsf_sync_enable(struct urtwm_softc *sc, struct ieee80211vap *vap)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct urtwm_vap *uvp = URTWM_VAP(vap);

	/* Reset TSF. */
	urtwm_write_1(sc, R92C_DUAL_TSF_RST, R92C_DUAL_TSF_RST0);

	switch (vap->iv_opmode) {
	case IEEE80211_M_STA:
		/* Enable TSF synchronization. */
		urtwm_setbits_1(sc, R92C_BCN_CTRL, R92C_BCN_CTRL_DIS_TSF_UDT0,
		    0);
		break;
	case IEEE80211_M_IBSS:
		ieee80211_runtask(ic, &uvp->tsf_sync_adhoc_task);
		/* FALLTHROUGH */
	case IEEE80211_M_HOSTAP:
		/* Enable beaconing. */
		urtwm_setbits_1(sc, R92C_BCN_CTRL, 0, R92C_BCN_CTRL_EN_BCN);
		break;
	default:
		device_printf(sc->sc_dev, "undefined opmode %d\n",
		    vap->iv_opmode);
		return;
	}
}

static uint32_t
urtwm_get_tsf_low(struct urtwm_softc *sc, int id)
{
	return (urtwm_read_4(sc, R92C_TSFTR(id)));
}

static uint32_t
urtwm_get_tsf_high(struct urtwm_softc *sc, int id)
{
	return (urtwm_read_4(sc, R92C_TSFTR(id) + 4));
}

static void
urtwm_get_tsf(struct urtwm_softc *sc, uint64_t *buf, int id)
{
	/* NB: we cannot read it at once. */
	*buf = urtwm_get_tsf_high(sc, id);
	*buf <<= 32;
	*buf += urtwm_get_tsf_low(sc, id);
}

static void
urtwm_set_led(struct urtwm_softc *sc, int led, int on)
{
	/* XXX minicard / solo / combo? */
	if (led == URTWM_LED_LINK) {
		if (on)
			urtwm_write_1(sc, R92C_LEDCFG2, R88A_LEDCFG2_ENA);
		else {
			urtwm_write_1(sc, R92C_LEDCFG2,
			    R88A_LEDCFG2_ENA | R92C_LEDCFG0_DIS);
		}
		sc->ledlink = on;	/* Save LED state. */
	}
}

static void
urtwm_set_mode(struct urtwm_softc *sc, uint8_t mode, int id)
{
	urtwm_setbits_1(sc, R92C_MSR, R92C_MSR_MASK << id * 2, mode << id * 2);
}

static void
urtwm_adhoc_recv_mgmt(struct ieee80211_node *ni, struct mbuf *m, int subtype,
    const struct ieee80211_rx_stats *rxs,
    int rssi, int nf)
{
	struct ieee80211vap *vap = ni->ni_vap;
	struct urtwm_softc *sc = vap->iv_ic->ic_softc;
	struct urtwm_vap *uvp = URTWM_VAP(vap);
	uint64_t ni_tstamp, curr_tstamp;

	uvp->recv_mgmt(ni, m, subtype, rxs, rssi, nf);

	if (vap->iv_state == IEEE80211_S_RUN &&
	    (subtype == IEEE80211_FC0_SUBTYPE_BEACON ||
	    subtype == IEEE80211_FC0_SUBTYPE_PROBE_RESP)) {
		ni_tstamp = le64toh(ni->ni_tstamp.tsf);
		URTWM_LOCK(sc);
		urtwm_get_tsf(sc, &curr_tstamp, 0);
		URTWM_UNLOCK(sc);

		if (ni_tstamp >= curr_tstamp)
			(void) ieee80211_ibss_merge(ni);
	}
}

static int
urtwm_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate, int arg)
{
	struct urtwm_vap *uvp = URTWM_VAP(vap);
	struct ieee80211com *ic = vap->iv_ic;
	struct urtwm_softc *sc = ic->ic_softc;
	struct ieee80211_node *ni;
	enum ieee80211_state ostate;
	uint32_t reg;
	uint8_t mode;
	int error = 0;

	ostate = vap->iv_state;
	URTWM_DPRINTF(sc, URTWM_DEBUG_STATE, "%s -> %s\n",
	    ieee80211_state_name[ostate], ieee80211_state_name[nstate]);

	IEEE80211_UNLOCK(ic);
	URTWM_LOCK(sc);
	if (ostate == IEEE80211_S_RUN) {
#ifdef URTWM_TODO
		/* Stop calibration. */
		callout_stop(&sc->sc_calib_to);
#endif

		if (vap->iv_opmode == IEEE80211_M_IBSS) {
			/* Stop periodical TSF synchronization. */
			callout_stop(&uvp->tsf_sync_adhoc);
		}

		/* Turn link LED off. */
		urtwm_set_led(sc, URTWM_LED_LINK, 0);

		/* Set media status to 'No Link'. */
		urtwm_set_mode(sc, R92C_MSR_NOLINK, 0);

		/* Stop Rx of data frames. */
		urtwm_write_2(sc, R92C_RXFLTMAP2, 0);

		/* Disable TSF synchronization / beaconing. */
		urtwm_setbits_1(sc, R92C_BCN_CTRL, R92C_BCN_CTRL_EN_BCN,
		    R92C_BCN_CTRL_DIS_TSF_UDT0);

		/* Reset TSF. */
		urtwm_write_1(sc, R92C_DUAL_TSF_RST, R92C_DUAL_TSF_RST0);

		/* Reset EDCA parameters. */
		urtwm_write_4(sc, R92C_EDCA_VO_PARAM, 0x002f3217);
		urtwm_write_4(sc, R92C_EDCA_VI_PARAM, 0x005e4317);
		urtwm_write_4(sc, R92C_EDCA_BE_PARAM, 0x00105320);
		urtwm_write_4(sc, R92C_EDCA_BK_PARAM, 0x0000a444);
	}

	switch (nstate) {
	case IEEE80211_S_SCAN:
		/* Pause AC Tx queues. */
		urtwm_setbits_1(sc, R92C_TXPAUSE, 0, R92C_TX_QUEUE_AC);
		break;
	case IEEE80211_S_RUN:
		if (vap->iv_opmode == IEEE80211_M_MONITOR) {
			/* Turn link LED on. */
			urtwm_set_led(sc, URTWM_LED_LINK, 1);
			break;
		}

		ni = ieee80211_ref_node(vap->iv_bss);

		if (ic->ic_bsschan == IEEE80211_CHAN_ANYC ||
		    ni->ni_chan == IEEE80211_CHAN_ANYC) {
			device_printf(sc->sc_dev,
			    "%s: could not move to RUN state\n", __func__);
			error = EINVAL;
			goto end_run;
		}

		switch (vap->iv_opmode) {
		case IEEE80211_M_STA:
			mode = R92C_MSR_INFRA;
			break;
		case IEEE80211_M_IBSS:
			mode = R92C_MSR_ADHOC;
			break;
		case IEEE80211_M_HOSTAP:
			mode = R92C_MSR_AP;
			break;
		default:
			device_printf(sc->sc_dev, "undefined opmode %d\n",
			    vap->iv_opmode);
			error = EINVAL;
			goto end_run;
		}

		/* Set media status to 'Associated'. */
		urtwm_set_mode(sc, mode, 0);

		/* Set BSSID. */
		urtwm_write_4(sc, R92C_BSSID + 0, le32dec(&ni->ni_bssid[0]));
		urtwm_write_4(sc, R92C_BSSID + 4, le16dec(&ni->ni_bssid[4]));

		/* Enable Rx of data frames. */
		urtwm_write_2(sc, R92C_RXFLTMAP2, 0xffff);

		/* Flush all AC queues. */
		urtwm_write_1(sc, R92C_TXPAUSE, 0);

		/* Set beacon interval. */
		urtwm_write_2(sc, R92C_BCN_INTERVAL, ni->ni_intval);

		/* Allow Rx from our BSSID only. */
		if (ic->ic_promisc == 0) {
			reg = urtwm_read_4(sc, R92C_RCR);

			if (vap->iv_opmode != IEEE80211_M_HOSTAP) {
				reg |= R92C_RCR_CBSSID_DATA;
				if (vap->iv_opmode != IEEE80211_M_IBSS)
					reg |= R92C_RCR_CBSSID_BCN;
			}

			urtwm_write_4(sc, R92C_RCR, reg);
		}

		if (vap->iv_opmode == IEEE80211_M_HOSTAP ||
		    vap->iv_opmode == IEEE80211_M_IBSS) {
			error = urtwm_setup_beacon(sc, ni);
			if (error != 0) {
				device_printf(sc->sc_dev,
				    "unable to push beacon into the chip, "
				    "error %d\n", error);
				goto end_run;
			}
		}

		/* Enable TSF synchronization. */
		urtwm_tsf_sync_enable(sc, vap);

#ifdef URTWM_TODO
		urtwn_write_1(sc, R92C_SIFS_CCK + 1, 10);
		urtwn_write_1(sc, R92C_SIFS_OFDM + 1, 10);
		urtwn_write_1(sc, R92C_SPEC_SIFS + 1, 10);
		urtwn_write_1(sc, R92C_MAC_SPEC_SIFS + 1, 10);
		urtwn_write_1(sc, R92C_R2T_SIFS + 1, 10);
		urtwn_write_1(sc, R92C_T2T_SIFS + 1, 10);
#endif

		/* Turn link LED on. */
		urtwm_set_led(sc, URTWM_LED_LINK, 1);

#ifdef URTWM_TODO
		/* Reset temperature calibration state machine. */
		sc->sc_flags &= ~URTWN_TEMP_MEASURED;
		sc->thcal_lctemp = 0;
		/* Start periodic calibration. */
		callout_reset(&sc->sc_calib_to, 2*hz, urtwn_calib_to, sc);
#endif

end_run:
		ieee80211_free_node(ni);
		break;
	default:
		break;
	}

	URTWM_UNLOCK(sc);
	IEEE80211_LOCK(ic);
	return (error != 0 ? error : uvp->newstate(vap, nstate, arg));
}

#ifdef URTWM_TODO
static void
urtwn_calib_to(void *arg)
{
	struct urtwn_softc *sc = arg;

	/* Do it in a process context. */
	urtwn_cmd_sleepable(sc, NULL, 0, urtwn_calib_cb);
}

static void
urtwn_calib_cb(struct urtwn_softc *sc, union sec_param *data)
{
	/* Do temperature compensation. */
	urtwn_temp_calib(sc);

	if ((urtwn_read_1(sc, R92C_MSR) & R92C_MSR_MASK) != R92C_MSR_NOLINK)
		callout_reset(&sc->sc_calib_to, 2*hz, urtwn_calib_to, sc);
}
#endif	/* URTWA_TODO */

static int8_t
urtwm_get_rssi_cck(struct urtwm_softc *sc, void *physt)
{
	/* XXX the structure a bit wrong */
	struct r88e_rx_cck *cck = (struct r88e_rx_cck *)physt;
	int8_t lna_idx, rssi;

	lna_idx = (cck->agc_rpt & 0xe0) >> 5;
	rssi = -6 - 2*(cck->agc_rpt & 0x1f);	/* Pout - (2 * VGA_idx) */

	switch (lna_idx) {
	case 5:
		rssi -= 32;
		break;
	case 4:
		rssi -= 24;
		break;
	case 2:
		rssi -= 11;
		break;
	case 1:
		rssi += 5;
		break;
	case 0:
		rssi += 21;
		break;
	}

	return (rssi);
}

static int8_t
urtwm_get_rssi_ofdm(struct urtwm_softc *sc, void *physt)
{
	/* XXX reuse path_agc from r88e_rx_cck here */
	struct r92c_rx_phystat *phy = (struct r92c_rx_phystat *)physt;
	int8_t rssi;

	rssi = ((le32toh(phy->phydw1) >> 1) & 0x7f) - 110;

	return (rssi);
}

static int8_t
urtwm_get_rssi(struct urtwm_softc *sc, int rate, void *physt)
{
	int8_t rssi;

	if (URTWM_RATE_IS_CCK(rate))
		rssi = urtwm_get_rssi_cck(sc, physt);
	else	/* OFDM/HT. */
		rssi = urtwm_get_rssi_ofdm(sc, physt);

	return (rssi);
}

static void
urtwm_tx_protection(struct urtwm_softc *sc, struct r88a_tx_desc *txd,
    enum ieee80211_protmode mode)
{

	switch (mode) {
	case IEEE80211_PROT_CTSONLY:
		txd->txdw3 |= htole32(R88A_TXDW3_CTS2SELF);
		break;
	case IEEE80211_PROT_RTSCTS:
		txd->txdw3 |= htole32(R88A_TXDW3_RTSEN);
		break;
	default:
		break;
	}

	if (mode == IEEE80211_PROT_CTSONLY ||
	    mode == IEEE80211_PROT_RTSCTS) {
		txd->txdw3 |= htole32(R88A_TXDW3_HWRTSEN);

		/* XXX TODO: rtsrate is configurable? 24mbit may
		 * be a bit high for RTS rate? */
		txd->txdw4 |= htole32(SM(R88A_TXDW4_RTSRATE,
		    URTWM_RIDX_OFDM24));
		/* RTS rate fallback limit (max). */
		txd->txdw4 |= htole32(SM(R88A_TXDW4_RTSRATE_FB_LMT, 0xf));
	}
}

static void
urtwm_tx_raid(struct urtwm_softc *sc, struct r88a_tx_desc *txd,
    struct ieee80211_node *ni, int ismcast)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211_channel *c = ic->ic_curchan;
	enum ieee80211_phymode mode;
	uint8_t raid;

	mode = ic->ic_curmode;
	if (mode == IEEE80211_MODE_AUTO)
		mode = ieee80211_chan2mode(c);

	/* NB: group addressed frames are done at 11bg rates for now */
	/*
	 * XXX TODO: this should be per-node, for 11b versus 11bg
	 * nodes in hostap mode
	 */
	if (ismcast || !(ni->ni_flags & IEEE80211_NODE_HT)) {
		switch (mode) {
		case IEEE80211_MODE_11A:
		case IEEE80211_MODE_11B:
		case IEEE80211_MODE_11G:
			break;
		case IEEE80211_MODE_11NA:
			mode = IEEE80211_MODE_11A;
			break;
		case IEEE80211_MODE_11NG:
			mode = IEEE80211_MODE_11G;
			break;
		default:
			device_printf(sc->sc_dev, "unknown mode(1) %d!\n",
			    ic->ic_curmode);
			return;
		}
	}

	switch (mode) {
	case IEEE80211_MODE_11A:
		raid = R88A_RAID_11G;
		break;
	case IEEE80211_MODE_11B:
		raid = R88A_RAID_11B;
		break;
	case IEEE80211_MODE_11G:
		raid = R88A_RAID_11BG;
		break;
	case IEEE80211_MODE_11NA:
		if (sc->ntxchains == 1)
			raid = R88A_RAID_11GN_1;
		else
			raid = R88A_RAID_11GN_2;
		break;
	case IEEE80211_MODE_11NG:
		if (sc->ntxchains == 1) {
			if (IEEE80211_IS_CHAN_HT40(c))
				raid = R88A_RAID_11BGN_1_40;
			else
				raid = R88A_RAID_11BGN_1;
		} else {
			if (IEEE80211_IS_CHAN_HT40(c))
				raid = R88A_RAID_11BGN_2_40;
			else
				raid = R88A_RAID_11BGN_2;
		}
		break;
	default:
		/* TODO: 80 MHz / 11ac */
		device_printf(sc->sc_dev, "unknown mode(2) %d!\n", mode);
		return;
	}

	txd->txdw1 |= htole32(SM(R88A_TXDW1_RAID, raid));
}

static int
urtwm_tx_data(struct urtwm_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m, struct urtwm_data *data)
{
	const struct ieee80211_txparam *tp;
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211vap *vap = ni->ni_vap;
	struct ieee80211_key *k = NULL;
	struct ieee80211_channel *chan;
	struct ieee80211_frame *wh;
	struct r88a_tx_desc *txd;
	uint8_t macid, rate, ridx, type, tid, qos, qsel;
	int hasqos, ismcast;

	URTWM_ASSERT_LOCKED(sc);

	wh = mtod(m, struct ieee80211_frame *);
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	hasqos = IEEE80211_QOS_HAS_SEQ(wh);
	ismcast = IEEE80211_IS_MULTICAST(wh->i_addr1);

	/* Select TX ring for this frame. */
	if (hasqos) {
		qos = ((const struct ieee80211_qosframe *)wh)->i_qos[0];
		tid = qos & IEEE80211_QOS_TID;
	} else {
		qos = 0;
		tid = 0;
	}

	chan = (ni->ni_chan != IEEE80211_CHAN_ANYC) ?
		ni->ni_chan : ic->ic_curchan;
	tp = &vap->iv_txparms[ieee80211_chan2mode(chan)];

	/* Choose a TX rate index. */
	if (type == IEEE80211_FC0_TYPE_MGT)
		rate = tp->mgmtrate;
	else if (ismcast)
		rate = tp->mcastrate;
	else if (tp->ucastrate != IEEE80211_FIXED_RATE_NONE)
		rate = tp->ucastrate;
	else if (m->m_flags & M_EAPOL)
		rate = tp->mgmtrate;
	else {
		if (URTWM_CHIP_HAS_RATECTL(sc)) {
			/* XXX pass pktlen */
			(void) ieee80211_ratectl_rate(ni, NULL, 0);
			rate = ni->ni_txrate;
		} else {
			/* XXX TODO: drop the default rate for 11b/11g? */
			if (ni->ni_flags & IEEE80211_NODE_HT)
				rate = IEEE80211_RATE_MCS | 0x4; /* MCS4 */
			else if (ic->ic_curmode != IEEE80211_MODE_11B)
				rate = 108;
			else
				rate = 22;
		}
	}

	ridx = rate2ridx(rate);

	if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED) {
		k = ieee80211_crypto_encap(ni, m);
		if (k == NULL) {
			device_printf(sc->sc_dev,
			    "ieee80211_crypto_encap returns NULL.\n");
			return (ENOBUFS);
		}

		/* in case packet header moved, reset pointer */
		wh = mtod(m, struct ieee80211_frame *);
	}

	/* Fill Tx descriptor. */
	txd = (struct r88a_tx_desc *)data->buf;
	memset(txd, 0, sizeof(*txd));

	txd->offset = sizeof(*txd);
	txd->flags0 = R88A_FLAGS0_LSG | R88A_FLAGS0_FSG | R88A_FLAGS0_OWN;
	if (ismcast)
		txd->flags0 |= R88A_FLAGS0_BMCAST;

	if (!ismcast) {
		/* Unicast frame, check if an ACK is expected. */
		if (!qos || (qos & IEEE80211_QOS_ACKPOLICY) !=
		    IEEE80211_QOS_ACKPOLICY_NOACK) {
			txd->txdw4 = htole32(R88A_TXDW4_RETRY_LMT_ENA);
			txd->txdw4 |= htole32(SM(R88A_TXDW4_RETRY_LMT,
			    tp->maxretry));
		}

#ifdef URTWM_TODO
		struct urtwm_node *un = URTWM_NODE(ni);
		macid = un->id;
#endif
		macid = URTWM_MACID_BSS;

		if (type == IEEE80211_FC0_TYPE_DATA) {
			qsel = tid % URTWM_MAX_TID;

#ifdef URTWM_TODO
			txd->txdw2 |= htole32(
			    R88A_TXDW2_AGGBK |
			    R88A_TXDW2_CCX_RPT);
#endif
			txd->txdw2 |= htole32(R88A_TXDW2_AGGBK);

			if (ic->ic_flags & IEEE80211_F_SHPREAMBLE)
				txd->txdw5 |= htole32(R88A_TXDW5_SHPRE);

			if (rate & IEEE80211_RATE_MCS) {
				urtwm_tx_protection(sc, txd,
				    ic->ic_htprotmode);
			} else if (ic->ic_flags & IEEE80211_F_USEPROT)
				urtwm_tx_protection(sc, txd, ic->ic_protmode);

			/* Data rate fallback limit (max). */
			txd->txdw4 |= htole32(SM(R88A_TXDW4_DATARATE_FB_LMT,
			    0x1f));
		} else	/* IEEE80211_FC0_TYPE_MGT */
			qsel = R88A_TXDW1_QSEL_MGNT;
	} else {
		macid = URTWM_MACID_BC;
		qsel = R88A_TXDW1_QSEL_MGNT;
	}

	txd->txdw1 |= htole32(SM(R88A_TXDW1_QSEL, qsel));

	/* XXX TODO: 40MHZ flag? */
	/* XXX TODO: AMPDU flag? (AGG_ENABLE or AGG_BREAK?) Density shift? */
	/* XXX Short preamble? */
	/* XXX Short-GI? */

	txd->txdw1 |= htole32(SM(R88A_TXDW1_MACID, macid));
	txd->txdw4 |= htole32(SM(R88A_TXDW4_DATARATE, ridx));
	urtwm_tx_raid(sc, txd, ni, ismcast);

	/* XXX no rate adaptation yet. */
#ifdef URTWM_TODO
	/* Force this rate if needed. */
	if (URTWM_CHIP_HAS_RATECTL(sc) || ismcast ||
	    (tp->ucastrate != IEEE80211_FIXED_RATE_NONE) ||
	    (m->m_flags & M_EAPOL) || type != IEEE80211_FC0_TYPE_DATA)
#endif
		txd->txdw3 |= htole32(R88A_TXDW3_DRVRATE);

	if (!hasqos) {
		/* Use HW sequence numbering for non-QoS frames. */
		txd->txdw8 |= htole32(R88A_TXDW8_HWSEQ_EN);
	} else {
		/* Set sequence number. */
		txd->txdw9 |= htole32(SM(R88A_TXDW9_SEQ,
		    M_SEQNO_GET(m) % IEEE80211_SEQ_RANGE));
	}

	if (k != NULL && !(k->wk_flags & IEEE80211_KEY_SWCRYPT)) {
		uint8_t cipher;

		switch (k->wk_cipher->ic_cipher) {
		case IEEE80211_CIPHER_WEP:
		case IEEE80211_CIPHER_TKIP:
			cipher = R88A_TXDW1_CIPHER_RC4;
			break;
		case IEEE80211_CIPHER_AES_CCM:
			cipher = R88A_TXDW1_CIPHER_AES;
			break;
		default:
			device_printf(sc->sc_dev, "%s: unknown cipher %d\n",
			    __func__, k->wk_cipher->ic_cipher);
			return (EINVAL);
		}

		txd->txdw1 |= htole32(SM(R88A_TXDW1_CIPHER, cipher));
	}

	if (ieee80211_radiotap_active_vap(vap)) {
		struct urtwm_tx_radiotap_header *tap = &sc->sc_txtap;

		tap->wt_flags = 0;
		if (k != NULL)
			tap->wt_flags |= IEEE80211_RADIOTAP_F_WEP;
		ieee80211_radiotap_tx(vap, m);
	}

	data->ni = ni;

	urtwm_tx_start(sc, m, type, data);

	return (0);
}

static int
urtwm_tx_raw(struct urtwm_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m, struct urtwm_data *data,
    const struct ieee80211_bpf_params *params)
{
	struct ieee80211vap *vap = ni->ni_vap;
	struct ieee80211_key *k = NULL;
	struct ieee80211_frame *wh;
	struct r88a_tx_desc *txd;
	uint8_t cipher, ridx, type;
	int ismcast;

	/* Encrypt the frame if need be. */
	cipher = R88A_TXDW1_CIPHER_NONE;
	if (params->ibp_flags & IEEE80211_BPF_CRYPTO) {
		/* Retrieve key for TX. */
		k = ieee80211_crypto_encap(ni, m);
		if (k == NULL)
			return (ENOBUFS);

		if (!(k->wk_flags & IEEE80211_KEY_SWCRYPT)) {
			switch (k->wk_cipher->ic_cipher) {
			case IEEE80211_CIPHER_WEP:
			case IEEE80211_CIPHER_TKIP:
				cipher = R88A_TXDW1_CIPHER_RC4;
				break;
			case IEEE80211_CIPHER_AES_CCM:
				cipher = R88A_TXDW1_CIPHER_AES;
				break;
			default:
				device_printf(sc->sc_dev,
				    "%s: unknown cipher %d\n",
				    __func__, k->wk_cipher->ic_cipher);
				return (EINVAL);
			}
		}
	}

	/* XXX TODO: 11n checks, matching urtwm_tx_data() */

	wh = mtod(m, struct ieee80211_frame *);
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	ismcast = IEEE80211_IS_MULTICAST(wh->i_addr1);

	/* Fill Tx descriptor. */
	txd = (struct r88a_tx_desc *)data->buf;
	memset(txd, 0, sizeof(*txd));

	txd->offset = sizeof(*txd);
	txd->flags0 |= R88A_FLAGS0_LSG | R88A_FLAGS0_FSG | R88A_FLAGS0_OWN;
	if (ismcast)
		txd->flags0 |= R88A_FLAGS0_BMCAST;

	if ((params->ibp_flags & IEEE80211_BPF_NOACK) == 0) {
		txd->txdw4 = htole32(R88A_TXDW4_RETRY_LMT_ENA);
		txd->txdw4 |= htole32(SM(R88A_TXDW4_RETRY_LMT,
		    params->ibp_try0));
	}
	if (params->ibp_flags & IEEE80211_BPF_SHORTPRE)
		txd->txdw5 |= htole32(R88A_TXDW5_SHPRE);
	if (params->ibp_flags & IEEE80211_BPF_RTS)
		urtwm_tx_protection(sc, txd, IEEE80211_PROT_RTSCTS);
	if (params->ibp_flags & IEEE80211_BPF_CTS)
		urtwm_tx_protection(sc, txd, IEEE80211_PROT_CTSONLY);

	txd->txdw1 |= htole32(SM(R88A_TXDW1_MACID, URTWM_MACID_BC));
	txd->txdw1 |= htole32(SM(R88A_TXDW1_QSEL, R88A_TXDW1_QSEL_MGNT));
	txd->txdw1 |= htole32(SM(R88A_TXDW1_CIPHER, cipher));

	/* Choose a TX rate index. */
	ridx = rate2ridx(params->ibp_rate0);
	txd->txdw4 |= htole32(SM(R88A_TXDW4_DATARATE, ridx));
	txd->txdw4 |= htole32(SM(R88A_TXDW4_DATARATE_FB_LMT, 0x1f));
	txd->txdw3 |= htole32(R88A_TXDW3_DRVRATE);
	urtwm_tx_raid(sc, txd, ni, ismcast);

	if (!IEEE80211_QOS_HAS_SEQ(wh)) {
		/* Use HW sequence numbering for non-QoS frames. */
		txd->txdw8 |= htole32(R88A_TXDW8_HWSEQ_EN);
	} else {
		/* Set sequence number. */
		txd->txdw9 |= htole32(SM(R88A_TXDW9_SEQ,
		    M_SEQNO_GET(m) % IEEE80211_SEQ_RANGE));
	}

	if (ieee80211_radiotap_active_vap(vap)) {
		struct urtwm_tx_radiotap_header *tap = &sc->sc_txtap;

		tap->wt_flags = 0;
		if (k != NULL)
			tap->wt_flags |= IEEE80211_RADIOTAP_F_WEP;
		ieee80211_radiotap_tx(vap, m);
	}

	data->ni = ni;

	urtwm_tx_start(sc, m, type, data);

	return (0);
}

static void
urtwm_tx_start(struct urtwm_softc *sc, struct mbuf *m, uint8_t type,
    struct urtwm_data *data)
{
	struct usb_xfer *xfer;
	struct r88a_tx_desc *txd;
	uint16_t ac;
	int xferlen;

	URTWM_ASSERT_LOCKED(sc);

	ac = M_WME_GETAC(m);

	switch (type) {
	case IEEE80211_FC0_TYPE_CTL:
	case IEEE80211_FC0_TYPE_MGT:
		xfer = sc->sc_xfer[URTWM_BULK_TX_VO];
		break;
	default:
		xfer = sc->sc_xfer[wme2queue[ac].qid];
		break;
	}

	txd = (struct r88a_tx_desc *)data->buf;
	txd->pktlen = htole16(m->m_pkthdr.len);

	/* Compute Tx descriptor checksum. */
	urtwm_tx_checksum(txd);

	xferlen = sizeof(*txd) + m->m_pkthdr.len;
	m_copydata(m, 0, m->m_pkthdr.len, (caddr_t)&txd[1]);

	data->buflen = xferlen;
	if (data->ni != NULL)
		data->m = m;

	STAILQ_INSERT_TAIL(&sc->sc_tx_pending, data, next);
	usbd_transfer_start(xfer);
}

static void
urtwm_tx_checksum(struct r88a_tx_desc *txd)
{
	uint16_t sum = 0;
	int i;

	/* NB: checksum calculation takes into account only first 32 bytes. */
	for (i = 0; i < 32 / 2; i++)
		sum ^= ((uint16_t *)txd)[i];
	txd->txdsum = sum;	/* NB: already little endian. */
}

static int
urtwm_transmit(struct ieee80211com *ic, struct mbuf *m)
{
	struct urtwm_softc *sc = ic->ic_softc;
	int error;

	URTWM_LOCK(sc);
	if ((sc->sc_flags & URTWM_RUNNING) == 0) {
		URTWM_UNLOCK(sc);
		return (ENXIO);
	}
	error = mbufq_enqueue(&sc->sc_snd, m);
	if (error) {
		URTWM_UNLOCK(sc);
		return (error);
	}
	urtwm_start(sc);
	URTWM_UNLOCK(sc);

	return (0);
}

static void
urtwm_start(struct urtwm_softc *sc)
{
	struct ieee80211_node *ni;
	struct mbuf *m;
	struct urtwm_data *bf;

	URTWM_ASSERT_LOCKED(sc);
	while ((m = mbufq_dequeue(&sc->sc_snd)) != NULL) {
		bf = urtwm_getbuf(sc);
		if (bf == NULL) {
			mbufq_prepend(&sc->sc_snd, m);
			break;
		}
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		m->m_pkthdr.rcvif = NULL;

		URTWM_DPRINTF(sc, URTWM_DEBUG_XMIT,
		    "%s: called; m %p, ni %p\n", __func__, m, ni);

		if (urtwm_tx_data(sc, ni, m, bf) != 0) {
			if_inc_counter(ni->ni_vap->iv_ifp,
			    IFCOUNTER_OERRORS, 1);
			STAILQ_INSERT_HEAD(&sc->sc_tx_inactive, bf, next);
			m_freem(m);
#ifdef D4054
			ieee80211_tx_watchdog_refresh(ni->ni_ic, -1, 0);
#endif
			ieee80211_free_node(ni);
			break;
		}
	}
}

static void
urtwm_parent(struct ieee80211com *ic)
{
	struct urtwm_softc *sc = ic->ic_softc;

	URTWM_LOCK(sc);
	if (sc->sc_flags & URTWM_DETACHED) {
		URTWM_UNLOCK(sc);
		return;
	}
	URTWM_UNLOCK(sc);

	if (ic->ic_nrunning > 0) {
		if (urtwm_init(sc) != 0) {
			struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
			if (vap != NULL)
				ieee80211_stop(vap);
		} else
			ieee80211_start_all(ic);
	} else
		urtwm_stop(sc);
}

static int
urtwm_power_on(struct urtwm_softc *sc)
{
#define URTWM_CHK(res) do {			\
	if (res != USB_ERR_NORMAL_COMPLETION)	\
		return (EIO);			\
} while(0)
	int ntries;

	/* Clear suspend and power down bits.*/
	URTWM_CHK(urtwm_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_AFSM_HSUS | R92C_APS_FSMCO_APDM_HPDN, 0, 1));

	/* Disable GPIO9 as EXT WAKEUP. */
	URTWM_CHK(urtwm_setbits_1(sc, R92C_GPIO_INTM + 2, 0x01, 0));

	/* Enable WL suspend. */
	URTWM_CHK(urtwm_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_AFSM_HSUS | R92C_APS_FSMCO_AFSM_PCIE, 0, 1));

	/* Enable LDOA12 MACRO block for all interfaces. */
	URTWM_CHK(urtwm_setbits_1(sc, R92C_LDOA15_CTRL, 0, R92C_LDOA15_CTRL_EN));

	/* Disable BT_GPS_SEL pins. */
	URTWM_CHK(urtwm_setbits_1(sc, 0x067, 0x10, 0));

	/* 1 ms delay. */
	urtwm_delay(sc, 1000);

	/* Release analog Ips to digital isolation. */
	URTWM_CHK(urtwm_setbits_1(sc, R92C_SYS_ISO_CTRL,
	    R92C_SYS_ISO_CTRL_IP2MAC, 0));

	/* Disable SW LPS and WL suspend. */
	URTWM_CHK(urtwm_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_APFM_RSM |
	    R92C_APS_FSMCO_AFSM_HSUS |
	    R92C_APS_FSMCO_AFSM_PCIE, 0, 1));

	/* Wait for power ready bit. */
	for (ntries = 0; ntries < 5000; ntries++) {
		if (urtwm_read_4(sc, R92C_APS_FSMCO) & R92C_APS_FSMCO_SUS_HOST)
			break;
		urtwm_delay(sc, 10);
	}
	if (ntries == 5000) {
		device_printf(sc->sc_dev,
		    "timeout waiting for chip power up\n");
		return (ETIMEDOUT);
	}

	/* Release WLON reset. */
	URTWM_CHK(urtwm_setbits_1_shift(sc, R92C_APS_FSMCO, 0,
	    R92C_APS_FSMCO_RDY_MACON, 2));

	/* Disable HWPDN. */
	URTWM_CHK(urtwm_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_APDM_HPDN, 0, 1));

	/* Disable WL suspend. */
	URTWM_CHK(urtwm_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_AFSM_HSUS | R92C_APS_FSMCO_AFSM_PCIE, 0, 1));

	URTWM_CHK(urtwm_setbits_1_shift(sc, R92C_APS_FSMCO, 0,
	    R92C_APS_FSMCO_APFM_ONMAC, 1));
	for (ntries = 0; ntries < 5000; ntries++) {
		if (!(urtwm_read_2(sc, R92C_APS_FSMCO) &
		    R92C_APS_FSMCO_APFM_ONMAC))
			break;
		urtwm_delay(sc, 10);
	}
	if (ntries == 5000)
		return (ETIMEDOUT);

	/* Switch DPDT_SEL_P output from WL BB. */
	URTWM_CHK(urtwm_setbits_1(sc, R92C_LEDCFG3, 0, 0x01));

	/* switch for PAPE_G/PAPE_A from WL BB; switch LNAON from WL BB. */
	URTWM_CHK(urtwm_setbits_1(sc, 0x067, 0, 0x30));

	URTWM_CHK(urtwm_setbits_1(sc, 0x025, 0x40, 0));

	/* Enable falling edge triggering interrupt. */
	URTWM_CHK(urtwm_setbits_1(sc, R92C_GPIO_INTM + 1, 0, 0x02));

	/* Enable GPIO9 interrupt mode. */
	URTWM_CHK(urtwm_setbits_1(sc, 0x063, 0, 0x02));

	/* Enable GPIO9 input mode. */
	URTWM_CHK(urtwm_setbits_1(sc, 0x062, 0x02, 0));

	/* Enable HSISR GPIO interrupt. */
	URTWM_CHK(urtwm_setbits_1(sc, R92C_HSIMR, 0, 0x01));

	/* Enable HSISR GPIO9 interrupt. */
	URTWM_CHK(urtwm_setbits_1(sc, R92C_HSIMR + 2, 0, 0x02));

	/* XTAL trim. */
	URTWM_CHK(urtwm_setbits_1(sc, R92C_APE_PLL_CTRL_EXT + 2, 0xFF, 0x82));

	URTWM_CHK(urtwm_setbits_1(sc, R92C_AFE_MISC, 0, 0x40));

	/* Enable MAC DMA/WMAC/SCHEDULE/SEC blocks. */
	URTWM_CHK(urtwm_write_2(sc, R92C_CR, 0x0000));
	URTWM_CHK(urtwm_setbits_2(sc, R92C_CR, 0,
	    R92C_CR_HCI_TXDMA_EN | R92C_CR_TXDMA_EN |
	    R92C_CR_HCI_RXDMA_EN | R92C_CR_RXDMA_EN |
	    R92C_CR_PROTOCOL_EN | R92C_CR_SCHEDULE_EN |
	    R92C_CR_ENSEC | R92C_CR_CALTMR_EN));

	if (urtwm_read_4(sc, R92C_SYS_CFG) & R92C_SYS_CFG_TRP_BT_EN)
		URTWM_CHK(urtwm_setbits_1(sc, 0x07C, 0, 0x40));

	return (0);
#undef URTWM_CHK
}

static void
urtwm_power_off(struct urtwm_softc *sc)
{
	int ntries;

	/* Disable any kind of TX reports. */
	urtwm_setbits_1(sc, R88E_TX_RPT_CTRL,
	    R88E_TX_RPT1_ENA | R88E_TX_RPT2_ENA, 0);

	/* Stop Rx. */
	urtwm_write_1(sc, R92C_CR, 0);

	/* Move card to Low Power state. */
	/* Block all Tx queues. */
	urtwm_write_1(sc, R92C_TXPAUSE, R92C_TX_QUEUE_ALL);

	for (ntries = 0; ntries < 5000; ntries++) {
		/* Should be zero if no packet is transmitting. */
		if (urtwm_read_4(sc, R88E_SCH_TXCMD) == 0)
			break;

		urtwm_delay(sc, 10);
	}
	if (ntries == 5000) {
		device_printf(sc->sc_dev, "%s: failed to block Tx queues\n",
		    __func__);
		return;
	}

	/* CCK and OFDM are disabled, and clock are gated. */
	urtwm_setbits_1(sc, R92C_SYS_FUNC_EN, R92C_SYS_FUNC_EN_BBRSTB, 0);

	urtwm_delay(sc, 1);

	/* Reset whole BB. */
	urtwm_setbits_1(sc, R92C_SYS_FUNC_EN, R92C_SYS_FUNC_EN_BB_GLB_RST, 0);

	/* Reset MAC TRX. */
	urtwm_write_1(sc, R92C_CR,
	    R92C_CR_HCI_TXDMA_EN | R92C_CR_HCI_RXDMA_EN);

	/* check if removed later. (?) */
	urtwm_setbits_1_shift(sc, R92C_CR, R92C_CR_ENSWBCN, 0, 1);

	/* Respond TxOK to scheduler */
	urtwm_setbits_1(sc, R92C_DUAL_TSF_RST, 0, R92C_DUAL_TSF_RST_TXOK);

	/* firmware reset code resides here. */

	/* Reset MCU. */
	urtwm_setbits_1_shift(sc, R92C_SYS_FUNC_EN, R92C_SYS_FUNC_EN_CPUEN,
	    0, 1);
	urtwm_write_1(sc, R92C_MCUFWDL, 0);

	/* Move card to Disabled state. */
	/* Turn off RF. */
	urtwm_write_1(sc, R92C_RF_CTRL, 0);

	urtwm_setbits_1(sc, R92C_LEDCFG3, 0x01, 0);

	/* Enable rising edge triggering interrupt. */
	urtwm_setbits_1(sc, R92C_GPIO_INTM + 1, 0x02, 0);

	/* Release WLON reset. */
	urtwm_setbits_1_shift(sc, R92C_APS_FSMCO, 0,
	    R92C_APS_FSMCO_RDY_MACON, 2);

	/* Turn off MAC by HW state machine */
	urtwm_setbits_1_shift(sc, R92C_APS_FSMCO, 0, R92C_APS_FSMCO_APFM_OFF,
	    1);
	for (ntries = 0; ntries < 5000; ntries++) {
		/* Wait until it will be disabled. */
		if ((urtwm_read_2(sc, R92C_APS_FSMCO) &
		    R92C_APS_FSMCO_APFM_OFF) == 0)
			break;

		urtwm_delay(sc, 10);
	}
	if (ntries == 5000) {
		device_printf(sc->sc_dev, "%s: could not turn off MAC\n",
		    __func__);
		return;
	}

	/* Analog Ips to digital isolation. */
        urtwm_setbits_1(sc, R92C_SYS_ISO_CTRL, 0, R92C_SYS_ISO_CTRL_IP2MAC);

	/* Disable LDOA12 MACRO block. */
	urtwm_setbits_1(sc, R92C_LDOA15_CTRL, R92C_LDOA15_CTRL_EN, 0);

	/* Enable WL suspend. */
	urtwm_setbits_1_shift(sc, R92C_APS_FSMCO, R92C_APS_FSMCO_AFSM_PCIE,
	    R92C_APS_FSMCO_AFSM_HSUS, 1);

	/* Enable GPIO9 as EXT WAKEUP. */
	urtwm_setbits_1(sc, R92C_GPIO_INTM + 2, 0, 0x01);
}

static int
urtwm_llt_init(struct urtwm_softc *sc)
{
	int i, error, page_count, pktbuf_count;

	page_count = R88A_TX_PAGE_COUNT;
	pktbuf_count = R88A_TXPKTBUF_COUNT;

	/* Reserve pages [0; page_count]. */
	for (i = 0; i < page_count; i++) {
		if ((error = urtwm_llt_write(sc, i, i + 1)) != 0)
			return (error);
	}
	/* NB: 0xff indicates end-of-list. */
	if ((error = urtwm_llt_write(sc, i, 0xff)) != 0)
		return (error);
	/*
	 * Use pages [page_count + 1; pktbuf_count - 1]
	 * as ring buffer.
	 */
	for (++i; i < pktbuf_count - 1; i++) {
		if ((error = urtwm_llt_write(sc, i, i + 1)) != 0)
			return (error);
	}
	/* Make the last page point to the beginning of the ring buffer. */
	error = urtwm_llt_write(sc, i, page_count + 1);
	return (error);
}

#ifdef URTWM_TODO
#ifndef URTWN_WITHOUT_UCODE
static void
urtwn_fw_reset(struct urtwn_softc *sc)
{
	uint16_t reg;
	int ntries;

	/* Tell 8051 to reset itself. */
	urtwn_write_1(sc, R92C_HMETFR + 3, 0x20);

	/* Wait until 8051 resets by itself. */
	for (ntries = 0; ntries < 100; ntries++) {
		reg = urtwn_read_2(sc, R92C_SYS_FUNC_EN);
		if (!(reg & R92C_SYS_FUNC_EN_CPUEN))
			return;
		urtwn_ms_delay(sc);
	}
	/* Force 8051 reset. */
	urtwn_write_2(sc, R92C_SYS_FUNC_EN, reg & ~R92C_SYS_FUNC_EN_CPUEN);
}

static void
urtwn_r88e_fw_reset(struct urtwn_softc *sc)
{
	uint16_t reg;

	reg = urtwn_read_2(sc, R92C_SYS_FUNC_EN);
	urtwn_write_2(sc, R92C_SYS_FUNC_EN, reg & ~R92C_SYS_FUNC_EN_CPUEN);
	urtwn_write_2(sc, R92C_SYS_FUNC_EN, reg | R92C_SYS_FUNC_EN_CPUEN);
}

static int
urtwn_fw_loadpage(struct urtwn_softc *sc, int page, const uint8_t *buf, int len)
{
	uint32_t reg;
	usb_error_t error = USB_ERR_NORMAL_COMPLETION;
	int off, mlen;

	reg = urtwn_read_4(sc, R92C_MCUFWDL);
	reg = RW(reg, R92C_MCUFWDL_PAGE, page);
	urtwn_write_4(sc, R92C_MCUFWDL, reg);

	off = R92C_FW_START_ADDR;
	while (len > 0) {
		if (len > 196)
			mlen = 196;
		else if (len > 4)
			mlen = 4;
		else
			mlen = 1;
		/* XXX fix this deconst */
		error = urtwn_write_region_1(sc, off,
		    __DECONST(uint8_t *, buf), mlen);
		if (error != USB_ERR_NORMAL_COMPLETION)
			break;
		off += mlen;
		buf += mlen;
		len -= mlen;
	}
	return (error);
}

static int
urtwn_load_firmware(struct urtwn_softc *sc)
{
	const struct firmware *fw;
	const struct r92c_fw_hdr *hdr;
	const char *imagename;
	const u_char *ptr;
	size_t len;
	uint32_t reg;
	int mlen, ntries, page, error;

	URTWN_UNLOCK(sc);
	/* Read firmware image from the filesystem. */
	if (sc->chip & URTWN_CHIP_88E)
		imagename = "urtwn-rtl8188eufw";
	else if ((sc->chip & (URTWN_CHIP_UMC_A_CUT | URTWN_CHIP_92C)) ==
		    URTWN_CHIP_UMC_A_CUT)
		imagename = "urtwn-rtl8192cfwU";
	else
		imagename = "urtwn-rtl8192cfwT";

	fw = firmware_get(imagename);
	URTWN_LOCK(sc);
	if (fw == NULL) {
		device_printf(sc->sc_dev,
		    "failed loadfirmware of file %s\n", imagename);
		return (ENOENT);
	}

	len = fw->datasize;

	if (len < sizeof(*hdr)) {
		device_printf(sc->sc_dev, "firmware too short\n");
		error = EINVAL;
		goto fail;
	}
	ptr = fw->data;
	hdr = (const struct r92c_fw_hdr *)ptr;
	/* Check if there is a valid FW header and skip it. */
	if ((le16toh(hdr->signature) >> 4) == 0x88c ||
	    (le16toh(hdr->signature) >> 4) == 0x88e ||
	    (le16toh(hdr->signature) >> 4) == 0x92c) {
		URTWN_DPRINTF(sc, URTWN_DEBUG_FIRMWARE,
		    "FW V%d.%d %02d-%02d %02d:%02d\n",
		    le16toh(hdr->version), le16toh(hdr->subversion),
		    hdr->month, hdr->date, hdr->hour, hdr->minute);
		ptr += sizeof(*hdr);
		len -= sizeof(*hdr);
	}

	if (urtwn_read_1(sc, R92C_MCUFWDL) & R92C_MCUFWDL_RAM_DL_SEL) {
		if (sc->chip & URTWN_CHIP_88E)
			urtwn_r88e_fw_reset(sc);
		else
			urtwn_fw_reset(sc);
		urtwn_write_1(sc, R92C_MCUFWDL, 0);
	}

	if (!(sc->chip & URTWN_CHIP_88E)) {
		urtwn_write_2(sc, R92C_SYS_FUNC_EN,
		    urtwn_read_2(sc, R92C_SYS_FUNC_EN) |
		    R92C_SYS_FUNC_EN_CPUEN);
	}
	urtwn_write_1(sc, R92C_MCUFWDL,
	    urtwn_read_1(sc, R92C_MCUFWDL) | R92C_MCUFWDL_EN);
	urtwn_write_1(sc, R92C_MCUFWDL + 2,
	    urtwn_read_1(sc, R92C_MCUFWDL + 2) & ~0x08);

	/* Reset the FWDL checksum. */
	urtwn_write_1(sc, R92C_MCUFWDL,
	    urtwn_read_1(sc, R92C_MCUFWDL) | R92C_MCUFWDL_CHKSUM_RPT);

	for (page = 0; len > 0; page++) {
		mlen = min(len, R92C_FW_PAGE_SIZE);
		error = urtwn_fw_loadpage(sc, page, ptr, mlen);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "could not load firmware page\n");
			goto fail;
		}
		ptr += mlen;
		len -= mlen;
	}
	urtwn_write_1(sc, R92C_MCUFWDL,
	    urtwn_read_1(sc, R92C_MCUFWDL) & ~R92C_MCUFWDL_EN);
	urtwn_write_1(sc, R92C_MCUFWDL + 1, 0);

	if (!(sc->chip & URTWN_CHIP_88E)) { 
		/* Wait for checksum report. */
		for (ntries = 0; ntries < 1000; ntries++) {
			if (urtwn_read_4(sc, R92C_MCUFWDL) &
			    R92C_MCUFWDL_CHKSUM_RPT)
				break;
			urtwn_ms_delay(sc);
		}
		if (ntries == 1000) {
			device_printf(sc->sc_dev,
			    "timeout waiting for checksum report\n");
			error = ETIMEDOUT;
			goto fail;
		}
	}

	reg = urtwn_read_4(sc, R92C_MCUFWDL);
	reg = (reg & ~R92C_MCUFWDL_WINTINI_RDY) | R92C_MCUFWDL_RDY;
	urtwn_write_4(sc, R92C_MCUFWDL, reg);
	if (sc->chip & URTWN_CHIP_88E)
		urtwn_r88e_fw_reset(sc);
	/* Wait for checksum report / firmware readiness. */
	for (ntries = 0; ntries < 1000; ntries++) {
		if ((urtwn_read_4(sc, R92C_MCUFWDL) &
		    (R92C_MCUFWDL_WINTINI_RDY | R92C_MCUFWDL_CHKSUM_RPT)) ==
		    (R92C_MCUFWDL_WINTINI_RDY | R92C_MCUFWDL_CHKSUM_RPT))
			break;
		urtwn_ms_delay(sc);
	}
	if (ntries == 1000) {
		device_printf(sc->sc_dev,
		    "timeout waiting for firmware readiness\n");
		error = ETIMEDOUT;
		goto fail;
	}
fail:
	firmware_put(fw, FIRMWARE_UNLOAD);
	return (error);
}
#endif
#endif	/* URTWM_TODO */

static int
urtwm_dma_init(struct urtwm_softc *sc)
{
#define URTWM_CHK(res) do {			\
	if (res != USB_ERR_NORMAL_COMPLETION)	\
		return (EIO);			\
} while(0)
	uint16_t reg;
	int hasnq, haslq, nqueues;
	int error, pagecount, npubqpages, nqpages, nrempages, tx_boundary;

	/* Initialize LLT table. */
	error = urtwm_llt_init(sc);
	if (error != 0)
		return (error);

	/* Get Tx queues to USB endpoints mapping. */
	hasnq = haslq = 0;
	switch (sc->ntx) {
	case 4:
	case 3:
		haslq = 1;
		/* FALLTHROUGH */
	case 2:
		hasnq = 1;
		/* FALLTHROUGH */
	default:
		break;
	}

	nqueues = 1 + hasnq + haslq;
	npubqpages = nqpages = nrempages = pagecount = 0;
	pagecount = R88A_TX_PAGE_COUNT;
	npubqpages = R88A_PUBQ_NPAGES;
	tx_boundary = R88A_TX_PAGE_BOUNDARY;

	/* Get the number of pages for each queue. */
	nqpages = (pagecount - npubqpages) / nqueues;

	/* 
	 * The remaining pages are assigned to the high priority
	 * queue.
	 */
	nrempages = (pagecount - npubqpages) % nqueues;

	URTWM_CHK(urtwm_write_1(sc, R92C_RQPN_NPQ, hasnq ? nqpages : 0));
	URTWM_CHK(urtwm_write_4(sc, R92C_RQPN,
	    /* Set number of pages for public queue. */
	    SM(R92C_RQPN_PUBQ, npubqpages) |
	    /* Set number of pages for high priority queue. */
	    SM(R92C_RQPN_HPQ, nqpages + nrempages) |
	    /* Set number of pages for low priority queue. */
	    SM(R92C_RQPN_LPQ, haslq ? nqpages : 0) |
	    /* Load values. */
	    R92C_RQPN_LD));

	/* Initialize TX buffer boundary. */
	URTWM_CHK(urtwm_write_1(sc, R92C_TXPKTBUF_BCNQ_BDNY, tx_boundary));
	URTWM_CHK(urtwm_write_1(sc, R92C_TXPKTBUF_MGQ_BDNY, tx_boundary));
	URTWM_CHK(urtwm_write_1(sc, R92C_TXPKTBUF_WMAC_LBK_BF_HD,
	    tx_boundary));
	URTWM_CHK(urtwm_write_1(sc, R92C_TRXFF_BNDY, tx_boundary));
	URTWM_CHK(urtwm_write_1(sc, R92C_TDECTRL + 1, tx_boundary));
	URTWM_CHK(urtwm_write_1(sc, R88E_TXPKTBUF_BCNQ1_BDNY,
	    tx_boundary + 8));
	URTWM_CHK(urtwm_write_1(sc, R88A_DWBCN1_CTRL + 1, tx_boundary + 8));
	URTWM_CHK(urtwm_setbits_1(sc, R88A_DWBCN1_CTRL + 2, 0,
	    R88A_DWBCN1_CTRL_SEL_EN));

	/* Set queue to USB pipe mapping. */
	switch (nqueues) {
	case 1:
		/* NB: should not happen for RTL8821AU. */
		reg = R92C_TRXDMA_CTRL_QMAP_HQ;
		break;
	case 2:
		reg = R92C_TRXDMA_CTRL_QMAP_HQ_NQ;
		break;
	default:
		reg = R92C_TRXDMA_CTRL_QMAP_3EP;
		break;
	}
	URTWM_CHK(urtwm_setbits_2(sc, R92C_TRXDMA_CTRL,
	    R92C_TRXDMA_CTRL_QMAP_M, reg));

	/* Set Tx/Rx transfer page boundary. */
	URTWM_CHK(urtwm_write_2(sc, R92C_TRXFF_BNDY + 2,
	    R88A_RX_DMA_BUFFER_SIZE - 1));

	/* Set Tx/Rx transfer page size. */
	URTWM_CHK(urtwm_write_1(sc, R92C_PBP,
	    SM(R92C_PBP_PSRX, R92C_PBP_128) |
	    SM(R92C_PBP_PSTX, R92C_PBP_512)));

	return (0);
}

static int
urtwm_mac_init(struct urtwm_softc *sc)
{
	usb_error_t error;
	int i;

	/* Write MAC initialization values. */
	for (i = 0; i < nitems(rtl8821au_mac); i++) {
		error = urtwm_write_1(sc, rtl8821au_mac[i].reg,
		    rtl8821au_mac[i].val);
		if (error != USB_ERR_NORMAL_COMPLETION)
			return (EIO);
	}

	return (0);
}

static void
urtwm_bb_init(struct urtwm_softc *sc)
{
	const struct urtwm_bb_prog *prog;
	int i;

	urtwm_setbits_1(sc, R92C_SYS_FUNC_EN, 0, R92C_SYS_FUNC_EN_USBA);

	/* Enable BB and RF. */
	urtwm_setbits_1(sc, R92C_SYS_FUNC_EN, 0,
	    R92C_SYS_FUNC_EN_BBRSTB | R92C_SYS_FUNC_EN_BB_GLB_RST);

	/* PathA RF Power On. */
	urtwm_write_1(sc, R92C_RF_CTRL,
	    R92C_RF_CTRL_EN | R92C_RF_CTRL_RSTB | R92C_RF_CTRL_SDMRSTB);

	/* PathB RF Power On. */
	urtwm_write_1(sc, R88A_RF_B_CTRL,
	    R92C_RF_CTRL_EN | R92C_RF_CTRL_RSTB | R92C_RF_CTRL_SDMRSTB);

	/* Select BB programming based on board type. */
	if ((sc->pa_type & R88A_ROM_PA_TYPE_EXTERNAL_5GHZ) &&
	    (sc->lna_type & R88A_ROM_LNA_TYPE_EXTERNAL_5GHZ))
		prog = &rtl8821au_ext_5ghz_bb_prog;
	else
		prog = &rtl8821au_bb_prog;

	/* Write BB initialization values. */
	for (i = 0; i < prog->count; i++) {
		urtwm_bb_write(sc, prog->regs[i], prog->vals[i]);
		urtwm_delay(sc, 1);
	}

	/* XXX meshpoint mode? */

	/* Write AGC values. */
	for (i = 0; i < prog->agccount; i++) {
		urtwm_bb_write(sc, 0x81C, prog->agcvals[i]);
		urtwm_delay(sc, 1);
	}

	urtwm_bb_write(sc, R92C_OFDM0_AGCCORE1(0), 0x00000022);
	urtwm_delay(sc, 1);
	urtwm_bb_write(sc, R92C_OFDM0_AGCCORE1(0), 0x00000020);
	urtwm_delay(sc, 1);
}

static void
urtwm_rf_init(struct urtwm_softc *sc)
{
	const struct urtwm_rf_prog *prog;
	int i, j;

	/* Select RF programming based on board type. */
	if (!(sc->pa_type & R88A_ROM_PA_TYPE_EXTERNAL_5GHZ) &&
	    !(sc->lna_type & R88A_ROM_LNA_TYPE_EXTERNAL_5GHZ))
		prog = rtl8821au_rf_prog;
	else if ((sc->pa_type & R88A_ROM_PA_TYPE_EXTERNAL_5GHZ) &&
		 (sc->lna_type & R88A_ROM_LNA_TYPE_EXTERNAL_5GHZ))
		prog = rtl8821au_ext_5ghz_rf_prog;
	else
		prog = rtl8821au_1_rf_prog;

	for (i = 0; i < sc->nrxchains; i++) {
		/* Write RF initialization values for this chain. */
		for (j = 0; j < prog[i].count; j++) {
			switch (prog[i].regs[j]) {
			/*
			 * These are fake RF registers offsets that
			 * indicate a delay is required.
			 */
			case 0xfe:
				urtwm_delay(sc, 50000);
				break;
			case 0xfd:
				urtwm_delay(sc, 5000);
				break;
			case 0xfc:
				urtwm_delay(sc, 1000);
				break;
			case 0xfb:
				urtwm_delay(sc, 50);
				break;
			case 0xfa:
				urtwm_delay(sc, 5);
				break;
			case 0xf9:
				urtwm_delay(sc, 1);
				break;
			default:
				urtwm_rf_write(sc, i, prog[i].regs[j],
				    prog[i].vals[j]);
				urtwm_delay(sc, 1);
				break;
			}
		}
	}
}

static void
urtwm_arfb_init(struct urtwm_softc *sc)
{
	/* ARFB table 9 for 11ac 5G 2SS. */
	urtwm_write_4(sc, R88A_ARFR_5G(0), 0x00000010);
	urtwm_write_4(sc, R88A_ARFR_5G(0) + 4, 0xfffff000);

	/* ARFB table 10 for 11ac 5G 1SS. */
	urtwm_write_4(sc, R88A_ARFR_5G(1), 0x00000010);
	urtwm_write_4(sc, R88A_ARFR_5G(1) + 4, 0x003ff000);

	/* ARFB table 11 for 11ac 2G 1SS. */
	urtwm_write_4(sc, R88A_ARFR_2G(0), 0x00000015);
	urtwm_write_4(sc, R88A_ARFR_2G(0) + 4, 0x003ff000);

	/* ARFB table 12 for 11ac 2G 2SS. */
	urtwm_write_4(sc, R88A_ARFR_2G(1), 0x00000015);
	urtwm_write_4(sc, R88A_ARFR_2G(1) + 4, 0xffcff000);
}

static void
urtwm_band_change(struct urtwm_softc *sc, struct ieee80211_channel *c,
    int force)
{
	uint8_t swing;
	int i;

	/* Check if band was changed. */
	if (!force && IEEE80211_IS_CHAN_5GHZ(c) ^
	    !(urtwm_read_1(sc, R88A_CCK_CHECK) & R88A_CCK_CHECK_5GHZ))
		return;

	if (IEEE80211_IS_CHAN_2GHZ(c)) {
		/* Stop Tx / Rx. */
		urtwm_bb_setbits(sc, R88A_OFDMCCK_EN,
		    R88A_OFDMCCK_EN_CCK | R88A_OFDMCCK_EN_OFDM, 0);

		/* Turn off RF PA and LNA. */
		urtwm_bb_setbits(sc, R88A_RFE_PINMUX(0),
		    R88A_RFE_PINMUX_LNA_MASK, 0x7);
		urtwm_bb_setbits(sc, R88A_RFE_PINMUX(0),
		    R88A_RFE_PINMUX_PA_A_MASK, 0x7);

		if (sc->lna_type & R88A_ROM_LNA_TYPE_EXTERNAL_2GHZ) {
			/* Turn on 2.4 GHz external LNA. */
			urtwm_bb_setbits(sc, R88A_RFE_INV(0), 0, 0x00100000);
			urtwm_bb_setbits(sc, R88A_RFE_INV(0), 0x00400000, 0);
			urtwm_bb_setbits(sc, R88A_RFE_PINMUX(0), 0x7, 0x2);
			urtwm_bb_setbits(sc, R88A_RFE_PINMUX(0), 0x700, 0x200);
		}

		/* Select AGC table. */
		urtwm_bb_setbits(sc, R88A_TX_SCALE(0), 0x0f00, 0);

		/* Write basic rates. */
		/* XXX check ic_curmode. */
		urtwm_setbits_4(sc, R92C_RRSR, R92C_RRSR_RATE_BITMAP_M,
		    0x15d);	/* 1, 5.5, 11, 6, 12, 24 */

		/* Enable CCK. */
		urtwm_bb_setbits(sc, R88A_OFDMCCK_EN, 0,
		    R88A_OFDMCCK_EN_CCK | R88A_OFDMCCK_EN_OFDM);

		urtwm_write_1(sc, R88A_CCK_CHECK, 0);

		swing = sc->tx_bbswing_2g;
	} else if (IEEE80211_IS_CHAN_5GHZ(c)) {
		int ntries;

		urtwm_bb_setbits(sc, R88A_RFE_PINMUX(0),
		    R88A_RFE_PINMUX_LNA_MASK, 0x5);
		urtwm_bb_setbits(sc, R88A_RFE_PINMUX(0),
		    R88A_RFE_PINMUX_PA_A_MASK, 0x4);

		if (sc->lna_type & R88A_ROM_LNA_TYPE_EXTERNAL_2GHZ) {
			/* Bypass 2.4 GHz external LNA. */
			urtwm_bb_setbits(sc, R88A_RFE_INV(0), 0x00100000, 0);
			urtwm_bb_setbits(sc, R88A_RFE_INV(0), 0x00400000, 0);
			urtwm_bb_setbits(sc, R88A_RFE_PINMUX(0), 0, 0x7);
			urtwm_bb_setbits(sc, R88A_RFE_PINMUX(0), 0, 0x700);
		}

		urtwm_write_1(sc, R88A_CCK_CHECK, 0x80);

		for (ntries = 0; ntries < 100; ntries++) {
			if ((urtwm_read_2(sc, R88A_TXPKT_EMPTY) & 0x30) ==
			    0x30)
				break;

			urtwm_delay(sc, 25);
		}
		if (ntries == 100) {
			device_printf(sc->sc_dev,
			    "%s: TXPKT_EMPTY check failed (%04X)\n",
			    __func__, urtwm_read_2(sc, R88A_TXPKT_EMPTY));
		}

		/* Stop Tx / Rx. */
		urtwm_bb_setbits(sc, R88A_OFDMCCK_EN,
		    R88A_OFDMCCK_EN_CCK | R88A_OFDMCCK_EN_OFDM, 0);

		/* Select AGC table. */
		urtwm_bb_setbits(sc, R88A_TX_SCALE(0), 0x0f00, 0x0100);

		/* Write basic rates. */
		/* XXX obtain from net80211. */
		urtwm_setbits_4(sc, R92C_RRSR, R92C_RRSR_RATE_BITMAP_M,
		    0x150);	/* 6, 12, 24 */

		/* Enable OFDM. */
		urtwm_bb_setbits(sc, R88A_OFDMCCK_EN, 0, R88A_OFDMCCK_EN_OFDM);

		swing = sc->tx_bbswing_5g;
	} else {
		KASSERT(0, ("wrong channel flags %08X\n", c->ic_flags));
		return;
	}

	/* XXX PATH_B is set by vendor driver. */
	for (i = 0; i < 2; i++) {
		uint16_t val;

		switch ((swing >> i) & 0x3) {
		case 0:
			val = 0x200;	/* 0 dB	*/
			break;
		case 1:
			val = 0x16a;	/* -3 dB */
			break;
		case 2:
			val = 0x101;	/* -6 dB */
			break;
		case 3:
			val = 0xb6;	/* -9 dB */
			break;
		}

		urtwm_bb_setbits(sc, R88A_TX_SCALE(i), R88A_TX_SCALE_SWING_M,
		    val << R88A_TX_SCALE_SWING_S);
	}
}

static void
urtwm_cam_init(struct urtwm_softc *sc)
{
	/* Invalidate all CAM entries. */
	urtwm_write_4(sc, R92C_CAMCMD,
	    R92C_CAMCMD_POLLING | R92C_CAMCMD_CLR);
}

static int
urtwm_cam_write(struct urtwm_softc *sc, uint32_t addr, uint32_t data)
{
	usb_error_t error;

	error = urtwm_write_4(sc, R92C_CAMWRITE, data);
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	error = urtwm_write_4(sc, R92C_CAMCMD,
	    R92C_CAMCMD_POLLING | R92C_CAMCMD_WRITE |
	    SM(R92C_CAMCMD_ADDR, addr));
	if (error != USB_ERR_NORMAL_COMPLETION)
		return (EIO);

	return (0);
}

static void
urtwm_rxfilter_init(struct urtwm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
	uint32_t rcr;
	uint16_t filter;

	URTWM_ASSERT_LOCKED(sc);

	/* Setup multicast filter. */
	urtwm_set_multi(sc);

	/* Filter for management frames. */
	filter = 0x7f3f;
	switch (vap->iv_opmode) {
	case IEEE80211_M_STA:
		filter &= ~(
		    R92C_RXFLTMAP_SUBTYPE(IEEE80211_FC0_SUBTYPE_ASSOC_REQ) |
		    R92C_RXFLTMAP_SUBTYPE(IEEE80211_FC0_SUBTYPE_REASSOC_REQ) |
		    R92C_RXFLTMAP_SUBTYPE(IEEE80211_FC0_SUBTYPE_PROBE_REQ));
		break;
	case IEEE80211_M_HOSTAP:
		filter &= ~(
		    R92C_RXFLTMAP_SUBTYPE(IEEE80211_FC0_SUBTYPE_ASSOC_RESP) |
		    R92C_RXFLTMAP_SUBTYPE(IEEE80211_FC0_SUBTYPE_REASSOC_RESP));
		break;
	case IEEE80211_M_MONITOR:
	case IEEE80211_M_IBSS:
		break;
	default:
		device_printf(sc->sc_dev, "%s: undefined opmode %d\n",
		    __func__, vap->iv_opmode);
		break;
	}
	urtwm_write_2(sc, R92C_RXFLTMAP0, filter);

	/* Reject all control frames. */
	urtwm_write_2(sc, R92C_RXFLTMAP1, 0x0000);

	/* Reject all data frames. */
	urtwm_write_2(sc, R92C_RXFLTMAP2, 0x0000);

	rcr = R92C_RCR_AM | R92C_RCR_AB | R92C_RCR_APM |
	      R92C_RCR_HTC_LOC_CTRL | R92C_RCR_APP_PHYSTS |
	      R92C_RCR_APP_ICV | R92C_RCR_APP_MIC;

	if (vap->iv_opmode == IEEE80211_M_MONITOR) {
		/* Accept all frames. */
		rcr |= R92C_RCR_ACF | R92C_RCR_ADF | R92C_RCR_AMF |
		       R92C_RCR_AAP;
	}

	/* Set Rx filter. */
	urtwm_write_4(sc, R92C_RCR, rcr);

	if (ic->ic_promisc != 0) {
		/* Update Rx filter. */
		urtwm_set_promisc(sc);
	}
}

static void
urtwm_edca_init(struct urtwm_softc *sc)
{
	/* SIFS */
	urtwm_write_2(sc, R92C_SPEC_SIFS, 0x100a);
	urtwm_write_2(sc, R92C_MAC_SPEC_SIFS, 0x100a);
	urtwm_write_2(sc, R92C_SIFS_CCK, 0x100a);
	urtwm_write_2(sc, R92C_SIFS_OFDM, 0x100a);
	/* TXOP */
	urtwm_write_4(sc, R92C_EDCA_BE_PARAM, 0x005ea42b);
	urtwm_write_4(sc, R92C_EDCA_BK_PARAM, 0x0000a44f);
	urtwm_write_4(sc, R92C_EDCA_VI_PARAM, 0x005ea324);
	urtwm_write_4(sc, R92C_EDCA_VO_PARAM, 0x002fa226);
	/* 80 MHz clock */
	urtwm_write_1(sc, R92C_USTIME_TSF, 0x50);
	urtwm_write_1(sc, R92C_USTIME_EDCA, 0x50);
}

static void
urtwm_mrr_init(struct urtwm_softc *sc)
{
	int i;

	/* Drop rate index by 1 per retry. */
	for (i = 0; i < R88A_MRR_SIZE; i++)
		urtwm_write_1(sc, R92C_DARFRC + i, i + 1);
}

static void
urtwm_write_txpower(struct urtwm_softc *sc, int chain,
    struct ieee80211_channel *c, uint16_t power[URTWM_RIDX_COUNT])
{

	if (IEEE80211_IS_CHAN_2GHZ(c)) {
		/* Write per-CCK rate Tx power. */
		urtwm_bb_write(sc, R88A_TXAGC_CCK11_1(chain),
		    SM(R88A_TXAGC_CCK1,  power[URTWM_RIDX_CCK1]) |
		    SM(R88A_TXAGC_CCK2,  power[URTWM_RIDX_CCK2]) |
		    SM(R88A_TXAGC_CCK55, power[URTWM_RIDX_CCK55]) |
		    SM(R88A_TXAGC_CCK11, power[URTWM_RIDX_CCK11]));
	}

	/* Write per-OFDM rate Tx power. */
	urtwm_bb_write(sc, R88A_TXAGC_OFDM18_6(chain),
	    SM(R88A_TXAGC_OFDM06, power[URTWM_RIDX_OFDM6]) |
	    SM(R88A_TXAGC_OFDM09, power[URTWM_RIDX_OFDM9]) |
	    SM(R88A_TXAGC_OFDM12, power[URTWM_RIDX_OFDM12]) |
	    SM(R88A_TXAGC_OFDM18, power[URTWM_RIDX_OFDM18]));
	urtwm_bb_write(sc, R88A_TXAGC_OFDM54_24(chain),
	    SM(R88A_TXAGC_OFDM24, power[URTWM_RIDX_OFDM24]) |
	    SM(R88A_TXAGC_OFDM36, power[URTWM_RIDX_OFDM36]) |
	    SM(R88A_TXAGC_OFDM48, power[URTWM_RIDX_OFDM48]) |
	    SM(R88A_TXAGC_OFDM54, power[URTWM_RIDX_OFDM54]));

	/* Write per-MCS Tx power. */
	urtwm_bb_write(sc, R88A_TXAGC_MCS3_0(chain),
	    SM(R88A_TXAGC_MCS0, power[URTWM_RIDX_MCS(0)]) |
	    SM(R88A_TXAGC_MCS1, power[URTWM_RIDX_MCS(1)]) |
	    SM(R88A_TXAGC_MCS2, power[URTWM_RIDX_MCS(2)]) |
	    SM(R88A_TXAGC_MCS3, power[URTWM_RIDX_MCS(3)]));
	urtwm_bb_write(sc, R88A_TXAGC_MCS7_4(chain),
	    SM(R88A_TXAGC_MCS4, power[URTWM_RIDX_MCS(4)]) |
	    SM(R88A_TXAGC_MCS5, power[URTWM_RIDX_MCS(5)]) |
	    SM(R88A_TXAGC_MCS6, power[URTWM_RIDX_MCS(6)]) |
	    SM(R88A_TXAGC_MCS7, power[URTWM_RIDX_MCS(7)]));
	urtwm_bb_write(sc, R88A_TXAGC_MCS11_8(chain),
	    SM(R88A_TXAGC_MCS8,  power[URTWM_RIDX_MCS(8)]) |
	    SM(R88A_TXAGC_MCS9,  power[URTWM_RIDX_MCS(9)]) |
	    SM(R88A_TXAGC_MCS10, power[URTWM_RIDX_MCS(10)]) |
	    SM(R88A_TXAGC_MCS11, power[URTWM_RIDX_MCS(11)]));
	urtwm_bb_write(sc, R88A_TXAGC_MCS15_12(chain),
	    SM(R88A_TXAGC_MCS12, power[URTWM_RIDX_MCS(12)]) |
	    SM(R88A_TXAGC_MCS13, power[URTWM_RIDX_MCS(13)]) |
	    SM(R88A_TXAGC_MCS14, power[URTWM_RIDX_MCS(14)]) |
	    SM(R88A_TXAGC_MCS15, power[URTWM_RIDX_MCS(15)]));

	/* TODO: VHT rates */
}

static int
urtwm_get_power_group(struct urtwm_softc *sc, struct ieee80211_channel *c)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint8_t chan;
	int group;

	chan = ieee80211_chan2ieee(ic, c);

	if (IEEE80211_IS_CHAN_2GHZ(c)) {
		if (chan <= 2)			group = 0;
		else if (chan <= 5)		group = 1;
		else if (chan <= 8)		group = 2;
		else if (chan <= 11)		group = 3;
		else if (chan <= 14)		group = 4;
		else {
			KASSERT(0, ("wrong 2GHz channel %d!\n", chan));
			return (-1);
		}
	} else if (IEEE80211_IS_CHAN_5GHZ(c)) {
		if (chan >= 36 && chan <= 42)	group = 0;
		else if (chan <= 48)		group = 1;
		else if (chan <= 58)		group = 2;
		else if (chan <= 64)		group = 3;
		else if (chan <= 106)		group = 4;
		else if (chan <= 114)		group = 5;
		else if (chan <= 122)		group = 6;
		else if (chan <= 130)		group = 7;
		else if (chan <= 138)		group = 8;
		else if (chan <= 144)		group = 9;
		else if (chan <= 155)		group = 10;
		else if (chan <= 161)		group = 11;
		else if (chan <= 171)		group = 12;
		else if (chan <= 177)		group = 13;
		else {
			KASSERT(0, ("wrong 5GHz channel %d!\n", chan));
			return (-1);
		}
	} else {
		KASSERT(0, ("wrong channel band (flags %08X)\n", c->ic_flags));
		return (-1);
	}

	return (group);
}

static void
urtwm_get_txpower(struct urtwm_softc *sc, int chain,
    struct ieee80211_channel *c, uint16_t power[URTWM_RIDX_COUNT])
{
	int i, ridx, group, max_mcs;

	/* Determine channel group. */
	group = urtwm_get_power_group(sc, c);
	if (group == -1) {	/* shouldn't happen */
		device_printf(sc->sc_dev, "%s: incorrect channel\n", __func__);
		return;
	}

	/* TODO: VHT rates. */
	max_mcs = URTWM_RIDX_MCS(sc->ntxchains * 8 - 1);

	/* XXX regulatory */
	/* XXX net80211 regulatory */

	if (IEEE80211_IS_CHAN_2GHZ(c)) {
		for (ridx = URTWM_RIDX_CCK1; ridx <= URTWM_RIDX_CCK11; ridx++)
			power[ridx] = sc->cck_tx_pwr[chain][group];
		for (ridx = URTWM_RIDX_OFDM6; ridx <= max_mcs; ridx++)
			power[ridx] = sc->ht40_tx_pwr_2g[chain][group];

		if (URTWM_RATE_IS_OFDM(ridx)) {
			uint8_t pwr_diff = sc->ofdm_tx_pwr_diff_2g[chain][0];
			for (ridx = URTWM_RIDX_CCK1; ridx <= max_mcs; ridx++)
				power[ridx] += pwr_diff;
		}

		for (i = 0; i < sc->ntxchains; i++) {
			uint8_t min_mcs;
			uint8_t pwr_diff;

			if (IEEE80211_IS_CHAN_HT20(c))
				pwr_diff = sc->bw20_tx_pwr_diff_2g[chain][i];
			else if (IEEE80211_IS_CHAN_HT40(c))
				pwr_diff = sc->bw40_tx_pwr_diff_2g[chain][i];
#ifdef notyet
			else if (IEEE80211_IS_CHAN_HT80(c)) {
				/* Vendor driver uses HT40 values here. */
				pwr_diff = sc->bw40_tx_pwr_diff_2g[chain][i];
			}
#endif

			min_mcs = URTWM_RIDX_MCS(i * 8 + 7);
			for (ridx = min_mcs; ridx <= max_mcs; ridx++)
				power[ridx] += pwr_diff;
		}
	} else {	/* 5GHz */
		for (ridx = URTWM_RIDX_OFDM6; ridx <= max_mcs; ridx++)
			power[ridx] = sc->ht40_tx_pwr_5g[chain][group];

		for (i = 0; i < sc->ntxchains; i++) {
			uint8_t min_mcs;
			uint8_t pwr_diff;

			if (IEEE80211_IS_CHAN_HT20(c))
				pwr_diff = sc->bw20_tx_pwr_diff_5g[chain][i];
			else if (IEEE80211_IS_CHAN_HT40(c))
				pwr_diff = sc->bw40_tx_pwr_diff_5g[chain][i];
#ifdef notyet
			else if (IEEE80211_IS_CHAN_HT80(c)) {
				/* TODO: calculate base value. */
				pwr_diff = sc->bw80_tx_pwr_diff_5g[chain][i];
			}
#endif

			min_mcs = URTWM_RIDX_MCS(i * 8 + 7);
			for (ridx = min_mcs; ridx <= max_mcs; ridx++)
				power[ridx] += pwr_diff;
		}
	}

	/* Apply max limit. */
	for (ridx = URTWM_RIDX_CCK1; ridx <= max_mcs; ridx++) {
		if (power[ridx] > R92C_MAX_TX_PWR)
			power[ridx] = R92C_MAX_TX_PWR;

	}

#ifdef USB_DEBUG
	if (sc->sc_debug & URTWM_DEBUG_TXPWR) {
		/* Dump per-rate Tx power values. */
		printf("Tx power for chain %d:\n", chain);
		for (ridx = URTWM_RIDX_CCK1; ridx < URTWM_RIDX_COUNT; ridx++)
			printf("Rate %d = %u\n", ridx, power[ridx]);
	}
#endif
}

static void
urtwm_set_txpower(struct urtwm_softc *sc, struct ieee80211_channel *c)
{
	uint16_t power[URTWM_RIDX_COUNT];
	int i;

	for (i = 0; i < sc->ntxchains; i++) {
		memset(power, 0, sizeof(power));
		/* Compute per-rate Tx power values. */
		urtwm_get_txpower(sc, i, c, power);
		/* Write per-rate Tx power values to hardware. */
		urtwm_write_txpower(sc, i, c, power);
	}
}

static void
urtwm_set_rx_bssid_all(struct urtwm_softc *sc, int enable)
{
	if (enable)
		urtwm_setbits_4(sc, R92C_RCR, R92C_RCR_CBSSID_BCN, 0);
	else
		urtwm_setbits_4(sc, R92C_RCR, 0, R92C_RCR_CBSSID_BCN);
}

static void
urtwm_scan_start(struct ieee80211com *ic)
{
	struct urtwm_softc *sc = ic->ic_softc;

	URTWM_LOCK(sc);
	/* Receive beacons / probe responses from any BSSID. */
	if (ic->ic_opmode != IEEE80211_M_IBSS &&
	    ic->ic_opmode != IEEE80211_M_HOSTAP)
		urtwm_set_rx_bssid_all(sc, 1);
	URTWM_UNLOCK(sc);
}

static void
urtwm_scan_curchan(struct ieee80211_scan_state *ss, unsigned long maxdwell)
{
	struct urtwm_softc *sc = ss->ss_ic->ic_softc;

	/* Make link LED blink during scan. */
	URTWM_LOCK(sc);
	urtwm_set_led(sc, URTWM_LED_LINK, !sc->ledlink);
	URTWM_UNLOCK(sc);

	sc->sc_scan_curchan(ss, maxdwell);
}

static void
urtwm_scan_end(struct ieee80211com *ic)
{
	struct urtwm_softc *sc = ic->ic_softc;
	struct ieee80211vap *vap;

	URTWM_LOCK(sc);
	/* Restore limitations. */
	if (ic->ic_promisc == 0 &&
	    ic->ic_opmode != IEEE80211_M_IBSS &&
	    ic->ic_opmode != IEEE80211_M_HOSTAP)
		urtwm_set_rx_bssid_all(sc, 0);

	vap = TAILQ_FIRST(&ic->ic_vaps);
	if (vap->iv_state == IEEE80211_S_RUN)
		urtwm_set_led(sc, URTWM_LED_LINK, 1);
	else
		urtwm_set_led(sc, URTWM_LED_LINK, 0);
	URTWM_UNLOCK(sc);
}

static void
urtwm_getradiocaps(struct ieee80211com *ic,
    int maxchans, int *nchans, struct ieee80211_channel chans[])
{
	uint8_t bands[IEEE80211_MODE_BYTES];

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	ieee80211_add_channel_list_2ghz(chans, maxchans, nchans,
	    urtwm_chan_2ghz, nitems(urtwm_chan_2ghz), bands, 0);

	setbit(bands, IEEE80211_MODE_11A);
	ieee80211_add_channel_list_5ghz(chans, maxchans, nchans,
	    urtwm_chan_5ghz, nitems(urtwm_chan_5ghz), bands, 0);
}

static void
urtwm_set_channel(struct ieee80211com *ic)
{
	struct urtwm_softc *sc = ic->ic_softc;
	struct ieee80211_channel *c = ic->ic_curchan;

	URTWM_LOCK(sc);
	urtwm_set_chan(sc, c);
	sc->sc_rxtap.wr_chan_freq = htole16(c->ic_freq);
	sc->sc_rxtap.wr_chan_flags = htole16(c->ic_flags);
	sc->sc_txtap.wt_chan_freq = htole16(c->ic_freq);
	sc->sc_txtap.wt_chan_flags = htole16(c->ic_flags);
	URTWM_UNLOCK(sc);
}

static int
urtwm_wme_update(struct ieee80211com *ic)
{
	struct urtwm_softc *sc = ic->ic_softc;
	struct wmeParams *wmep = sc->cap_wmeParams;
	uint8_t aifs, acm, slottime;
	int ac;

	/* Prevent possible races. */
	IEEE80211_LOCK(ic);	/* XXX */
	URTWM_LOCK(sc);
	memcpy(wmep, ic->ic_wme.wme_chanParams.cap_wmeParams,
	    sizeof(sc->cap_wmeParams));
	URTWM_UNLOCK(sc);
	IEEE80211_UNLOCK(ic);

	acm = 0;
	slottime = IEEE80211_GET_SLOTTIME(ic);

	URTWM_LOCK(sc);
	for (ac = WME_AC_BE; ac < WME_NUM_AC; ac++) {
		/* AIFS[AC] = AIFSN[AC] * aSlotTime + aSIFSTime. */
		aifs = wmep[ac].wmep_aifsn * slottime + IEEE80211_DUR_SIFS;
		urtwm_write_4(sc, wme2queue[ac].reg,
		    SM(R92C_EDCA_PARAM_TXOP, wmep[ac].wmep_txopLimit) |
		    SM(R92C_EDCA_PARAM_ECWMIN, wmep[ac].wmep_logcwmin) |
		    SM(R92C_EDCA_PARAM_ECWMAX, wmep[ac].wmep_logcwmax) |
		    SM(R92C_EDCA_PARAM_AIFS, aifs));
		if (ac != WME_AC_BE)
			acm |= wmep[ac].wmep_acm << ac;
	}

	if (acm != 0)
		acm |= R92C_ACMHWCTRL_EN;
	urtwm_setbits_1(sc, R92C_ACMHWCTRL, R92C_ACMHWCTRL_ACM_MASK, acm);
	URTWM_UNLOCK(sc);

	return 0;
}

static void
urtwm_update_slot(struct ieee80211com *ic)
{
	urtwm_cmd_sleepable(ic->ic_softc, NULL, 0, urtwm_update_slot_cb);
}

static void
urtwm_update_slot_cb(struct urtwm_softc *sc, union sec_param *data)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint8_t slottime;

	slottime = IEEE80211_GET_SLOTTIME(ic);

	URTWM_DPRINTF(sc, URTWM_DEBUG_ANY, "%s: setting slot time to %uus\n",
	    __func__, slottime);

	urtwm_write_1(sc, R92C_SLOT, slottime);
	urtwm_update_aifs(sc, slottime);
}

static void
urtwm_update_aifs(struct urtwm_softc *sc, uint8_t slottime)
{
	const struct wmeParams *wmep = sc->cap_wmeParams;
	uint8_t aifs, ac;

	for (ac = WME_AC_BE; ac < WME_NUM_AC; ac++) {
		/* AIFS[AC] = AIFSN[AC] * aSlotTime + aSIFSTime. */
		aifs = wmep[ac].wmep_aifsn * slottime + IEEE80211_DUR_SIFS;
		urtwm_write_1(sc, wme2queue[ac].reg, aifs);
	}
}

static uint8_t
urtwm_get_multi_pos(const uint8_t maddr[])
{
	uint64_t mask = 0x00004d101df481b4;
	uint8_t pos = 0x27;	/* initial value */
	int i, j;

	for (i = 0; i < IEEE80211_ADDR_LEN; i++)
		for (j = (i == 0) ? 1 : 0; j < 8; j++)
			if ((maddr[i] >> j) & 1)
				pos ^= (mask >> (i * 8 + j - 1));

	pos &= 0x3f;

	return (pos);
}

static void
urtwm_set_multi(struct urtwm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t mfilt[2];

	URTWM_ASSERT_LOCKED(sc);

	/* general structure was copied from ath(4). */
	if (ic->ic_allmulti == 0) {
		struct ieee80211vap *vap;
		struct ifnet *ifp;
		struct ifmultiaddr *ifma;

		/*
		 * Merge multicast addresses to form the hardware filter.
		 */
		mfilt[0] = mfilt[1] = 0;
		TAILQ_FOREACH(vap, &ic->ic_vaps, iv_next) {
			ifp = vap->iv_ifp;
			if_maddr_rlock(ifp);
			TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
				caddr_t dl;
				uint8_t pos;

				dl = LLADDR((struct sockaddr_dl *)
				    ifma->ifma_addr);
				pos = urtwm_get_multi_pos(dl);

				mfilt[pos / 32] |= (1 << (pos % 32));
			}
			if_maddr_runlock(ifp);
		}
	} else
		mfilt[0] = mfilt[1] = ~0;


	urtwm_write_4(sc, R92C_MAR + 0, mfilt[0]);
	urtwm_write_4(sc, R92C_MAR + 4, mfilt[1]);

	URTWM_DPRINTF(sc, URTWM_DEBUG_STATE, "%s: MC filter %08x:%08x\n",
	     __func__, mfilt[0], mfilt[1]);
}

static void
urtwm_set_promisc(struct urtwm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
	uint32_t mask1, mask2;

	URTWM_ASSERT_LOCKED(sc);

	if (vap->iv_opmode == IEEE80211_M_MONITOR)
		return;

	mask1 = R92C_RCR_ACF | R92C_RCR_ADF | R92C_RCR_AMF | R92C_RCR_AAP;
	mask2 = R92C_RCR_APM;

	if (vap->iv_state == IEEE80211_S_RUN) {
		switch (vap->iv_opmode) {
		case IEEE80211_M_STA:
			mask2 |= R92C_RCR_CBSSID_BCN;
			/* FALLTHROUGH */
		case IEEE80211_M_IBSS:
			mask2 |= R92C_RCR_CBSSID_DATA;
			break;
		case IEEE80211_M_HOSTAP:
			break;
		default:
			device_printf(sc->sc_dev, "%s: undefined opmode %d\n",
			    __func__, vap->iv_opmode);
			return;
		}
	}

	if (ic->ic_promisc == 0)
		urtwm_setbits_4(sc, R92C_RCR, mask1, mask2);
	else
		urtwm_setbits_4(sc, R92C_RCR, mask2, mask1);
}

static void
urtwm_update_promisc(struct ieee80211com *ic)
{
	struct urtwm_softc *sc = ic->ic_softc;

	URTWM_LOCK(sc);
	if (sc->sc_flags & URTWM_RUNNING)
		urtwm_set_promisc(sc);
	URTWM_UNLOCK(sc);
}

static void
urtwm_update_mcast(struct ieee80211com *ic)
{
	struct urtwm_softc *sc = ic->ic_softc;

	URTWM_LOCK(sc);
	if (sc->sc_flags & URTWM_RUNNING)
		urtwm_set_multi(sc);
	URTWM_UNLOCK(sc);
}

#ifdef URTWM_TODO
static struct ieee80211_node *
urtwn_node_alloc(struct ieee80211vap *vap,
    const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct urtwn_node *un;

	un = malloc(sizeof (struct urtwn_node), M_80211_NODE,
	    M_NOWAIT | M_ZERO);

	if (un == NULL)
		return NULL;

	un->id = URTWN_MACID_UNDEFINED;

	return &un->ni;
}

static void
urtwn_newassoc(struct ieee80211_node *ni, int isnew)
{
	struct urtwn_softc *sc = ni->ni_ic->ic_softc;
	struct urtwn_node *un = URTWN_NODE(ni);
	uint8_t id;

	/* Only do this bit for R88E chips */
	if (! (sc->chip & URTWN_CHIP_88E))
		return;

	if (!isnew)
		return;

	URTWN_NT_LOCK(sc);
	for (id = 0; id <= URTWN_MACID_MAX(sc); id++) {
		if (id != URTWN_MACID_BC && sc->node_list[id] == NULL) {
			un->id = id;
			sc->node_list[id] = ni;
			break;
		}
	}
	URTWN_NT_UNLOCK(sc);

	if (id > URTWN_MACID_MAX(sc)) {
		device_printf(sc->sc_dev, "%s: node table is full\n",
		    __func__);
	}
}

static void
urtwn_node_free(struct ieee80211_node *ni)
{
	struct urtwn_softc *sc = ni->ni_ic->ic_softc;
	struct urtwn_node *un = URTWN_NODE(ni);

	URTWN_NT_LOCK(sc);
	if (un->id != URTWN_MACID_UNDEFINED)
		sc->node_list[un->id] = NULL;
	URTWN_NT_UNLOCK(sc);

	sc->sc_node_free(ni);
}
#endif	/* URTWM_TODO */

static void
urtwm_set_chan(struct urtwm_softc *sc, struct ieee80211_channel *c)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t val;
	uint16_t chan;
	int i;

	urtwm_band_change(sc, c, 0);

	chan = ieee80211_chan2ieee(ic, c);	/* XXX center freq! */
	KASSERT(chan != 0 && chan != IEEE80211_CHAN_ANY,
	    ("invalid channel %x\n", chan));

	if (36 <= chan && chan <= 48)
		val = 0x09280000;
	else if (50 <= chan && chan <= 64)
		val = 0x08a60000;
	else if (100 <= chan && chan <= 116)
		val = 0x08a40000;
	else if (118 <= chan)
		val = 0x08240000;
	else
		val = 0x12d40000;

	urtwm_bb_setbits(sc, R88A_FC_AREA, 0x1ffe0000, val);

	for (i = 0; i < sc->nrxchains; i++) {
		if (36 <= chan && chan <= 64)
			val = 0x10100;
		else if (100 <= chan && chan <= 140)
			val = 0x30100;
		else if (140 < chan)
			val = 0x50100;
		else
			val = 0x00000;

		urtwm_rf_setbits(sc, i, R92C_RF_CHNLBW, 0x70300, val);
		urtwm_rf_setbits(sc, i, R92C_RF_CHNLBW, 0xff, chan);
	}

#ifdef notyet
	if (IEEE80211_IS_CHAN_HT80(c)) {	/* 80 MHz */
		urtwm_setbits_2(sc, R88A_WMAC_TRXPTCL_CTL, 0x80, 0x100);

		/* TODO */
	} else
#endif
	if (IEEE80211_IS_CHAN_HT40(c)) {	/* 40 MHz */
		uint8_t ext_chan;

		if (IEEE80211_IS_CHAN_HT40U(c))
			ext_chan = R88A_DATA_SEC_PRIM_DOWN_20;
		else
			ext_chan = R88A_DATA_SEC_PRIM_UP_20;

		urtwm_setbits_2(sc, R88A_WMAC_TRXPTCL_CTL, 0x100, 0x80);
		urtwm_write_1(sc, R88A_DATA_SEC, ext_chan);

		urtwm_bb_setbits(sc, R88A_RFMOD, 0x003003c3, 0x00300201);
		urtwm_bb_setbits(sc, R88A_ADC_BUF_CLK, 0x40000000, 0);

		/* discard high 4 bits */
		val = urtwm_bb_read(sc, R88A_RFMOD);
		val = RW(val, R88A_RFMOD_EXT_CHAN, ext_chan);
		urtwm_bb_write(sc, R88A_RFMOD, val);

		val = urtwm_bb_read(sc, R88A_CCA_ON_SEC);
		val = RW(val, R88A_CCA_ON_SEC_EXT_CHAN, ext_chan);
		urtwm_bb_write(sc, R88A_CCA_ON_SEC, val);

		if (urtwm_read_1(sc, 0x837) & 0x04)
			val = 0x01800000;
		else if (sc->nrxchains == 2 && sc->ntxchains == 2)
			val = 0x01c00000;
		else
			val = 0x02000000;

		urtwm_bb_setbits(sc, R88A_L1_PEAK_TH, 0x03c00000, val);

		if (IEEE80211_IS_CHAN_HT40U(c))
			urtwm_bb_setbits(sc, R92C_CCK0_SYSTEM, 0x10, 0);
		else
			urtwm_bb_setbits(sc, R92C_CCK0_SYSTEM, 0, 0x10);

		for (i = 0; i < 2; i++)
			urtwm_rf_setbits(sc, i, R92C_RF_CHNLBW, 0x800, 0x400);
	} else {	/* 20 MHz */
		urtwm_setbits_2(sc, R88A_WMAC_TRXPTCL_CTL, 0x180, 0);
		urtwm_write_1(sc, R88A_DATA_SEC, R88A_DATA_SEC_NO_EXT);

		urtwm_bb_setbits(sc, R88A_RFMOD, 0x003003c3, 0x00300200);
		urtwm_bb_setbits(sc, R88A_ADC_BUF_CLK, 0x40000000, 0);

		if (sc->nrxchains == 2 && sc->ntxchains == 2)
			val = 0x01c00000;
		else
			val = 0x02000000;

		urtwm_bb_setbits(sc, R88A_L1_PEAK_TH, 0x03c00000, val);

		for (i = 0; i < 2; i++)
			urtwm_rf_setbits(sc, i, R92C_RF_CHNLBW, 0, 0xc00);
	}

	/* Set Tx power for this new channel. */
	urtwm_set_txpower(sc, c);
}

static void
urtwm_antsel_init(struct urtwm_softc *sc)
{
	uint32_t reg;

	urtwm_write_1(sc, R92C_LEDCFG2, 0x82);
	urtwm_bb_setbits(sc, R92C_FPGA0_RFPARAM(0), 0, 0x2000);
	reg = urtwm_bb_read(sc, R92C_FPGA0_RFIFACEOE(0));
	sc->sc_ant = MS(reg, R88A_FPGA0_RFIFACEOE0_ANT);
}

#ifdef URTWM_TODO
static void
urtwn_iq_calib(struct urtwn_softc *sc)
{
	/* TODO */
}

static void
urtwn_lc_calib(struct urtwn_softc *sc)
{
	uint32_t rf_ac[2];
	uint8_t txmode;
	int i;

	txmode = urtwn_read_1(sc, R92C_OFDM1_LSTF + 3);
	if ((txmode & 0x70) != 0) {
		/* Disable all continuous Tx. */
		urtwn_write_1(sc, R92C_OFDM1_LSTF + 3, txmode & ~0x70);

		/* Set RF mode to standby mode. */
		for (i = 0; i < sc->nrxchains; i++) {
			rf_ac[i] = urtwn_rf_read(sc, i, R92C_RF_AC);
			urtwn_rf_write(sc, i, R92C_RF_AC,
			    RW(rf_ac[i], R92C_RF_AC_MODE,
				R92C_RF_AC_MODE_STANDBY));
		}
	} else {
		/* Block all Tx queues. */
		urtwn_write_1(sc, R92C_TXPAUSE, R92C_TX_QUEUE_ALL);
	}
	/* Start calibration. */
	urtwn_rf_write(sc, 0, R92C_RF_CHNLBW,
	    urtwn_rf_read(sc, 0, R92C_RF_CHNLBW) | R92C_RF_CHNLBW_LCSTART);

	/* Give calibration the time to complete. */
	usb_pause_mtx(&sc->sc_mtx, hz / 10);		/* 100ms */

	/* Restore configuration. */
	if ((txmode & 0x70) != 0) {
		/* Restore Tx mode. */
		urtwn_write_1(sc, R92C_OFDM1_LSTF + 3, txmode);
		/* Restore RF mode. */
		for (i = 0; i < sc->nrxchains; i++)
			urtwn_rf_write(sc, i, R92C_RF_AC, rf_ac[i]);
	} else {
		/* Unblock all Tx queues. */
		urtwn_write_1(sc, R92C_TXPAUSE, 0x00);
	}
}

static void
urtwn_temp_calib(struct urtwn_softc *sc)
{
	uint8_t temp;

	URTWN_ASSERT_LOCKED(sc);

	if (!(sc->sc_flags & URTWN_TEMP_MEASURED)) {
		/* Start measuring temperature. */
		URTWN_DPRINTF(sc, URTWN_DEBUG_TEMP,
		    "%s: start measuring temperature\n", __func__);
		if (sc->chip & URTWN_CHIP_88E) {
			urtwn_rf_write(sc, 0, R88E_RF_T_METER,
			    R88E_RF_T_METER_START);
		} else {
			urtwn_rf_write(sc, 0, R92C_RF_T_METER,
			    R92C_RF_T_METER_START);
		}
		sc->sc_flags |= URTWN_TEMP_MEASURED;
		return;
	}
	sc->sc_flags &= ~URTWN_TEMP_MEASURED;

	/* Read measured temperature. */
	if (sc->chip & URTWN_CHIP_88E) {
		temp = MS(urtwn_rf_read(sc, 0, R88E_RF_T_METER),
		    R88E_RF_T_METER_VAL);
	} else {
		temp = MS(urtwn_rf_read(sc, 0, R92C_RF_T_METER),
		    R92C_RF_T_METER_VAL);
	}
	if (temp == 0) {	/* Read failed, skip. */
		URTWN_DPRINTF(sc, URTWN_DEBUG_TEMP,
		    "%s: temperature read failed, skipping\n", __func__);
		return;
	}

	URTWN_DPRINTF(sc, URTWN_DEBUG_TEMP,
	    "%s: temperature: previous %u, current %u\n",
	    __func__, sc->thcal_lctemp, temp);

	/*
	 * Redo LC calibration if temperature changed significantly since
	 * last calibration.
	 */
	if (sc->thcal_lctemp == 0) {
		/* First LC calibration is performed in urtwm_init(). */
		sc->thcal_lctemp = temp;
	} else if (abs(temp - sc->thcal_lctemp) > 1) {
		URTWN_DPRINTF(sc, URTWN_DEBUG_TEMP,
		    "%s: LC calib triggered by temp: %u -> %u\n",
		    __func__, sc->thcal_lctemp, temp);
		urtwn_lc_calib(sc);
		/* Record temperature of last LC calibration. */
		sc->thcal_lctemp = temp;
	}
}
#endif	/* URTWM_TODO */

static int
urtwm_init(struct urtwm_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
	struct urtwm_vap *uvp = URTWM_VAP(vap);
	usb_error_t usb_err = USB_ERR_NORMAL_COMPLETION;
	int i, error;

	URTWM_LOCK(sc);
	if (sc->sc_flags & URTWM_RUNNING) {
		URTWM_UNLOCK(sc);
		return (0);
	}

	/* Allocate Tx/Rx buffers. */
	error = urtwm_alloc_rx_list(sc);
	if (error != 0)
		goto fail;

	error = urtwm_alloc_tx_list(sc);
	if (error != 0)
		goto fail;

	/* Power on adapter. */
	error = urtwm_power_on(sc);
	if (error != 0)
		goto fail;

	/* TODO: Firmware loading is done here. */

	/* Initialize MAC block. */
	error = urtwm_mac_init(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: error while initializing MAC block\n", __func__);
		goto fail;
	}

	/* Initialize DMA. */
	error = urtwm_dma_init(sc);
	if (error != 0)
		goto fail;

	/* Drop incorrect TX. */
	urtwm_setbits_2(sc, R92C_TXDMA_OFFSET_CHK, 0,
	    R92C_TXDMA_OFFSET_DROP_DATA_EN);

	/* Set info size in Rx descriptors (in 64-bit words). */
	/* XXX optimize? */
	urtwm_write_1(sc, R92C_RX_DRVINFO_SZ, 4);

	/* Init interrupts. */
	urtwm_write_4(sc, R88E_HIMR, 0);
	urtwm_write_4(sc, R88E_HIMRE, 0);

	/* Set MAC address. */
	usb_err = urtwm_write_region_1(sc, R92C_MACID, vap->iv_myaddr,
	    IEEE80211_ADDR_LEN);
	if (usb_err != USB_ERR_NORMAL_COMPLETION)
		goto fail;

	/* Set initial network type. */
	urtwm_set_mode(sc, R92C_MSR_NOLINK, 0);

	/* Initialize Rx filter. */
	urtwm_rxfilter_init(sc);

	/* Set response rate. */
	urtwm_setbits_4(sc, R92C_RRSR, R92C_RRSR_RATE_BITMAP_M,
	    R92C_RRSR_RATE_CCK_ONLY_1M);

	/* Set short/long retry limits. */
	urtwm_write_2(sc, R92C_RL,
	    SM(R92C_RL_SRL, 0x30) | SM(R92C_RL_LRL, 0x30));

	/* Initialize EDCA parameters. */
	urtwm_edca_init(sc);

	urtwm_setbits_1(sc, R92C_FWHW_TXQ_CTRL, 0,
	    R92C_FWHW_TXQ_CTRL_AMPDU_RTY_NEW);
	/* Set ACK timeout. */
	urtwm_write_1(sc, R92C_ACKTO, 0x80);

	/* Setup USB aggregation. */
	/* Tx aggregation. */
	urtwm_setbits_4(sc, R92C_TDECTRL, R92C_TDECTRL_BLK_DESC_NUM_M, 6);
	/* RTL8821AU specific. */
	urtwm_write_1(sc, R88A_DWBCN1_CTRL, (6 << 1));

	/* Rx aggregation (DMA). */
	if (usbd_get_speed(sc->sc_udev) == USB_SPEED_SUPER)
		urtwm_write_2(sc, R92C_RXDMA_AGG_PG_TH, 0x1a7);
	else
		urtwm_write_2(sc, R92C_RXDMA_AGG_PG_TH, 0x106);
	urtwm_setbits_1(sc, R92C_TRXDMA_CTRL, 0,
	    R92C_TRXDMA_CTRL_RXDMA_AGG_EN);

	/* Initialize beacon parameters. */
	urtwm_write_2(sc, R92C_BCN_CTRL, 0x1010);
	urtwm_write_2(sc, R92C_TBTT_PROHIBIT, 0x6404);
	urtwm_write_1(sc, R92C_DRVERLYINT, 0x05);
	urtwm_write_1(sc, R92C_BCNDMATIM, 0x02);
	urtwm_write_2(sc, R92C_BCNTCFG, 0x660f);

	/* Rx interval (USB3). */
	urtwm_write_1(sc, 0xf050, 0x01);

	/* burst length = 4 */
	urtwm_write_2(sc, R92C_RXDMA_STATUS, 0x7400);

	urtwm_write_1(sc, R92C_RXDMA_STATUS + 1, 0xf5);

	/* Setup AMPDU aggregation. */
	urtwm_write_1(sc, R88A_AMPDU_MAX_TIME, 0x5e);
	urtwm_write_4(sc, R88A_AMPDU_MAX_LENGTH, 0xffffffff);

	/* 80 MHz clock (again?) */
	urtwm_write_1(sc, R92C_USTIME_TSF, 0x50);
	urtwm_write_1(sc, R92C_USTIME_EDCA, 0x50);

	if ((urtwm_read_1(sc, R92C_USB_INFO) & 0x30) == 0) {
		/* Set burst packet length to 512 B. */
		urtwm_setbits_1(sc, R88A_RXDMA_PRO, 0x20, 0x10);
		urtwm_write_2(sc, R88A_RXDMA_PRO, 0x1e);
	} else {
		/* Set burst packet length to 64 B. */
		urtwm_setbits_1(sc, R88A_RXDMA_PRO, 0x10, 0x20);
	}

	/* Reset 8051. */
	/* XXX vendor driver contains bug here (results in noop). */
#ifdef rtl8812a_behavior
	urtwm_setbits_1(sc, R92C_SYS_FUNC_EN, R92C_SYS_FUNC_EN_CPUEN, 0);
#else
	urtwm_setbits_1_shift(sc, R92C_SYS_FUNC_EN, R92C_SYS_FUNC_EN_CPUEN,
	    0, 1);
#endif

	/* Enable single packet AMPDU. */
	urtwm_setbits_1(sc, R88A_HT_SINGLE_AMPDU, 0,
	    R88A_HT_SINGLE_AMPDU_PKT_ENA);

	/* 11K packet length for VHT. */
	urtwm_write_1(sc, R92C_RX_PKT_LIMIT, 0x18);

	urtwm_write_1(sc, R92C_PIFS, 0);

	urtwm_write_2(sc, R92C_MAX_AGGR_NUM, 0x0a0a);
	urtwm_write_1(sc, R92C_FWHW_TXQ_CTRL,
	    R92C_FWHW_TXQ_CTRL_AMPDU_RTY_NEW);
	urtwm_write_4(sc, R92C_FAST_EDCA_CTRL, 0x03087777);

	/* Do not reset MAC. */
	urtwm_setbits_1(sc, R92C_RSV_CTRL, 0, 0x60);

	urtwm_arfb_init(sc);

	/* Init MACTXEN / MACRXEN after setting RxFF boundary. */
	urtwm_setbits_2(sc, R92C_CR, 0, R92C_CR_MACTXEN | R92C_CR_MACRXEN);

	/* Initialize BB/RF blocks. */
	urtwm_bb_init(sc);
	urtwm_rf_init(sc);

	/* Initialize wireless band. */
	urtwm_band_change(sc, ic->ic_curchan, 1);

	/* Clear per-station keys table. */
	urtwm_cam_init(sc);

	/* Enable decryption / encryption. */
	urtwm_write_2(sc, R92C_SECCFG,
	    R92C_SECCFG_TXUCKEY_DEF | R92C_SECCFG_RXUCKEY_DEF |
	    R92C_SECCFG_TXENC_ENA | R92C_SECCFG_RXDEC_ENA |
	    R92C_SECCFG_TXBCKEY_DEF | R92C_SECCFG_RXBCKEY_DEF);

	/* Initialize antenna selection. */
	urtwm_antsel_init(sc);

	/* Enable hardware sequence numbering. */
	urtwm_write_1(sc, R92C_HWSEQ_CTRL, R92C_TX_QUEUE_ALL);

	/* Disable BAR. */
	urtwm_write_4(sc, R92C_BAR_MODE_CTRL, 0x0201ffff);

	/* NAV limit. */
	urtwm_write_1(sc, R92C_NAV_UPPER, 0);

	/* Initialize GPIO setting. */
	urtwm_setbits_1(sc, R92C_GPIO_MUXCFG, R92C_GPIO_MUXCFG_ENBT, 0);

	/* Setup RTS BW (equal to data BW). */
	urtwm_setbits_1(sc, R92C_QUEUE_CTRL, 0x08, 0);

	urtwm_write_1(sc, R88A_EARLY_MODE_CONTROL + 3, 0x01);

	/* XXX TODO: enable TX report. */
#ifdef URTWM_TODO
	urtwm_write_1(sc, R92C_FWHW_TXQ_CTRL + 1, 0x0f);

	/* XXX vendor driver sets only RPT2. */
	urtwm_setbits_1(sc, R88E_TX_RPT_CTRL,
	    R88E_TX_RPT1_ENA | R88E_TX_RPT2_ENA);

	urtwm_write_2(sc, R88E_TX_RPT_TIME, 0x3df0);
#endif
	/* Initialize MRR. */
	urtwm_mrr_init(sc);

	/* Reset USB mode switch setting. */
	urtwm_write_1(sc, R88A_SDIO_CTRL, 0);
	urtwm_write_1(sc, R92C_ACLK_MON, 0);

#ifdef URTWM_TODO
	/* Perform LO and IQ calibrations. */
	urtwn_iq_calib(sc);
	/* Perform LC calibration. */
	urtwn_lc_calib(sc);
#endif

	urtwm_write_1(sc, R92C_USB_HRPWM, 0);

#ifdef URTWM_TODO
	/* ACK for xmit management frames. */
	urtwm_setbits_1_shift(sc, R92C_FWHW_TXQ_CTRL, 0, 0x10, 1);
#endif

	usbd_transfer_start(sc->sc_xfer[URTWM_BULK_RX]);
	usbd_transfer_start(sc->sc_xfer[URTWM_INTR_RD]);

	sc->sc_flags |= URTWM_RUNNING;

	/*
	 * Install static keys (if any).
	 * Must be called after urtwm_cam_init().
	 */
	for (i = 0; i < IEEE80211_WEP_NKID; i++) {
		const struct ieee80211_key *k = uvp->keys[i];
		if (k != NULL) {
			urtwm_cmd_sleepable(sc, k, sizeof(*k),
			    urtwm_key_set_cb);
		}
	}
fail:
	if (usb_err != USB_ERR_NORMAL_COMPLETION)
		error = EIO;

	URTWM_UNLOCK(sc);

	return (error);
}

static void
urtwm_stop(struct urtwm_softc *sc)
{

	URTWM_LOCK(sc);
	if (!(sc->sc_flags & URTWM_RUNNING)) {
		URTWM_UNLOCK(sc);
		return;
	}

	sc->sc_flags &= ~URTWM_RUNNING;
#ifdef URTWM_TODO
	sc->thcal_lctemp = 0;
#endif

	urtwm_abort_xfers(sc);
	urtwm_drain_mbufq(sc);
	urtwm_power_off(sc);
	URTWM_UNLOCK(sc);
}

static void
urtwm_abort_xfers(struct urtwm_softc *sc)
{
	int i;

	URTWM_ASSERT_LOCKED(sc);

	/* abort any pending transfers */
	for (i = 0; i < URTWM_N_TRANSFER; i++)
		usbd_transfer_stop(sc->sc_xfer[i]);
}

static int
urtwm_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
    const struct ieee80211_bpf_params *params)
{
	struct ieee80211com *ic = ni->ni_ic;
	struct urtwm_softc *sc = ic->ic_softc;
	struct urtwm_data *bf;
	int error;

	URTWM_DPRINTF(sc, URTWM_DEBUG_XMIT, "%s: called; m %p, ni %p\n",
	    __func__, m, ni);

	/* prevent management frames from being sent if we're not ready */
	URTWM_LOCK(sc);
	if (!(sc->sc_flags & URTWM_RUNNING)) {
		error = ENETDOWN;
		goto end;
	}

	bf = urtwm_getbuf(sc);
	if (bf == NULL) {
		error = ENOBUFS;
		goto end;
	}

	if (params == NULL) {
		/*
		 * Legacy path; interpret frame contents to decide
		 * precisely how to send the frame.
		 */
		error = urtwm_tx_data(sc, ni, m, bf);
	} else {
		/*
		 * Caller supplied explicit parameters to use in
		 * sending the frame.
		 */
		error = urtwm_tx_raw(sc, ni, m, bf, params);
	}
	if (error != 0) {
		STAILQ_INSERT_HEAD(&sc->sc_tx_inactive, bf, next);
		goto end;
	}

end:
	if (error != 0)
		m_freem(m);

	URTWM_UNLOCK(sc);

	
	return (error);
}

static void
urtwm_delay(struct urtwm_softc *sc, int usec)
{
	/* No, 1ms delay is too big. */
	if (usec < 1000)
		DELAY(usec);
	else
		usb_pause_mtx(&sc->sc_mtx, USB_MS_TO_TICKS(usec / 1000 + 1));
}

static device_method_t urtwm_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		urtwm_match),
	DEVMETHOD(device_attach,	urtwm_attach),
	DEVMETHOD(device_detach,	urtwm_detach),

	DEVMETHOD_END
};

static driver_t urtwm_driver = {
	"urtwm",
	urtwm_methods,
	sizeof(struct urtwm_softc)
};

static devclass_t urtwm_devclass;

DRIVER_MODULE(urtwm, uhub, urtwm_driver, urtwm_devclass, NULL, NULL);
MODULE_DEPEND(urtwm, usb, 1, 1, 1);
MODULE_DEPEND(urtwm, wlan, 1, 1, 1);
MODULE_VERSION(urtwm, 1);
USB_PNP_HOST_INFO(urtwm_devs);
