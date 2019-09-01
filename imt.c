/*-
 * Copyright (c) 2014-2019 Vladimir Kondratyev <wulf@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * MS Windows 7/8/10 compatible I2C HID Multi-touch Device driver.
 * https://msdn.microsoft.com/en-us/library/windows/hardware/jj151569(v=vs.85).aspx
 * http://download.microsoft.com/download/7/d/d/7dd44bb7-2a7a-4505-ac1c-7227d3d96d5b/hid-over-i2c-protocol-spec-v1-0.docx
 * https://www.kernel.org/doc/Documentation/input/multi-touch-protocol.txt
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <dev/evdev/evdev.h>
#include <dev/evdev/input.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbhid.h>

#include "iichid.h"

#define	IMT_DEBUG
#define	IMT_DEBUG_VAR	wmt_debug

/* Check if debugging is enabled. */
#ifdef IMT_DEBUG_VAR
#ifdef IMT_DEBUG
#define	DPRINTFN(n,fmt,...) do {					\
	if ((IMT_DEBUG_VAR) >= (n)) {					\
		printf("%s: " fmt, __FUNCTION__ ,##__VA_ARGS__);	\
	}								\
} while (0)
#define	DPRINTF(...)	DPRINTFN(1, __VA_ARGS__)
#else
#define	DPRINTF(...) do { } while (0)
#define	DPRINTFN(...) do { } while (0)
#endif
#endif

#ifdef IMT_DEBUG
static int wmt_debug = 0;

static SYSCTL_NODE(_hw, OID_AUTO, imt, CTLFLAG_RW, 0,
    "I2C MSWindows 7/8/10 compatible Multi-touch Device");
SYSCTL_INT(_hw_imt, OID_AUTO, debug, CTLFLAG_RWTUN,
    &wmt_debug, 1, "Debug level");
#endif

#define	WMT_BSIZE	1024	/* bytes, buffer size */

enum {
	WMT_INTR_DT,
	WMT_N_TRANSFER,
};

enum imt_input_mode {
	IMT_INPUT_MODE_MOUSE =		0x0,
	IMT_INPUT_MODE_MT_TOUCHSCREEN =	0x2,
	IMT_INPUT_MODE_MT_TOUCHPAD =	0x3,
};

enum {
	WMT_TIP_SWITCH,
#define	WMT_SLOT	WMT_TIP_SWITCH
	WMT_WIDTH,
#define	WMT_MAJOR	WMT_WIDTH
	WMT_HEIGHT,
#define WMT_MINOR	WMT_HEIGHT
	WMT_ORIENTATION,
	WMT_X,
	WMT_Y,
	WMT_CONTACTID,
	WMT_PRESSURE,
	WMT_IN_RANGE,
	WMT_CONFIDENCE,
	WMT_TOOL_X,
	WMT_TOOL_Y,
	WMT_N_USAGES,
};

#define	WMT_NO_CODE	(ABS_MAX + 10)
#define	WMT_NO_USAGE	-1

struct wmt_hid_map_item {
	char		name[5];
	int32_t 	usage;		/* HID usage */
	uint32_t	code;		/* Evdev event code */
	bool		required;	/* Required for MT Digitizers */
};

