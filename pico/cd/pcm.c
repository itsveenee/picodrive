/*
 * Emulation routines for the RF5C164 PCM chip
 * (C) notaz, 2007, 2013
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include "../pico_int.h"

#define PCM_STEP_SHIFT 11

void pcd_pcm_write(unsigned int a, unsigned int d)
{
  unsigned int cycles = SekCyclesDoneS68k();
  if ((int)(cycles - Pico_mcd->pcm.update_cycles) >= 384)
    pcd_pcm_sync(cycles);

  if (a < 7)
  {
    Pico_mcd->pcm.ch[Pico_mcd->pcm.cur_ch].regs[a] = d;
  }
  else if (a == 7) // control register
  {
    if (d & 0x40)
      Pico_mcd->pcm.cur_ch = d & 7;
    else
      Pico_mcd->pcm.bank = d & 0xf;
    Pico_mcd->pcm.control = d;
    elprintf(EL_CD, "pcm control %02x", Pico_mcd->pcm.control);
  }
  else if (a == 8)
  {
    Pico_mcd->pcm.enabled = ~d;
  }
  Pico_mcd->pcm_regs_dirty = 1;
}

unsigned int pcd_pcm_read(unsigned int a)
{
  unsigned int d, cycles = SekCyclesDoneS68k();
  if ((int)(cycles - Pico_mcd->pcm.update_cycles) >= 384)
    pcd_pcm_sync(cycles);

  d = Pico_mcd->pcm.ch[(a >> 1) & 7].addr >> PCM_STEP_SHIFT;
  if (a & 1)
    d >>= 8;

  return d & 0xff;
}

#if defined(RENDER_GSKIT_PS2)
/* AURORA_ACCURACY_PERF_RECOVERY_V6_SEGACD_SAFE_20260917
 * AURORA_SEGACD_PCM_SIGNMAG_BRANCHLESS_V6_20260917
 *
 * Exact branchless form of:
 *     if (smp & 0x80) smp = -(smp & 0x7f);
 * The 0xFF loop marker is handled before this helper, unchanged.
 */
static INLINE int AuroraSegaCdPcmDecodeSignMagnitude(int smp)
{
  const int sign_mask = -(smp >> 7);
  return ((smp & 0x7f) ^ sign_mask) - sign_mask;
}
#endif

