/*
 * PicoDrive
 * (c) Copyright Dave, 2004
 * (C) notaz, 2006-2010
 * (C) irixxxx, 2020-2024
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include "pico_int.h"
#include <cpu/debug.h>
#if defined(RENDER_GSKIT_PS2)
/* AURORA_V4_4_BUILD_FIX_32X_VIDEO_FIRST_20260830
 * Minimal EE-kernel ABI declarations.
 * Do NOT include ps2sdk kernel.h here: PicoDrive defines u32/s32 itself,
 * while tamtypes.h defines incompatible typedef spellings under the EE ABI. */
typedef struct AuroraEeSemaT
{
  int count, max_count, init_count, wait_threads;
  unsigned int attr, option;
} AuroraEeSemaT;

typedef struct AuroraEeThreadT
{
  int status;
  void *func;
  void *stack;
  int stack_size;
  void *gp_reg;
  int initial_priority;
  int current_priority;
  unsigned int attr;
  unsigned int option;
} AuroraEeThreadT;

typedef struct AuroraEeThreadStatusT
{
  int status;
  void *func;
  void *stack;
  int stack_size;
  void *gp_reg;
  int initial_priority;
  int current_priority;
  unsigned int attr;
  unsigned int option;
  unsigned int waitType;
  unsigned int waitId;
  unsigned int wakeupCount;
} AuroraEeThreadStatusT;

extern int CreateSema(void *);
extern int DeleteSema(int);
extern int SignalSema(int);
extern int WaitSema(int);
extern int CreateThread(void *);
extern int DeleteThread(int);
extern int StartThread(int, void *);
extern int ReferThreadStatus(int, void *);
extern int DelayThread(unsigned int);
extern void *_gp;

#define AURORA_EE_SYNC() __asm__ __volatile__("sync")
#endif

#if defined(USE_LIBCHDR)
#include "libchdr/chd.h"
#include "libchdr/cdrom.h"
#endif

#include <unzip/unzip.h>
#include <zlib.h>

static int rom_alloc_size;

#if defined(RENDER_GSKIT_PS2)
/* AURORA_CD_AUDIO_STREAM_V2_PD_CACHE_20260829
 * AURORA_CD_AUDIO_STREAM_V4_PD_REFILL32_20260829
 * AURORA_CD_AUDIO_STREAM_V5_PD_REFILL64_20260829
 * AURORA_CD_AUDIO_STREAM_V6_V3_BASELINE_128K_20260829
 *
 * Real-PS2 reference point:
 *   V3 128 KiB = best observed overall behaviour so far: gameplay/audio are
 *   smooth between refills, with one larger synchronous stall at refill.
 *   V4  32 KiB shortened video stalls but made CDDA stutter much more often.
 *   V5  64 KiB was an intermediate experiment.
 *
 * V6 intentionally restores the V3 128 KiB logical window while isolating
 * the entire experimental path to actual Sega CD track streams.
 */
#define AURORA_PD_STREAM_CACHE_BYTES (128 * 1024)

typedef struct AuroraPdStreamState
{
  long pos;
  /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830
   * Async identity/path are private to CDDA; the normal FILE* remains the
   * authoritative synchronous DATA handle. */
  unsigned int async_serial;
  char async_path[1024];
} AuroraPdStreamState;

static unsigned char s_AuroraPdStreamCache[AURORA_PD_STREAM_CACHE_BYTES]
  __attribute__((aligned(64)));
static pm_file *s_AuroraPdStreamCacheOwner;
static long s_AuroraPdStreamCacheStart;
static size_t s_AuroraPdStreamCacheLength;

/* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830
 *
 * Strict producer/consumer window.
 *
 * - main/emulation thread: memcpy from READY bytes or emits silence.
 * - worker EE thread: owns a separate unbuffered FILE* and may block forever.
 * - main thread never WaitSema(), fread(), fseek() or waits for the worker.
 * - 8 KiB is deliberately small to bound filesystem-device monopolisation.
 * - worker is one priority level ABOVE the caller. Aurora's gsKit VBlank wait
 *   is a busy-spin, so a lower-priority worker would otherwise starve.
 */
#define AURORA_PD_ASYNC_BYTES (256 * 1024)
#define AURORA_PD_ASYNC_CHUNK (32 * 1024)
#define AURORA_PD_ASYNC_REBASE_LAG (16 * 1024)

static unsigned char s_AuroraPdAsyncBuffer[AURORA_PD_ASYNC_BYTES]
  __attribute__((aligned(64)));
static unsigned char s_AuroraPdAsyncStack[16 * 1024]
  __attribute__((aligned(16)));

static int s_AuroraPdAsyncThreadId = -1;
static int s_AuroraPdAsyncSema = -1;
static unsigned int s_AuroraPdAsyncSerialCounter;

static volatile unsigned int s_AuroraPdAsyncReqSeq;
static volatile unsigned int s_AuroraPdAsyncReqSerial;
static volatile long s_AuroraPdAsyncReqStart;
static char s_AuroraPdAsyncReqPath[1024];

static volatile unsigned int s_AuroraPdAsyncBufSerial;
static volatile long s_AuroraPdAsyncBufStart;
static volatile size_t s_AuroraPdAsyncBufReady;

static void AuroraPdAsyncThread(void *arg)
{
  unsigned int handled_seq = 0;
  unsigned int active_seq = 0;
  long active_start = 0;
  long worker_pos = 0;
  char active_path[1024];
  char open_path[1024];
  /* AURORA_V4_7_SEGACD_CDDA_WORKER_VFS_SEEK_FIX_20260830
   * Explicit RFILE/VFS: never rely on stdio-transform macro semantics in
   * the async CDDA worker. */
  RFILE *worker_file = NULL;

  (void)arg;
  active_path[0] = 0;
  open_path[0] = 0;

  for (;;)
  {
    unsigned int seq;
    size_t done, want;
    int64_t got;

    WaitSema(s_AuroraPdAsyncSema);
    seq = s_AuroraPdAsyncReqSeq;

    if (seq != handled_seq)
    {
      handled_seq = seq;
      active_seq = seq;
      active_start = s_AuroraPdAsyncReqStart;
      strncpy(active_path, s_AuroraPdAsyncReqPath,
              sizeof(active_path) - 1);
      active_path[sizeof(active_path) - 1] = 0;

      if (!active_path[0])
      {
        if (worker_file)
          filestream_close(worker_file);
        worker_file = NULL;
        open_path[0] = 0;
        continue;
      }

      if (!worker_file || strcmp(open_path, active_path))
      {
        if (worker_file)
          filestream_close(worker_file);
        worker_file = filestream_open(
          active_path,
          RETRO_VFS_FILE_ACCESS_READ,
          RETRO_VFS_FILE_ACCESS_HINT_NONE);
        open_path[0] = 0;
        if (worker_file)
        {
          /* VFS RFILE has no stdio setvbuf; separate handle is enough. */
          strncpy(open_path, active_path, sizeof(open_path) - 1);
          open_path[sizeof(open_path) - 1] = 0;
        }
      }

      if (!worker_file ||
          filestream_seek(worker_file, active_start,
                          RETRO_VFS_SEEK_POSITION_START) < 0)
      {
        if (worker_file)
        {
          filestream_close(worker_file);
          worker_file = NULL;
          open_path[0] = 0;
        }
        continue;
      }
      worker_pos = active_start;
    }

    if (!worker_file || active_seq != s_AuroraPdAsyncReqSeq)
      continue;

    done = s_AuroraPdAsyncBufReady;
    if (done >= AURORA_PD_ASYNC_BYTES)
      continue;

    if (worker_pos != active_start + (long)done)
    {
      if (filestream_seek(worker_file,
                          active_start + (long)done,
                          RETRO_VFS_SEEK_POSITION_START) < 0)
        continue;
      worker_pos = active_start + (long)done;
    }

    want = AURORA_PD_ASYNC_BYTES - done;
    if (want > AURORA_PD_ASYNC_CHUNK)
      want = AURORA_PD_ASYNC_CHUNK;

    got = filestream_read(
      worker_file, s_AuroraPdAsyncBuffer + done, (int64_t)want);

    if (active_seq != s_AuroraPdAsyncReqSeq)
      continue;

    if (got > 0)
    {
      worker_pos += (long)got;
      AURORA_EE_SYNC();
      s_AuroraPdAsyncBufReady = done + (size_t)got;
    }
  }
}

static int AuroraPdAsyncEnsureThread(void)
{
  AuroraEeSemaT sema;
  AuroraEeThreadT thread;
  AuroraEeThreadStatusT current;
  int priority = 40;

  if (s_AuroraPdAsyncThreadId >= 0 && s_AuroraPdAsyncSema >= 0)
    return 1;

  memset(&sema, 0, sizeof(sema));
  sema.init_count = 0;
  sema.max_count = 1;
  s_AuroraPdAsyncSema = CreateSema(&sema);
  if (s_AuroraPdAsyncSema < 0)
    return 0;

  memset(&current, 0, sizeof(current));
  if (ReferThreadStatus(0, &current) >= 0)
  {
    priority = current.current_priority;
    if (priority > 1)
      --priority;
  }

  memset(&thread, 0, sizeof(thread));
  thread.func = (void *)AuroraPdAsyncThread;
  thread.stack = s_AuroraPdAsyncStack;
  thread.stack_size = sizeof(s_AuroraPdAsyncStack);
  thread.gp_reg = &_gp;
  thread.initial_priority = priority;

  s_AuroraPdAsyncThreadId = CreateThread(&thread);
  if (s_AuroraPdAsyncThreadId < 0)
  {
    DeleteSema(s_AuroraPdAsyncSema);
    s_AuroraPdAsyncSema = -1;
    return 0;
  }

  if (StartThread(s_AuroraPdAsyncThreadId, NULL) < 0)
  {
    DeleteThread(s_AuroraPdAsyncThreadId);
    DeleteSema(s_AuroraPdAsyncSema);
    s_AuroraPdAsyncThreadId = -1;
    s_AuroraPdAsyncSema = -1;
    return 0;
  }

  return 1;
}

static void AuroraPdAsyncSignal(void)
{
  if (s_AuroraPdAsyncSema >= 0)
    (void)SignalSema(s_AuroraPdAsyncSema);
}

static void AuroraPdAsyncReset(unsigned int serial,
                               const char *path, long start)
{
  if (!serial || !path || !*path || !AuroraPdAsyncEnsureThread())
    return;

  s_AuroraPdAsyncReqSerial = serial;
  s_AuroraPdAsyncReqStart = start;
  strncpy(s_AuroraPdAsyncReqPath, path,
          sizeof(s_AuroraPdAsyncReqPath) - 1);
  s_AuroraPdAsyncReqPath[sizeof(s_AuroraPdAsyncReqPath) - 1] = 0;

  s_AuroraPdAsyncBufSerial = serial;
  s_AuroraPdAsyncBufStart = start;
  s_AuroraPdAsyncBufReady = 0;
  AURORA_EE_SYNC();
  ++s_AuroraPdAsyncReqSeq;
  AuroraPdAsyncSignal();
}