static const struct wmt_hid_map_item wmt_hid_map[WMT_N_USAGES] = {

	[WMT_TIP_SWITCH] = {	/* WMT_SLOT */
		.name = "TIP",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_SWITCH),
		.code = ABS_MT_SLOT,
		.required = true,
	},
	[WMT_WIDTH] = {		/* WMT_MAJOR */
		.name = "WDTH",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_WIDTH),
		.code = ABS_MT_TOUCH_MAJOR,
		.required = false,
	},
	[WMT_HEIGHT] = {	/* WMT_MINOR */
		.name = "HGHT",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_HEIGHT),
		.code = ABS_MT_TOUCH_MINOR,
		.required = false,
	},
	[WMT_ORIENTATION] = {
		.name = "ORIE",
		.usage = WMT_NO_USAGE,
		.code = ABS_MT_ORIENTATION,
		.required = false,
	},
	[WMT_X] = {
		.name = "X",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X),
		.code = ABS_MT_POSITION_X,
		.required = true,
	},
	[WMT_Y] = {
		.name = "Y",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y),
		.code = ABS_MT_POSITION_Y,
		.required = true,
	},
	[WMT_CONTACTID] = {
		.name = "C_ID",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACTID),
		.code = ABS_MT_TRACKING_ID,
		.required = true,
	},
	[WMT_PRESSURE] = {
		.name = "PRES",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_PRESSURE),
		.code = ABS_MT_PRESSURE,
		.required = false,
	},
	[WMT_IN_RANGE] = {
		.name = "RANG",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_IN_RANGE),
		.code = ABS_MT_DISTANCE,
		.required = false,
	},
	[WMT_CONFIDENCE] = {
		.name = "CONF",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_CONFIDENCE),
		.code = WMT_NO_CODE,
		.required = false,
	},
	[WMT_TOOL_X] = {	/* Shares HID usage with WMT_X */
		.name = "TL_X",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X),
		.code = ABS_MT_TOOL_X,
		.required = false,
	},
	[WMT_TOOL_Y] = {	/* Shares HID usage with WMT_Y */
		.name = "TL_Y",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y),
		.code = ABS_MT_TOOL_Y,
		.required = false,
	},
};

struct wmt_absinfo {
	int32_t			min;
	int32_t			max;
	int32_t			res;
};

#define	USAGE_SUPPORTED(caps, usage)	((caps) & (1 << (usage)))
#define	WMT_FOREACH_USAGE(caps, usage)			\
	for ((usage) = 0; (usage) < WMT_N_USAGES; ++(usage))	\
		if (USAGE_SUPPORTED((caps), (usage)))

struct imt_softc {
	device_t dev;
	struct mtx              lock;
	int			type;

	struct wmt_absinfo      ai[WMT_N_USAGES];
	struct hid_location     locs[MAX_MT_SLOTS][WMT_N_USAGES];
	struct hid_location     cont_count_loc;

	struct evdev_dev        *evdev;

	uint32_t                slot_data[WMT_N_USAGES];
	uint32_t                caps;
	uint32_t                isize;
	uint32_t                nconts_per_report;
	uint32_t		nconts_todo;
	uint8_t                 report_id;

	struct hid_location     cont_max_loc;
	uint32_t                cont_max_rlen;
	uint8_t                 cont_max_rid;
	uint32_t                thqa_cert_rlen;
	uint8_t                 thqa_cert_rid;
	struct hid_location	input_mode_loc;
	uint32_t		input_mode_rlen;
	uint8_t			input_mode_rid;

	uint8_t			buf[WMT_BSIZE] __aligned(4);
};

static int wmt_hid_parse(struct imt_softc *, const void *, uint16_t);
static void wmt_cont_max_parse(struct imt_softc *, const void *, uint16_t);
static int imt_set_input_mode(struct imt_softc *, enum imt_input_mode);

static iichid_intr_t		imt_intr;

static device_probe_t		imt_probe;
static device_attach_t		imt_attach;
static device_detach_t		imt_detach;

static evdev_open_t	imt_ev_open;
static evdev_close_t	imt_ev_close;

static devclass_t imt_devclass;

static device_method_t imt_methods[] = {

	DEVMETHOD(device_probe,		imt_probe),
	DEVMETHOD(device_attach,	imt_attach),
	DEVMETHOD(device_detach,	imt_detach),

	DEVMETHOD_END
};

static driver_t imt_driver = {
	.name = "imt",
	.methods = imt_methods,
	.size = sizeof(struct imt_softc),
};

static const struct evdev_methods imt_evdev_methods = {
	.ev_open = &imt_ev_open,
	.ev_close = &imt_ev_close,
};

static int
imt_ev_close(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	return (iichid_intr_stop(device_get_parent(dev)));
}

static int
imt_ev_open(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	return (iichid_intr_start(device_get_parent(dev)));
}

static int
imt_probe(device_t dev)
{
	device_t iichid = device_get_parent(dev);
	struct iichid_hw *hw = device_get_ivars(dev);
	void *d_ptr;
	int d_len;
	int error;
	int hid_type;

	error = iichid_get_report_desc(iichid, &d_ptr, &d_len);
	if (error) {
		device_printf(dev, "could not retrieve report descriptor from device: %d\n", error);
		error = ENXIO;
		goto out;
	}

	/* Check if report descriptor belongs to a HID multitouch device */
	hid_type = wmt_hid_parse(NULL, d_ptr, d_len);
	error = hid_type != 0 ? BUS_PROBE_DEFAULT : ENXIO;

out:
	if (error <= 0)
		device_set_desc(dev, hw->hid);

	return (error);
}

