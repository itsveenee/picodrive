/*
 * PicoDrive
 * (c) Copyright Dave, 2004
 * (C) notaz, 2006-2009
 * (C) irixxxx, 2019-2024
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include <string.h>
#include "../pico_int.h"
#include "ym2612.h"
#include "ym2413.h"
#include "sn76496.h"
#include "../cd/megasd.h"
#include "resampler.h"
#include "mix.h"

#if defined(RENDER_GSKIT_PS2)
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
int PicoDriveAurora_CdMusicEnabled(void);
/* AURORA_V4_9_SEGACD_CDDA_CHASE_REVIVE_20260830 */
void PicoDriveAurora_PrimeCdAudio(pm_file *stream);
/* AURORA_V4_4_BUILD_FIX_32X_VIDEO_FIRST_20260830 */
int PicoDriveAurora_32xAudioSacrifice(void);
#endif

#define YM2612_CH6PAN   0x1b6   // panning register for channel 6 (used for DAC)

void (*PsndMix_32_to_16)(s16 *dest, s32 *src, int count) = mix_32_to_16_stereo;

// master int buffer to mix to
// +1 for a fill triggered by an instruction overhanging into the next scanline
static s32 PsndBuffer[2*(54000+100)/50+2];

// cdda output buffer
s16 cdda_out_buffer[2*1152];

// FM resampling polyphase FIR
static resampler_t *ym2612_resampler;
static resampler_t *ym2413_resampler;
static int (*PsndFMUpdate)(s32 *buffer, int length, int stereo, int is_buf_empty);

#if defined(RENDER_GSKIT_PS2)
/* AURORA_SMS_FM_FAST_PS2_V1
 *
 * emu2413 is still stepped at the real YM2413 sample cadence (~49.7 kHz).
 * Only the expensive external polyphase FIR is replaced on PS2 by a
 * fixed-point sample selector. This keeps pitch/timing while reducing the
 * post-OPLL cost substantially at Aurora's usual 16 kHz output rate. */
static u32 ym2413_ps2_step_q16;
static u32 ym2413_ps2_phase_q16;
static s16 ym2413_ps2_last;

static void YM2413PS2FastSetup(int inrate, int outrate)
{
  if (inrate <= 0) inrate = 1;
  if (outrate <= 0) outrate = 1;

  ym2413_ps2_step_q16 =
      (u32)(((u64)(unsigned)inrate << 16) / (unsigned)outrate);

  if (ym2413_ps2_step_q16 < 0x10000u)
    ym2413_ps2_step_q16 = 0x10000u;

  ym2413_ps2_phase_q16 = 0;
  ym2413_ps2_last = 0;
}

static s16 YM2413PS2FastNext(void)
{
  unsigned native_samples;

  if (!ym2413_ps2_step_q16)
    YM2413PS2FastSetup(opll ? (int)opll->rate : 49716, PicoIn.sndRate);

  ym2413_ps2_phase_q16 += ym2413_ps2_step_q16;
  native_samples = ym2413_ps2_phase_q16 >> 16;
  ym2413_ps2_phase_q16 &= 0xffffu;

  do {
    ym2413_ps2_last = (s16)(OPLL_calc(opll) * 3);
  } while (--native_samples);

  return ym2413_ps2_last;
}

static int YM2413UpdatePS2Fast(s32 *buffer, int length, int stereo,
                               int is_buf_empty)
{
  (void)is_buf_empty;

  while (length-- > 0) {
    const s32 sample = YM2413PS2FastNext();
    *buffer++ = sample;
    if (stereo)
      *buffer++ = sample;
  }
  return 0;
}

static void YM2413MixPS2Fast(s16 *buffer, int length, int stereo)
{
  while (length-- > 0) {
    const s16 sample = YM2413PS2FastNext();
    *buffer++ += sample;
    if (stereo)
      *buffer++ += sample;
  }
}
#endif