static void AuroraPdAsyncKick(AuroraPdStreamState *state, long pos)
{
  long ready_end;

  if (!state || !state->async_serial || !state->async_path[0])
    return;

  if (s_AuroraPdAsyncBufSerial != state->async_serial ||
      pos < s_AuroraPdAsyncBufStart ||
      pos >= s_AuroraPdAsyncBufStart + AURORA_PD_ASYNC_BYTES)
  {
    AuroraPdAsyncReset(state->async_serial, state->async_path, pos);
    return;
  }

  ready_end = s_AuroraPdAsyncBufStart +
              (long)s_AuroraPdAsyncBufReady;

  /* AURORA_V4_9_SEGACD_CDDA_CHASE_REVIVE_20260830
   * Audio chases game: if READY bytes are materially behind the logical
   * playhead, discard that obsolete generation and refill from NOW. */
  if (pos > ready_end + AURORA_PD_ASYNC_REBASE_LAG)
  {
    AuroraPdAsyncReset(state->async_serial, state->async_path, pos);
    return;
  }

  if (s_AuroraPdAsyncBufReady < AURORA_PD_ASYNC_BYTES)
    AuroraPdAsyncSignal();
}

/* V4_12_1: implementation lives in the private-fileXio block below. */
static void AuroraPdFxRequest(AuroraPdStreamState *state,
                              long pos, int force);

void PicoDriveAurora_PrimeCdAudio(pm_file *stream)
{
  AuroraPdStreamState *state;

  if (!stream || stream->type != PMT_CD_UNCOMPRESSED)
    return;

  state = (AuroraPdStreamState *)stream->param;
  if (!state)
    return;

  AuroraPdFxRequest(state, state->pos, 1);
}


static void AuroraPdAsyncForget(unsigned int serial)
{
  if (!serial)
    return;

  if (s_AuroraPdAsyncBufSerial == serial ||
      s_AuroraPdAsyncReqSerial == serial)
  {
    s_AuroraPdAsyncBufSerial = 0;
    s_AuroraPdAsyncBufReady = 0;
    s_AuroraPdAsyncReqSerial = 0;
    s_AuroraPdAsyncReqPath[0] = 0;
    AURORA_EE_SYNC();
    ++s_AuroraPdAsyncReqSeq;
    AuroraPdAsyncSignal();
  }
}

static void AuroraPdAsyncCancelAll(void)
{
  s_AuroraPdAsyncBufSerial = 0;
  s_AuroraPdAsyncBufReady = 0;
  s_AuroraPdAsyncReqSerial = 0;
  s_AuroraPdAsyncReqPath[0] = 0;
  AURORA_EE_SYNC();
  ++s_AuroraPdAsyncReqSeq;
  AuroraPdAsyncSignal();
}

/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
static int s_AuroraPdCdAudioSafeWindow;
static int s_AuroraPdCdAudioRefillRequested;
/* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830 */
static pm_file *s_AuroraPdCdAudioPendingStream;
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
static int s_AuroraPdCdMusicEnabled = 1;

void PicoDriveAurora_SetCdAudioSafeWindow(int allowed)
{
  s_AuroraPdCdAudioSafeWindow = allowed ? 1 : 0;
}

int PicoDriveAurora_ConsumeCdAudioRefillRequest(void)
{
  int requested = s_AuroraPdCdAudioRefillRequested;
  s_AuroraPdCdAudioRefillRequested = 0;
  return requested;
}

static void AuroraPdRequestCdAudioRefill(pm_file *stream)
{
  s_AuroraPdCdAudioRefillRequested = 1;
  if (stream)
    s_AuroraPdCdAudioPendingStream = stream;
}

/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
void PicoDriveAurora_SetCdMusicEnabled(int enabled)
{
  s_AuroraPdCdMusicEnabled = enabled ? 1 : 0;
  if (!s_AuroraPdCdMusicEnabled)
  {
    s_AuroraPdCdAudioSafeWindow = 0;
    s_AuroraPdCdAudioRefillRequested = 0;
    s_AuroraPdCdAudioPendingStream = NULL;
    /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830 */
    AuroraPdAsyncCancelAll();
  }
}

int PicoDriveAurora_CdMusicEnabled(void)
{
  return s_AuroraPdCdMusicEnabled;
}

#if defined(USE_LIBCHDR)
static int AuroraPdPrefetchChdAudio(pm_file *stream);
#endif

/* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830
 * Prefill at current logical CDDA position without consuming samples. */
int PicoDriveAurora_PrefetchCdAudio(void)
{
  /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830
   * Retired: synchronous CDDA prefetch is forbidden on every host tick. */
  return 0;
}

static size_t AuroraPdReadCached(void *ptr, size_t bytes, pm_file *stream)
{
  AuroraPdStreamState *state;
  unsigned char *out;
  size_t total;

  if (!stream || !ptr || bytes == 0)
    return 0;

  state = (AuroraPdStreamState *)stream->param;
  if (!state)
    return fread(ptr, 1, bytes, stream->file);

  out = (unsigned char *)ptr;
  total = 0;

  /* AURORA_CD_AUDIO_STREAM_V4_PD_DIRECT_THRESHOLD_20260829
   * AURORA_CD_AUDIO_STREAM_V5_PD_THRESHOLD32_20260829
   * AURORA_CD_AUDIO_STREAM_V6_PD_THRESHOLD64_20260829
   * V3 geometry restored: the 128 KiB cache keeps a 64 KiB direct-read
   * threshold. CDDA/sector-sized reads remain cached. */
  if (bytes >= (AURORA_PD_STREAM_CACHE_BYTES / 2))
  {
    size_t got;
    fseek(stream->file, state->pos, SEEK_SET);
    got = fread(out, 1, bytes, stream->file);
    state->pos += (long)got;

    if (s_AuroraPdStreamCacheOwner == stream)
    {
      s_AuroraPdStreamCacheOwner = NULL;
      s_AuroraPdStreamCacheLength = 0;
    }
    return got;
  }

  while (bytes > 0)
  {
    size_t available = 0;

    if (s_AuroraPdStreamCacheOwner == stream &&
        state->pos >= s_AuroraPdStreamCacheStart &&
        state->pos < s_AuroraPdStreamCacheStart +
                     (long)s_AuroraPdStreamCacheLength)
    {
      available = s_AuroraPdStreamCacheLength -
        (size_t)(state->pos - s_AuroraPdStreamCacheStart);
    }
    else
    {
      size_t want = AURORA_PD_STREAM_CACHE_BYTES;
      size_t got;

      if (state->pos < 0)
        break;

      if ((unsigned long)state->pos < (unsigned long)stream->size)
      {
        unsigned long remaining =
          (unsigned long)stream->size - (unsigned long)state->pos;
        if (remaining < want)
          want = (size_t)remaining;
      }
      else if ((unsigned long)state->pos >= (unsigned long)stream->size)
      {
        break;
      }

      fseek(stream->file, state->pos, SEEK_SET);
      got = fread(s_AuroraPdStreamCache, 1, want, stream->file);

      s_AuroraPdStreamCacheOwner = stream;
      s_AuroraPdStreamCacheStart = state->pos;
      s_AuroraPdStreamCacheLength = got;
      available = got;

      if (available == 0)
        break;
    }

    if (available > bytes)
      available = bytes;

    memcpy(out,
           s_AuroraPdStreamCache +
             (size_t)(state->pos - s_AuroraPdStreamCacheStart),
           available);

    out += available;
    bytes -= available;
    total += available;
    state->pos += (long)available;
  }

  return total;
}

/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
 * If CDDA bytes are not already resident, a presented frame gets silence
 * rather than fseek/fread. Logical playback time still advances. */

/* AURORA_V4_12_PRIVATE_FILEXIO_CDDA_PCE_TOC2CUE_20260830
 *
 * New Sega CD CDDA transport:
 *   control plane = tiny EE thread, blocking only itself on OPEN/LSEEK/CLOSE;
 *   data plane    = second/private fileXio RPC client, READ is NOWAIT.
 *
 * The emulation thread never waits for storage.
 */
#define AURORA_PD_FX_CHUNK (16 * 1024)
#define AURORA_PD_FX_FREE    0
#define AURORA_PD_FX_PENDING 1
#define AURORA_PD_FX_READY   2

extern int  AuroraCdFxOpenSeek(const char *path, long offset);
extern int  AuroraCdFxStartRead(void *buffer, int bytes);
extern int  AuroraCdFxPollRead(int *outBytes);
extern int  AuroraCdFxWaitRead(int *outBytes);
extern void AuroraCdFxClose(void);

static unsigned char s_AuroraPdFxBuffer[2][AURORA_PD_FX_CHUNK]
  __attribute__((aligned(64)));
static unsigned char s_AuroraPdFxStack[8 * 1024]
  __attribute__((aligned(16)));

static volatile int s_AuroraPdFxBufState[2];
static volatile long s_AuroraPdFxBufStart[2];
static volatile int s_AuroraPdFxBufLength[2];
static volatile int s_AuroraPdFxBufRequest[2];
static volatile unsigned int s_AuroraPdFxBufGeneration[2];

static int s_AuroraPdFxThreadId = -1;
static int s_AuroraPdFxSema = -1;
static volatile int s_AuroraPdFxControlBusy;
static volatile int s_AuroraPdFxPendingIndex = -1;

static volatile unsigned int s_AuroraPdFxReqSeq;
static volatile unsigned int s_AuroraPdFxReqSerial;
static volatile long s_AuroraPdFxReqStart;
static char s_AuroraPdFxReqPath[1024];

static volatile unsigned int s_AuroraPdFxAppliedSeq;
static volatile unsigned int s_AuroraPdFxOwnerSerial;
static volatile unsigned int s_AuroraPdFxGeneration;
static volatile long s_AuroraPdFxFdPos;

static void AuroraPdFxClearBuffers(void)
{
  int i;
  for (i = 0; i < 2; ++i)
  {
    s_AuroraPdFxBufState[i] = AURORA_PD_FX_FREE;
    s_AuroraPdFxBufStart[i] = 0;
    s_AuroraPdFxBufLength[i] = 0;
    s_AuroraPdFxBufRequest[i] = 0;
    s_AuroraPdFxBufGeneration[i] = 0;
  }
  s_AuroraPdFxPendingIndex = -1;
}

