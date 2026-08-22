/*
 * common code for base/cd/32x
 * (C) notaz, 2007-2009,2013
 * (C) irixxxx, 2020-2024
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#define CYCLES_M68K_LINE     488 // suitable for both PAL/NTSC
#define CYCLES_M68K_VINT_LAG 112

/* AURORA_PICODRIVE_HYBRID_FAST_V2
 * Adaptive replacement for the old all-or-nothing Fast renderer.
 * Keep draw2 for frames it can represent faithfully; fall back to the
 * normal scanline renderer only for frames requiring unsupported
 * raster behaviour. This is presentation-only: CPU/VDP timing is not
 * relaxed and the Golden Axe III VINT/HINT timing path is untouched. */
extern int PicoAltRendererFallbackFrame;

static int PicoAltRendererNeedsGoodFrame(struct PicoVideo *pv)
{
  int lines, htab, plane, y, yy, yend;
  u16 h;

  /* draw2 has no shadow/highlight renderer. */
  if (pv->reg[12] & 0x08)
    return 1;

  /* draw2.c builds with VSRAM=0: 2-cell vertical scroll is unsupported. */
  if (pv->reg[11] & 0x04)
    return 1;

  /* draw2.c builds with INTERLACE=0: interlace mode 2 needs Good. */
  if ((pv->reg[12] & 0x06) == 0x06)
    return 1;

  /* Cell H-scroll (8-line granularity) matches draw2's tile-row model.
   * True line-scroll does not. Only fall back if values actually vary
   * inside an 8-line block; games using mode 3 with repeated values
   * still get the Fast path. */
  if ((pv->reg[11] & 0x03) == 0x03)
  {
    lines = (pv->reg[1] & 0x08) ? 240 : 224;
    htab = pv->reg[13] << 9;
    for (plane = 0; plane < 2; ++plane)
    {
      for (y = 0; y < lines; y += 8)
      {
        yend = y + 8;
        if (yend > lines) yend = lines;
        h = PicoMem.vram[(htab + plane + (y << 1)) & 0x7fff];
        for (yy = y + 1; yy < yend; ++yy)
          if (PicoMem.vram[(htab + plane + (yy << 1)) & 0x7fff] != h)
            return 1;
      }
    }
  }

  return 0;
}

// CPUS_RUN
#ifndef CPUS_RUN
#define CPUS_RUN(m68k_cycles) \
  SekRunM68k(m68k_cycles)
#endif

// sync m68k to Pico.t.m68c_aim
static void SekExecM68k(int cyc_do)
{
  Pico.t.m68c_cnt += cyc_do;

#if defined(EMU_C68K)
  PicoCpuCM68k.cycles = cyc_do;
  CycloneRun(&PicoCpuCM68k);
  Pico.t.m68c_cnt -= PicoCpuCM68k.cycles;
#elif defined(EMU_M68K)
  Pico.t.m68c_cnt += m68k_execute(cyc_do) - cyc_do;
#elif defined(EMU_F68K)
  Pico.t.m68c_cnt += fm68k_emulate(&PicoCpuFM68k, cyc_do, 0) - cyc_do;
#endif
  SekCyclesLeft = 0;
}

static int SekSyncM68k(int once)
{
  int cyc_do;

  pprof_start(m68k);
  pevt_log_m68k_o(EVT_RUN_START);

  while ((cyc_do = Pico.t.m68c_aim - Pico.t.m68c_cnt) > 0) {
    // the Z80 CPU is stealing some bus cycles from the 68K main CPU when 
    // accessing the main bus. Account for these by shortening the time
    // the 68K CPU runs.
    int z80_buscyc = Pico.t.z80_buscycles >> (4+(~Pico.m.scanline & 1));
    // NB the Z80 isn't fast enough to steal more than half the bandwidth.
    // the fastest would be POP cc which takes 10+~3.3*2 z-cyc (~35 cyc) for a
    // 16 bit value, but 68k is only blocked for ~16 cyc for the 2 bus cycles.
    if (z80_buscyc > cyc_do/2)
      z80_buscyc = cyc_do/2;
    SekExecM68k(cyc_do - z80_buscyc);
    Pico.t.m68c_cnt += z80_buscyc;
    Pico.t.z80_buscycles -= z80_buscyc<<4;
    if (once) break;
  }

  SekTrace(0);
  pevt_log_m68k_o(EVT_RUN_END);
  pprof_end(m68k);

  return Pico.t.m68c_aim > Pico.t.m68c_cnt;
}