PICO_INTERNAL void PsndInit(void)
{
  opll = OPLL_new(OSC_NTSC/15, OSC_NTSC/15/72);
  OPLL_setChipType(opll,0);
  OPLL_reset(opll);
}

PICO_INTERNAL void PsndExit(void)
{
  if (opll)
    OPLL_delete(opll);
  opll = NULL;

  resampler_free(ym2612_resampler); ym2612_resampler = NULL;
  resampler_free(ym2413_resampler); ym2413_resampler = NULL;
}

PICO_INTERNAL void PsndReset(void)
{
  // PsndRerate calls YM2612Init, which also resets
  PsndRerate(0);
  timers_reset();
}

// FM polyphase FIR resampling
#define FMFIR_TAPS	8

// resample FM from its native 53267Hz/52781Hz with polyphase FIR filter
static int ymchans;
static void YM2612Update(s32 *buffer, int length, int stereo)
{
  ymchans = YM2612UpdateOne(buffer, length, stereo, 1);
}

static int YM2612UpdateFIR(s32 *buffer, int length, int stereo, int is_buf_empty)
{
  resampler_update(ym2612_resampler, buffer, length, YM2612Update);
  return ymchans;
}

// resample SMS FM from its native 49716Hz/49262Hz with polyphase FIR filter
static void YM2413Update(s32 *buffer, int length, int stereo)
{
  while (length-- > 0) {
    int16_t getdata = OPLL_calc(opll) * 3;
    *buffer++ = getdata;
    buffer += stereo; // only left for stereo, to be mixed to right later
  }
}

static int YM2413UpdateFIR(s32 *buffer, int length, int stereo, int is_buf_empty)
{
  if (!is_buf_empty) memset(buffer, 0, (length << stereo) * sizeof(*buffer));
  resampler_update(ym2413_resampler, buffer, length, YM2413Update);
  return 0;
}

// FIR setup, looks for a close enough rational number matching the ratio
static resampler_t *YMFM_setup_FIR(int inrate, int outrate, int stereo)
{
  int mindiff = 999;
  int diff, mul, div;
  int minmult = 11, maxmult = 61; // min,max interpolation factor

  // compute filter ratio with largest multiplier for smallest error
  for (mul = minmult; mul <= maxmult; mul++) {
    div = (inrate*mul + outrate/2) / outrate;
    diff = outrate*div/mul - inrate;
    if (abs(diff) < abs(mindiff)) {
      mindiff = diff;
      Pico.snd.fm_fir_mul = mul;
      Pico.snd.fm_fir_div = div;
      if (abs(mindiff)*1000 <= inrate) break; // below error limit
    }
  }
  printf("FM polyphase FIR ratio=%d/%d error=%.3f%%\n",
        Pico.snd.fm_fir_mul, Pico.snd.fm_fir_div, 100.0*mindiff/inrate);

  return resampler_new(FMFIR_TAPS, Pico.snd.fm_fir_mul, Pico.snd.fm_fir_div,
        0.85, 2, 2*inrate/50, stereo);
}

// wrapper for the YM2612UpdateONE macro
static int YM2612UpdateONE(s32 *buffer, int length, int stereo, int is_buf_empty)
{
  return YM2612UpdateOne(buffer, length, stereo, is_buf_empty);
}

static int ymclock;
static int ymrate;
static int ymopts;