static void AuroraPdFxThread(void *arg)
{
  unsigned int handled = 0;
  (void)arg;

  for (;;)
  {
    unsigned int seq, serial;
    long start;
    char path[1024];
    int ignored = 0;
    int ok = 0;

    WaitSema(s_AuroraPdFxSema);

    seq = s_AuroraPdFxReqSeq;
    if (seq == handled)
      continue;
    handled = seq;

    serial = s_AuroraPdFxReqSerial;
    start = s_AuroraPdFxReqStart;
    strncpy(path, s_AuroraPdFxReqPath, sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;

    s_AuroraPdFxControlBusy = 1;
    AURORA_EE_SYNC();

    if (s_AuroraPdFxPendingIndex >= 0)
    {
      (void)AuroraCdFxWaitRead(&ignored);
      s_AuroraPdFxPendingIndex = -1;
    }

    AuroraPdFxClearBuffers();

    if (serial && path[0] && start >= 0)
    {
      ok = AuroraCdFxOpenSeek(path, start);
      if (ok)
      {
        s_AuroraPdFxOwnerSerial = serial;
        s_AuroraPdFxFdPos = start;
        ++s_AuroraPdFxGeneration;
        if (!s_AuroraPdFxGeneration)
          ++s_AuroraPdFxGeneration;
      }
      else
      {
        AuroraCdFxClose();
        s_AuroraPdFxOwnerSerial = 0;
      }
    }
    else
    {
      AuroraCdFxClose();
      s_AuroraPdFxOwnerSerial = 0;
    }

    s_AuroraPdFxAppliedSeq = seq;
    AURORA_EE_SYNC();
    s_AuroraPdFxControlBusy = 0;
    AURORA_EE_SYNC();

    if (s_AuroraPdFxReqSeq != handled)
      (void)SignalSema(s_AuroraPdFxSema);
  }
}

static int AuroraPdFxEnsureThread(void)
{
  AuroraEeSemaT sema;
  AuroraEeThreadT thread;
  AuroraEeThreadStatusT current;
  int priority = 40;

  if (s_AuroraPdFxThreadId >= 0 && s_AuroraPdFxSema >= 0)
    return 1;

  memset(&sema, 0, sizeof(sema));
  sema.init_count = 0;
  sema.max_count = 1;
  s_AuroraPdFxSema = CreateSema(&sema);
  if (s_AuroraPdFxSema < 0)
    return 0;

  memset(&current, 0, sizeof(current));
  if (ReferThreadStatus(0, &current) >= 0)
  {
    priority = current.current_priority;
    if (priority > 1)
      --priority;
  }

  memset(&thread, 0, sizeof(thread));
  thread.func = (void *)AuroraPdFxThread;
  thread.stack = s_AuroraPdFxStack;
  thread.stack_size = sizeof(s_AuroraPdFxStack);
  thread.gp_reg = &_gp;
  thread.initial_priority = priority;

  s_AuroraPdFxThreadId = CreateThread(&thread);
  if (s_AuroraPdFxThreadId < 0)
  {
    DeleteSema(s_AuroraPdFxSema);
    s_AuroraPdFxSema = -1;
    return 0;
  }

  if (StartThread(s_AuroraPdFxThreadId, NULL) < 0)
  {
    DeleteThread(s_AuroraPdFxThreadId);
    DeleteSema(s_AuroraPdFxSema);
    s_AuroraPdFxThreadId = -1;
    s_AuroraPdFxSema = -1;
    return 0;
  }

  return 1;
}

static void AuroraPdFxQueue(unsigned int serial,
                            const char *path, long start)
{
  if (!AuroraPdFxEnsureThread())
    return;

  s_AuroraPdFxReqSerial = serial;
  s_AuroraPdFxReqStart = start;

  if (path)
  {
    strncpy(s_AuroraPdFxReqPath, path,
            sizeof(s_AuroraPdFxReqPath) - 1);
    s_AuroraPdFxReqPath[sizeof(s_AuroraPdFxReqPath) - 1] = 0;
  }
  else
    s_AuroraPdFxReqPath[0] = 0;

  AURORA_EE_SYNC();
  ++s_AuroraPdFxReqSeq;
  if (!s_AuroraPdFxReqSeq)
    ++s_AuroraPdFxReqSeq;
  AURORA_EE_SYNC();

  if (s_AuroraPdFxSema >= 0)
    (void)SignalSema(s_AuroraPdFxSema);
}

static void AuroraPdFxRequest(AuroraPdStreamState *state,
                              long pos, int force)
{
  if (!state || !state->async_serial || !state->async_path[0] ||
      pos < 0)
    return;

  if (!force &&
      s_AuroraPdFxReqSerial == state->async_serial &&
      pos >= s_AuroraPdFxReqStart &&
      pos < s_AuroraPdFxReqStart + (AURORA_PD_FX_CHUNK * 2))
    return;

  AuroraPdFxQueue(state->async_serial, state->async_path, pos);
}

static void AuroraPdFxForget(unsigned int serial)
{
  if (!serial)
    return;

  if (s_AuroraPdFxOwnerSerial == serial ||
      s_AuroraPdFxReqSerial == serial)
    AuroraPdFxQueue(0, NULL, 0);
}

static void AuroraPdFxPoll(void)
{
  int idx, got = 0, rc;

  if (s_AuroraPdFxControlBusy)
    return;

  idx = s_AuroraPdFxPendingIndex;
  if (idx < 0 || idx > 1)
    return;

  rc = AuroraCdFxPollRead(&got);
  if (!rc)
    return;

  s_AuroraPdFxPendingIndex = -1;

  if (s_AuroraPdFxBufGeneration[idx] != s_AuroraPdFxGeneration ||
      got <= 0)
  {
    s_AuroraPdFxBufState[idx] = AURORA_PD_FX_FREE;
    s_AuroraPdFxBufLength[idx] = 0;
    return;
  }

  if (got > s_AuroraPdFxBufRequest[idx])
    got = s_AuroraPdFxBufRequest[idx];

  s_AuroraPdFxBufLength[idx] = got;
  s_AuroraPdFxBufState[idx] = AURORA_PD_FX_READY;
  s_AuroraPdFxFdPos =
    s_AuroraPdFxBufStart[idx] + (long)got;
  AURORA_EE_SYNC();
}

/* AURORA_V4_17_SAFE_CD_GAME_SWITCH_QUIESCE_20260830
 *
 * Game switching must never enter PicoExitMCD()/pm_close() while the private
 * CDDA fileXio client still owns a NOWAIT read.  The normal control worker
 * historically resolves a track change with AuroraCdFxWaitRead(), which is
 * correct during playback but can become an unbounded teardown wait.
 *
 * This boundary is intentionally conservative:
 *   - stop host-side CDDA refill requests;
 *   - retire the unused legacy async request generation;
 *   - poll the private READ only (never WaitSema on the main thread);
 *   - once no READ is pending, ask the control worker to CLOSE its private fd;
 *   - yield to the higher-priority worker for at most 250 ms.
 *
 * Success means the private transport is fully idle before cdd_unload().
 * Timeout means "do not switch yet": the caller keeps the current core alive
 * and may retry later.  No thread is killed and no in-flight DMA/RPC buffer is
 * freed underneath the IOP.
 */
int PicoDriveAurora_PrepareGameSwitch(void)
{
  int i;
  int close_queued = 0;

  s_AuroraPdCdAudioSafeWindow = 0;
  s_AuroraPdCdAudioRefillRequested = 0;
  s_AuroraPdCdAudioPendingStream = NULL;

  /* Currently dormant for CDDA, but invalidate it as part of the same
   * lifetime boundary so an older/future producer cannot survive a switch. */
  AuroraPdAsyncCancelAll();

  for (i = 0; i < 250; ++i)
  {
    if (!s_AuroraPdFxControlBusy)
    {
      /* Poll is strictly non-blocking.  If the callback has completed,
       * this retires s_AuroraPdFxPendingIndex and its buffer generation. */
      AuroraPdFxPoll();

      if (!s_AuroraPdFxControlBusy &&
          s_AuroraPdFxPendingIndex < 0 &&
          s_AuroraPdFxAppliedSeq == s_AuroraPdFxReqSeq)
      {
        if (s_AuroraPdFxOwnerSerial == 0 &&
            s_AuroraPdFxReqSerial == 0)
          return 1;

        /* Queue CLOSE only after the data-plane READ is known complete.
         * Therefore AuroraPdFxThread cannot enter AuroraCdFxWaitRead() for
         * this game-switch request. */
        if (!close_queued)
        {
          AuroraPdFxQueue(0, NULL, 0);
          close_queued = 1;
        }
      }
    }

    /* Never spin against the worker.  It is created one EE priority above
     * the caller, so this also gives OPEN/CLOSE and the completion callback
     * a chance to finish. */
    DelayThread(1000);
  }

  return 0;
}

/* AURORA_V4_17_SAFE_CD_GAME_SWITCH_QUIESCE_20260830 */

static long AuroraPdFxFurthestCoverage(void)
{
  long furthest = s_AuroraPdFxFdPos;
  int i;

  for (i = 0; i < 2; ++i)
  {
    long end;
    if (s_AuroraPdFxBufState[i] == AURORA_PD_FX_READY)
      end = s_AuroraPdFxBufStart[i] +
            (long)s_AuroraPdFxBufLength[i];
    else if (s_AuroraPdFxBufState[i] == AURORA_PD_FX_PENDING)
      end = s_AuroraPdFxBufStart[i] +
            (long)s_AuroraPdFxBufRequest[i];
    else
      continue;

    if (end > furthest)
      furthest = end;
  }

  return furthest;
}

static void AuroraPdFxLaunch(pm_file *stream,
                             AuroraPdStreamState *state)
{
  int idx = -1;
  long remaining;
  int want;
  int i;

  if (!stream || !state ||
      s_AuroraPdFxControlBusy ||
      s_AuroraPdFxPendingIndex >= 0 ||
      s_AuroraPdFxAppliedSeq != s_AuroraPdFxReqSeq ||
      s_AuroraPdFxOwnerSerial != state->async_serial)
    return;

  for (i = 0; i < 2; ++i)
    if (s_AuroraPdFxBufState[i] == AURORA_PD_FX_FREE)
    {
      idx = i;
      break;
    }

  if (idx < 0 || s_AuroraPdFxFdPos < 0 ||
      (unsigned long)s_AuroraPdFxFdPos >=
        (unsigned long)stream->size)
    return;

  remaining = (long)stream->size - s_AuroraPdFxFdPos;
  want = remaining > AURORA_PD_FX_CHUNK
       ? AURORA_PD_FX_CHUNK : (int)remaining;

  want &= ~63;
  if (want < 64)
    return;

  s_AuroraPdFxBufStart[idx] = s_AuroraPdFxFdPos;
  s_AuroraPdFxBufLength[idx] = 0;
  s_AuroraPdFxBufRequest[idx] = want;
  s_AuroraPdFxBufGeneration[idx] = s_AuroraPdFxGeneration;
  s_AuroraPdFxBufState[idx] = AURORA_PD_FX_PENDING;
  s_AuroraPdFxPendingIndex = idx;
  AURORA_EE_SYNC();

  if (!AuroraCdFxStartRead(s_AuroraPdFxBuffer[idx], want))
  {
    s_AuroraPdFxPendingIndex = -1;
    s_AuroraPdFxBufState[idx] = AURORA_PD_FX_FREE;
  }
}

static void AuroraPdFxMaintain(pm_file *stream,
                               AuroraPdStreamState *state)
{
  long furthest;
  int i;

  if (!stream || !state)
    return;

  AuroraPdFxPoll();

  if (s_AuroraPdFxAppliedSeq != s_AuroraPdFxReqSeq ||
      s_AuroraPdFxControlBusy ||
      s_AuroraPdFxOwnerSerial != state->async_serial)
  {
    if (s_AuroraPdFxReqSerial != state->async_serial)
      AuroraPdFxRequest(state, state->pos, 1);
    return;
  }

  for (i = 0; i < 2; ++i)
  {
    if (s_AuroraPdFxBufState[i] == AURORA_PD_FX_READY &&
        state->pos >= s_AuroraPdFxBufStart[i] +
                      s_AuroraPdFxBufLength[i])
      s_AuroraPdFxBufState[i] = AURORA_PD_FX_FREE;
  }

  furthest = AuroraPdFxFurthestCoverage();

  if (state->pos > furthest)
  {
    const long lag = state->pos - furthest;

    /* AURORA_V4_13_CDDA_STARVATION_PCE_TRACK_GEOMETRY_20260830
     *
     * A small failsoft lead is normal while OPEN/LSEEK or a previous async
     * request finishes. Do not immediately rebase and return without ever
     * launching a read. One 16 KiB read from the current fd position still
     * covers a playhead that is less than one chunk ahead.
     */
    if (lag >= AURORA_PD_FX_CHUNK)
    {
      AuroraPdFxRequest(state, state->pos, 1);
      return;
    }
  }

  AuroraPdFxLaunch(stream, state);
}

static size_t AuroraPdReadAudioFailsoft(void *ptr, size_t bytes,
                                        pm_file *stream)
{
  AuroraPdStreamState *state;
  unsigned char *out = (unsigned char *)ptr;
  size_t total = 0;

  if (!stream || !ptr || bytes == 0)
    return 0;

  state = (AuroraPdStreamState *)stream->param;
  if (!state)
  {
    memset(ptr, 0, bytes);
    return bytes;
  }

  while (bytes > 0)
  {
    size_t take = 0;
    int i;

    if (state->pos < 0 ||
        (unsigned long)state->pos >= (unsigned long)stream->size)
      break;

    AuroraPdFxMaintain(stream, state);

    if (!s_AuroraPdFxControlBusy &&
        s_AuroraPdFxAppliedSeq == s_AuroraPdFxReqSeq &&
        s_AuroraPdFxOwnerSerial == state->async_serial)
    {
      for (i = 0; i < 2; ++i)
      {
        long start, endpos;

        if (s_AuroraPdFxBufState[i] != AURORA_PD_FX_READY ||
            s_AuroraPdFxBufGeneration[i] != s_AuroraPdFxGeneration)
          continue;

        start = s_AuroraPdFxBufStart[i];
        endpos = start + s_AuroraPdFxBufLength[i];

        if (state->pos >= start && state->pos < endpos)
        {
          size_t available = (size_t)(endpos - state->pos);
          take = available < bytes ? available : bytes;

          memcpy(out,
                 s_AuroraPdFxBuffer[i] +
                   (size_t)(state->pos - start),
                 take);
          break;
        }
      }
    }

    if (take > 0)
    {
      out += take;
      bytes -= take;
      total += take;
      state->pos += (long)take;
      AuroraPdFxMaintain(stream, state);
      continue;
    }

    {
      size_t drop = bytes;
      unsigned long remaining =
        (unsigned long)stream->size - (unsigned long)state->pos;

      if ((unsigned long)drop > remaining)
        drop = (size_t)remaining;
      if (!drop)
        break;

      memset(out, 0, drop);
      out += drop;
      bytes -= drop;
      total += drop;
      state->pos += (long)drop;
      AuroraPdFxMaintain(stream, state);
    }
  }

  return total;
}

static int AuroraPdSeekCached(pm_file *stream, long offset, int whence)
{
  AuroraPdStreamState *state;
  long newpos;

  if (!stream)
    return -1;

  state = (AuroraPdStreamState *)stream->param;
  if (!state)
  {
    fseek(stream->file, offset, whence);
    return ftell(stream->file);
  }

  switch (whence)
  {
    case SEEK_SET:
      newpos = offset;
      break;
    case SEEK_CUR:
      newpos = state->pos + offset;
      break;
    case SEEK_END:
      newpos = (long)stream->size + offset;
      break;
    default:
      return -1;
  }

  if (newpos < 0)
    return -1;

  state->pos = newpos;
  return (int)newpos;
}

static void AuroraPdCloseCached(pm_file *stream)
{
  if (!stream)
    return;

  /* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830 */
  if (s_AuroraPdCdAudioPendingStream == stream)
    s_AuroraPdCdAudioPendingStream = NULL;

  if (s_AuroraPdStreamCacheOwner == stream)
  {
    s_AuroraPdStreamCacheOwner = NULL;
    s_AuroraPdStreamCacheLength = 0;
  }

  if (stream->param)
  {
    /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830 */
    AuroraPdFxForget(
      ((AuroraPdStreamState *)stream->param)->async_serial);
    AuroraPdAsyncForget(
      ((AuroraPdStreamState *)stream->param)->async_serial);
    free(stream->param);
    stream->param = NULL;
  }
}

/* AURORA_PD_BORROW_AURORA_ROM_V1
 *
 * Aurora already holds the cartridge in one 64-byte-aligned 8 MiB+1 KiB
 * BSS buffer. Avoid allocating/copying a second ROM on the 32 MiB PS2.
 *
 * external_* is the buffer offered for the next PicoCartLoad().
 * borrowed_* describes the buffer currently used by Pico.rom.
 * They are separate because PicoLoadMedia() calls PicoCartUnload()
 * before PicoCartLoad().
 */
static const unsigned char *ps2_external_rom;
static unsigned int ps2_external_rom_capacity;
static int ps2_rom_borrowed;
static unsigned int ps2_borrowed_capacity;

void PicoCartSetExternalRomBuffer(const unsigned char *rom,
  unsigned int capacity)
{
  ps2_external_rom = rom;
  ps2_external_rom_capacity = capacity;
}
#endif
static const char *rom_exts[] = { "bin", "gen", "smd", "md", "32x", "pco", "iso", "sms", "gg", "sg", "sc" };

void (*PicoCartUnloadHook)(void);
void (*PicoCartMemSetup)(void);

void (*PicoCartLoadProgressCB)(int percent) = NULL;
void (*PicoCDLoadProgressCB)(const char *fname, int percent) = NULL; // handled in Pico/cd/cd_file.c

int PicoGameLoaded;

static void PicoCartDetect(const char *carthw_cfg);
static void PicoCartDetectMS(void);

/* cso struct */
typedef struct _cso_struct
{
  unsigned char in_buff[2*2048];
  unsigned char out_buff[2048];
  struct {
    char          magic[4];
    unsigned int  unused;
    unsigned int  total_bytes;
    unsigned int  total_bytes_high; // ignored here
    unsigned int  block_size;  // 10h
    unsigned char ver;
    unsigned char align;
    unsigned char reserved[2];
  } header;
  unsigned int  fpos_in;  // input file read pointer
  unsigned int  fpos_out; // pos in virtual decompressed file
  int block_in_buff;      // block which we have read in in_buff
  int pad;
  int index[0];
}
cso_struct;

static int uncompress_buf(void *dest, int destLen, void *source, int sourceLen)
{
    z_stream stream;
    int err;

    stream.next_in = (Bytef*)source;
    stream.avail_in = (uInt)sourceLen;
    stream.next_out = dest;
    stream.avail_out = (uInt)destLen;

    stream.zalloc = NULL;
    stream.zfree = NULL;

    err = inflateInit2(&stream, -15);
    if (err != Z_OK) return err;

    err = inflate(&stream, Z_FINISH);
    if (err != Z_STREAM_END) {
        inflateEnd(&stream);
        return err;
    }
    //*destLen = stream.total_out;

    return inflateEnd(&stream);
}

static const char *get_ext(const char *path)
{
  const char *ext;
  if (strlen(path) < 4)
    return ""; // no ext

  // allow 2 or 3 char extensions for now
  ext = path + strlen(path) - 2;
  if (ext[-1] != '.') ext--;
  if (ext[-1] != '.')
    return "";
  return ext;
}

struct zip_file {
  pm_file file;
  ZIP *zip;
  struct zipent *entry;
  z_stream stream;
  unsigned char inbuf[16384];
  long start;
  unsigned int pos;
};

#if defined(USE_LIBCHDR)
struct chd_struct {
  pm_file file;
  int fpos;
  int sectorsize;
  chd_file *chd;
  int unitbytes;
  int hunkunits;
  u8 *hunk;
  int hunknum;
};

/* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830
 * Defined here because struct chd_struct is complete only at this point. */
static int AuroraPdPrefetchChdAudio(pm_file *stream)
{
  struct chd_struct *chd;
  int sector, hunknum;

  if (!stream)
    return 0;

  chd = (struct chd_struct *)stream->file;
  if (!chd || chd->fpos < 0)
    return 0;

  sector = chd->fpos / CD_MAX_SECTOR_DATA;
  hunknum = sector / chd->hunkunits;

  if (hunknum != chd->hunknum)
  {
    if (chd_read(chd->chd, hunknum, chd->hunk) != CHDERR_NONE)
      return 0;
    chd->hunknum = hunknum;
  }

  s_AuroraPdCdAudioPendingStream = NULL;
  return 1;
}
#endif

static pm_file *pm_open_internal(const char *path, int cd_stream)
{
  pm_file *file = NULL;
  const char *ext;
  FILE *f;

  /* AURORA_CD_AUDIO_STREAM_V6_CD_OPEN_IMPL_20260829
   * cd_stream is resolved by the caller at media-open time, never per frame. */
  (void)cd_stream;

  if (path == NULL)
    return NULL;

  ext = get_ext(path);
  if (strcasecmp(ext, "zip") == 0)
  {
    struct zip_file *zfile = NULL;
    struct zipent *zipentry;
    ZIP *zipfile;
    int i, ret;

    zipfile = openzip(path);
    if (zipfile != NULL)
    {
      /* search for suitable file (right extension or large enough file) */
      while ((zipentry = readzip(zipfile)) != NULL)
      {
        ext = get_ext(zipentry->name);

        if (zipentry->uncompressed_size >= 32*1024)
          goto found_rom_zip;

        for (i = 0; i < sizeof(rom_exts)/sizeof(rom_exts[0]); i++)
          if (strcasecmp(ext, rom_exts[i]) == 0)
            goto found_rom_zip;
      }

      /* zipfile given, but nothing found suitable for us inside */
      goto zip_failed;

found_rom_zip:
      zfile = calloc(1, sizeof(*zfile));
      if (zfile == NULL)
        goto zip_failed;
      ret = seekcompresszip(zipfile, zipentry);
      if (ret != 0)
        goto zip_failed;
      ret = inflateInit2(&zfile->stream, -15);
      if (ret != Z_OK) {
        elprintf(EL_STATUS, "zip: inflateInit2 %d", ret);
        goto zip_failed;
      }
      zfile->zip = zipfile;
      zfile->entry = zipentry;
      zfile->start = ftell(zipfile->fp);
      zfile->file.file = zfile;
      zfile->file.size = zipentry->uncompressed_size;
      zfile->file.type = PMT_ZIP;
      strncpy(zfile->file.ext, ext, sizeof(zfile->file.ext) - 1);
      return &zfile->file;

zip_failed:
      closezip(zipfile);
      free(zfile);
      return NULL;
    }
  }
  else if (strcasecmp(ext, "cso") == 0)
  {
    cso_struct *cso = NULL, *tmp = NULL;
    int i, size;
    f = fopen(path, "rb");
    if (f == NULL)
      goto cso_failed;

#ifdef __GP2X__
    /* we use our own buffering */
    setvbuf(f, NULL, _IONBF, 0);
#endif

    cso = malloc(sizeof(*cso));
    if (cso == NULL)
      goto cso_failed;

    if (fread(&cso->header, 1, sizeof(cso->header), f) != sizeof(cso->header))
      goto cso_failed;
    cso->header.block_size = CPU_LE4(cso->header.block_size);
    cso->header.total_bytes = CPU_LE4(cso->header.total_bytes);
    cso->header.total_bytes_high = CPU_LE4(cso->header.total_bytes_high);

    if (strncmp(cso->header.magic, "CISO", 4) != 0) {
      elprintf(EL_STATUS, "cso: bad header");
      goto cso_failed;
    }

    if (cso->header.block_size != 2048) {
      elprintf(EL_STATUS, "cso: bad block size (%u)", cso->header.block_size);
      goto cso_failed;
    }

    size = ((cso->header.total_bytes >> 11) + 1)*4 + sizeof(*cso);
    tmp = realloc(cso, size);
    if (tmp == NULL)
      goto cso_failed;
    cso = tmp;
    elprintf(EL_STATUS, "allocated %i bytes for CSO struct", size);

    size -= sizeof(*cso); // index size
    if (fread(cso->index, 1, size, f) != size) {
      elprintf(EL_STATUS, "cso: premature EOF");
      goto cso_failed;
    }
    for (i = 0; i < size/4; i++)
      cso->index[i] = CPU_LE4(cso->index[i]);

    // all ok
    cso->fpos_in = ftell(f);
    cso->fpos_out = 0;
    cso->block_in_buff = -1;
    file = calloc(1, sizeof(*file));
    if (file == NULL) goto cso_failed;
    file->file  = f;
    file->param = cso;
    file->size  = cso->header.total_bytes;
    file->type  = PMT_CSO;
    strncpy(file->ext, ext, sizeof(file->ext) - 1);
    return file;

cso_failed:
    if (cso != NULL) free(cso);
    if (f != NULL) fclose(f);
    return NULL;
  }
#if defined(USE_LIBCHDR)
  else if (strcasecmp(ext, "chd") == 0)
  {
    struct chd_struct *chd = NULL;
    chd_file *cf = NULL;
    const chd_header *head;

    if (chd_open(path, CHD_OPEN_READ, NULL, &cf) != CHDERR_NONE)
      goto chd_failed;

    // sanity check
    head = chd_get_header(cf);
    if ((head->hunkbytes == 0) || (head->hunkbytes % CD_FRAME_SIZE))
      goto chd_failed;

    chd = calloc(1, sizeof(*chd));
    if (chd == NULL)
      goto chd_failed;
    chd->hunk = (u8 *)malloc(head->hunkbytes);
    if (!chd->hunk)
      goto chd_failed;

    chd->chd = cf;
    chd->unitbytes = head->unitbytes;
    chd->hunkunits = head->hunkbytes / head->unitbytes;
    chd->sectorsize = CD_MAX_SECTOR_DATA; // default to RAW mode

    chd->fpos = 0;
    chd->hunknum = -1;

    chd->file.file = chd;
    chd->file.type = PMT_CHD;
    // subchannel data is skipped, remove it from total size
    chd->file.size = head->logicalbytes / CD_FRAME_SIZE * CD_MAX_SECTOR_DATA;
    strncpy(chd->file.ext, ext, sizeof(chd->file.ext) - 1);
    return &chd->file;

chd_failed:
    /* invalid CHD file */
    if (chd != NULL) free(chd);
    if (cf != NULL) chd_close(cf);
    return NULL;
  }
#endif

  /* not a zip, treat as uncompressed file */
#if defined(RENDER_GSKIT_PS2) && defined(USE_LIBRETRO_VFS)
  /* AURORA_CD_AUDIO_STREAM_V6_CD_OPEN_HINT_20260829
   * The hint reaches setvbuf before libretro-common's first size/seek pass. */
  if (cd_stream)
    f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
                        AURORA_PD_VFS_HINT_CD_STREAM);
  else
    f = fopen(path, "rb");
#else
  f = fopen(path, "rb");
#endif
  if (f == NULL) return NULL;

  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    fclose(f);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  file->file  = f;
  file->param = NULL;
  file->size  = ftell(f);
#if defined(RENDER_GSKIT_PS2)
  /* AURORA_CD_AUDIO_STREAM_V6_CD_ONLY_ASSIGN_20260829 */
  file->type  = cd_stream ? PMT_CD_UNCOMPRESSED : PMT_UNCOMPRESSED;
#else
  file->type  = PMT_UNCOMPRESSED;
#endif
  strncpy(file->ext, ext, sizeof(file->ext) - 1);
  fseek(f, 0, SEEK_SET);

#if defined(RENDER_GSKIT_PS2)
  /* AURORA_CD_AUDIO_STREAM_V2_PD_STATE_20260829
   * AURORA_CD_AUDIO_STREAM_V6_CD_ONLY_STATE_20260829
   * Tiny state exists only for a PS2 Sega CD uncompressed track. The shared
   * data window is the restored V3 128 KiB cache. */
  if (cd_stream)
  {
    AuroraPdStreamState *state =
      (AuroraPdStreamState *)calloc(1, sizeof(*state));
    if (state)
    {
      state->pos = 0;
      state->async_serial = ++s_AuroraPdAsyncSerialCounter;
      if (state->async_serial == 0)
        state->async_serial = ++s_AuroraPdAsyncSerialCounter;
      if (strlen(path) < sizeof(state->async_path))
        strcpy(state->async_path, path);
      else
        state->async_path[0] = 0;
      file->param = state;
      /* V4_12: prepare only the control thread. */
      (void)AuroraPdFxEnsureThread();
    }
  }
#endif

#ifdef __GP2X__
  if (file->size > 0x400000)
    /* we use our own buffering */
    setvbuf(f, NULL, _IONBF, 0);
#endif

  return file;
}

