#ifndef PICO_AURORA_SMD_H
#define PICO_AURORA_SMD_H

/* AURORA_SUPER_MAGIC_DRIVE_V1_20260902 */
#ifdef __cplusplus
extern "C" {
#endif

int PicoDriveAurora_SmdStart(const unsigned char *bios, unsigned int bios_bytes,
                             unsigned int dram_bytes, const char *disk_path);
void PicoDriveAurora_SmdPrepareUnload(void);
void PicoDriveAurora_SmdShutdown(void);
int PicoDriveAurora_SmdIsActive(void);
int PicoDriveAurora_SmdIsFirmwareMode(void);
int PicoDriveAurora_SmdHasCartridge(void);
int PicoDriveAurora_SmdHasDisk(void);
const char *PicoDriveAurora_SmdDiskPath(void);
const char *PicoDriveAurora_SmdLastError(void);
int PicoDriveAurora_SmdInsertCartridge(const unsigned char *rom,
                                       unsigned int rom_bytes);
void PicoDriveAurora_SmdEjectCartridge(void);
int PicoDriveAurora_SmdSwapDisk(const char *path);
void PicoDriveAurora_SmdPowerCycle(void);
void PicoDriveAurora_SmdPostFrame(void);
void PicoDriveAurora_SmdMemSetupMS(void);
unsigned int PicoDriveAurora_SmdStateSize(void);
int PicoDriveAurora_SmdSaveState(void *dst, unsigned int bytes);
int PicoDriveAurora_SmdPrepareLoadState(const void *src, unsigned int bytes);
int PicoDriveAurora_SmdLoadState(const void *src, unsigned int bytes);
unsigned char *PicoDriveAurora_SmdBatteryRam(void);
unsigned int PicoDriveAurora_SmdBatteryRamBytes(void);

#ifdef __cplusplus
}
#endif
#endif