// to be called after changing sound rate or chips
void PsndRerate(int preserve_state)
{
  void *state = NULL;
  /* AURORA_PD_NTSC_5994_CLOCK_V1_20260822
   * PS2 NTSC presentation is 60000/1001 Hz. Keep PAL at exact 50 Hz,
   * but make NTSC audio scheduling use the host-family rational instead
   * of rounded 60.000. Other PicoDrive platforms retain upstream timing. */
#if defined(RENDER_GSKIT_PS2)
  int target_fps_num = Pico.m.pal ? 50 : 60000;
  int target_fps_den = Pico.m.pal ? 1 : 1001;
#else
  int target_fps_num = Pico.m.pal ? 50 : 60;
  int target_fps_den = 1;
#endif
  int target_lines = Pico.m.pal ? 313 : 262;
  int sms_clock = Pico.m.pal ? OSC_PAL/15 : OSC_NTSC/15;
  int ym2413_rate = (sms_clock + 36) / 72;
  int ym2612_clock = Pico.m.pal ? OSC_PAL/7 : OSC_NTSC/7;
  int ym2612_rate = YM2612_NATIVE_RATE();
  int ym2612_init = !preserve_state;
  int state_size = 4096;

  // don't init YM2612 if preserve_state and no parameter changes
  ym2612_init |= ymclock != ym2612_clock || ymopts != (PicoIn.opt & (POPT_DIS_FM_SSGEG|POPT_FM_YM2612));
  ym2612_init |= ymrate != (PicoIn.opt & POPT_EN_FM_FILTER ? ym2612_rate : PicoIn.sndRate);
  ymclock = ym2612_clock;
  ymrate = (PicoIn.opt & POPT_EN_FM_FILTER ? ym2612_rate : PicoIn.sndRate);
  ymopts = PicoIn.opt & (POPT_DIS_FM_SSGEG|POPT_FM_YM2612);

  if (preserve_state && ym2612_init) {
    state = malloc(state_size);
    if (state)
      state_size = YM2612PicoStateSave3(state, state_size);
  }

  if (opll && opll->rate != ym2413_rate) {
    OPLL_setRate(opll, ym2413_rate);
    if (!preserve_state)
      OPLL_reset(opll);
    resampler_free(ym2413_resampler);
#if !defined(RENDER_GSKIT_PS2)
    ym2413_resampler = YMFM_setup_FIR(ym2413_rate, PicoIn.sndRate, 0);
#else
    ym2413_resampler = NULL;
#endif
  }
  if (PicoIn.AHW & PAHW_SMS) {
#if defined(RENDER_GSKIT_PS2)
    /* Output rate can change while the native OPLL rate remains constant. */
    resampler_free(ym2413_resampler);
    ym2413_resampler = NULL;
    YM2413PS2FastSetup(ym2413_rate, PicoIn.sndRate);
    PsndFMUpdate = YM2413UpdatePS2Fast;
#else
    PsndFMUpdate = YM2413UpdateFIR;
#endif
  } else if ((PicoIn.opt & POPT_EN_FM_FILTER) && ym2612_rate != PicoIn.sndRate) {
    // polyphase FIR resampler, resampling directly from native to output rate
    if (ym2612_init)
      YM2612Init(ym2612_clock, ym2612_rate,
        ((PicoIn.opt&POPT_DIS_FM_SSGEG) ? 0 : ST_SSG) |
        ((PicoIn.opt&POPT_FM_YM2612)    ? ST_DAC : 0));
    resampler_free(ym2612_resampler);
    ym2612_resampler = YMFM_setup_FIR(ym2612_rate, PicoIn.sndRate,
        PicoIn.opt & POPT_EN_STEREO);
    PsndFMUpdate = YM2612UpdateFIR;
  } else {
    if (ym2612_init)
      YM2612Init(ym2612_clock, PicoIn.sndRate,
        ((PicoIn.opt&POPT_DIS_FM_SSGEG) ? 0 : ST_SSG) |
        ((PicoIn.opt&POPT_FM_YM2612)    ? ST_DAC : 0));
    PsndFMUpdate = YM2612UpdateONE;
  }

  if (state) {
    YM2612PicoStateLoad3(state, state_size);
    free(state);
  }

  if (preserve_state)
    SN76496_set_clockrate(Pico.m.pal ? OSC_PAL/15 : OSC_NTSC/15, PicoIn.sndRate);
  else
    SN76496_init(Pico.m.pal ? OSC_PAL/15 : OSC_NTSC/15, PicoIn.sndRate);

  // calculate Pico.snd.len using the exact frame-rate rational
  Pico.snd.len = (int)(
      (long long)PicoIn.sndRate * target_fps_den / target_fps_num);
  Pico.snd.len_e_add = (int)(
      ((((long long)PicoIn.sndRate * target_fps_den -
          (long long)Pico.snd.len * target_fps_num) << 16) /
        target_fps_num));
  Pico.snd.len_e_cnt = 0; // Q16

  // samples per line (Q16)
  Pico.snd.smpl_mult =
      65536LL * PicoIn.sndRate * target_fps_den /
      ((long long)target_fps_num * target_lines);
  // samples per z80 clock (Q20)
  Pico.snd.clkz_mult = 16 * Pico.snd.smpl_mult * 15/7 / 488.5;
  // samples per 44.1 KHz sample (Q16)
  Pico.snd.cdda_mult = 65536LL * 44100 / PicoIn.sndRate;
  Pico.snd.cdda_div  = 65536LL * PicoIn.sndRate / 44100;

  // clear all buffers
  memset32(PsndBuffer, 0, sizeof(PsndBuffer)/4);
  memset(cdda_out_buffer, 0, sizeof(cdda_out_buffer));
  if (PicoIn.sndOut)
    PsndClear();

  // set mixer
  PsndMix_32_to_16 = (PicoIn.opt & POPT_EN_STEREO) ? mix_32_to_16_stereo : mix_32_to_16_mono;
  mix_reset(PicoIn.opt & POPT_EN_SNDFILTER ? PicoIn.sndFilterAlpha : 0);

  if (PicoIn.AHW & PAHW_PICO)
    PicoReratePico();
}