pm_file *pm_open(const char *path)
{
  return pm_open_internal(path, 0);
}

pm_file *pm_open_cd(const char *path)
{
  /* AURORA_CD_AUDIO_STREAM_V6_CD_OPEN_WRAPPER_20260829 */
#if defined(RENDER_GSKIT_PS2)
  return pm_open_internal(path, 1);
#else
  return pm_open_internal(path, 0);
#endif
}

void pm_sectorsize(int length, pm_file *stream)
{
  // CHD reading needs to know how much binary data is in one data sector(=unit)
#if defined(USE_LIBCHDR)
  if (stream->type == PMT_CHD) {
    struct chd_struct *chd = stream->file;
    chd->sectorsize = length;
    if (chd->sectorsize > chd->unitbytes)
      elprintf(EL_STATUS|EL_ANOMALY, "cd: sector size %d too large for unit %d", chd->sectorsize, chd->unitbytes);
  }
#endif
}

#if defined(USE_LIBCHDR)
static size_t _pm_read_chd(void *ptr, size_t bytes, pm_file *stream, int is_audio)
{
  int ret = 0;

  if (stream->type == PMT_CHD) {
    struct chd_struct *chd = stream->file;
    // calculate sector and offset in sector
    int sectsz = is_audio ? CD_MAX_SECTOR_DATA : chd->sectorsize;
    int sector = chd->fpos / sectsz;
    int offset = chd->fpos - (sector * sectsz);
    // calculate hunk and sector offset in hunk
    int hunknum = sector / chd->hunkunits;
    int hunksec = sector - (hunknum * chd->hunkunits);
    int hunkofs = hunksec * chd->unitbytes;

    while (bytes != 0) {
      // data left in current sector
      int len = sectsz - offset;

      // update hunk cache if needed
      if (hunknum != chd->hunknum) {
        chd_read(chd->chd, hunknum, chd->hunk);
        chd->hunknum = hunknum;
      }
      if (len > bytes)
        len = bytes;

#if CPU_IS_LE
      if (is_audio) {
        // convert big endian audio samples
        u16 *dst = ptr, v;
        u8 *src = chd->hunk + hunkofs + offset;
        int i;

        for (i = 0; i < len; i += 4) {
          v = *src++ << 8; *dst++ = v | *src++;
          v = *src++ << 8; *dst++ = v | *src++;
        }
      } else
#endif
        memcpy(ptr, chd->hunk + hunkofs + offset, len);

      // house keeping
      ret += len;
      chd->fpos += len;
      bytes -= len;

      // no need to advance internals if there's no more data to read
      if (bytes) {
        ptr += len;
        offset = 0;

        sector ++;
        hunksec ++;
        hunkofs += chd->unitbytes;
        if (hunksec >= chd->hunkunits) {
          hunksec = 0;
          hunkofs = 0;
          hunknum ++;
        }
      }
    }
  }

  return ret;
}
#endif