static int
imt_attach(device_t dev)
{
	struct imt_softc *sc = device_get_softc(dev);
	device_t iichid = device_get_parent(dev);
	struct iichid_hw *hw = device_get_ivars(dev);
	int error;
	void *d_ptr;
	int d_len;
	size_t i;

	error = iichid_get_report_desc(iichid, &d_ptr, &d_len);
	if (error) {
		device_printf(dev, "could not retrieve report descriptor from device: %d\n", error);
		return (ENXIO);
	}

	mtx_init(&sc->lock, "imt lock", NULL, MTX_DEF);
	sc->dev = dev;

	sc->type = wmt_hid_parse(sc, d_ptr, d_len);
	if (sc->type == 0) {
		DPRINTF("multi-touch HID descriptor not found\n");
		goto detach;
	}

	/* Fetch and parse "Contact count maximum" feature report */
	if (sc->cont_max_rlen > 0 && sc->cont_max_rlen <= WMT_BSIZE) {
		error = iichid_get_report(iichid, sc->buf, sc->cont_max_rlen,
		    I2C_HID_REPORT_TYPE_FEATURE, sc->cont_max_rid);
		if (error == 0)
			wmt_cont_max_parse(sc, sc->buf, sc->cont_max_rlen);
		else
			DPRINTF("usbd_req_get_report error=%d\n", error);
	} else
		DPRINTF("Feature report %hhu size invalid or too large: %u\n",
		    sc->cont_max_rid, sc->cont_max_rlen);

	/* Fetch THQA certificate to enable some devices like WaveShare */
	if (sc->thqa_cert_rlen > 0 && sc->thqa_cert_rlen <= WMT_BSIZE &&
	    sc->thqa_cert_rid != sc->cont_max_rid)
		(void)iichid_get_report(iichid, sc->buf, sc->thqa_cert_rlen,
		    I2C_HID_REPORT_TYPE_FEATURE, sc->thqa_cert_rid);

	if (sc->type == HUD_TOUCHPAD && sc->input_mode_rlen != 0) {
		error = imt_set_input_mode(sc, IMT_INPUT_MODE_MT_TOUCHPAD);
		if (error) {
			DPRINTF("Failed to set input mode: %d\n", error);
			goto detach;
		}
	}

	iichid_intr_setup(iichid, &sc->lock, imt_intr, sc);

	sc->evdev = evdev_alloc();
	evdev_set_name(sc->evdev, device_get_desc(dev));
	evdev_set_phys(sc->evdev, device_get_nameunit(dev));
	evdev_set_id(sc->evdev, BUS_I2C, hw->idVendor, hw->idProduct,
	    hw->idVersion);
//	evdev_set_serial(sc->evdev, usb_get_serial(uaa->device));
	evdev_set_methods(sc->evdev, dev, &imt_evdev_methods);
	evdev_set_flag(sc->evdev, EVDEV_FLAG_MT_STCOMPAT);
	switch (sc->type) {
	case HUD_TOUCHSCREEN:
		evdev_support_prop(sc->evdev, INPUT_PROP_DIRECT);
		break;
	case HUD_TOUCHPAD:
		evdev_support_prop(sc->evdev, INPUT_PROP_POINTER);
	}
	evdev_support_event(sc->evdev, EV_SYN);
	evdev_support_event(sc->evdev, EV_ABS);
	WMT_FOREACH_USAGE(sc->caps, i) {
		if (wmt_hid_map[i].code != WMT_NO_CODE)
			evdev_support_abs(sc->evdev, wmt_hid_map[i].code, 0,
			    sc->ai[i].min, sc->ai[i].max, 0, 0, sc->ai[i].res);
	}

	error = evdev_register_mtx(sc->evdev, &sc->lock);
	if (!error)
		return (0);

	iichid_intr_unsetup(iichid);
detach:
	mtx_destroy(&sc->lock);

	return (ENXIO);
}