PICO_INTERNAL void PsndStartFrame(void)
{
  // compensate for float part of Pico.snd.len
  Pico.snd.len_use = Pico.snd.len;
  Pico.snd.len_e_cnt += Pico.snd.len_e_add;
  if (Pico.snd.len_e_cnt >= 0x10000) {
    Pico.snd.len_e_cnt -= 0x10000;
    Pico.snd.len_use++;
  }
}

PICO_INTERNAL void PsndDoDAC(int cyc_to)
{
  int pos, len;
  int dout = ym2612.dacout;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // number of samples to fill in buffer (Q20)
  len = (cyc_to * Pico.snd.clkz_mult) - Pico.snd.dac_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.dac_pos+0x80000) >> 20;
  Pico.snd.dac_pos += len;
  len = ((Pico.snd.dac_pos+0x80000) >> 20) - pos;

  // avoid loss of the 1st sample of a new block (Q rounding issues)
  if (pos+len == 0)
    len = 1, Pico.snd.dac_pos += 0x80000;
  if (len <= 0)
    return;

  // fill buffer, applying a rather weak order 1 bessel IIR on the way
  // y[n] = (x[n] + x[n-1])*(1/2) (3dB cutoff at 11025 Hz, no gain)
  // 1 sample delay for correct IIR filtering over audio frame boundaries
  if (PicoIn.opt & POPT_EN_STEREO) {
    s16 *d = PicoIn.sndOut + pos*2;
    int pan = ym2612.REGS[YM2612_CH6PAN];
    int l = pan & 0x80 ? Pico.snd.dac_val : 0;
    int r = pan & 0x40 ? Pico.snd.dac_val : 0;
    *d++ += pan & 0x80 ? Pico.snd.dac_val2 : 0;
    *d++ += pan & 0x40 ? Pico.snd.dac_val2 : 0;
    while (--len) *d++ += l, *d++ += r;
  } else {
    s16 *d = PicoIn.sndOut + pos;
    *d++ += Pico.snd.dac_val2;
    while (--len) *d++ += Pico.snd.dac_val;
  }
  Pico.snd.dac_val2 = (Pico.snd.dac_val + dout) >> 1;
  Pico.snd.dac_val = dout;
}