#if defined(RENDER_GSKIT_PS2) && defined(USE_LIBCHDR)
/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
 * Resident CHD hunk is allowed; an uncached hunk is deferred to Safe Frameskip. */
static size_t AuroraPdReadChdAudioFailsoft(void *ptr, size_t bytes,
                                           pm_file *stream)
{
  struct chd_struct *chd;
  int start_sector, end_sector;
  int start_hunk, end_hunk;
  size_t drop;

  if (!stream || !ptr || bytes == 0)
    return 0;

  chd = (struct chd_struct *)stream->file;
  if (!chd || chd->fpos < 0 || (unsigned)chd->fpos >= stream->size)
    return 0;

  start_sector = chd->fpos / CD_MAX_SECTOR_DATA;
  end_sector = (chd->fpos + (int)bytes - 1) / CD_MAX_SECTOR_DATA;
  start_hunk = start_sector / chd->hunkunits;
  end_hunk = end_sector / chd->hunkunits;

  if (start_hunk == chd->hunknum && end_hunk == chd->hunknum)
    return _pm_read_chd(ptr, bytes, stream, 1);

  drop = bytes;
  if ((unsigned long)drop >
      (unsigned long)stream->size - (unsigned long)chd->fpos)
    drop = (size_t)((unsigned long)stream->size -
                    (unsigned long)chd->fpos);

  memset(ptr, 0, drop);
  chd->fpos += (int)drop;
  return drop;
}

#endif

size_t pm_read(void *ptr, size_t bytes, pm_file *stream)
{
  int ret;

  if (stream == NULL)
    return -1;
  else if (stream->type == PMT_UNCOMPRESSED)
  {
    /* AURORA_CD_AUDIO_STREAM_V6_GENERIC_READ_ORIGINAL_20260829
     * Generic cartridge/media file path: original PicoDrive behaviour. */
    ret = fread(ptr, 1, bytes, stream->file);
  }
#if defined(RENDER_GSKIT_PS2)
  else if (stream->type == PMT_CD_UNCOMPRESSED)
  {
    /* AURORA_CD_AUDIO_STREAM_V2_PD_READ_20260829
     * AURORA_CD_AUDIO_STREAM_V6_CD_ONLY_READ_20260829 */
    if (stream->param)
      ret = (int)AuroraPdReadCached(ptr, bytes, stream);
    else
      ret = fread(ptr, 1, bytes, stream->file);
  }
#endif
  else if (stream->type == PMT_ZIP)
  {
    struct zip_file *z = stream->file;

    if (z->entry->compression_method == 0) {
      int ret = fread(ptr, 1, bytes, z->zip->fp);
      z->pos += ret;
      return ret;
    }

    z->stream.next_out = ptr;
    z->stream.avail_out = bytes;
    while (z->stream.avail_out != 0) {
      if (z->stream.avail_in == 0) {
        z->stream.avail_in = fread(z->inbuf, 1, sizeof(z->inbuf), z->zip->fp);
        if (z->stream.avail_in == 0)
          break;
        z->stream.next_in = z->inbuf;
      }
      ret = inflate(&z->stream, Z_NO_FLUSH);
      if (ret == Z_STREAM_END)
        break;
      if (ret != Z_OK) {
        elprintf(EL_STATUS, "zip: inflate: %d", ret);
        return 0;
      }
    }
    z->pos += bytes - z->stream.avail_out;
    return bytes - z->stream.avail_out;
  }
  else if (stream->type == PMT_CSO)
  {
    cso_struct *cso = stream->param;
    int read_pos, read_len, out_offs, rret;
    int block = cso->fpos_out >> 11;
    int index = cso->index[block];
    int index_end = cso->index[block+1];
    unsigned char *out = ptr, *tmp_dst;

    ret = 0;
    while (bytes != 0)
    {
      out_offs = cso->fpos_out&0x7ff;
      if (out_offs == 0 && bytes >= 2048)
           tmp_dst = out;
      else tmp_dst = cso->out_buff;

      read_pos = (index&0x7fffffff) << cso->header.align;

      if (index < 0) {
        if (read_pos != cso->fpos_in)
          fseek(stream->file, read_pos, SEEK_SET);
        rret = fread(tmp_dst, 1, 2048, stream->file);
        cso->fpos_in = read_pos + rret;
        if (rret != 2048) break;
      } else {
        read_len = (((index_end&0x7fffffff) << cso->header.align) - read_pos) & 0xfff;
        if (block != cso->block_in_buff)
        {
          if (read_pos != cso->fpos_in)
            fseek(stream->file, read_pos, SEEK_SET);
          rret = fread(cso->in_buff, 1, read_len, stream->file);
          cso->fpos_in = read_pos + rret;
          if (rret != read_len) {
            elprintf(EL_STATUS, "cso: read failed @ %08x", read_pos);
            break;
          }
          cso->block_in_buff = block;
        }
        rret = uncompress_buf(tmp_dst, 2048, cso->in_buff, read_len);
        if (rret != 0) {
          elprintf(EL_STATUS, "cso: uncompress failed @ %08x with %i", read_pos, rret);
          break;
        }
      }

      rret = 2048;
      if (out_offs != 0 || bytes < 2048) {
        //elprintf(EL_STATUS, "cso: unaligned/nonfull @ %08x, offs=%i, len=%u", cso->fpos_out, out_offs, bytes);
        if (bytes < rret) rret = bytes;
        if (2048 - out_offs < rret) rret = 2048 - out_offs;
        memcpy(out, tmp_dst + out_offs, rret);
      }
      ret += rret;
      out += rret;
      cso->fpos_out += rret;
      bytes -= rret;
      block++;
      index = index_end;
      index_end = cso->index[block+1];
    }
  }
#if defined(USE_LIBCHDR)
  else if (stream->type == PMT_CHD)
  {
    ret = _pm_read_chd(ptr, bytes, stream, 0);
  }
#endif
  else
    ret = 0;

  return ret;
}