static __inline void SekAimM68k(int cyc, int mult)
{
  // refresh slowdown, for cart: 2 cycles every 128 - make this 1 every 64,
  // for RAM: seems to be 0-3 every 128. Carts usually run from the cart
  // area, but MCD games only use RAM, hence a different multiplier is needed.
  // NB must be quite accurate, so handle fractions as well (c/f OutRunners)
  int delay = (Pico.t.refresh_delay += cyc*mult) >> 14;
  Pico.t.m68c_cnt += delay;
  Pico.t.refresh_delay -= delay << 14;
  Pico.t.m68c_aim += cyc;
}

static __inline void SekRunM68k(int cyc)
{
  // TODO 0x100 would be 2 cycles/128, moreover far too sensitive
  SekAimM68k(cyc, 0x108); // OutRunners, testpico, VDPFIFOTesting
  SekSyncM68k(0);
}

static void SyncCPUs(unsigned int cycles)
{
  // sync cpus
  if (Pico.m.z80Run && !Pico.m.z80_reset && (PicoIn.opt&POPT_EN_Z80))
    PicoSyncZ80(cycles);

#ifdef PICO_CD
  if (PicoIn.AHW & PAHW_MCD)
    pcd_sync_s68k(cycles, 0);
#endif
#ifdef PICO_32X
  p32x_sync_sh2s(cycles);
#endif
}

static void do_hint(struct PicoVideo *pv)
{
  pv->pending_ints |= 0x10;
  if (pv->reg[0] & 0x10) {
    elprintf(EL_INTS, "hint: @ %06x [%u]", SekPc, SekCyclesDone());
    if (SekIrqLevel < pv->hint_irq)
      SekInterrupt(pv->hint_irq);
  }
}

static void do_timing_hacks_end(struct PicoVideo *pv)
{
  PicoVideoFIFOSync(CYCLES_M68K_LINE);

  // need rather tight Z80 sync for emulation of main bus cycle stealing
  if (Pico.m.scanline&1)
    if (Pico.m.z80Run && !Pico.m.z80_reset && (PicoIn.opt&POPT_EN_Z80))
      PicoSyncZ80(Pico.t.m68c_aim);
}

static void do_timing_hacks_start(struct PicoVideo *pv)
{
  int cycles = PicoVideoFIFOHint();

  SekCyclesBurn(cycles); // prolong cpu HOLD if necessary
  // XXX how to handle Z80 bus cycle stealing during DMA correctly?
  if ((Pico.t.z80_buscycles -= cycles<<4) < 0)
    Pico.t.z80_buscycles = 0;
  Pico.t.m68c_aim += Pico.m.scanline&1; // add 1 every 2 lines for 488.5 cycles
}