PICO_INTERNAL void PsndDoPSG(int cyc_to)
{
  int pos, len;
  int stereo = 0;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // number of samples to fill in buffer (Q20)
  len = (cyc_to * Pico.snd.clkz_mult) - Pico.snd.psg_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.psg_pos+0x80000) >> 20;
  Pico.snd.psg_pos += len;
  len = ((Pico.snd.psg_pos+0x80000) >> 20) - pos;

  if (len <= 0)
    return;
  if (!(PicoIn.opt & POPT_EN_PSG))
    return;

  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }
  SN76496Update(PicoIn.sndOut + pos, len, stereo);
}

PICO_INTERNAL void PsndDoSMSFM(int cyc_to)
{
  int pos, len;
  int stereo = 0;
  s32 *buf32 = PsndBuffer;
  s16 *buf = PicoIn.sndOut;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // number of samples to fill in buffer (Q20)
  len = (cyc_to * Pico.snd.clkz_mult) - Pico.snd.ym2413_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.ym2413_pos+0x80000) >> 20;
  Pico.snd.ym2413_pos += len;
  len = ((Pico.snd.ym2413_pos+0x80000) >> 20) - pos;

  if (len <= 0)
    return;
  if (!(PicoIn.opt & POPT_EN_YM2413))
    return;

  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }

  if (Pico.m.hardware & PMS_HW_FMUSED) {
    buf += pos;
#if defined(RENDER_GSKIT_PS2)
    YM2413MixPS2Fast(buf, len, stereo);
#else
    YM2413UpdateFIR(buf32, len, 0, 0);
    if (stereo)
      while (len--) {
        *buf++ += *buf32;
        *buf++ += *buf32++;
      }
    else
      while (len--) {
        *buf++ += *buf32++;
      }
#endif
  }
}

PICO_INTERNAL void PsndDoFM(int cyc_to)
{
  int pos, len;
  int stereo = 0;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // Q20, number of samples since last call
  len = (cyc_to * Pico.snd.clkz_mult) - Pico.snd.fm_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.fm_pos+0x80000) >> 20;
  Pico.snd.fm_pos += len;
  len = ((Pico.snd.fm_pos+0x80000) >> 20) - pos;
  if (len <= 0)
    return;

  // fill buffer
  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }
  if (PicoIn.opt & POPT_EN_FM)
    PsndFMUpdate(PsndBuffer + pos, len, stereo, 1);
}

PICO_INTERNAL void PsndDoPCM(int cyc_to)
{
  int pos, len;
  int stereo = 0;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // Q20, number of samples since last call
  len = (cyc_to * Pico.snd.clkz_mult) - Pico.snd.pcm_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.pcm_pos+0x80000) >> 20;
  Pico.snd.pcm_pos += len;
  len = ((Pico.snd.pcm_pos+0x80000) >> 20) - pos;
  if (len <= 0)
    return;

  // fill buffer
  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }
  PicoPicoPCMUpdate(PicoIn.sndOut + pos, len, stereo);
}