static int
imt_detach(device_t dev)
{
	struct imt_softc *sc = device_get_softc(dev);
	device_t iichid = device_get_parent(dev);

	evdev_free(sc->evdev);

	iichid_intr_unsetup(iichid);

	mtx_destroy(&sc->lock);

	return (0);
}

static void
imt_intr(void *context, void *buf, int len, uint8_t id)
{
	struct imt_softc *sc = context;
	size_t usage;
	uint32_t *slot_data = sc->slot_data;
	uint32_t cont;
	uint32_t cont_count;
	uint32_t width;
	uint32_t height;
	int32_t slot;

	mtx_assert(&sc->lock, MA_OWNED);

	/* Ignore irrelevant reports */
	if (sc->report_id != id) {
		DPRINTF("Skip report with unexpected ID: %hhu\n", id);
		return;
	}

	/* Make sure we don't process old data */
	if (len < sc->isize)
		bzero((uint8_t *)buf + len, sc->isize - len);

	/*
	 * "In Parallel mode, devices report all contact information in a
	 * single packet. Each physical contact is represented by a logical
	 * collection that is embedded in the top-level collection."
	 *
	 * Since additional contacts that were not present will still be in the
	 * report with contactid=0 but contactids are zero-based, find
	 * contactcount first.
	 */
	cont_count = hid_get_data_unsigned(buf, len, &sc->cont_count_loc);
	/*
	 * "In Hybrid mode, the number of contacts that can be reported in one
	 * report is less than the maximum number of contacts that the device
	 * supports. For example, a device that supports a maximum of
	 * 4 concurrent physical contacts, can set up its top-level collection
	 * to deliver a maximum of two contacts in one report. If four contact
	 * points are present, the device can break these up into two serial
	 * reports that deliver two contacts each.
	 *
	 * "When a device delivers data in this manner, the Contact Count usage
	 * value in the first report should reflect the total number of
	 * contacts that are being delivered in the hybrid reports. The other
	 * serial reports should have a contact count of zero (0)."
	 */
	if (cont_count != 0)
		sc->nconts_todo = cont_count;

#ifdef IMT_DEBUG
	DPRINTFN(6, "cont_count:%2u", (unsigned)cont_count);
	if (wmt_debug >= 6) {
		WMT_FOREACH_USAGE(sc->caps, usage) {
			if (wmt_hid_map[usage].usage != WMT_NO_USAGE)
				printf(" %-4s", wmt_hid_map[usage].name);
		}
		printf("\n");
	}
#endif

	/* Find the number of contacts reported in current report */
	cont_count = MIN(sc->nconts_todo, sc->nconts_per_report);

	/* Use protocol Type B for reporting events */
	for (cont = 0; cont < cont_count; cont++) {

		bzero(slot_data, sizeof(sc->slot_data));
		WMT_FOREACH_USAGE(sc->caps, usage) {
			if (sc->locs[cont][usage].size > 0)
				slot_data[usage] = hid_get_data_unsigned(
				    buf, len, &sc->locs[cont][usage]);
		}

		slot = evdev_get_mt_slot_by_tracking_id(sc->evdev,
		    slot_data[WMT_CONTACTID]);

#ifdef IMT_DEBUG
		DPRINTFN(6, "cont%01x: data = ", cont);
		if (wmt_debug >= 6) {
			WMT_FOREACH_USAGE(sc->caps, usage) {
				if (wmt_hid_map[usage].usage != WMT_NO_USAGE)
					printf("%04x ", slot_data[usage]);
			}
			printf("slot = %d\n", (int)slot);
		}
#endif

		if (slot == -1) {
			DPRINTF("Slot overflow for contact_id %u\n",
			    (unsigned)slot_data[WMT_CONTACTID]);
			continue;
		}

		if (slot_data[WMT_TIP_SWITCH] != 0 &&
		    !(USAGE_SUPPORTED(sc->caps, WMT_CONFIDENCE) &&
		      slot_data[WMT_CONFIDENCE] == 0)) {
			/* This finger is in proximity of the sensor */
			slot_data[WMT_SLOT] = slot;
			slot_data[WMT_IN_RANGE] = !slot_data[WMT_IN_RANGE];
			/* Divided by two to match visual scale of touch */
			width = slot_data[WMT_WIDTH] >> 1;
			height = slot_data[WMT_HEIGHT] >> 1;
			slot_data[WMT_ORIENTATION] = width > height;
			slot_data[WMT_MAJOR] = MAX(width, height);
			slot_data[WMT_MINOR] = MIN(width, height);

			WMT_FOREACH_USAGE(sc->caps, usage)
				if (wmt_hid_map[usage].code != WMT_NO_CODE)
					evdev_push_abs(sc->evdev,
					    wmt_hid_map[usage].code,
					    slot_data[usage]);
		} else {
			evdev_push_abs(sc->evdev, ABS_MT_SLOT, slot);
			evdev_push_abs(sc->evdev, ABS_MT_TRACKING_ID, -1);
		}
	}

	sc->nconts_todo -= cont_count;
	if (sc->nconts_todo == 0)
		evdev_sync(sc->evdev);
}