static int PicoFrameHints(void)
{
  struct PicoVideo *pv = &Pico.video;
  int lines, y, lines_vis, skip;
  int hint; // Hint counter

  pevt_log_m68k_o(EVT_FRAME_START);

  skip = PicoIn.skipFrame;

  PicoAltRendererFallbackFrame =
    (PicoIn.opt & POPT_ALT_RENDERER) &&
    PicoAltRendererNeedsGoodFrame(pv);

  Pico.t.m68c_frame_start = Pico.t.m68c_aim;
  PsndStartFrame();
  PicoPortUpdate();

  hint = pv->hint_cnt;

  // === active display ===
  pv->status |= PVS_ACTIVE;

  for (y = 0; y < 240; y++)
  {
    if (y == 224 && !(pv->reg[1] & 8))
      break;

    Pico.m.scanline = y;
    pv->v_counter = PicoVideoGetV(y, 0);

    PicoPortTick();

    // H-Interrupts:
    if (--hint < 0)
    {
      hint = pv->reg[10]; // Reload H-Int counter
      do_hint(pv);
    }

    /* Hybrid Fast deliberately waits until the end of active display.
     * Any rendering-affecting VDP sync before then can promote this
     * frame to Good without having already committed a stale draw2 frame. */

    // Run scanline:
    Pico.t.m68c_line_start = Pico.t.m68c_aim;
    do_timing_hacks_start(pv);
    CPUS_RUN(CYCLES_M68K_LINE);
    do_timing_hacks_end(pv);

    if (PicoLineHook) PicoLineHook();
    pevt_log_m68k_o(EVT_NEXT_LINE);
  }

  SyncCPUs(Pico.t.m68c_aim);

  /* Commit draw2 only after the whole active display proved safe.
   * If a mid-frame VDP change selected fallback, the existing Good
   * completion block below renders the remaining scanlines instead. */
  if (unlikely(PicoIn.opt & POPT_ALT_RENDERER) &&
      !PicoAltRendererFallbackFrame && !skip)
  {
    if (Pico.est.rendstatus & PDRAW_SYNC_NEEDED)
      PicoFrameFull();
#ifdef DRAW_FINISH_FUNC
    DRAW_FINISH_FUNC();
#endif
    Pico.est.rendstatus &= ~PDRAW_SYNC_NEEDED;
    skip = 1;
  }

  if (!skip)
  {
    if (Pico.est.DrawScanline < y)
      PicoVideoSync(-1);
#ifdef DRAW_FINISH_FUNC
    DRAW_FINISH_FUNC();
#endif
    Pico.est.rendstatus &= ~PDRAW_SYNC_NEEDED;
  }
#ifdef PICO_32X
  p32x_render_frame();
#endif

  // === VBLANK, 1st line ===
  lines_vis = (pv->reg[1] & 8) ? 240 : 224;
  if (y == lines_vis)
    pv->status &= ~PVS_ACTIVE;
  Pico.m.scanline = y;
  pv->v_counter = PicoVideoGetV(y, 0);

  memcpy(PicoIn.padInt, PicoIn.pad, sizeof(PicoIn.padInt));
  PicoPortTick();

  // Last H-Int (normally):
  if (--hint < 0)
  {
    hint = pv->reg[10]; // Reload H-Int counter
    do_hint(pv);
  }

  pv->status |= SR_VB | PVS_VB2; // go into vblank
#ifdef PICO_32X
  p32x_start_blank();
#endif

  // the following SekRun is there for several reasons:
  // there must be a delay after vblank bit is set and irq is asserted (Mazin Saga)
  // also delay between F bit (bit 7) is set in SR and IRQ happens (Ex-Mutants)
  // also delay between last H-int and V-int (Golden Axe 3)
  Pico.t.m68c_line_start = Pico.t.m68c_aim;
  PicoVideoFIFOMode(pv->reg[1]&0x40, pv->reg[12]&1);
  do_timing_hacks_start(pv);
  CPUS_RUN(CYCLES_M68K_VINT_LAG);

  SyncCPUs(Pico.t.m68c_aim);

  // slot 0x00
  pv->status |= SR_F;
  pv->pending_ints |= 0x20;

  if (pv->reg[1] & 0x20) {
    // as per https://gendev.spritesmind.net/forum/viewtopic.php?t=2202, IRQ
    // is usually sampled after operand reading, so the next instruction will
    // be executed before the IRQ is taken.
    if (Pico.t.m68c_cnt - Pico.t.m68c_aim < 40) // CPU blocked?
      SekExecM68k(4);
    elprintf(EL_INTS, "vint: @ %06x [%u]", SekPc, SekCyclesDone());
    // TODO: IRQ usually sampled after operand reading, so insn can't turn it
    // off? single exception is MOVE.L which samples IRQ after the 1st write?
    SekInterrupt(6);
  }

  // slot 0x01
  if (pv->reg[12] & 2)
    pv->status ^= SR_ODD; // change odd bit in interlace modes
  else
    pv->status &= ~SR_ODD; // never set in non-interlace modes

  // assert Z80 interrupt for one scanline even in busrq hold (Teddy Blues)
  if (/*Pico.m.z80Run &&*/ !Pico.m.z80_reset && (PicoIn.opt&POPT_EN_Z80)) {
    elprintf(EL_INTS, "zint");
    z80_int_assert(1);
  }

  // Run scanline:
  CPUS_RUN(CYCLES_M68K_LINE - CYCLES_M68K_VINT_LAG);
  do_timing_hacks_end(pv);

  if (PicoLineHook) PicoLineHook();
  pevt_log_m68k_o(EVT_NEXT_LINE);

  if (Pico.m.z80Run && !Pico.m.z80_reset && (PicoIn.opt&POPT_EN_Z80))
    PicoSyncZ80(Pico.t.m68c_aim);
  z80_int_assert(0);

  // === VBLANK ===
  lines = Pico.m.pal ? 313 : 262;
  for (y++; y < lines - 1; y++)
  {
    Pico.m.scanline = y;
    pv->v_counter = PicoVideoGetV(y, 1);

    PicoPortTick();

    if (unlikely(pv->status & PVS_ACTIVE) && --hint < 0)
    {
      hint = pv->reg[10]; // Reload H-Int counter
      do_hint(pv);
    }

    // Run scanline:
    Pico.t.m68c_line_start = Pico.t.m68c_aim;
    do_timing_hacks_start(pv);
    CPUS_RUN(CYCLES_M68K_LINE);
    do_timing_hacks_end(pv);

    if (PicoLineHook) PicoLineHook();
    pevt_log_m68k_o(EVT_NEXT_LINE);
  }

  if (unlikely(PicoIn.overclockM68k)) {
    unsigned int l = PicoIn.overclockM68k * lines / 100;
    while (l-- > 0) {
      Pico.t.m68c_cnt -= CYCLES_M68K_LINE;
      do_timing_hacks_start(pv);
      SekSyncM68k(0);
      do_timing_hacks_end(pv);
    }
  }

  SyncCPUs(Pico.t.m68c_aim);

  // === VBLANK last line ===
  pv->status &= ~(SR_VB | PVS_VB2);
  pv->status |= ((pv->reg[1] >> 3) ^ SR_VB) & SR_VB; // forced blanking
#ifdef PICO_32X
  p32x_end_blank();
#endif

  // last scanline
  Pico.m.scanline = y++;
  pv->v_counter = 0xff;

  PicoPortTick();

  if (unlikely(pv->status & PVS_ACTIVE)) {
    if (--hint < 0) {
      hint = pv->reg[10]; // Reload H-Int counter
      do_hint(pv);
    }
  }
  else
    hint = pv->reg[10];

  // Run scanline:
  Pico.t.m68c_line_start = Pico.t.m68c_aim;
  PicoVideoFIFOMode(pv->reg[1]&0x40, pv->reg[12]&1);
  do_timing_hacks_start(pv);
  CPUS_RUN(CYCLES_M68K_LINE);
  do_timing_hacks_end(pv);

  if (PicoLineHook) PicoLineHook();
  pevt_log_m68k_o(EVT_NEXT_LINE);

  SyncCPUs(Pico.t.m68c_aim);

  // get samples from sound chips
  PsndGetSamples(y);

  timers_cycle(cycles_68k_to_z80(Pico.t.m68c_aim - Pico.t.m68c_frame_start));
  z80_resetCycles();

  pv->hint_cnt = hint;

  return 0;
}

#undef CPUS_RUN

// vim:shiftwidth=2:ts=2:expandtab