// cdda
static void cdda_raw_update(s32 *buffer, int length, int stereo)
{
  int ret, cdda_bytes;

  // apply start offset in frame (offset to 1st lba to play)
  int offs = Pico_mcd->cdda_frame_offs * Pico.snd.cdda_div >> 16;
  length -= offs;
  buffer += offs * (stereo ? 2 : 1);
  Pico_mcd->cdda_frame_offs = 0;

  cdda_bytes = (length * Pico.snd.cdda_mult >> 16) * 4;

  // compute offset of last played sample in this frame (need for save/load)
  Pico_mcd->m.cdda_lba_offset += cdda_bytes/4;
  while (Pico_mcd->m.cdda_lba_offset >= 2352/4)
    Pico_mcd->m.cdda_lba_offset -= 2352/4;

#if defined(RENDER_GSKIT_PS2)
  /* AURORA_CD_MUSIC_REDBOOK_V3_20260830
   * OFF advances Red Book time with a logical seek only: no fread,
   * no refill request and no CDDA mix. PCM/FM/PSG are untouched. */
  if (!PicoDriveAurora_CdMusicEnabled())
  {
    pm_seek(Pico_mcd->cdda_stream, cdda_bytes, SEEK_CUR);
    return;
  }
#endif

  ret = pm_read_audio(cdda_out_buffer, cdda_bytes, Pico_mcd->cdda_stream);
  if (ret < cdda_bytes) {
    memset((char *)cdda_out_buffer + ret, 0, cdda_bytes - ret);
    Pico_mcd->cdda_stream = NULL;
  }

  // now mix
  if (stereo) switch (Pico.snd.cdda_mult) {
    case 0x10000: mix_16h_to_32(buffer, cdda_out_buffer, length*2);     break;
    case 0x20000: mix_16h_to_32_s1(buffer, cdda_out_buffer, length*2);  break;
    case 0x40000: mix_16h_to_32_s2(buffer, cdda_out_buffer, length*2);  break;
    default: mix_16h_to_32_resample_stereo(buffer, cdda_out_buffer, length, Pico.snd.cdda_mult);
  } else
    mix_16h_to_32_resample_mono(buffer, cdda_out_buffer, length, Pico.snd.cdda_mult);
}

void cdda_start_play(int lba_base, int lba_offset, int lb_len)
{
#if defined(RENDER_GSKIT_PS2)
  /* AURORA_CD_MUSIC_REDBOOK_V3_20260830
   * With CD music Off, do not initialise opaque MP3/OGG storage/decoders. */
  if (!PicoDriveAurora_CdMusicEnabled() &&
      (Pico_mcd->cdda_type == CT_MP3 ||
       Pico_mcd->cdda_type == CT_OGG))
    return;
#endif

  if (Pico_mcd->cdda_type == CT_MP3)
  {
    int pos1024 = 0;

    if (lba_offset)
      pos1024 = lba_offset * 1024 / lb_len;

    mp3_start_play(Pico_mcd->cdda_stream, pos1024);
    return;
  } else if (Pico_mcd->cdda_type == CT_OGG) {
    ogg_start_play(Pico_mcd->cdda_stream,
                  lba_offset * 588 + Pico_mcd->m.cdda_lba_offset);
    return;
  }

  // on restart after loading, consider offset of last played sample
  pm_seek(Pico_mcd->cdda_stream, (lba_base + lba_offset) * 2352 +
                                  Pico_mcd->m.cdda_lba_offset * 4, SEEK_SET);
  if (Pico_mcd->cdda_type == CT_WAV)
  {
    // skip headers, assume it's 44kHz stereo uncompressed
    pm_seek(Pico_mcd->cdda_stream, 44, SEEK_CUR);
  }

#if defined(RENDER_GSKIT_PS2)
  /* CDDA-only asynchronous head start. DATA tracks never enter here. */
  if (PicoDriveAurora_CdMusicEnabled())
    PicoDriveAurora_PrimeCdAudio(Pico_mcd->cdda_stream);
#endif
}

void cdda_stop_play(void)
{
  if (Pico_mcd->cdda_type == CT_OGG)
    ogg_stop_play();
  Pico_mcd->cdda_stream = NULL;
}


PICO_INTERNAL void PsndClear(void)
{
  int len = Pico.snd.len;
  if (Pico.snd.len_e_add) len++;

  // drop pos remainder to avoid rounding errors (not entirely correct though)
  Pico.snd.dac_pos = Pico.snd.fm_pos = Pico.snd.psg_pos = Pico.snd.ym2413_pos = Pico.snd.pcm_pos = 0;
  if (!PicoIn.sndOut) return;

  if (PicoIn.opt & POPT_EN_STEREO)
    memset32((int *) PicoIn.sndOut, 0, len); // assume PicoIn.sndOut to be aligned
  else {
    s16 *out = PicoIn.sndOut;
    if ((uintptr_t)out & 2) { *out++ = 0; len--; }
    memset32((int *) out, 0, len/2);
    if (len & 1) out[len-1] = 0;
  }
  if (!(PicoIn.opt & POPT_EN_FM))
    memset32(PsndBuffer, 0, PicoIn.opt & POPT_EN_STEREO ? len*2 : len);
}