/*
 * Port of userland hid_report_size() from usbhid(3) to kernel.
 * Unlike it USB-oriented predecessor it does not reserve a 1 byte for
 * report ID as other (I2C) buses encodes report ID in different ways.
 */
static int
wmt_hid_report_size(const void *buf, uint16_t len, enum hid_kind k, uint8_t id)
{
	struct hid_data *d;
	struct hid_item h;
	uint32_t temp;
	uint32_t hpos;
	uint32_t lpos;

	hpos = 0;
	lpos = 0xFFFFFFFF;

	for (d = hid_start_parse(buf, len, 1 << k); hid_get_item(d, &h);) {
		if (h.kind == k && h.report_ID == id) {
			/* compute minimum */
			if (lpos > h.loc.pos)
				lpos = h.loc.pos;
			/* compute end position */
			temp = h.loc.pos + (h.loc.size * h.loc.count);
			/* compute maximum */
			if (hpos < temp)
				hpos = temp;
		}
	}
	hid_end_parse(d);

	/* safety check - can happen in case of currupt descriptors */
	if (lpos > hpos)
		temp = 0;
	else
		temp = hpos - lpos;

	/* return length in bytes rounded up */
	return ((temp + 7) / 8);
}

static int
wmt_hid_parse(struct imt_softc *sc, const void *d_ptr, uint16_t d_len)
{
	struct hid_item hi;
	struct hid_data *hd;
	size_t i;
	size_t cont = 0;
	int type = 0;
	uint32_t caps = 0;
	int32_t cont_count_max = 0;
	uint8_t report_id = 0;
	uint8_t cont_max_rid = 0;
	uint8_t thqa_cert_rid = 0;
	uint8_t input_mode_rid = 0;
	bool touch_coll = false;
	bool finger_coll = false;
	bool conf_coll = false;
	bool cont_count_found = false;
	bool scan_time_found = false;

#define WMT_HI_ABSOLUTE(hi)	\
	(((hi).flags & (HIO_CONST|HIO_VARIABLE|HIO_RELATIVE)) == HIO_VARIABLE)
#define	HUMS_THQA_CERT	0xC5

	/* Parse features for maximum contact count */
	hd = hid_start_parse(d_ptr, d_len, 1 << hid_feature);
	while (hid_get_item(hd, &hi)) {
		switch (hi.kind) {
		case hid_collection:
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHSCREEN))
				touch_coll = true;
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHPAD))
				touch_coll = true;
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_CONFIG))
				conf_coll = true;
			break;
		case hid_endcollection:
			if (hi.collevel == 0 && touch_coll)
				touch_coll = false;
			break;
			if (hi.collevel == 0 && conf_coll)
				conf_coll = false;
			break;
		case hid_feature:
			if (hi.collevel == 1 && touch_coll && hi.usage ==
			      HID_USAGE2(HUP_MICROSOFT, HUMS_THQA_CERT)) {
				thqa_cert_rid = hi.report_ID;
				break;
			}
			if (hi.collevel == 1 && touch_coll &&
			    WMT_HI_ABSOLUTE(hi) && hi.usage ==
			      HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACT_MAX)) {
				cont_count_max = hi.logical_maximum;
				cont_max_rid = hi.report_ID;
				if (sc != NULL)
					sc->cont_max_loc = hi.loc;
			}
			if (conf_coll && WMT_HI_ABSOLUTE(hi) && hi.usage ==
			      HID_USAGE2(HUP_DIGITIZERS, HUD_INPUT_MODE)) {
				input_mode_rid = hi.report_ID;
				if (sc != NULL)
					sc->input_mode_loc = hi.loc;
			}
			break;
		default:
			break;
		}
	}
	hid_end_parse(hd);

	/* Maximum contact count is required usage */
	if (cont_max_rid == 0)
		return (0);

	touch_coll = false;

	/* Parse input for other parameters */
	hd = hid_start_parse(d_ptr, d_len, 1 << hid_input);
	while (hid_get_item(hd, &hi)) {
		switch (hi.kind) {
		case hid_collection:
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHSCREEN)) {
				touch_coll = true;
				type = HUD_TOUCHSCREEN;
			}
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHPAD)) {
				touch_coll = true;
				type = HUD_TOUCHPAD;
			}
			else if (touch_coll && hi.collevel == 2 &&
			    (report_id == 0 || report_id == hi.report_ID) &&
			    hi.usage == HID_USAGE2(HUP_DIGITIZERS, HUD_FINGER))
				finger_coll = true;
			break;
		case hid_endcollection:
			if (hi.collevel == 1 && finger_coll) {
				finger_coll = false;
				cont++;
			} else if (hi.collevel == 0 && touch_coll)
				touch_coll = false;
			break;
		case hid_input:
			/*
			 * Ensure that all usages are located within the same
			 * report and proper collection.
			 */
			if (WMT_HI_ABSOLUTE(hi) && touch_coll &&
			    (report_id == 0 || report_id == hi.report_ID))
				report_id = hi.report_ID;
			else
				break;

			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACTCOUNT)) {
				cont_count_found = true;
				if (sc != NULL)
					sc->cont_count_loc = hi.loc;
				break;
			}
			/* Scan time is required but clobbered by evdev */
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_SCAN_TIME)) {
				scan_time_found = true;
				break;
			}

			if (!finger_coll || hi.collevel != 2)
				break;
			if (sc == NULL && cont > 0)
				break;
			if (cont >= MAX_MT_SLOTS) {
				DPRINTF("Finger %zu ignored\n", cont);
				break;
			}

			for (i = 0; i < WMT_N_USAGES; i++) {
				if (hi.usage == wmt_hid_map[i].usage) {
					if (sc == NULL) {
						if (USAGE_SUPPORTED(caps, i))
							continue;
						caps |= 1 << i;
						break;
					}
					/*
					 * HUG_X usage is an array mapped to
					 * both ABS_MT_POSITION and ABS_MT_TOOL
					 * events. So don`t stop search if we
					 * already have HUG_X mapping done.
					 */
					if (sc->locs[cont][i].size)
						continue;
					sc->locs[cont][i] = hi.loc;
					/*
					 * Hid parser returns valid logical and
					 * physical sizes for first finger only
					 * at least on ElanTS 0x04f3:0x0012.
					 */
					if (cont > 0)
						break;
					caps |= 1 << i;
					sc->ai[i] = (struct wmt_absinfo) {
					    .max = hi.logical_maximum,
					    .min = hi.logical_minimum,
					    .res = hid_item_resolution(&hi),
					};
					break;
				}
			}
			break;
		default:
			break;
		}
	}
	hid_end_parse(hd);

	/* Check for required HID Usages */
	if (!cont_count_found || !scan_time_found || cont == 0)
		return (0);
	for (i = 0; i < WMT_N_USAGES; i++) {
		if (wmt_hid_map[i].required && !USAGE_SUPPORTED(caps, i))
			return (0);
	}

	/* Stop probing here */
	if (sc == NULL)
		return (type);

	/*
	 * According to specifications 'Contact Count Maximum' should be read
	 * from Feature Report rather than from HID descriptor. Set sane
	 * default value now to handle the case of 'Get Report' request failure
	 */
	if (cont_count_max < 1)
		cont_count_max = cont;

	/* Cap contact count maximum to MAX_MT_SLOTS */
	if (cont_count_max > MAX_MT_SLOTS)
		cont_count_max = MAX_MT_SLOTS;

	/* Set number of MT protocol type B slots */
	sc->ai[WMT_SLOT] = (struct wmt_absinfo) {
		.min = 0,
		.max = cont_count_max - 1,
		.res = 0,
	};

	/* Report touch orientation if both width and height are supported */
	if (USAGE_SUPPORTED(caps, WMT_WIDTH) &&
	    USAGE_SUPPORTED(caps, WMT_HEIGHT)) {
		caps |= (1 << WMT_ORIENTATION);
		sc->ai[WMT_ORIENTATION].max = 1;
	}

	sc->isize = wmt_hid_report_size(d_ptr, d_len, hid_input, report_id);
	sc->cont_max_rlen = wmt_hid_report_size(d_ptr, d_len, hid_feature,
	    cont_max_rid);
	if (thqa_cert_rid > 0)
		sc->thqa_cert_rlen = wmt_hid_report_size(d_ptr, d_len,
		    hid_feature, thqa_cert_rid);
	if (input_mode_rid > 0)
		sc->input_mode_rlen = wmt_hid_report_size(d_ptr, d_len,
		    hid_feature, input_mode_rid);

	sc->report_id = report_id;
	sc->caps = caps;
	sc->nconts_per_report = cont;
	sc->cont_max_rid = cont_max_rid;
	sc->thqa_cert_rid = thqa_cert_rid;
	sc->input_mode_rid = input_mode_rid;

	/* Announce information about the touch device */
	device_printf(sc->dev,
	    "%d contacts and [%s%s%s%s%s]. Report range [%d:%d] - [%d:%d]\n",
	    (int)cont_count_max,
	    USAGE_SUPPORTED(sc->caps, WMT_IN_RANGE) ? "R" : "",
	    USAGE_SUPPORTED(sc->caps, WMT_CONFIDENCE) ? "C" : "",
	    USAGE_SUPPORTED(sc->caps, WMT_WIDTH) ? "W" : "",
	    USAGE_SUPPORTED(sc->caps, WMT_HEIGHT) ? "H" : "",
	    USAGE_SUPPORTED(sc->caps, WMT_PRESSURE) ? "P" : "",
	    (int)sc->ai[WMT_X].min, (int)sc->ai[WMT_Y].min,
	    (int)sc->ai[WMT_X].max, (int)sc->ai[WMT_Y].max);
	return (type);
}