size_t pm_read_audio(void *ptr, size_t bytes, pm_file *stream)
{
  if (stream == NULL)
    return -1;

#if defined(RENDER_GSKIT_PS2)
  /* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
  if (stream->type == PMT_CD_UNCOMPRESSED)
    return AuroraPdReadAudioFailsoft(ptr, bytes, stream);
#endif

#if !CPU_IS_LE
  if (stream->type == PMT_UNCOMPRESSED)
  {
    // convert little endian audio samples from WAV file
    int ret = pm_read(ptr, bytes, stream);
    u16 *dst = ptr, v;
    u8 *src = ptr;
    int i;

    for (i = 0; i < ret; i += 4) {
      v = *src++; *dst++ = v | (*src++ << 8);
      v = *src++; *dst++ = v | (*src++ << 8);
    }
    return ret;
  }
#endif

#if defined(USE_LIBCHDR)
  if (stream->type == PMT_CHD)
  {
#if defined(RENDER_GSKIT_PS2)
    return AuroraPdReadChdAudioFailsoft(ptr, bytes, stream);
#else
    return _pm_read_chd(ptr, bytes, stream, 1);
#endif
  }
#endif

  return pm_read(ptr, bytes, stream);
}

int pm_seek(pm_file *stream, long offset, int whence)
{
  if (stream == NULL)
    return -1;
  else if (stream->type == PMT_UNCOMPRESSED)
  {
    /* AURORA_CD_AUDIO_STREAM_V6_GENERIC_SEEK_ORIGINAL_20260829 */
    fseek(stream->file, offset, whence);
    return ftell(stream->file);
  }
#if defined(RENDER_GSKIT_PS2)
  else if (stream->type == PMT_CD_UNCOMPRESSED)
  {
    /* AURORA_CD_AUDIO_STREAM_V2_PD_SEEK_20260829
     * AURORA_CD_AUDIO_STREAM_V6_CD_ONLY_SEEK_20260829 */
    if (stream->param)
      return AuroraPdSeekCached(stream, offset, whence);
    fseek(stream->file, offset, whence);
    return ftell(stream->file);
  }
#endif
  else if (stream->type == PMT_ZIP)
  {
    struct zip_file *z = stream->file;
    unsigned int pos = z->pos;
    int ret;

    switch (whence)
    {
      case SEEK_CUR: pos += offset; break;
      case SEEK_SET: pos  = offset; break;
      case SEEK_END: pos  = stream->size - offset; break;
    }
    if (z->entry->compression_method == 0) {
      ret = fseek(z->zip->fp, z->start + pos, SEEK_SET);
      if (ret == 0)
        return (z->pos = pos);
      return -1;
    }
    offset = pos - z->pos;
    if (pos < z->pos) {
      // full decompress from the start
      fseek(z->zip->fp, z->start, SEEK_SET);
      z->stream.avail_in = 0;
      z->stream.next_in = z->inbuf;
      inflateReset(&z->stream);
      z->pos = 0;
      offset = pos;
    }

    if (PicoIn.osdMessage != NULL && offset > 4 * 1024 * 1024)
      PicoIn.osdMessage("Decompressing data...");

    while (offset > 0) {
      char buf[16 * 1024];
      size_t l = offset > sizeof(buf) ? sizeof(buf) : offset;
      ret = pm_read(buf, l, stream);
      if (ret != l)
        break;
      offset -= l;
    }
    return z->pos;
  }
  else if (stream->type == PMT_CSO)
  {
    cso_struct *cso = stream->param;
    switch (whence)
    {
      case SEEK_CUR: cso->fpos_out += offset; break;
      case SEEK_SET: cso->fpos_out  = offset; break;
      case SEEK_END: cso->fpos_out  = cso->header.total_bytes - offset; break;
    }
    return cso->fpos_out;
  }
#if defined(USE_LIBCHDR)
  else if (stream->type == PMT_CHD)
  {
    struct chd_struct *chd = stream->file;
    switch (whence)
    {
      case SEEK_CUR: chd->fpos += offset; break;
      case SEEK_SET: chd->fpos  = offset; break;
      case SEEK_END: chd->fpos  = stream->size - offset; break;
    }
    return chd->fpos;
  }
#endif
  else
    return -1;
}

int pm_close(pm_file *fp)
{
  int ret = 0;

  if (fp == NULL) return EOF;

#if defined(RENDER_GSKIT_PS2)
  /* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830 */
  if (s_AuroraPdCdAudioPendingStream == fp)
    s_AuroraPdCdAudioPendingStream = NULL;
#endif

  if (fp->type == PMT_UNCOMPRESSED)
  {
    /* AURORA_CD_AUDIO_STREAM_V6_GENERIC_CLOSE_ORIGINAL_20260829 */
    fclose(fp->file);
  }
#if defined(RENDER_GSKIT_PS2)
  else if (fp->type == PMT_CD_UNCOMPRESSED)
  {
    /* AURORA_CD_AUDIO_STREAM_V2_PD_CLOSE_20260829
     * AURORA_CD_AUDIO_STREAM_V6_CD_ONLY_CLOSE_20260829 */
    AuroraPdCloseCached(fp);
    fclose(fp->file);
  }
#endif
  else if (fp->type == PMT_ZIP)
  {
    struct zip_file *z = fp->file;
    inflateEnd(&z->stream);
    closezip(z->zip);
  }
  else if (fp->type == PMT_CSO)
  {
    free(fp->param);
    fclose(fp->file);
  }
#if defined(USE_LIBCHDR)
  else if (fp->type == PMT_CHD)
  {
    struct chd_struct *chd = fp->file;
    chd_close(chd->chd);
    if (chd->hunk)
      free(chd->hunk);
  }
#endif
  else
    ret = EOF;

  free(fp);
  return ret;
}

// byteswap, data needs to be int aligned, src can match dst
void Byteswap(void *dst, const void *src, int len)
{
#if CPU_IS_LE
  const unsigned int *ps = src;
  unsigned int *pd = dst;
  int i, m;

  if (len < 2)
    return;

  m = 0x00ff00ff;
  for (i = 0; i < len / 4; i++) {
    unsigned int t = ps[i];
    pd[i] = ((t & m) << 8) | ((t & ~m) >> 8);
  }
#endif
}

// Interleve a 16k block and byteswap
static int InterleveBlock(unsigned char *dest,unsigned char *src)
{
  int i=0;
  for (i=0;i<0x2000;i++) dest[(i<<1)+MEM_BE2(1)]=src[       i]; // Odd
  for (i=0;i<0x2000;i++) dest[(i<<1)+MEM_BE2(0)]=src[0x2000+i]; // Even
  return 0;
}

// Decode a SMD file
static int DecodeSmd(unsigned char *data,int len)
{
  unsigned char *temp=NULL;
  int i=0;

  temp=(unsigned char *)malloc(0x4000);
  if (temp==NULL) return 1;
  memset(temp,0,0x4000);

  // Interleve each 16k block and shift down by 0x200:
  for (i=0; i+0x4200<=len; i+=0x4000)
  {
    InterleveBlock(temp,data+0x200+i); // Interleve 16k to temporary buffer
    memcpy(data+i,temp,0x4000); // Copy back in
  }

  free(temp);
  return 0;
}

static int PicoCartCalcAllocSize(int filesize, int is_sms)
{
  int alloc_size;
  int s = 0, tmp = filesize;

  // make size power of 2 for easier banking handling
  while ((tmp >>= 1) != 0)
    s++;
  if (filesize > (1 << s))
    s++;
  alloc_size = 1 << s;

  if (is_sms) {
    // be sure we can cover all address space
    if (alloc_size < 0x10000)
      alloc_size = 0x10000;
  }
  else {
    // align to 512K for memhandlers
    alloc_size = (alloc_size + 0x7ffff) & ~0x7ffff;
  }

  if (alloc_size - filesize < 4)
    alloc_size += 4; // padding for out-of-bound exec protection

  return alloc_size;
}

void *PicoCartAlloc(int filesize, int is_sms)
{
  unsigned char *rom;

  rom_alloc_size = PicoCartCalcAllocSize(filesize, is_sms);

  // Allocate space for the rom plus padding
  // use special address for 32x dynarec
  rom = plat_mmap(0x02000000, rom_alloc_size, 0, 0);
  return rom;
}