static int PsndRender(int offset, int length)
{
  s32 *buf32;
  int stereo = (PicoIn.opt & 8) >> 3;
  int fmlen = ((Pico.snd.fm_pos+0x80000) >> 20);
  int daclen = ((Pico.snd.dac_pos+0x80000) >> 20);
  int psglen = ((Pico.snd.psg_pos+0x80000) >> 20);
  int pcmlen = ((Pico.snd.pcm_pos+0x80000) >> 20);

  buf32 = PsndBuffer+(offset<<stereo);

  pprof_start(sound);

  // Add in parts of the PSG output not yet done
  if (length-psglen > 0 && PicoIn.sndOut) {
    s16 *psgbuf = PicoIn.sndOut + (psglen << stereo);
    Pico.snd.psg_pos += (length-psglen) << 20;
    if (PicoIn.opt & POPT_EN_PSG)
      SN76496Update(psgbuf, length-psglen, stereo);
  }

  if (PicoIn.AHW & PAHW_PICO) {
    // always need to render sound for interrupts
    s16 *buf16 = PicoIn.sndOut ? PicoIn.sndOut + (pcmlen<<stereo) : NULL;
    PicoPicoPCMUpdate(buf16, length-pcmlen, stereo);
    return length;
  }

  // Fill up DAC output in case of missing samples (Q rounding errors)
  if (length-daclen > 0 && PicoIn.sndOut) {
    Pico.snd.dac_pos += (length-daclen) << 20;
    if (PicoIn.opt & POPT_EN_STEREO) {
      s16 *d = PicoIn.sndOut + daclen*2;
      int pan = ym2612.REGS[YM2612_CH6PAN];
      int l = pan & 0x80 ? Pico.snd.dac_val : 0;
      int r = pan & 0x40 ? Pico.snd.dac_val : 0;
      *d++ += pan & 0x80 ? Pico.snd.dac_val2 : 0;
      *d++ += pan & 0x40 ? Pico.snd.dac_val2 : 0;
      if (l|r) for (daclen++; length-daclen > 0; daclen++)
          *d++ += l, *d++ += r;
    } else {
      s16 *d = PicoIn.sndOut + daclen;
      *d++ += Pico.snd.dac_val2;
      if (Pico.snd.dac_val) for (daclen++; length-daclen > 0; daclen++)
        *d++ += Pico.snd.dac_val;
    }
    Pico.snd.dac_val2 = Pico.snd.dac_val;
  }

  // Add in parts of the FM buffer not yet done
  if (length-fmlen > 0 && PicoIn.sndOut) {
    s32 *fmbuf = buf32 + ((fmlen-offset) << stereo);
    Pico.snd.fm_pos += (length-fmlen) << 20;
    if (PicoIn.opt & POPT_EN_FM)
      PsndFMUpdate(fmbuf, length-fmlen, stereo, 1);
  }

  // CD: PCM sound
  if (PicoIn.AHW & PAHW_MCD) {
    pcd_pcm_update(buf32, length-offset, stereo);
  }

  // CD: CDDA audio
  // CD mode, cdda enabled, not data track, CDC is reading
  if ((PicoIn.AHW & PAHW_MCD) && (PicoIn.opt & POPT_EN_MCD_CDDA)
      && Pico_mcd->cdda_stream != NULL
      && (!(Pico_mcd->s68k_regs[0x36] & 1) || Pico_msd.state == 3))
  {
#if defined(RENDER_GSKIT_PS2)
    /* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
     * Opaque MP3/OGG decoder I/O cannot be proven nonblocking here.
     * Extreme policy therefore mutes it on PS2. Raw/WAV/CHD stay fail-soft. */
    if (Pico_mcd->cdda_type != CT_MP3 &&
        Pico_mcd->cdda_type != CT_OGG)
      cdda_raw_update(buf32, length-offset, stereo);
#else
    if (Pico_mcd->cdda_type == CT_MP3)
      mp3_update(buf32, length-offset, stereo);
    else if (Pico_mcd->cdda_type == CT_OGG)
      ogg_update(buf32, length-offset, stereo);
    else
      cdda_raw_update(buf32, length-offset, stereo);
#endif
  }

  if ((PicoIn.AHW & PAHW_32X) && (PicoIn.opt & POPT_EN_PWM))
    p32x_pwm_update(buf32, length-offset, stereo);

  // convert + limit to normal 16bit output
#if defined(RENDER_GSKIT_PS2)
  if (PicoIn.sndOut &&
      !((PicoIn.AHW & PAHW_32X) &&
        PicoDriveAurora_32xAudioSacrifice()))
#else
  if (PicoIn.sndOut)
#endif
    PsndMix_32_to_16(PicoIn.sndOut+(offset<<stereo), buf32, length-offset);

  pprof_end(sound);

  return length;
}

