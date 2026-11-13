/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hda.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Intel High Definition Audio (ICH6 8086:2668) + Full-Duplex Codec (Codec 0):
 *   NID 0: Root (1af4:0022)
 *   NID 1: Audio Function Group (FUNC_01) -> NIDs 2..5
 *   NID 2: Stereo DAC (Output Converter)
 *   NID 3: Speakers Pin Complex (0x01114010, jack sense = plugged) -> fed by NID 2
 *   NID 4: Stereo ADC (Input Converter) <- fed by NID 5
 *   NID 5: Microphone Pin Complex (0x01a19020, jack sense = plugged)
 */

#define HDA_BAR_SIZE 0x4000u

/* Supported PCM format bitmap (HDA 1.0a §7.3.4.7):
 *   Bit 17 = 16-bit PCM (0x00020000)
 *   Bit 6  = 48.0 kHz   (0x00000040)
 *   Bit 5  = 44.1 kHz   (0x00000020)
 */
#define HDA_SUPP_PCM 0x00020060u

static int hda_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_HDA_LOG") != NULL;
    return on;
}

static int mic_test_tone(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_MIC_TEST_TONE") != NULL;
    return on;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t walclk(const HvHda *d) {
    uint64_t dt = now_ns() - d->base_ns;
    /* 24.000 MHz link clock = 24 ticks per microsecond = 24 * ns / 1000 */
    return (uint32_t)((dt * 24ull) / 1000ull);
}

static void decode_fmt(uint16_t fmt, unsigned *rate_hz, unsigned *channels, unsigned *sample_bytes) {
    unsigned base = (fmt & (1u << 14)) ? 44100u : 48000u;
    unsigned mult = ((fmt >> 11) & 7u) + 1u;
    unsigned div = ((fmt >> 8) & 7u) + 1u;
    unsigned bits_code = (fmt >> 4) & 7u;
    unsigned ch = (fmt & 0xfu) + 1u;
    unsigned bytes = bits_code == 0 ? 1u : bits_code == 1 ? 2u : 4u;
    *rate_hz = (base * mult) / div;
    if (*rate_hz < 8000u) *rate_hz = 48000u;
    *channels = ch;
    *sample_bytes = bytes;
}

static void codec_reset_defaults(HvHda *d) {
    d->subsys_id = 0x1af40022u;
    d->afg_power = 0;

    d->dac_fmt = 0x0011u; /* 48 kHz, 16-bit, 2ch */
    d->dac_stream_chan = 0;
    d->dac_power = 0;
    d->dac_amp_l = 0x4au; /* 0 dB, unmuted */
    d->dac_amp_r = 0x4au;

    d->spk_pin_ctl = 0x40u; /* OUT_EN */
    d->spk_unsol = 0;
    d->spk_eapd = 0x02u;
    d->spk_cfg = 0x01114010u; /* Rear Green Speaker Out, Assoc 1, Seq 0 */

    d->adc_fmt = 0x0011u; /* 48 kHz, 16-bit, 2ch */
    d->adc_stream_chan = 0;
    d->adc_power = 0;
    d->adc_amp_l = 0x32u; /* 0 dB, unmuted */
    d->adc_amp_r = 0x32u;
    d->adc_conn_sel = 0;

    d->mic_pin_ctl = 0x24u; /* IN_EN | VREF_80 */
    d->mic_unsol = 0;
    d->mic_cfg = 0x01a19020u; /* Rear Pink Mic In, Assoc 2, Seq 0 */
}

static void stream_reset_regs(HvHda *d, int s) {
    HvHdaStream *st = &d->st[s];
    if (st->running) {
        st->running = 0;
        if (s >= HV_HDA_ISS && d->backend.stop_out) d->backend.stop_out(d->backend.opaque);
        if (s < HV_HDA_ISS && d->backend.stop_in) d->backend.stop_in(d->backend.opaque);
    }
    st->ctl &= 0x000001u; /* preserve SRST bit only */
    st->sts = 0x20u;      /* FIFORDY */
    st->lpib = 0;
    st->cbl = 0;
    st->lvi = 0;
    st->fifos = 0x00bfu;  /* 192-byte hardware FIFO */
    st->fmt = 0x0011u;
    st->bdpl = 0;
    st->bdpu = 0;
    st->rem_num = 0;
}

static void controller_reset(HvHda *d) {
    for (int s = 0; s < HV_HDA_NSTREAMS; s++) {
        d->st[s].ctl = 0;
        stream_reset_regs(d, s);
    }
    d->gctl = 0;
    d->wakeen = 0;
    d->statests = 0;
    d->gsts = 0;
    d->intctl = 0;
    d->ssync = 0;

    d->corblbase = 0;
    d->corbubase = 0;
    d->corbwp = 0;
    d->corbrp = 0;
    d->corbctl = 0;
    d->corbsts = 0;
    d->corbsize = 0x42u; /* 256 entries cap (0x40) | 256 selected (0x02) */

    d->rirblbase = 0;
    d->rirbubase = 0;
    d->rirbwp = 0;
    d->rintcnt = 0;
    d->rirbctl = 0;
    d->rirbsts = 0;
    d->rirbsize = 0x42u;
    d->rirb_cnt = 0;

    d->icoi = 0;
    d->icii = 0;
    d->icis = 0;

    d->dplbase = 0;
    d->dpubase = 0;

    codec_reset_defaults(d);
}

static uint32_t compute_intsts(const HvHda *d) {
    uint32_t v = 0;
    for (int s = 0; s < HV_HDA_NSTREAMS; s++) {
        /* Stream interrupt pending when any status bit (BCIS/FIFOE/DESE) matches its enable bit in SDnCTL */
        if ((d->st[s].sts & 0x1cu) & (d->st[s].ctl & 0x1cu))
            v |= (1u << s);
    }
    int cis = ((d->rirbsts & 0x05u) & (d->rirbctl & 0x05u)) != 0 || ((d->statests & d->wakeen) != 0);
    if (cis) v |= (1u << 30);
    if (((v & 0xffu) & (d->intctl & 0xffu)) || (cis && (d->intctl & (1u << 30))))
        v |= (1u << 31);
    return v;
}

static void update_irq(HvHda *d) {
    uint32_t ists = compute_intsts(d);
    int level = ((d->intctl & (1u << 31)) != 0) && ((ists & (1u << 31)) != 0);
    hv_pci_set_intx(&d->pci, level);
}

/* Execute one 32-bit HDA verb against Codec 0. Returns 1 if valid response generated. */
static int codec_exec_verb(HvHda *d, uint32_t cmd, uint32_t *resp_out) {
    uint32_t cad = (cmd >> 28) & 0xfu;
    if (cad != 0) return 0; /* Only Codec 0 is populated */
    if ((cmd >> 27) & 1u) {
        *resp_out = 0;
        return 1;
    }

    uint32_t nid = (cmd >> 20) & 0x7fu;
    uint32_t payload = cmd & 0xfffffu;
    uint32_t verb12 = (payload >> 8) & 0xfffu;
    uint32_t parm8 = payload & 0xffu;
    uint32_t verb4 = (payload >> 16) & 0xfu;
    uint32_t parm16 = payload & 0xffffu;
    uint32_t r = 0;

    /* 4-bit verbs with 16-bit payload: 0x2, 0x3, 0xa, 0xb */
    if (verb4 == 0x2u) { /* SET_CONV_FMT */
        if (nid == 2) d->dac_fmt = (uint16_t)parm16;
        else if (nid == 4) d->adc_fmt = (uint16_t)parm16;
        *resp_out = 0;
        return 1;
    }
    if (verb4 == 0xau) { /* GET_CONV_FMT */
        if (nid == 2) r = d->dac_fmt;
        else if (nid == 4) r = d->adc_fmt;
        *resp_out = r;
        return 1;
    }
    if (verb4 == 0x3u) { /* SET_AMP_GAIN_MUTE */
        uint8_t val = (uint8_t)(parm16 & 0xffu);
        if (nid == 2 && (parm16 & 0x8000u)) { /* Output amp */
            if (parm16 & 0x2000u) d->dac_amp_l = val;
            if (parm16 & 0x1000u) d->dac_amp_r = val;
        } else if (nid == 4 && (parm16 & 0x4000u)) { /* Input amp */
            if (parm16 & 0x2000u) d->adc_amp_l = val;
            if (parm16 & 0x1000u) d->adc_amp_r = val;
        }
        *resp_out = 0;
        return 1;
    }
    if (verb4 == 0xbu) { /* GET_AMP_GAIN_MUTE */
        int left = (parm16 & 0x2000u) != 0;
        int out_amp = (parm16 & 0x8000u) != 0;
        if (nid == 2 && out_amp) r = left ? d->dac_amp_l : d->dac_amp_r;
        else if (nid == 4 && !out_amp) r = left ? d->adc_amp_l : d->adc_amp_r;
        *resp_out = r;
        return 1;
    }

    /* 12-bit verbs with 8-bit parameter */
    switch (verb12) {
    case 0xf00u: /* GET_PARAMETER */
        switch (nid) {
        case 0: /* Root Node */
            if (parm8 == 0x00) r = 0x1af40022u; /* Vendor/Device: Red Hat HDA Duplex */
            else if (parm8 == 0x02) r = 0x00100101u; /* Revision 1.0 */
            else if (parm8 == 0x04) r = 0x00010001u; /* Start NID 1, count 1 */
            break;
        case 1: /* Audio Function Group */
            if (parm8 == 0x04) r = 0x00020004u;      /* Start NID 2, count 4 (2..5) */
            else if (parm8 == 0x05) r = 0x00000001u; /* Audio Function Group */
            else if (parm8 == 0x08) r = 0x00010808u; /* Audio FG Caps */
            else if (parm8 == 0x0a) r = HDA_SUPP_PCM;
            else if (parm8 == 0x0b) r = 0x00000001u; /* PCM */
            else if (parm8 == 0x0d) r = 0x80034a32u; /* In Amp Cap */
            else if (parm8 == 0x0f) r = 0x00000009u; /* D0 | D3 */
            else if (parm8 == 0x11) r = 0x00000000u; /* GPIO count */
            else if (parm8 == 0x12) r = 0x80034a4au; /* Out Amp Cap */
            break;
        case 2: /* Stereo DAC */
            if (parm8 == 0x09) r = 0x0000041du;      /* Audio Output, Stereo, OutAmp, FmtOvr, Pwr */
            else if (parm8 == 0x0a) r = HDA_SUPP_PCM;
            else if (parm8 == 0x0b) r = 0x00000001u;
            else if (parm8 == 0x0f) r = 0x00000009u;
            else if (parm8 == 0x12) r = 0x80034a4au;
            break;
        case 3: /* Speaker Pin Complex */
            if (parm8 == 0x09) r = 0x00400181u;      /* Pin Complex, ConnList, Unsol, Stereo */
            else if (parm8 == 0x0c) r = 0x00010014u; /* EAPD | Output | Presence Detect */
            else if (parm8 == 0x0e) r = 0x00000001u; /* 1 connection: NID 2 */
            break;
        case 4: /* Stereo ADC */
            if (parm8 == 0x09) r = 0x0010051bu;      /* Audio Input, Stereo, InAmp, FmtOvr, ConnList, Pwr */
            else if (parm8 == 0x0a) r = HDA_SUPP_PCM;
            else if (parm8 == 0x0b) r = 0x00000001u;
            else if (parm8 == 0x0d) r = 0x80034a32u;
            else if (parm8 == 0x0e) r = 0x00000001u; /* 1 connection: NID 5 */
            else if (parm8 == 0x0f) r = 0x00000009u;
            break;
        case 5: /* Microphone Pin Complex */
            if (parm8 == 0x09) r = 0x00400081u;      /* Pin Complex, Unsol, Stereo */
            else if (parm8 == 0x0c) r = 0x00001724u; /* VRef | Input | Presence Detect */
            break;
        default:
            break;
        }
        break;
    case 0xf01u: /* GET_CONN_SELECT */
        if (nid == 4) r = d->adc_conn_sel;
        break;
    case 0x701u: /* SET_CONN_SELECT */
        if (nid == 4) d->adc_conn_sel = (uint8_t)parm8;
        break;
    case 0xf02u: /* GET_CONN_LIST_ENTRY */
        if (parm8 == 0) {
            if (nid == 3) r = 0x00000002u;      /* Speaker fed by DAC NID 2 */
            else if (nid == 4) r = 0x00000005u; /* ADC fed by Mic NID 5 */
        }
        break;
    case 0xf05u: /* GET_POWER_STATE */
        if (nid == 1) r = ((uint32_t)d->afg_power << 4) | d->afg_power;
        else if (nid == 2) r = ((uint32_t)d->dac_power << 4) | d->dac_power;
        else if (nid == 4) r = ((uint32_t)d->adc_power << 4) | d->adc_power;
        break;
    case 0x705u: /* SET_POWER_STATE */
        if (nid == 1) d->afg_power = (uint8_t)(parm8 & 0x3u);
        else if (nid == 2) d->dac_power = (uint8_t)(parm8 & 0x3u);
        else if (nid == 4) d->adc_power = (uint8_t)(parm8 & 0x3u);
        break;
    case 0xf06u: /* GET_CONV_STREAM_CHAN */
        if (nid == 2) r = d->dac_stream_chan;
        else if (nid == 4) r = d->adc_stream_chan;
        break;
    case 0x706u: /* SET_CONV_STREAM_CHAN */
        if (nid == 2) d->dac_stream_chan = (uint8_t)parm8;
        else if (nid == 4) d->adc_stream_chan = (uint8_t)parm8;
        break;
    case 0xf07u: /* GET_PIN_WIDGET_CONTROL */
        if (nid == 3) r = d->spk_pin_ctl;
        else if (nid == 5) r = d->mic_pin_ctl;
        break;
    case 0x707u: /* SET_PIN_WIDGET_CONTROL */
        if (nid == 3) d->spk_pin_ctl = (uint8_t)parm8;
        else if (nid == 5) d->mic_pin_ctl = (uint8_t)parm8;
        break;
    case 0xf08u: /* GET_UNSOL_ENABLE */
        if (nid == 3) r = d->spk_unsol;
        else if (nid == 5) r = d->mic_unsol;
        break;
    case 0x708u: /* SET_UNSOL_ENABLE */
        if (nid == 3) d->spk_unsol = (uint8_t)parm8;
        else if (nid == 5) d->mic_unsol = (uint8_t)parm8;
        break;
    case 0xf09u: /* GET_PIN_SENSE */
        if (nid == 3 || nid == 5) r = 0x80000000u; /* Presence Detect = Plugged In */
        break;
    case 0x709u: /* EXECUTE_PIN_SENSE */
        break;
    case 0xf0cu: /* GET_EAPD_BTL */
        if (nid == 3) r = d->spk_eapd;
        break;
    case 0x70cu: /* SET_EAPD_BTL */
        if (nid == 3) d->spk_eapd = (uint8_t)parm8;
        break;
    case 0xf1cu: /* GET_CONFIG_DEFAULT */
        if (nid == 3) r = d->spk_cfg;
        else if (nid == 5) r = d->mic_cfg;
        break;
    case 0x71cu: case 0x71du: case 0x71eu: case 0x71fu: {
        uint32_t shift = (verb12 - 0x71cu) * 8u;
        if (nid == 3) d->spk_cfg = (d->spk_cfg & ~(0xffu << shift)) | (parm8 << shift);
        else if (nid == 5) d->mic_cfg = (d->mic_cfg & ~(0xffu << shift)) | (parm8 << shift);
        break;
    }
    case 0xf20u: /* GET_SUBSYSTEM_ID */
        if (nid == 1) r = d->subsys_id;
        break;
    case 0x720u: case 0x721u: case 0x722u: case 0x723u: {
        uint32_t shift = (verb12 - 0x720u) * 8u;
        if (nid == 1) d->subsys_id = (d->subsys_id & ~(0xffu << shift)) | (parm8 << shift);
        break;
    }
    case 0x7ffu: /* FUNCTION_RESET */
        if (nid == 1) codec_reset_defaults(d);
        break;
    default:
        break;
    }

    if (hda_log())
        fprintf(stderr, "hda: verb cmd=%08x (nid=%u v12=%03x p8=%02x) -> %08x\n", cmd, nid, verb12, parm8, r);
    *resp_out = r;
    return 1;
}

static uint32_t ring_entries(uint8_t sz_reg) {
    unsigned sel = sz_reg & 3u;
    return sel == 0 ? 2u : sel == 1 ? 16u : 256u;
}

static void corb_process(HvHda *d) {
    if (!(d->gctl & 1u) || !(d->corbctl & 0x02u) || (d->corbrp & 0x8000u)) return;
    uint32_t corb_n = ring_entries(d->corbsize);
    uint32_t rirb_n = ring_entries(d->rirbsize);
    uint64_t corb_pa = ((uint64_t)d->corbubase << 32) | (d->corblbase & ~0x7fu);
    uint64_t rirb_pa = ((uint64_t)d->rirbubase << 32) | (d->rirblbase & ~0x7fu);
    if (!corb_pa) return;

    int pushed = 0;
    uint32_t guard = 0;
    while ((d->corbrp & 0xffu) != (d->corbwp & (corb_n - 1u)) && guard++ < 512u) {
        uint32_t next_rp = ((d->corbrp & 0xffu) + 1u) % corb_n;
        d->corbrp = (uint16_t)next_rp;
        uint32_t cmd = 0;
        hv_phys_read(d->m, corb_pa + (uint64_t)next_rp * 4u, &cmd, 4);

        uint32_t resp = 0;
        if (codec_exec_verb(d, cmd, &resp) && (d->rirbctl & 0x02u) && rirb_pa) {
            d->rirbwp = (uint16_t)(((d->rirbwp & 0xffu) + 1u) % rirb_n);
            uint32_t entry[2] = {resp, 0}; /* resp_ex = 0 (Codec 0, solicited) */
            hv_phys_write(d->m, rirb_pa + (uint64_t)d->rirbwp * 8u, entry, 8);
            d->rirb_cnt++;
            pushed = 1;
            uint32_t thresh = d->rintcnt ? d->rintcnt : 256u;
            if (d->rirb_cnt >= thresh) {
                d->rirb_cnt = 0;
                if (d->rirbctl & 0x01u) d->rirbsts |= 0x01u;
            }
        }
    }
    if (pushed && (d->rirbctl & 0x01u)) {
        d->rirb_cnt = 0;
        d->rirbsts |= 0x01u; /* RINTFL */
    }
}

static void write_dpib(HvHda *d, int s, uint32_t lpib) {
    if (!(d->dplbase & 1u)) return;
    uint64_t dp_pa = ((uint64_t)d->dpubase << 32) | (d->dplbase & ~0x7fu);
    if (!dp_pa) return;
    hv_phys_write(d->m, dp_pa + (uint64_t)s * 8u, &lpib, 4);
}

static void stream_sync_run(HvHda *d, int s) {
    HvHdaStream *st = &d->st[s];
    int want_run = (d->gctl & 1u) && !(st->ctl & 1u) && ((st->ctl & 2u) != 0);
    if (want_run && !st->running) {
        unsigned rate_hz = 48000, ch = 2, sb = 2;
        decode_fmt(st->fmt, &rate_hz, &ch, &sb);
        st->running = 1;
        st->last_ns = now_ns();
        st->rem_num = 0;
        if (s >= HV_HDA_ISS) {
            atomic_store_explicit(&d->out_rate_hz, rate_hz, memory_order_release);
            atomic_store_explicit(&d->out_channels, ch, memory_order_release);
            if (d->backend.start_out) d->backend.start_out(d->backend.opaque, ch, rate_hz);
        } else {
            atomic_store_explicit(&d->in_rate_hz, rate_hz, memory_order_release);
            atomic_store_explicit(&d->in_channels, ch, memory_order_release);
            if (d->backend.start_in) d->backend.start_in(d->backend.opaque, ch, rate_hz);
        }
        if (hda_log())
            fprintf(stderr, "hda: stream %d (%s) START rate=%u ch=%u cbl=%u lvi=%u fmt=%04x\n",
                    s, s >= HV_HDA_ISS ? "OUT" : "IN", rate_hz, ch, st->cbl, st->lvi, st->fmt);
    } else if (!want_run && st->running) {
        st->running = 0;
        if (s >= HV_HDA_ISS && d->backend.stop_out) d->backend.stop_out(d->backend.opaque);
        if (s < HV_HDA_ISS && d->backend.stop_in) d->backend.stop_in(d->backend.opaque);
        if (hda_log())
            fprintf(stderr, "hda: stream %d (%s) STOP\n", s, s >= HV_HDA_ISS ? "OUT" : "IN");
    }
}

static uint8_t reg_read8(HvHda *d, uint32_t off) {
    /* HDA +0x2000 Wall Clock & LPIB Alias Window */
    if (off >= 0x2000u && off < 0x2200u) {
        uint32_t sub = off - 0x2000u;
        if (sub >= 0x30u && sub < 0x34u) off = sub;
        else if (sub >= 0x80u && ((sub - 0x80u) & 0x1fu) >= 4u && ((sub - 0x80u) & 0x1fu) < 8u) off = sub;
        else return 0;
    }

    if (off >= 0x80u && off < 0x80u + HV_HDA_NSTREAMS * 0x20u) {
        int s = (int)((off - 0x80u) / 0x20u);
        uint32_t r = (off - 0x80u) & 0x1fu;
        const HvHdaStream *st = &d->st[s];
        switch (r) {
        case 0x00: return (uint8_t)(st->ctl & 0xffu);
        case 0x01: return (uint8_t)((st->ctl >> 8) & 0xffu);
        case 0x02: return (uint8_t)((st->ctl >> 16) & 0xffu);
        case 0x03: return (uint8_t)(st->sts | 0x20u);
        case 0x04: case 0x05: case 0x06: case 0x07: return (uint8_t)(st->lpib >> ((r - 0x04) * 8));
        case 0x08: case 0x09: case 0x0a: case 0x0b: return (uint8_t)(st->cbl >> ((r - 0x08) * 8));
        case 0x0c: case 0x0d: return (uint8_t)(st->lvi >> ((r - 0x0c) * 8));
        case 0x10: case 0x11: return (uint8_t)(st->fifos >> ((r - 0x10) * 8));
        case 0x12: case 0x13: return (uint8_t)(st->fmt >> ((r - 0x12) * 8));
        case 0x18: case 0x19: case 0x1a: case 0x1b: return (uint8_t)(st->bdpl >> ((r - 0x18) * 8));
        case 0x1c: case 0x1d: case 0x1e: case 0x1f: return (uint8_t)(st->bdpu >> ((r - 0x1c) * 8));
        default: return 0;
        }
    }

    switch (off) {
    case 0x00: return 0x01u; /* GCAP low: 64OK = 1 */
    case 0x01: return (uint8_t)((HV_HDA_OSS << 4) | HV_HDA_ISS); /* GCAP high: OSS=4, ISS=4 -> 0x4401 */
    case 0x02: return 0x00u; /* VMIN = 0 */
    case 0x03: return 0x01u; /* VMAJ = 1 */
    case 0x04: return 0x3cu; /* OUTPAY */
    case 0x05: return 0x00u;
    case 0x06: return 0x1du; /* INPAY */
    case 0x07: return 0x00u;
    case 0x08: case 0x09: case 0x0a: case 0x0b: return (uint8_t)(d->gctl >> ((off - 0x08) * 8));
    case 0x0c: case 0x0d: return (uint8_t)(d->wakeen >> ((off - 0x0c) * 8));
    case 0x0e: case 0x0f: return (uint8_t)(d->statests >> ((off - 0x0e) * 8));
    case 0x10: case 0x11: return (uint8_t)(d->gsts >> ((off - 0x10) * 8));
    case 0x20: case 0x21: case 0x22: case 0x23: return (uint8_t)(d->intctl >> ((off - 0x20) * 8));
    case 0x24: case 0x25: case 0x26: case 0x27: {
        uint32_t ists = compute_intsts(d);
        return (uint8_t)(ists >> ((off - 0x24) * 8));
    }
    case 0x30: case 0x31: case 0x32: case 0x33: {
        uint32_t wc = walclk(d);
        return (uint8_t)(wc >> ((off - 0x30) * 8));
    }
    case 0x34: case 0x35: case 0x36: case 0x37: return (uint8_t)(d->ssync >> ((off - 0x34) * 8));
    case 0x38: case 0x39: case 0x3a: case 0x3b: return (uint8_t)(d->ssync >> ((off - 0x38) * 8));
    case 0x40: case 0x41: case 0x42: case 0x43: return (uint8_t)(d->corblbase >> ((off - 0x40) * 8));
    case 0x44: case 0x45: case 0x46: case 0x47: return (uint8_t)(d->corbubase >> ((off - 0x44) * 8));
    case 0x48: case 0x49: return (uint8_t)(d->corbwp >> ((off - 0x48) * 8));
    case 0x4a: case 0x4b: return (uint8_t)(d->corbrp >> ((off - 0x4a) * 8));
    case 0x4c: return d->corbctl;
    case 0x4d: return d->corbsts;
    case 0x4e: return d->corbsize;
    case 0x50: case 0x51: case 0x52: case 0x53: return (uint8_t)(d->rirblbase >> ((off - 0x50) * 8));
    case 0x54: case 0x55: case 0x56: case 0x57: return (uint8_t)(d->rirbubase >> ((off - 0x54) * 8));
    case 0x58: case 0x59: return (uint8_t)(d->rirbwp >> ((off - 0x58) * 8));
    case 0x5a: case 0x5b: return (uint8_t)(d->rintcnt >> ((off - 0x5a) * 8));
    case 0x5c: return d->rirbctl;
    case 0x5d: return d->rirbsts;
    case 0x5e: return d->rirbsize;
    case 0x60: case 0x61: case 0x62: case 0x63: return (uint8_t)(d->icoi >> ((off - 0x60) * 8));
    case 0x64: case 0x65: case 0x66: case 0x67: return (uint8_t)(d->icii >> ((off - 0x64) * 8));
    case 0x68: case 0x69: return (uint8_t)(d->icis >> ((off - 0x68) * 8));
    case 0x70: case 0x71: case 0x72: case 0x73: return (uint8_t)(d->dplbase >> ((off - 0x70) * 8));
    case 0x74: case 0x75: case 0x76: case 0x77: return (uint8_t)(d->dpubase >> ((off - 0x74) * 8));
    default: return 0;
    }
}

static void reg_write8(HvHda *d, uint32_t off, uint8_t b, int * touched_corb, int *touched_icb, uint32_t *touched_streams) {
    if (off >= 0x80u && off < 0x80u + HV_HDA_NSTREAMS * 0x20u) {
        int s = (int)((off - 0x80u) / 0x20u);
        uint32_t r = (off - 0x80u) & 0x1fu;
        HvHdaStream *st = &d->st[s];
        *touched_streams |= (1u << s);
        switch (r) {
        case 0x00: {
            uint8_t old_srst = st->ctl & 1u;
            st->ctl = (st->ctl & ~0xffu) | b;
            if ((b & 1u) && !old_srst) stream_reset_regs(d, s);
            break;
        }
        case 0x01: st->ctl = (st->ctl & ~0xff00u) | ((uint32_t)b << 8); break;
        case 0x02: st->ctl = (st->ctl & ~0xff0000u) | ((uint32_t)b << 16); break;
        case 0x03: st->sts = (uint8_t)((st->sts & ~(b & 0x1cu)) | 0x20u); break; /* RW1C on bits 2..4 */
        case 0x08: case 0x09: case 0x0a: case 0x0b: {
            uint32_t sh = (r - 0x08) * 8u;
            st->cbl = (st->cbl & ~(0xffu << sh)) | ((uint32_t)b << sh);
            break;
        }
        case 0x0c: case 0x0d: {
            uint32_t sh = (r - 0x0c) * 8u;
            st->lvi = (uint16_t)((st->lvi & ~(0xffu << sh)) | ((uint16_t)b << sh));
            break;
        }
        case 0x12: case 0x13: {
            uint32_t sh = (r - 0x12) * 8u;
            st->fmt = (uint16_t)((st->fmt & ~(0xffu << sh)) | ((uint16_t)b << sh));
            break;
        }
        case 0x18: case 0x19: case 0x1a: case 0x1b: {
            uint32_t sh = (r - 0x18) * 8u;
            st->bdpl = ((st->bdpl & ~(0xffu << sh)) | ((uint32_t)b << sh)) & ~0x7fu;
            break;
        }
        case 0x1c: case 0x1d: case 0x1e: case 0x1f: {
            uint32_t sh = (r - 0x1c) * 8u;
            st->bdpu = (st->bdpu & ~(0xffu << sh)) | ((uint32_t)b << sh);
            break;
        }
        default: break;
        }
        return;
    }

    switch (off) {
    case 0x08: {
        uint8_t old_crst = d->gctl & 1u;
        if (!(b & 1u)) {
            controller_reset(d);
        } else {
            d->gctl = (d->gctl & ~0xffu) | (b & 0x03u);
            if (!old_crst) d->statests |= 0x0001u; /* Codec 0 detected on SDI0 */
        }
        break;
    }
    case 0x09: d->gctl = (d->gctl & ~0xff00u) | ((uint32_t)(b & 1u) << 8); break;
    case 0x0c: d->wakeen = (uint16_t)((d->wakeen & 0xff00u) | b); break;
    case 0x0d: d->wakeen = (uint16_t)((d->wakeen & 0x00ffu) | ((uint16_t)b << 8)); break;
    case 0x0e: d->statests &= (uint16_t)~b; break; /* RW1C */
    case 0x0f: d->statests &= (uint16_t)~((uint16_t)b << 8); break;
    case 0x10: d->gsts &= (uint16_t)~b; break;     /* RW1C */
    case 0x20: case 0x21: case 0x22: case 0x23: {
        uint32_t sh = (off - 0x20) * 8u;
        d->intctl = (d->intctl & ~(0xffu << sh)) | ((uint32_t)b << sh);
        break;
    }
    case 0x34: case 0x35: case 0x36: case 0x37: {
        uint32_t sh = (off - 0x34) * 8u;
        d->ssync = (d->ssync & ~(0xffu << sh)) | ((uint32_t)b << sh);
        break;
    }
    case 0x38: case 0x39: case 0x3a: case 0x3b: {
        uint32_t sh = (off - 0x38) * 8u;
        d->ssync = (d->ssync & ~(0xffu << sh)) | ((uint32_t)b << sh);
        break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: {
        uint32_t sh = (off - 0x40) * 8u;
        d->corblbase = ((d->corblbase & ~(0xffu << sh)) | ((uint32_t)b << sh)) & ~0x7fu;
        break;
    }
    case 0x44: case 0x45: case 0x46: case 0x47: {
        uint32_t sh = (off - 0x44) * 8u;
        d->corbubase = (d->corbubase & ~(0xffu << sh)) | ((uint32_t)b << sh);
        break;
    }
    case 0x48:
        d->corbwp = (uint16_t)((d->corbwp & 0xff00u) | b);
        *touched_corb = 1;
        break;
    case 0x49:
        d->corbwp = (uint16_t)((d->corbwp & 0x00ffu) | ((uint16_t)(b & 0u) << 8));
        *touched_corb = 1;
        break;
    case 0x4b: /* CORBRP high byte: bit 7 is CORBRPRST (bit 15 of CORBRP) */
        if (b & 0x80u) d->corbrp = 0x8000u; /* reset RP to 0 and latch bit 15 = 1 */
        else {
            d->corbrp &= 0x00ffu;
            *touched_corb = 1;
        }
        break;
    case 0x4c:
        d->corbctl = b & 0x03u;
        *touched_corb = 1;
        break;
    case 0x4d: d->corbsts &= (uint8_t)~(b & 0x01u); break;
    case 0x4e: d->corbsize = (uint8_t)(0x70u | (b & 0x03u)); break;
    case 0x50: case 0x51: case 0x52: case 0x53: {
        uint32_t sh = (off - 0x50) * 8u;
        d->rirblbase = ((d->rirblbase & ~(0xffu << sh)) | ((uint32_t)b << sh)) & ~0x7fu;
        break;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: {
        uint32_t sh = (off - 0x54) * 8u;
        d->rirbubase = (d->rirbubase & ~(0xffu << sh)) | ((uint32_t)b << sh);
        break;
    }
    case 0x59:
        if (b & 0x80u) d->rirbwp = 0; /* RIRBWPRST */
        break;
    case 0x5a: d->rintcnt = (uint16_t)((d->rintcnt & 0xff00u) | b); break;
    case 0x5b: d->rintcnt = (uint16_t)((d->rintcnt & 0x00ffu) | ((uint16_t)(b & 0u) << 8)); break;
    case 0x5c:
        d->rirbctl = b & 0x07u;
        *touched_corb = 1;
        break;
    case 0x5d: d->rirbsts &= (uint8_t)~(b & 0x05u); break; /* RW1C */
    case 0x5e: d->rirbsize = (uint8_t)(0x70u | (b & 0x03u)); break;
    case 0x60: case 0x61: case 0x62: case 0x63: {
        uint32_t sh = (off - 0x60) * 8u;
        d->icoi = (d->icoi & ~(0xffu << sh)) | ((uint32_t)b << sh);
        break;
    }
    case 0x68:
        if (b & 0x02u) d->icis &= (uint16_t)~0x02u; /* IRV RW1C */
        if (b & 0x01u) *touched_icb = 1;            /* ICB */
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: {
        uint32_t sh = (off - 0x70) * 8u;
        uint32_t mask = (off == 0x70) ? 0x81u : 0xffu; /* preserve bits 6:1 = 0, bit 0 = DPBE */
        d->dplbase = (d->dplbase & ~(0xffu << sh)) | ((uint32_t)(b & mask) << sh);
        break;
    }
    case 0x74: case 0x75: case 0x76: case 0x77: {
        uint32_t sh = (off - 0x74) * 8u;
        d->dpubase = (d->dpubase & ~(0xffu << sh)) | ((uint32_t)b << sh);
        break;
    }
    default: break;
    }
}

static uint64_t bar_read(HvPciDev *pci, int bar, uint64_t off, unsigned size) {
    (void)bar;
    HvHda *d = pci->opaque;
    uint64_t v = 0;
    for (unsigned i = 0; i < size && off + i < HDA_BAR_SIZE; i++)
        v |= (uint64_t)reg_read8(d, (uint32_t)(off + i)) << (i * 8u);
    return v;
}

static void bar_write(HvPciDev *pci, int bar, uint64_t off, uint64_t v, unsigned size) {
    (void)bar;
    HvHda *d = pci->opaque;
    int touched_corb = 0, touched_icb = 0;
    uint32_t touched_streams = 0;
    for (unsigned i = 0; i < size && off + i < HDA_BAR_SIZE; i++) {
        uint8_t b = (uint8_t)(v >> (i * 8u));
        reg_write8(d, (uint32_t)(off + i), b, &touched_corb, &touched_icb, &touched_streams);
    }
    if (touched_icb) {
        uint32_t resp = 0;
        if (codec_exec_verb(d, d->icoi, &resp)) {
            d->icii = resp;
            d->icis = (d->icis & ~0x01u) | 0x02u; /* ICB=0, IRV=1 */
        } else {
            d->icis &= ~0x01u;
        }
    }
    if (touched_corb) corb_process(d);
    if (touched_streams) {
        for (int s = 0; s < HV_HDA_NSTREAMS; s++)
            if (touched_streams & (1u << s)) stream_sync_run(d, s);
    }
    update_irq(d);
}

static void on_cfg_write(HvPciDev *pci) {
    HvHda *d = pci->opaque;
    update_irq(d);
}

/* ---- PCM Ring Helpers ---- */

static void tx_ring_push(HvHda *d, const uint8_t *src, uint32_t len, uint8_t amp_l, uint8_t amp_r) {
    uint64_t w = atomic_load_explicit(&d->tx_written, memory_order_relaxed);
    uint64_t c = atomic_load_explicit(&d->tx_consumed, memory_order_acquire);
    /* Drop oldest audio if ring would overflow so playback stays low-latency (<150 ms) */
    uint32_t max_keep = HV_HDA_RING_BYTES / 4u; /* 64 KiB (~340 ms max queue) */
    if (w - c + len > max_keep) {
        uint64_t drop = (w - c + len) - (max_keep / 2u);
        drop &= ~3ull;
        atomic_compare_exchange_strong(&d->tx_consumed, &c, c + drop);
    }

    int mute_l = (amp_l & 0x80u) != 0, mute_r = (amp_r & 0x80u) != 0;
    int gain_l = amp_l & 0x7fu, gain_r = amp_r & 0x7fu; /* 0x4a = 0 dB */

    uint32_t off = 0;
    while (off < len) {
        uint32_t at = (uint32_t)((w + off) % HV_HDA_RING_BYTES);
        uint32_t n = len - off;
        if (n > HV_HDA_RING_BYTES - at) n = HV_HDA_RING_BYTES - at;
        if (mute_l && mute_r) {
            memset(d->tx_ring + at, 0, n);
        } else {
            memcpy(d->tx_ring + at, src + off, n);
            if ((mute_l || mute_r || gain_l != 0x4a || gain_r != 0x4a) && (n & 3u) == 0) {
                int16_t *pcm = (int16_t *)(d->tx_ring + at);
                uint32_t frames = n / 4u;
                int mul_l = mute_l ? 0 : (gain_l * 256) / 0x4a;
                int mul_r = mute_r ? 0 : (gain_r * 256) / 0x4a;
                for (uint32_t f = 0; f < frames; f++) {
                    int32_t sl = (pcm[f * 2] * mul_l) >> 8;
                    int32_t sr = (pcm[f * 2 + 1] * mul_r) >> 8;
                    if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
                    if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
                    pcm[f * 2] = (int16_t)sl;
                    pcm[f * 2 + 1] = (int16_t)sr;
                }
            }
        }
        off += n;
    }
    atomic_store_explicit(&d->tx_written, w + len, memory_order_release);
}

uint32_t hv_hda_pull_playback(HvHda *d, void *dst, uint32_t bytes) {
    if (!d) {
        memset(dst, 0, bytes);
        return 0;
    }
    uint64_t c = atomic_load_explicit(&d->tx_consumed, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&d->tx_written, memory_order_acquire);
    uint32_t avail = (w > c) ? (uint32_t)(w - c) : 0;
    uint32_t n = avail < bytes ? avail : bytes;
    n &= ~3u; /* frame-aligned stereo S16LE */

    uint32_t off = 0;
    while (off < n) {
        uint32_t at = (uint32_t)((c + off) % HV_HDA_RING_BYTES);
        uint32_t k = n - off;
        if (k > HV_HDA_RING_BYTES - at) k = HV_HDA_RING_BYTES - at;
        memcpy((uint8_t *)dst + off, d->tx_ring + at, k);
        off += k;
    }
    if (n < bytes) memset((uint8_t *)dst + n, 0, bytes - n);
    atomic_compare_exchange_strong(&d->tx_consumed, &c, c + n);
    return n;
}

void hv_hda_push_capture(HvHda *d, const void *src, uint32_t bytes) {
    if (!d || !bytes) return;
    bytes &= ~3u;
    uint64_t w = atomic_load_explicit(&d->rx_written, memory_order_relaxed);
    uint64_t c = atomic_load_explicit(&d->rx_consumed, memory_order_acquire);
    uint32_t max_keep = HV_HDA_RING_BYTES / 4u;
    if (w - c + bytes > max_keep) {
        uint64_t drop = (w - c + bytes) - (max_keep / 2u);
        drop &= ~3ull;
        atomic_compare_exchange_strong(&d->rx_consumed, &c, c + drop);
    }
    uint32_t off = 0;
    while (off < bytes) {
        uint32_t at = (uint32_t)((w + off) % HV_HDA_RING_BYTES);
        uint32_t k = bytes - off;
        if (k > HV_HDA_RING_BYTES - at) k = HV_HDA_RING_BYTES - at;
        memcpy(d->rx_ring + at, (const uint8_t *)src + off, k);
        off += k;
    }
    atomic_store_explicit(&d->rx_written, w + bytes, memory_order_release);
}

static void rx_ring_pull(HvHda *d, uint8_t *dst, uint32_t len, unsigned rate_hz, uint8_t amp_l, uint8_t amp_r) {
    int muted = ((amp_l & 0x80u) != 0) && ((amp_r & 0x80u) != 0);
    uint64_t c = atomic_load_explicit(&d->rx_consumed, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&d->rx_written, memory_order_acquire);
    uint32_t avail = (w > c) ? (uint32_t)(w - c) : 0;
    uint32_t n = avail < len ? avail : len;
    n &= ~3u;

    uint32_t off = 0;
    while (off < n) {
        uint32_t at = (uint32_t)((c + off) % HV_HDA_RING_BYTES);
        uint32_t k = n - off;
        if (k > HV_HDA_RING_BYTES - at) k = HV_HDA_RING_BYTES - at;
        memcpy(dst + off, d->rx_ring + at, k);
        off += k;
    }
    atomic_compare_exchange_strong(&d->rx_consumed, &c, c + n);

    if (n < len) {
        if (mic_test_tone() && !muted) {
            /* Deterministic 440 Hz stereo sine wave for automated headless mic verification */
            int16_t *pcm = (int16_t *)(dst + n);
            uint32_t frames = (len - n) / 4u;
            for (uint32_t f = 0; f < frames; f++) {
                double ph = 2.0 * 3.14159265358979323846 * 440.0 * (double)(d->rx_frames_total + f) / (double)rate_hz;
                int16_t v = (int16_t)(sin(ph) * 12000.0);
                pcm[f * 2] = v;
                pcm[f * 2 + 1] = v;
            }
        } else {
            memset(dst + n, 0, len - n);
        }
    }
    if (muted) memset(dst, 0, len);
}

/* ---- Periodic Wall-Clock Stream DMA Engine ---- */

typedef struct HdaBdlEntry {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;
} HdaBdlEntry;

void hv_hda_reset(HvHda *d) {
    if (d) controller_reset(d);
}

void hv_hda_poll(HvHda *d) {
    if (!d || !(d->gctl & 1u)) return;

    uint64_t now = now_ns();
    int irq_dirty = 0;
    uint8_t scratch[4096];

    for (int s = 0; s < HV_HDA_NSTREAMS; s++) {
        HvHdaStream *st = &d->st[s];
        if (!st->running || st->cbl == 0) continue;

        uint64_t dt = now > st->last_ns ? now - st->last_ns : 0;
        st->last_ns = now;
        if (dt == 0) continue;
        if (dt > 100000000ull) dt = 100000000ull; /* cap burst at 100 ms */

        unsigned rate_hz = 48000, ch = 2, sb = 2;
        decode_fmt(st->fmt, &rate_hz, &ch, &sb);
        uint32_t frame_bytes = ch * sb;
        if (frame_bytes == 0) frame_bytes = 4;

        uint64_t byte_rate = (uint64_t)rate_hz * frame_bytes;
        uint64_t total_num = st->rem_num + dt * byte_rate;
        uint32_t bytes_due = (uint32_t)(total_num / 1000000000ull);
        bytes_due -= bytes_due % frame_bytes;
        if (bytes_due == 0) {
            st->rem_num = total_num;
            continue;
        }
        st->rem_num = total_num - (uint64_t)bytes_due * 1000000000ull;

        uint64_t bdl_pa = ((uint64_t)st->bdpu << 32) | (st->bdpl & ~0x7fu);
        if (!bdl_pa) continue;
        uint32_t n_bdl = (uint32_t)st->lvi + 1u;
        if (n_bdl > 256u) n_bdl = 256u;

        HdaBdlEntry bdl[256];
        hv_phys_read(d->m, bdl_pa, bdl, n_bdl * sizeof(HdaBdlEntry));

        while (bytes_due > 0) {
            /* Locate BDL segment covering st->lpib */
            uint32_t acc = 0, idx = 0, seg_off = 0;
            int found = 0;
            for (uint32_t k = 0; k < n_bdl; k++) {
                if (bdl[k].len == 0) continue;
                if (st->lpib >= acc && st->lpib < acc + bdl[k].len) {
                    idx = k;
                    seg_off = st->lpib - acc;
                    found = 1;
                    break;
                }
                acc += bdl[k].len;
            }
            if (!found) {
                st->lpib = 0;
                break;
            }

            uint32_t seg_rem = bdl[idx].len - seg_off;
            uint32_t cbl_rem = st->lpib < st->cbl ? (st->cbl - st->lpib) : seg_rem;
            uint32_t step = bytes_due;
            if (step > seg_rem) step = seg_rem;
            if (step > cbl_rem) step = cbl_rem;
            if (step > sizeof scratch) step = sizeof scratch;
            if (step == 0) {
                st->lpib = 0;
                break;
            }

            if (s >= HV_HDA_ISS) {
                /* Output stream (Guest Speaker -> Host CoreAudio) */
                hv_phys_read(d->m, bdl[idx].addr + seg_off, scratch, step);
                tx_ring_push(d, scratch, step, d->dac_amp_l, d->dac_amp_r);
                d->tx_frames_total += step / frame_bytes;
            } else {
                /* Input stream (Host Microphone -> Guest ADC) */
                rx_ring_pull(d, scratch, step, rate_hz, d->adc_amp_l, d->adc_amp_r);
                hv_phys_write(d->m, bdl[idx].addr + seg_off, scratch, step);
                d->rx_frames_total += step / frame_bytes;
            }

            st->lpib += step;
            bytes_due -= step;

            int seg_done = (seg_off + step >= bdl[idx].len) || (st->lpib >= st->cbl);
            if (st->lpib >= st->cbl) st->lpib = 0;
            if (seg_done && (bdl[idx].flags & 1u)) {
                st->sts |= 0x04u; /* BCIS */
                irq_dirty = 1;
            }
        }

        write_dpib(d, s, st->lpib);
    }

    if (irq_dirty) update_irq(d);
}

HvHda *hv_hda_create(HvMachine *m, HvPcie *bus) {
    HvHda *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->m = m;
    d->base_ns = now_ns();
    d->tx_ring = calloc(1, HV_HDA_RING_BYTES);
    d->rx_ring = calloc(1, HV_HDA_RING_BYTES);
    atomic_store(&d->out_rate_hz, 48000u);
    atomic_store(&d->out_channels, 2u);
    atomic_store(&d->in_rate_hz, 48000u);
    atomic_store(&d->in_channels, 2u);
    controller_reset(d);

    /* Intel 82801FB ICH6 HD Audio Controller (8086:2668), Class 0x040300 (HD Audio) */
    hv_pci_init(&d->pci, 0x8086, 0x2668, 0x04030001u, 0x1af4, 0x1100);
    d->pci.opaque = d;
    d->pci.bar_read = bar_read;
    d->pci.bar_write = bar_write;
    d->pci.on_config_write = on_cfg_write;
    d->pci.config[0x40] = 0x01u; /* HDCTL: HD Audio mode enabled */
    d->pci.wmask[0x40] = 0x01u;
    d->pci.wmask[0x44] = 0x07u;  /* TCSEL */

    hv_pci_add_bar(&d->pci, 0, HDA_BAR_SIZE, 0);
    hv_pci_add_pm_cap(&d->pci, 0x60);
    hv_pci_add_pcie_cap(&d->pci, 0x70);
    hv_pcie_attach(bus, &d->pci);
    return d;
}