void pcd_pcm_sync(unsigned int to)
{
  unsigned int cycles = Pico_mcd->pcm.update_cycles;
  int mul_l, mul_r, inc, smp;
  struct pcm_chan *ch;
  unsigned int addr;
  int c, s, steps;
  int enabled;
  int *out;

  if ((int)(to - cycles) < 384)
    return;

  steps = DIVQ32(to - cycles, 384);
  if (Pico_mcd->pcm_mixpos + steps > PCM_MIXBUF_LEN)
    // shouldn't happen, but occasionally does
    steps = PCM_MIXBUF_LEN - Pico_mcd->pcm_mixpos;

  // PCM disabled or all channels off
  enabled = Pico_mcd->pcm.enabled;
  if (!(Pico_mcd->pcm.control & 0x80))
    enabled = 0;
#if defined(RENDER_GSKIT_PS2)
  if (!enabled)
  {
    if (!Pico_mcd->pcm_regs_dirty)
      goto end;

    /* AURORA_SEGACD_PCM_ALL_OFF_FASTPATH_V6_20260917
     * With every channel off, the original loop can only reset channel
     * addresses; it cannot write a sample. Do those eight required state
     * updates directly and do not falsely mark an all-zero mix buffer dirty.
     * If earlier PCM in this frame already made it dirty, that flag is left
     * untouched so those earlier samples are still mixed normally. */
    for (c = 0; c < 8; c++)
    {
      ch = &Pico_mcd->pcm.ch[c];
      ch->addr = ch->regs[6] << (PCM_STEP_SHIFT + 8);
    }
    Pico_mcd->pcm_regs_dirty = 0;
    goto end;
  }
#else
  if (!enabled && !Pico_mcd->pcm_regs_dirty)
    goto end;
#endif

  out = Pico_mcd->pcm_mixbuf + Pico_mcd->pcm_mixpos * 2;
  Pico_mcd->pcm_mixbuf_dirty = 1;
  Pico_mcd->pcm_regs_dirty = 0;

  for (c = 0; c < 8; c++)
  {
    ch = &Pico_mcd->pcm.ch[c];

    if (!(enabled & (1 << c))) {
      ch->addr = ch->regs[6] << (PCM_STEP_SHIFT + 8);
      continue; // channel disabled
    }

    addr = ch->addr;
    inc = ch->regs[2] + (ch->regs[3]<<8);
    mul_l = (int)ch->regs[0] * (ch->regs[1] & 0xf);
    mul_r = (int)ch->regs[0] * (ch->regs[1] >>  4);

#if defined(RENDER_GSKIT_PS2)
    {
      /* The RF5C164 loop address is register state for the whole channel
       * slice. Precomputing it changes no address or marker semantics. */
      const unsigned int loop_addr =
        ch->regs[4] + (ch->regs[5] << 8);

      if (mul_l == 0 && mul_r == 0)
      {
        /* AURORA_SEGACD_PCM_ZERO_OUTPUT_FASTPATH_V6_20260917
         * An enabled-but-muted channel still advances its address and obeys
         * both levels of the 0xFF loop-marker rule. Audio math and output
         * RMWs are provably zero, so do not perform them. */
        for (s = 0; s < steps; s++)
        {
          smp = Pico_mcd->pcm_ram[addr >> PCM_STEP_SHIFT];

          if (smp == 0xff)
          {
            addr = loop_addr;
            smp = Pico_mcd->pcm_ram[addr];
            addr <<= PCM_STEP_SHIFT;
            if (smp == 0xff)
              break;
          }
          else
            addr = (addr + inc) & 0x07FFFFFF;
        }
      }
      else if (mul_l == 0 || mul_r == 0)
      {
        /* AURORA_SEGACD_PCM_HARDPAN_FASTPATH_V6_20260917
         * One side is mathematically zero. Preserve the nonzero side's
         * exact multiply/shift and avoid the zero multiply plus output RMW. */
        int *dst = out + (mul_l == 0);
        const int mul = mul_l ? mul_l : mul_r;

        for (s = 0; s < steps; s++)
        {
          smp = Pico_mcd->pcm_ram[addr >> PCM_STEP_SHIFT];

          if (smp == 0xff)
          {
            addr = loop_addr;
            smp = Pico_mcd->pcm_ram[addr];
            addr <<= PCM_STEP_SHIFT;
            if (smp == 0xff)
              break;
          }
          else
            addr = (addr + inc) & 0x07FFFFFF;

          smp = AuroraSegaCdPcmDecodeSignMagnitude(smp);
          *dst += (smp * mul) >> 5;
          dst += 2;
        }
      }
      else if (mul_l == mul_r)
      {
        /* AURORA_SEGACD_PCM_EQUAL_PAN_FASTPATH_V6_20260917
         * Equal pan has identical operands on both sides. Compute the same
         * product/rounding once and add that exact value to L and R. */
        int *dst = out;

        for (s = 0; s < steps; s++)
        {
          int mixed;

          smp = Pico_mcd->pcm_ram[addr >> PCM_STEP_SHIFT];

          if (smp == 0xff)
          {
            addr = loop_addr;
            smp = Pico_mcd->pcm_ram[addr];
            addr <<= PCM_STEP_SHIFT;
            if (smp == 0xff)
              break;
          }
          else
            addr = (addr + inc) & 0x07FFFFFF;

          smp = AuroraSegaCdPcmDecodeSignMagnitude(smp);
          mixed = (smp * mul_l) >> 5;
          *dst++ += mixed;
          *dst++ += mixed;
        }
      }
      else
      {
        int *dst = out;

        for (s = 0; s < steps; s++)
        {
          smp = Pico_mcd->pcm_ram[addr >> PCM_STEP_SHIFT];

          if (smp == 0xff)
          {
            addr = loop_addr;
            smp = Pico_mcd->pcm_ram[addr];
            addr <<= PCM_STEP_SHIFT;
            if (smp == 0xff)
              break;
          }
          else
            addr = (addr + inc) & 0x07FFFFFF;

          smp = AuroraSegaCdPcmDecodeSignMagnitude(smp);
          *dst++ += (smp * mul_l) >> 5;
          *dst++ += (smp * mul_r) >> 5;
        }
      }
    }
#else
    for (s = 0; s < steps; s++)
    {
      smp = Pico_mcd->pcm_ram[addr >> PCM_STEP_SHIFT];

      // test for loop signal
      if (smp == 0xff)
      {
        addr = ch->regs[4] + (ch->regs[5]<<8); // loop_addr
        smp = Pico_mcd->pcm_ram[addr];
        addr <<= PCM_STEP_SHIFT;
        if (smp == 0xff)
          break;
      } else
        addr = (addr + inc) & 0x07FFFFFF;

      if (smp & 0x80)
        smp = -(smp & 0x7f);

      out[s*2  ] += (smp * mul_l) >> 5; // max 127 * 255 * 15 / 32 = 15180
      out[s*2+1] += (smp * mul_r) >> 5;
    }
#endif
    ch->addr = addr;
  }

end:
  Pico_mcd->pcm.update_cycles = cycles + steps * 384;
  Pico_mcd->pcm_mixpos += steps;
}

void pcd_pcm_update(s32 *buf32, int length, int stereo)
{
  int step, *pcm;
  int p = 0;

  pcd_pcm_sync(SekCyclesDoneS68k());

  if (!Pico_mcd->pcm_mixbuf_dirty || !(PicoIn.opt & POPT_EN_MCD_PCM) || !buf32)
    goto out;

  step = (Pico_mcd->pcm_mixpos << 16) / length;
  pcm = Pico_mcd->pcm_mixbuf;

  if (stereo) {
    while (length-- > 0) {
      *buf32++ += pcm[0];
      *buf32++ += pcm[1];

      p += step;
      pcm += (p >> 16) * 2;
      p &= 0xffff;
    }
  }
  else {
    while (length-- > 0) {
      // mostly unused
      *buf32++ += (pcm[0] + pcm[1]) >> 1;

      p += step;
      pcm += (p >> 16) * 2;
      p &= 0xffff;
    }
  }

  memset(Pico_mcd->pcm_mixbuf, 0,
    Pico_mcd->pcm_mixpos * 2 * sizeof(Pico_mcd->pcm_mixbuf[0]));

out:
  Pico_mcd->pcm_mixbuf_dirty = 0;
  Pico_mcd->pcm_mixpos = 0;
}

// vim:shiftwidth=2:ts=2:expandtab