PICO_INTERNAL void PsndGetSamples(int y)
{
  static int curr_pos = 0;

  curr_pos  = PsndRender(0, Pico.snd.len_use);

#if defined(RENDER_GSKIT_PS2)
  if (PicoIn.writeSound && PicoIn.sndOut &&
      !((PicoIn.AHW & PAHW_32X) &&
        PicoDriveAurora_32xAudioSacrifice()))
#else
  if (PicoIn.writeSound && PicoIn.sndOut)
#endif
    PicoIn.writeSound(curr_pos * ((PicoIn.opt & POPT_EN_STEREO) ? 4 : 2));
  // clear sound buffer
  PsndClear();
}

static int PsndRenderMS(int offset, int length)
{
  s32 *buf32 = PsndBuffer;
  int stereo = (PicoIn.opt & 8) >> 3;
  int psglen = ((Pico.snd.psg_pos+0x80000) >> 20);
  int ym2413len = ((Pico.snd.ym2413_pos+0x80000) >> 20);

  if (!PicoIn.sndOut)
    return length;

  pprof_start(sound);

  // Add in parts of the PSG output not yet done
  if (length-psglen > 0) {
    s16 *psgbuf = PicoIn.sndOut + (psglen << stereo);
    Pico.snd.psg_pos += (length-psglen) << 20;
    if (PicoIn.opt & POPT_EN_PSG)
      SN76496Update(psgbuf, length-psglen, stereo);
  }

  if (length-ym2413len > 0) {
    s16 *ym2413buf = PicoIn.sndOut + (ym2413len << stereo);
    Pico.snd.ym2413_pos += (length-ym2413len) << 20;
    int len = (length-ym2413len);
    if (Pico.m.hardware & PMS_HW_FMUSED) {
#if defined(RENDER_GSKIT_PS2)
      YM2413MixPS2Fast(ym2413buf, len, stereo);
#else
      PsndFMUpdate(buf32, len, 0, 0);
      if (stereo)
        while (len--) {
          *ym2413buf++ += *buf32;
          *ym2413buf++ += *buf32++;
        }
      else
        while (len--) {
          *ym2413buf++ += *buf32++;
        }
#endif
    }
  }

  pprof_end(sound);

  return length;
}

PICO_INTERNAL void PsndGetSamplesMS(int y)
{
  static int curr_pos = 0;

  curr_pos  = PsndRenderMS(0, Pico.snd.len_use);

  if (PicoIn.writeSound != NULL && PicoIn.sndOut)
    PicoIn.writeSound(curr_pos * ((PicoIn.opt & POPT_EN_STEREO) ? 4 : 2));
  PsndClear();
}

// vim:shiftwidth=2:ts=2:expandtab

/* AURORA_V4_9_SEGACD_CDDA_CHASE_REVIVE_20260830 */