int PicoCartLoad(pm_file *f, const unsigned char *rom, unsigned int romsize,
  unsigned char **prom, unsigned int *psize, int is_sms)
{
  unsigned char *rom_data = NULL;
  int size, bytes_read;
#if defined(RENDER_GSKIT_PS2)
  int borrowed_alloc_size;
#endif

  if (!f && !rom)
    return 1;

  if (!rom)
    size = f->size;
  else
    size = romsize;

  if (size <= 0) return 1;
  size = (size+3)&~3; // Round up to a multiple of 4

  // Allocate space for the rom plus padding
#if defined(RENDER_GSKIT_PS2)
  borrowed_alloc_size = PicoCartCalcAllocSize(size, is_sms);
  ps2_rom_borrowed = 0;
  ps2_borrowed_capacity = 0;

  if (rom != NULL &&
      rom == ps2_external_rom &&
      borrowed_alloc_size > 0 &&
      (unsigned int)borrowed_alloc_size <= ps2_external_rom_capacity)
  {
    /* Borrow Aurora's already-resident mutable ROM buffer. PicoDrive
     * byteswaps in place, which is safe after Aurora's SetRom boundary.
     * Zero the rounded tail to match mmap's zero-filled padding and make
     * sure bytes from a previous cartridge cannot leak into banked reads. */
    rom_alloc_size = borrowed_alloc_size;
    rom_data = (unsigned char *)rom;
    if ((unsigned int)rom_alloc_size > romsize)
      memset(rom_data + romsize, 0,
        (unsigned int)rom_alloc_size - romsize);

    ps2_rom_borrowed = 1;
    ps2_borrowed_capacity = ps2_external_rom_capacity;
    elprintf(EL_STATUS, "PS2: borrowing frontend ROM buffer (%i bytes)",
      rom_alloc_size);
  }
  else
#endif
  {
    rom_data = PicoCartAlloc(size, is_sms);
    if (rom_data == NULL) {
      elprintf(EL_STATUS, "out of memory (wanted %i)", size);
      return 2;
    }
  }

  if (!rom) {
    if (PicoCartLoadProgressCB != NULL)
    {
      // read ROM in blocks, just for fun
      int ret;
      unsigned char *p = rom_data;
      bytes_read=0;
      do
      {
        int todo = size - bytes_read;
        if (todo > 256*1024) todo = 256*1024;
        ret = pm_read(p,todo,f);
        bytes_read += ret;
        p += ret;
        PicoCartLoadProgressCB(bytes_read * 100LL / size);
      }
      while (ret > 0);
    }
    else
      bytes_read = pm_read(rom_data,size,f); // Load up the rom

    if (bytes_read <= 0) {
      elprintf(EL_STATUS, "read failed");
      plat_munmap(rom_data, rom_alloc_size);
      rom_alloc_size = 0;
      return 3;
    }
  }
  else if (rom_data != rom)
    memcpy(rom_data, rom, romsize);

  if (!is_sms)
  {
    // Check for SMD:
    if (size >= 0x4200 && (size&0x3fff) == 0x200 &&
        ((rom_data[0x2280] == 'S' && rom_data[0x280] == 'E') || (rom_data[0x280] == 'S' && rom_data[0x2281] == 'E'))) {
      elprintf(EL_STATUS, "SMD format detected.");
      DecodeSmd(rom_data,size); size-=0x200; // Decode and byteswap SMD
    }
    else Byteswap(rom_data, rom_data, size); // Just byteswap
  }
  else
  {
    if (size >= 0x4200 && (size&0x3fff) == 0x200) {
      elprintf(EL_STATUS, "SMD format detected.");
      // at least here it's not interleaved
      size -= 0x200;
      memmove(rom_data, rom_data + 0x200, size);
    }
  }

  if (prom)  *prom = rom_data;
  if (psize) *psize = size;

  return 0;
}

// Insert a cartridge:
int PicoCartInsert(unsigned char *rom, unsigned int romsize, const char *carthw_cfg)
{
  // notaz: add a 68k "jump one op back" opcode to the end of ROM.
  // This will hang the emu, but will prevent nasty crashes.
  // note: 4 bytes are padded to every ROM
  if (rom != NULL)
    *(u32 *)(rom+romsize) = CPU_BE2(0x6000FFFE);

  Pico.rom=rom;
  Pico.romsize=romsize;

  if (Pico.sv.data) {
    free(Pico.sv.data);
    Pico.sv.data = NULL;
  }

  if (PicoCartUnloadHook != NULL) {
    PicoCartUnloadHook();
    PicoCartUnloadHook = NULL;
  }
  pdb_cleanup();

  PicoIn.AHW &= ~(PAHW_32X|PAHW_SVP);

  PicoCartMemSetup = NULL;
  PicoDmaHook = NULL;
  PicoResetHook = NULL;
  PicoLineHook = NULL;
  PicoLoadStateHook = NULL;
  carthw_chunks = NULL;

  if (!(PicoIn.AHW & (PAHW_SMS|PAHW_PICO)))
    PicoCartDetect(carthw_cfg);
  if (PicoIn.AHW & PAHW_SMS)
    PicoCartDetectMS();
  if (PicoIn.AHW & PAHW_SVP)
    PicoSVPStartup();
  if (PicoIn.AHW & PAHW_PICO)
    PicoInitPico();

  // setup correct memory map for loaded ROM
  switch (PicoIn.AHW & ~(PAHW_GG|PAHW_SG|PAHW_SC)) {
    default:
      elprintf(EL_STATUS|EL_ANOMALY, "starting in unknown hw configuration: %x", PicoIn.AHW);
    case 0:
    case PAHW_SVP:  PicoMemSetup(); break;
    case PAHW_MCD|PAHW_VGM:
    case PAHW_MCD:  PicoMemSetupCD(); break;
    case PAHW_PICO: PicoMemSetupPico(); break;
    case PAHW_SMS:  PicoMemSetupMS(); break;
  }

  if (PicoCartMemSetup != NULL)
    PicoCartMemSetup();

  if (PicoIn.AHW & PAHW_SMS)
    PicoPowerMS();
  else
    PicoPower();

  PicoGameLoaded = 1;
  return 0;
}

int PicoCartResize(int newsize)
{
#if defined(RENDER_GSKIT_PS2)
  if (ps2_rom_borrowed) {
    if (newsize < 0 || (unsigned int)newsize > ps2_borrowed_capacity)
      return -1;

    if (newsize > rom_alloc_size)
      memset(Pico.rom + rom_alloc_size, 0, newsize - rom_alloc_size);
    rom_alloc_size = newsize;
    return 0;
  }
#endif

  void *tmp = plat_mremap(Pico.rom, rom_alloc_size, newsize);
  if (tmp == NULL)
    return -1;

  Pico.rom = tmp;
  rom_alloc_size = newsize;
  return 0;
}

void PicoCartUnload(void)
{
  if (PicoCartUnloadHook != NULL) {
    PicoCartUnloadHook();
    PicoCartUnloadHook = NULL;
  }

  PicoUnload32x();

  if (Pico.rom != NULL) {
    SekFinishIdleDet();
#if defined(RENDER_GSKIT_PS2)
    if (!ps2_rom_borrowed)
#endif
      plat_munmap(Pico.rom, rom_alloc_size);

    rom_alloc_size = 0;
    Pico.rom = NULL;
    Pico.romsize = 0;
#if defined(RENDER_GSKIT_PS2)
    ps2_rom_borrowed = 0;
    ps2_borrowed_capacity = 0;
#endif
  }
  PicoGameLoaded = 0;
}

static unsigned int rom_crc32(int size)
{
  unsigned int crc;
  elprintf(EL_STATUS, "calculating CRC32..");
  if (size <= 0 || size > Pico.romsize) size = Pico.romsize;

  // have to unbyteswap for calculation..
  Byteswap(Pico.rom, Pico.rom, size);
  crc = crc32(0, Pico.rom, size);
  Byteswap(Pico.rom, Pico.rom, size);
  return crc;
}

int rom_strcmp(void *rom, int size, int offset, const char *s1)
{
  int i, len = strlen(s1);
  const char *s_rom = (const char *)rom;
  if (offset + len > size)
    return 1;

  if (PicoIn.AHW & PAHW_SMS)
    return strncmp(s_rom + offset, s1, strlen(s1));

  for (i = 0; i < len; i++)
    if (s1[i] != s_rom[MEM_BE2(i + offset)])
      return 1;
  return 0;
}

static unsigned int rom_read32(int addr)
{
  unsigned short *m = (unsigned short *)(Pico.rom + addr);
  return (m[0] << 16) | m[1];
}

static char *sskip(char *s)
{
  while (*s && isspace_(*s))
    s++;
  return s;
}

static void rstrip(char *s)
{
  char *p;
  for (p = s + strlen(s) - 1; p >= s; p--)
    if (isspace_(*p))
      *p = 0;
}

static int parse_3_vals(char *p, int *val0, int *val1, int *val2)
{
  char *r;
  *val0 = strtoul(p, &r, 0);
  if (r == p)
    goto bad;
  p = sskip(r);
  if (*p++ != ',')
    goto bad;
  *val1 = strtoul(p, &r, 0);
  if (r == p)
    goto bad;
  p = sskip(r);
  if (*p++ != ',')
    goto bad;
  *val2 = strtoul(p, &r, 0);
  if (r == p)
    goto bad;

  return 1;
bad:
  return 0;
}

static int is_expr(const char *expr, char **pr)
{
  int len = strlen(expr);
  char *p = *pr;

  if (strncmp(expr, p, len) != 0)
    return 0;
  p = sskip(p + len);
  if (*p != '=')
    return 0; // wrong or malformed

  *pr = sskip(p + 1);
  return 1;
}

#include "carthw_cfg.c"