static void
wmt_cont_max_parse(struct imt_softc *sc, const void *r_ptr, uint16_t r_len)
{
	uint32_t cont_count_max;

	cont_count_max = hid_get_data_unsigned((const uint8_t *)r_ptr,
	    r_len, &sc->cont_max_loc);
	if (cont_count_max > MAX_MT_SLOTS) {
		DPRINTF("Hardware reported %d contacts while only %d is "
		    "supported\n", (int)cont_count_max, MAX_MT_SLOTS);
		cont_count_max = MAX_MT_SLOTS;
	}
	/* Feature report is a primary source of 'Contact Count Maximum' */
	if (cont_count_max > 0 &&
	    cont_count_max != sc->ai[WMT_SLOT].max + 1) {
		sc->ai[WMT_SLOT].max = cont_count_max - 1;
		device_printf(sc->dev, "%d feature report contacts\n",
		    cont_count_max);
	}
}

static int
imt_set_input_mode(struct imt_softc *sc, enum imt_input_mode mode)
{
	device_t iichid = device_get_parent(sc->dev);
	int error;

	if (sc->input_mode_rlen == 0 && sc->input_mode_rlen > WMT_BSIZE)
		return (EINVAL);

	/* Input Mode report is not strictly required to be readable */
	error = iichid_get_report(iichid, sc->buf, sc->input_mode_rlen,
	    I2C_HID_REPORT_TYPE_FEATURE, sc->input_mode_rid);
	if (error)
		bzero(sc->buf, sc->input_mode_rlen);

	hid_put_data_unsigned(sc->buf, sc->input_mode_rlen,
	    &sc->input_mode_loc, mode);

	error = iichid_set_report(iichid, sc->buf, sc->input_mode_rlen,
	    I2C_HID_REPORT_TYPE_FEATURE, sc->input_mode_rid);

	return (error);
}

DRIVER_MODULE(imt, iichid, imt_driver, imt_devclass, NULL, 0);
MODULE_DEPEND(imt, iichid, 1, 1, 1);
MODULE_DEPEND(imt, usb, 1, 1, 1);
MODULE_DEPEND(imt, evdev, 1, 1, 1);
MODULE_VERSION(imt, 1);