static void parse_carthw(const char *carthw_cfg, int *fill_sram,
  int *hw_detected)
{
  int line = 0, any_checks_passed = 0, skip_sect = 0;
  const char *s, *builtin = builtin_carthw_cfg;
  int tmp, rom_crc = 0;
  char buff[256], *p, *r;
  FILE *f;

  f = fopen(carthw_cfg, "r");
  if (f == NULL)
    f = fopen("pico/carthw.cfg", "r");
  if (f == NULL)
    elprintf(EL_STATUS, "couldn't open carthw.cfg!");

  for (;;)
  {
    if (f != NULL) {
      p = fgets(buff, sizeof(buff), f);
      if (p == NULL)
        break;
    }
    else {
      if (*builtin == 0)
        break;
      for (s = builtin; *s != 0 && *s != '\n'; s++)
        ;
      while (*s == '\n')
        s++;
      tmp = s - builtin;
      if (tmp > sizeof(buff) - 1)
        tmp = sizeof(buff) - 1;
      memcpy(buff, builtin, tmp);
      buff[tmp] = 0;
      p = buff;
      builtin = s;
    }

    line++;
    p = sskip(p);
    if (*p == 0 || *p == '#')
      continue;

    if (*p == '[') {
      any_checks_passed = 0;
      skip_sect = 0;
      continue;
    }
    
    if (skip_sect)
      continue;

    /* look for checks */
    if (is_expr("check_str", &p))
    {
      int offs;
      offs = strtoul(p, &r, 0);
      if (offs < 0) {
        elprintf(EL_STATUS, "carthw:%d: check_str offs out of range: %d\n", line, offs);
	goto bad;
      }
      p = sskip(r);
      if (*p != ',')
        goto bad;
      p = sskip(p + 1);
      if (*p != '"')
        goto bad;
      p++;
      r = strchr(p, '"');
      if (r == NULL)
        goto bad;
      *r = 0;

      if (rom_strcmp(Pico.rom, Pico.romsize, offs, p) == 0)
        any_checks_passed = 1;
      else
        skip_sect = 1;
      continue;
    }
    else if (is_expr("check_size_gt", &p))
    {
      int size;
      size = strtoul(p, &r, 0);
      if (r == p || size < 0)
        goto bad;

      if (Pico.romsize > size)
        any_checks_passed = 1;
      else
        skip_sect = 1;
      continue;
    }
    else if (is_expr("check_csum", &p))
    {
      int csum;
      csum = strtoul(p, &r, 0);
      if (r == p || (csum & 0xffff0000))
        goto bad;

      if (csum == (rom_read32(0x18c) & 0xffff))
        any_checks_passed = 1;
      else
        skip_sect = 1;
      continue;
    }
    else if (is_expr("check_crc32", &p))
    {
      unsigned int crc;
      crc = strtoul(p, &r, 0);
      if (r == p)
        goto bad;

      if (rom_crc == 0)
        rom_crc = rom_crc32(64*1024);
      if (crc == rom_crc)
        any_checks_passed = 1;
      else
        skip_sect = 1;
      continue;
    }

    /* now time for actions */
    if (is_expr("hw", &p)) {
      if (!any_checks_passed)
        goto no_checks;
      *hw_detected = 1;
      rstrip(p);

      if      (strcmp(p, "svp") == 0)
        PicoIn.AHW = PAHW_SVP;
      else if (strcmp(p, "pico") == 0)
        PicoIn.AHW = PAHW_PICO;
      else if (strcmp(p, "j_cart") == 0)
        carthw_jcart_startup();
      else if (strcmp(p, "prot") == 0)
        carthw_sprot_startup();
      else if (strcmp(p, "flash") == 0)
        carthw_flash_startup();
      else if (strcmp(p, "ssf2_mapper") == 0)
        carthw_ssf2_startup();
      else if (strcmp(p, "x_in_1_mapper") == 0)
        carthw_Xin1_startup();
      else if (strcmp(p, "realtec_mapper") == 0)
        carthw_realtec_startup();
      else if (strcmp(p, "radica_mapper") == 0)
        carthw_radica_startup();
      else if (strcmp(p, "piersolar_mapper") == 0)
        carthw_pier_startup();
      else if (strcmp(p, "sf001_mapper") == 0)
        carthw_sf001_startup();
      else if (strcmp(p, "sf002_mapper") == 0)
        carthw_sf002_startup();
      else if (strcmp(p, "sf004_mapper") == 0)
        carthw_sf004_startup();
      else if (strcmp(p, "lk3_mapper") == 0)
        carthw_lk3_startup();
      else if (strcmp(p, "smw64_mapper") == 0)
        carthw_smw64_startup();
      else {
        elprintf(EL_STATUS, "carthw:%d: unsupported mapper: %s", line, p);
        skip_sect = 1;
        *hw_detected = 0;
      }
      continue;
    }
    if (is_expr("sram_range", &p)) {
      int start, end;

      if (!any_checks_passed)
        goto no_checks;
      rstrip(p);

      start = strtoul(p, &r, 0);
      if (r == p)
        goto bad;
      p = sskip(r);
      if (*p != ',')
        goto bad;
      p = sskip(p + 1);
      end = strtoul(p, &r, 0);
      if (r == p)
        goto bad;
      if (((start | end) & 0xff000000) || start > end) {
        elprintf(EL_STATUS, "carthw:%d: bad sram_range: %08x - %08x", line, start, end);
        goto bad_nomsg;
      }
      Pico.sv.start = start;
      Pico.sv.end = end;
      continue;
    }
    else if (is_expr("prop", &p)) {
      if (!any_checks_passed)
        goto no_checks;
      rstrip(p);

      if      (strcmp(p, "no_sram") == 0)
        Pico.sv.flags &= ~SRF_ENABLED;
      else if (strcmp(p, "no_eeprom") == 0)
        Pico.sv.flags &= ~SRF_EEPROM;
      else if (strcmp(p, "filled_sram") == 0)
        *fill_sram = 1;
      else if (strcmp(p, "wwfraw_hack") == 0)
        PicoIn.quirks |= PQUIRK_WWFRAW_HACK;
      else if (strcmp(p, "blackthorne_hack") == 0)
        PicoIn.quirks |= PQUIRK_BLACKTHORNE_HACK;
      else if (strcmp(p, "marscheck_hack") == 0)
        PicoIn.quirks |= PQUIRK_MARSCHECK_HACK;
      else if (strcmp(p, "force_6btn") == 0)
        PicoIn.quirks |= PQUIRK_FORCE_6BTN;
      else if (strcmp(p, "no_z80_bus_lock") == 0)
        PicoIn.quirks |= PQUIRK_NO_Z80_BUS_LOCK;
      else {
        elprintf(EL_STATUS, "carthw:%d: unsupported prop: %s", line, p);
        goto bad_nomsg;
      }
      elprintf(EL_STATUS, "game prop: %s", p);
      continue;
    }
    else if (is_expr("eeprom_type", &p)) {
      int type;
      if (!any_checks_passed)
        goto no_checks;
      rstrip(p);

      type = strtoul(p, &r, 0);
      if (r == p || type < 0)
        goto bad;
      Pico.sv.eeprom_type = type;
      Pico.sv.flags |= SRF_EEPROM;
      continue;
    }
    else if (is_expr("eeprom_lines", &p)) {
      int scl, sda_in, sda_out;
      if (!any_checks_passed)
        goto no_checks;
      rstrip(p);

      if (!parse_3_vals(p, &scl, &sda_in, &sda_out))
        goto bad;
      if (scl < 0 || scl > 15 || sda_in < 0 || sda_in > 15 ||
          sda_out < 0 || sda_out > 15)
        goto bad;

      Pico.sv.eeprom_bit_cl = scl;
      Pico.sv.eeprom_bit_in = sda_in;
      Pico.sv.eeprom_bit_out= sda_out;
      continue;
    }
    else if ((tmp = is_expr("prot_ro_value16", &p)) || is_expr("prot_rw_value16", &p)) {
      int addr, mask, val;
      if (!any_checks_passed)
        goto no_checks;
      rstrip(p);

      if (!parse_3_vals(p, &addr, &mask, &val))
        goto bad;

      carthw_sprot_new_location(addr, mask, val, tmp ? 1 : 0);
      continue;
    }


bad:
    elprintf(EL_STATUS, "carthw:%d: unrecognized expression: %s", line, buff);
bad_nomsg:
    skip_sect = 1;
    continue;

no_checks:
    elprintf(EL_STATUS, "carthw:%d: command without any checks before it: %s", line, buff);
    skip_sect = 1;
    continue;
  }

  if (f != NULL)
    fclose(f);
}

/*
 * various cart-specific things, which can't be handled by generic code
 */
static void PicoCartDetect(const char *carthw_cfg)
{
  int carthw_detected = 0;
  int fill_sram = 0;

  memset(&Pico.sv, 0, sizeof(Pico.sv));
  if (Pico.rom[MEM_BE2(0x1B0)] == 'R' && Pico.rom[MEM_BE2(0x1B1)] == 'A')
  {
    Pico.sv.start =  rom_read32(0x1B4) & ~0xff000001; // align
    Pico.sv.end   = (rom_read32(0x1B8) & ~0xff000000) | 1;
    if (Pico.rom[MEM_BE2(0x1B3)] & 0x40)
      // EEPROM
      Pico.sv.flags |= SRF_EEPROM;
    Pico.sv.flags |= SRF_ENABLED;
  }
  if (Pico.sv.end == 0 || Pico.sv.start > Pico.sv.end)
  {
    // some games may have bad headers, like S&K and Sonic3
    // note: majority games use 0x200000 as starting address, but there are some which
    // use something else (0x300000 by HardBall '95). Luckily they have good headers.
    Pico.sv.start = 0x200000;
    Pico.sv.end   = 0x203FFF;
    Pico.sv.flags |= SRF_ENABLED;
  }

  // set EEPROM defaults, in case it gets detected
  Pico.sv.eeprom_type   = 0; // 7bit (24C01)
  Pico.sv.eeprom_bit_cl = 1;
  Pico.sv.eeprom_bit_in = 0;
  Pico.sv.eeprom_bit_out= 0;

  if (carthw_cfg != NULL)
    parse_carthw(carthw_cfg, &fill_sram, &carthw_detected);

  // assume the standard mapper for large roms
  if (!carthw_detected && Pico.romsize > 0x400000)
    carthw_ssf2_startup();

  if (Pico.sv.flags & SRF_ENABLED)
  {
    if (Pico.sv.flags & SRF_EEPROM)
      Pico.sv.size = 0x2000;
    else
      Pico.sv.size = Pico.sv.end - Pico.sv.start + 1;

    Pico.sv.data = calloc(Pico.sv.size, 1);
    if (Pico.sv.data == NULL)
      Pico.sv.flags &= ~SRF_ENABLED;

    if (Pico.sv.eeprom_type == 1)	// 1 == 0 in PD EEPROM code
      Pico.sv.eeprom_type = 0;
  }

  if ((Pico.sv.flags & SRF_ENABLED) && fill_sram)
  {
    elprintf(EL_STATUS, "SRAM fill");
    memset(Pico.sv.data, 0xff, Pico.sv.size);
  }

  // tweak for Blackthorne: master SH2 overwrites stack of slave SH2 being in PWM
  // interrupt. On real hardware, nothing happens since slave fetches the values
  // it has written from its cache, but picodrive doesn't emulate caching.
  // move master memory area down by 0x100 bytes.
  // XXX replace this abominable hack. It might cause other problems in the game!
  if (PicoIn.quirks & PQUIRK_BLACKTHORNE_HACK) {
    int i;
    unsigned a = 0;
    for (i = 0; i < Pico.romsize; i += 4) {
      unsigned v = CPU_BE2(*(u32 *) (Pico.rom + i));
      if (a && v == a + 0x400) { // patch if 2 pointers with offset 0x400 are found
        elprintf(EL_STATUS, "auto-patching @%06x: %08x->%08x\n", i, v, v - 0x100);
        *(u32 *) (Pico.rom + i) = CPU_BE2(v - 0x100);
      }
      // detect a pointer into the incriminating area
      a = 0;
      if (v >> 12 == 0x0603f000 >> 12 && !(v & 3))
        a = v;
    }
  }

  // tweak for Mars Check Program: copies 32K longwords (128KB) from a 64KB buffer
  // in ROM or DRAM to SDRAM with DMA in 4-longword mode, overwriting an SDRAM comm
  // area in turn. This crashes the test on emulators without CPU cache emulation.
  // This may be a bug in Mars Check, since it's only checking for the 64KB result.
  // Patch the DMA transfers so that they transfer only 64KB.
  if (PicoIn.quirks & PQUIRK_MARSCHECK_HACK) {
    int i;
    unsigned a = 0;
    for (i = 0; i < Pico.romsize; i += 4) {
      unsigned v = CPU_BE2(*(u32 *) (Pico.rom + i));
      if (a == 0xffffff8c && v == 0x5ee1) { // patch if 4-long xfer written to CHCR
        elprintf(EL_STATUS, "auto-patching @%06x: %08x->%08x\n", i, v, v & ~0x800);
        *(u32 *) (Pico.rom + i) = CPU_BE2(v & ~0x800); // change to half-sized xfer
      }
      a = v;
    }
  }
}

static void PicoCartDetectMS(void)
{
  memset(&Pico.sv, 0, sizeof(Pico.sv));

  // Always map SRAM, since there's no indicator in ROM if it's needed or not
  // TODO: this should somehow be coming from a cart database!

  Pico.sv.size  = 0x8000; // Sega mapper, 2 banks of 16 KB each
  Pico.sv.flags |= SRF_ENABLED;
  Pico.sv.data = calloc(Pico.sv.size, 1);
  if (Pico.sv.data == NULL)
    Pico.sv.flags &= ~SRF_ENABLED;
}
// vim:shiftwidth=2:expandtab

/* AURORA_V4_4_BUILD_FIX_32X_VIDEO_FIRST_20260830 */

/* AURORA_V4_7_SEGACD_CDDA_WORKER_VFS_SEEK_FIX_20260830 */

/* AURORA_V4_9_SEGACD_CDDA_CHASE_REVIVE_20260830 */

/* AURORA_V4_10_SEGACD_PRIME_COMPILE_FIX_20260830 */

/* AURORA_V4_12_PRIVATE_FILEXIO_CDDA_PCE_TOC2CUE_20260830 */

/* AURORA_V4_12_1_RESUME_COMPILEFIX_20260830 */

/* AURORA_V4_13_CDDA_STARVATION_PCE_TRACK_GEOMETRY_20260830 */
