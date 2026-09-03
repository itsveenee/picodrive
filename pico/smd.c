/* AURORA_SUPER_MAGIC_DRIVE_V1_20260902
 * Super Magic Drive / Magic Drive front copier for PicoDrive.
 *
 * SMD V3 is Z80 code running through MD Master-System compatibility mode.
 * The BIOS selects 16-KiB pages at $2000, machine mode at $2001, reads the
 * physical cart at $4000-$7fff, copier DRAM at $8000-$bfff and the floppy
 * controller through $200b-$200e.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico_int.h"
#include "memory.h"
#include "smd.h"

#define SMD_BIOS_BYTES       0x2000u
#define SMD_BRAM_BYTES       0x8000u
#define SMD_PAGE_BYTES       0x4000u
#define SMD_MAX_DRAM_BYTES   0x400000u
#define SMD_D88_TRACKS       160u
#define SMD_D88_TABLE        164u
#define SMD_D88_MAX_SECTORS  36u
#define SMD_D88_SHDR         16u
#define SMD_SECTOR_MAX       1024u

enum { SMD_FDC_COMMAND=0, SMD_FDC_READ, SMD_FDC_WRITE,
       SMD_FDC_RESULT, SMD_FDC_FORMAT };

typedef struct {
  unsigned int magic, version, total_bytes, dram_bytes, cart_bytes, run_bytes;
  unsigned char active, mode, pending_mode, page, ctrl2, ctrl3;
  unsigned char running_dram, dram_swapped;
  unsigned char dor, dcr, cylinder, head, drive, irq, last_st0;
  unsigned char reset_sense_pending;
  unsigned char fdc_phase, command[16], command_n, command_expected;
  unsigned char result[16], result_n, result_i;
  unsigned short sector_i, sector_bytes;
  unsigned char data_c, data_h, data_r, data_n, data_eot, data_mt;
  unsigned char format_ids[SMD_D88_MAX_SECTORS*4];
  unsigned short format_id_bytes, format_id_i;
  unsigned char format_sc, format_fill;
  unsigned char sector[SMD_SECTOR_MAX];
} SmdStateHeader;

typedef struct {
  int active;
  unsigned char mode, pending_mode, page, ctrl2, ctrl3;
  unsigned char bios_align_pad; /* AURORA_SMD_V1_7_RECOVERY_BOOT_20260903 */
  unsigned char bios[SMD_BIOS_BYTES];
  /* AURORA_SMD_V1_8_DYNAMIC_FETCH_WINDOWS_20260903 */
  unsigned short cart_fetch_words[SMD_PAGE_BYTES/2];
  unsigned short open_fetch_words[SMD_PAGE_BYTES/2];
  unsigned short mmio_fetch_words[0x2000/2];
  unsigned char bram[SMD_BRAM_BYTES];
  unsigned char *dram;
  unsigned int dram_bytes;
  unsigned char *cart;
  unsigned int cart_bytes;
  int running_dram, dram_swapped;
  unsigned int run_bytes;

  FILE *disk;
  unsigned char *d88;
  unsigned int d88_bytes;
  unsigned int track_off[SMD_D88_TABLE];
  unsigned int sec_off[SMD_D88_TRACKS][SMD_D88_MAX_SECTORS];
  unsigned short sec_bytes[SMD_D88_TRACKS][SMD_D88_MAX_SECTORS];
  unsigned char sec_n[SMD_D88_TRACKS][SMD_D88_MAX_SECTORS];
  unsigned char track_spt[SMD_D88_TRACKS];
  unsigned char track_dirty[SMD_D88_TRACKS];
  int disk_writable;
  char disk_path[1024];

  unsigned char dor, dcr, cylinder, head, drive, irq, last_st0;
  unsigned char reset_sense_pending;
  unsigned char fdc_phase, command[16], command_n, command_expected;
  unsigned char result[16], result_n, result_i;
  unsigned char sector[SMD_SECTOR_MAX];
  unsigned short sector_i, sector_bytes;
  unsigned char data_c, data_h, data_r, data_n, data_eot, data_mt;
  unsigned char format_ids[SMD_D88_MAX_SECTORS*4];
  unsigned short format_id_bytes, format_id_i;
  unsigned char format_sc, format_fill;
  char error[192];
} SmdDevice;

static SmdDevice smd;

static unsigned short g16(const unsigned char *p) {
  return (unsigned short)(p[0] | ((unsigned short)p[1] << 8));
}
static unsigned int g32(const unsigned char *p) {
  return (unsigned int)p[0] | ((unsigned int)p[1]<<8) |
         ((unsigned int)p[2]<<16) | ((unsigned int)p[3]<<24);
}
static void seterr(const char *s) {
  snprintf(smd.error,sizeof(smd.error),"%s",s?s:"unknown SMD error");
}
static unsigned int ssize(unsigned char n) {
  return n <= 3 ? (128u << n) : 0;
}
static unsigned int mirror(unsigned int size, unsigned int pos) {
  unsigned int mask;
  if (!size) return 0;
  if (pos < size) return pos;
  mask=0x80000000u; while (mask && !(pos&mask)) mask>>=1;
  if (!mask) return pos%size;
  if (size <= (pos&mask)) return mirror(size,pos-mask);
  return mask + mirror(size-mask,pos-mask);
}
static void wordswap(unsigned char *p,unsigned int n) {
  unsigned int i; n&=~1u;
  for(i=0;i<n;i+=2){ unsigned char t=p[i]; p[i]=p[i+1]; p[i+1]=t; }
}
static void dram68k(void) {
  if(smd.dram && !smd.dram_swapped){ wordswap(smd.dram,smd.run_bytes); smd.dram_swapped=1; }
}
static void dramz80(void) {
  if(smd.dram && smd.dram_swapped){ wordswap(smd.dram,smd.run_bytes); smd.dram_swapped=0; }
}

/* ---------------- D88 ---------------- */
static int fmtok(unsigned char n,unsigned char c) {
  if(n==2) return c==9||c==10||c==15||c==18||c==20;
  if(n==3) return c==5||c==8||c==9||c==10;
  return 0;
}
static void metaclear(void) {
  memset(smd.track_off,0,sizeof(smd.track_off));
  memset(smd.sec_off,0,sizeof(smd.sec_off));
  memset(smd.sec_bytes,0,sizeof(smd.sec_bytes));
  memset(smd.sec_n,0,sizeof(smd.sec_n));
  memset(smd.track_spt,0,sizeof(smd.track_spt));
  memset(smd.track_dirty,0,sizeof(smd.track_dirty));
}
static int probe(unsigned char *img,unsigned int bytes) {
  unsigned int first=0,tc=0,prev=0,i;
  if(!img||bytes<0x2a0u||g32(img+0x1c)!=bytes) return 0;
  if(img[0x1b]!=0x10&&img[0x1b]!=0x20) return 0;
  for(i=0;i<SMD_D88_TRACKS;i++){ unsigned int o=g32(img+0x20+i*4); if(o){first=o;break;} }
  if(first==0x2a0u) tc=160; else if(first==0x2b0u) tc=164; else return 0;
  metaclear();
  for(i=0;i<tc;i++){
    unsigned int o=g32(img+0x20+i*4);
    if(i>=SMD_D88_TRACKS){ if(o&&o!=bytes) return 0; continue; }
    if(!o||o<first||o>=bytes||(prev&&o<=prev)) return 0;
    smd.track_off[i]=o; prev=o;
  }
  for(i=0;i<SMD_D88_TRACKS;i++){
    unsigned int start=smd.track_off[i],end=(i+1<SMD_D88_TRACKS)?smd.track_off[i+1]:bytes,pos=start,k;
    unsigned short count; unsigned char seen[SMD_D88_MAX_SECTORS+1];
    if(!start||end<=start||start+SMD_D88_SHDR>end) return 0;
    count=g16(img+start+4); if(!count||count>SMD_D88_MAX_SECTORS) return 0;
    memset(seen,0,sizeof(seen));
    for(k=0;k<count;k++){
      unsigned char *sh; unsigned short dc,sb; unsigned char r,n;
      if(pos+SMD_D88_SHDR>end) return 0;
      sh=img+pos; dc=g16(sh+4); sb=g16(sh+14); r=sh[2]; n=sh[3];
      if(dc!=count||sh[0]!=(unsigned char)(i>>1)||sh[1]!=(unsigned char)(i&1)||
         !r||r>SMD_D88_MAX_SECTORS||seen[r]||(n!=2&&n!=3)||sb!=ssize(n)||
         pos+SMD_D88_SHDR+sb>end) return 0;
      seen[r]=1; smd.sec_off[i][r-1]=pos+SMD_D88_SHDR;
      smd.sec_bytes[i][r-1]=sb; smd.sec_n[i][r-1]=n;
      pos+=SMD_D88_SHDR+sb;
    }
    smd.track_spt[i]=(unsigned char)count;
  }
  return 1;
}
static int flushdisk(void) {
  unsigned int i;
  if(!smd.disk||!smd.d88) return 1;
  for(i=0;i<SMD_D88_TRACKS;i++) if(smd.track_dirty[i]){
    unsigned int a=smd.track_off[i],b=(i+1<SMD_D88_TRACKS)?smd.track_off[i+1]:smd.d88_bytes;
    if(!a||b<=a||b>smd.d88_bytes||fseek(smd.disk,(long)a,SEEK_SET)!=0||
       fwrite(smd.d88+a,1,b-a,smd.disk)!=b-a){ seterr("SMD D88 track flush failed"); return 0; }
  }
  if(fflush(smd.disk)!=0){ seterr("SMD D88 fflush failed"); return 0; }
  memset(smd.track_dirty,0,sizeof(smd.track_dirty)); return 1;
}
static void closedisk(void) {
  (void)flushdisk(); if(smd.disk)fclose(smd.disk); if(smd.d88)free(smd.d88);
  smd.disk=NULL; smd.d88=NULL; smd.d88_bytes=0; smd.disk_writable=0; smd.disk_path[0]=0; metaclear();
}
static int mountdisk(const char *path) {
  FILE *f; long n; unsigned char *img; int wr=1;
  if(!path||!*path){seterr("empty SMD D88 path");return 0;}
  f=fopen(path,"r+b"); if(!f){wr=0;f=fopen(path,"rb");}
  if(!f){seterr("cannot open SMD D88");return 0;}
  if(fseek(f,0,SEEK_END)!=0||(n=ftell(f))<0x2a0L||n>4L*1024L*1024L){fclose(f);seterr("invalid SMD D88 size");return 0;}
  img=(unsigned char*)malloc((size_t)n); if(!img){fclose(f);seterr("not enough EE memory for SMD D88 cache");return 0;}
  fseek(f,0,SEEK_SET); if(fread(img,1,(size_t)n,f)!=(size_t)n||!probe(img,(unsigned int)n)){free(img);fclose(f);seterr("unsupported/invalid SMD D88");return 0;}
  if(!flushdisk()){free(img);fclose(f);return 0;}
  if(smd.disk)fclose(smd.disk); if(smd.d88)free(smd.d88);
  smd.disk=f; smd.d88=img; smd.d88_bytes=(unsigned int)n; smd.disk_writable=wr&&!img[0x1a];
  snprintf(smd.disk_path,sizeof(smd.disk_path),"%s",path); memset(smd.track_dirty,0,sizeof(smd.track_dirty)); return 1;
}
static int findsec(unsigned char c,unsigned char h,unsigned char r,unsigned char n,unsigned int *off,unsigned int *bytes) {
  unsigned int ti=(unsigned int)c*2u+h,o; unsigned short b;
  if(!smd.disk||!smd.d88||ti>=SMD_D88_TRACKS||!r||r>SMD_D88_MAX_SECTORS)return 0;
  o=smd.sec_off[ti][r-1]; b=smd.sec_bytes[ti][r-1];
  if(!o||!b||smd.sec_n[ti][r-1]!=n||o+b>smd.d88_bytes)return 0;
  if(off)*off=o;if(bytes)*bytes=b;return 1;
}
static int formattrack(void) {
  unsigned int ti=(unsigned int)smd.cylinder*2u+smd.head,start,end,slot,pos=0,sb=ssize(smd.data_n),i;
  unsigned char *tmp,seen[SMD_D88_MAX_SECTORS+1];
  if(!smd.disk||!smd.d88||!smd.disk_writable||ti>=SMD_D88_TRACKS||!sb||
     !fmtok(smd.data_n,smd.format_sc)||smd.format_id_bytes!=(unsigned short)smd.format_sc*4u)return 0;
  start=smd.track_off[ti]; end=(ti+1<SMD_D88_TRACKS)?smd.track_off[ti+1]:smd.d88_bytes;
  if(!start||end<=start||end>smd.d88_bytes)return 0; slot=end-start;
  if((unsigned int)smd.format_sc*(SMD_D88_SHDR+sb)>slot){seterr("SMD D88 track slot too small");return 0;}
  memset(seen,0,sizeof(seen));
  for(i=0;i<smd.format_sc;i++){
    unsigned char c=smd.format_ids[i*4],h=smd.format_ids[i*4+1],r=smd.format_ids[i*4+2],n=smd.format_ids[i*4+3];
    if(c!=smd.cylinder||h!=smd.head||!r||r>SMD_D88_MAX_SECTORS||seen[r]||n!=smd.data_n)return 0; seen[r]=1;
  }
  tmp=(unsigned char*)calloc(1,slot); if(!tmp){seterr("not enough EE memory to format SMD D88 track");return 0;}
  memset(smd.sec_off[ti],0,sizeof(smd.sec_off[ti])); memset(smd.sec_bytes[ti],0,sizeof(smd.sec_bytes[ti])); memset(smd.sec_n[ti],0,sizeof(smd.sec_n[ti]));
  for(i=0;i<smd.format_sc;i++){
    unsigned char *sh=tmp+pos,r=smd.format_ids[i*4+2];
    sh[0]=smd.format_ids[i*4];sh[1]=smd.format_ids[i*4+1];sh[2]=r;sh[3]=smd.data_n;
    sh[4]=smd.format_sc;sh[5]=0;sh[13]=1;sh[14]=(unsigned char)(sb&0xff);sh[15]=(unsigned char)(sb>>8);
    memset(sh+SMD_D88_SHDR,smd.format_fill,sb);
    smd.sec_off[ti][r-1]=start+pos+SMD_D88_SHDR;smd.sec_bytes[ti][r-1]=(unsigned short)sb;smd.sec_n[ti][r-1]=smd.data_n;
    pos+=SMD_D88_SHDR+sb;
  }
  memcpy(smd.d88+start,tmp,slot);free(tmp);smd.track_spt[ti]=smd.format_sc;smd.track_dirty[ti]=1;return flushdisk();
}

/* ---------------- FDC ---------------- */
static int ready(unsigned char u){(void)u;return smd.disk&&(smd.dor&0x04)&&(smd.dor&0xf0);}
static void freset(int rising){
  smd.fdc_phase=SMD_FDC_COMMAND;smd.command_n=smd.command_expected=0;smd.result_n=smd.result_i=0;
  smd.sector_i=0;smd.format_id_bytes=smd.format_id_i=0;smd.cylinder=smd.head=smd.drive=0;smd.last_st0=0;
  smd.irq=rising?1:0;smd.reset_sense_pending=rising?4:0;
}
static unsigned char msr(void){
  if(!(smd.dor&0x04))return 0; switch(smd.fdc_phase){case SMD_FDC_READ:return 0xf0;case SMD_FDC_RESULT:return 0xd0;case SMD_FDC_WRITE:case SMD_FDC_FORMAT:return 0xb0;default:return smd.command_n?0x90:0x80;}
}
static unsigned char clen(unsigned char c){switch(c&0x1f){case 3:return 3;case 4:return 2;case 2:case 5:case 6:case 9:case 0x0c:return 9;case 7:return 2;case 8:return 1;case 0x0a:return 2;case 0x0d:return 6;case 0x0f:return 3;case 0x10:return 1;default:return 1;}}
static void result(const unsigned char*p,unsigned char n,int irq){if(n>sizeof(smd.result))n=sizeof(smd.result);if(n&&p)memcpy(smd.result,p,n);smd.result_n=n;smd.result_i=0;smd.fdc_phase=n?SMD_FDC_RESULT:SMD_FDC_COMMAND;smd.command_n=smd.command_expected=0;if(irq)smd.irq=1;}
static int loadsec(void){unsigned int o,b;if(!ready(smd.drive)||!findsec(smd.data_c,smd.data_h,smd.data_r,smd.data_n,&o,&b)||b>sizeof(smd.sector))return 0;memcpy(smd.sector,smd.d88+o,b);smd.sector_i=0;smd.sector_bytes=(unsigned short)b;return 1;}
static int storesec(void){unsigned int o,b,ti;if(!smd.disk_writable||!ready(smd.drive)||!findsec(smd.data_c,smd.data_h,smd.data_r,smd.data_n,&o,&b)||b!=smd.sector_bytes)return 0;memcpy(smd.d88+o,smd.sector,b);ti=(unsigned int)smd.data_c*2u+smd.data_h;if(ti>=SMD_D88_TRACKS)return 0;smd.track_dirty[ti]=1;return 1;}
static int advance(void){unsigned int o,b;if(smd.data_r<smd.data_eot&&smd.data_r<SMD_D88_MAX_SECTORS){unsigned char n=(unsigned char)(smd.data_r+1);if(findsec(smd.data_c,smd.data_h,n,smd.data_n,&o,&b)){smd.data_r=n;return 1;}}if(smd.data_mt&&smd.data_h==0&&findsec(smd.data_c,1,1,smd.data_n,&o,&b)){smd.data_h=smd.head=1;smd.data_r=1;return 1;}return 0;}
static void finish(int ok,unsigned char st1,unsigned char st2){unsigned char r[7];if(!flushdisk()){ok=0;if(!st1)st1=0x20;}r[0]=ok?(unsigned char)((smd.data_h<<2)|(smd.drive&3)):(unsigned char)(0x40|(smd.data_h<<2)|(smd.drive&3));r[1]=st1;r[2]=st2;r[3]=smd.data_c;r[4]=smd.data_h;r[5]=smd.data_r;r[6]=smd.data_n;smd.last_st0=r[0];result(r,7,1);}
static void execfdc(void){
  unsigned char cmd=smd.command[0]&0x1f,r[7];
  switch(cmd){
    case 3: result(NULL,0,0);return;
    case 4: smd.drive=smd.command[1]&3;smd.head=(smd.command[1]>>2)&1;r[0]=(unsigned char)(smd.drive|(smd.head<<2)|0x20);if(smd.cylinder==0)r[0]|=0x10;if(smd.disk&&!smd.disk_writable)r[0]|=0x40;result(r,1,0);return;
    case 2:case 5:case 6:case 9:case 0x0c:{
      unsigned int ti;smd.drive=smd.command[1]&3;smd.data_c=smd.command[2];smd.data_h=smd.command[3];smd.head=smd.data_h;smd.data_r=smd.command[4];smd.data_n=smd.command[5];smd.data_eot=smd.command[6];smd.data_mt=(smd.command[0]&0x80)?1:0;smd.cylinder=smd.data_c;
      if(cmd==2){ti=(unsigned int)smd.data_c*2u+smd.data_h;if(ti>=SMD_D88_TRACKS){finish(0,4,0);return;}smd.data_r=1;if(!smd.data_eot||smd.data_eot>smd.track_spt[ti])smd.data_eot=smd.track_spt[ti];smd.data_mt=0;}
      if(!ready(smd.drive)||!ssize(smd.data_n)||!findsec(smd.data_c,smd.data_h,smd.data_r,smd.data_n,NULL,NULL)){finish(0,4,0);return;}
      smd.sector_i=0;smd.sector_bytes=(unsigned short)ssize(smd.data_n);
      if(cmd==6||cmd==0x0c||cmd==2){if(!loadsec()){finish(0,0x20,0);return;}smd.fdc_phase=SMD_FDC_READ;}
      else{if(!smd.disk_writable){finish(0,2,0);return;}memset(smd.sector,0,smd.sector_bytes);smd.fdc_phase=SMD_FDC_WRITE;}
      smd.command_n=smd.command_expected=0;return;}
    case 7:smd.drive=smd.command[1]&3;smd.cylinder=0;smd.last_st0=(unsigned char)(0x20|smd.drive);smd.irq=1;result(NULL,0,0);return;
    case 8:if(smd.reset_sense_pending){unsigned char u=(unsigned char)(4-smd.reset_sense_pending);r[0]=(unsigned char)(0xc0|(u&3));r[1]=0;--smd.reset_sense_pending;smd.irq=smd.reset_sense_pending?1:0;result(r,2,0);return;}if(!smd.irq){r[0]=0x80;result(r,1,0);return;}r[0]=smd.last_st0;r[1]=smd.cylinder;result(r,2,0);return;
    case 0x0a:{unsigned int ti;smd.drive=smd.command[1]&3;smd.head=(smd.command[1]>>2)&1;ti=(unsigned int)smd.cylinder*2u+smd.head;if(!ready(smd.drive)||ti>=SMD_D88_TRACKS||!smd.track_spt[ti]||!smd.sec_off[ti][0]){smd.data_c=smd.cylinder;smd.data_h=smd.head;smd.data_r=1;smd.data_n=2;finish(0,4,0);return;}r[0]=(unsigned char)((smd.head<<2)|smd.drive);r[1]=r[2]=0;r[3]=smd.cylinder;r[4]=smd.head;r[5]=1;r[6]=smd.sec_n[ti][0];result(r,7,1);return;}
    case 0x0d:smd.drive=smd.command[1]&3;smd.head=(smd.command[1]>>2)&1;smd.data_n=smd.command[2];smd.format_sc=smd.command[3];smd.format_fill=smd.command[5];smd.format_id_bytes=(unsigned short)smd.format_sc*4u;if(smd.format_id_bytes>sizeof(smd.format_ids)||!fmtok(smd.data_n,smd.format_sc)||!ready(smd.drive)||!smd.disk_writable){smd.data_c=smd.cylinder;smd.data_h=smd.head;smd.data_r=1;finish(0,smd.disk_writable?4:2,0);return;}smd.format_id_i=0;smd.fdc_phase=SMD_FDC_FORMAT;smd.command_n=smd.command_expected=0;return;
    case 0x0f:smd.drive=smd.command[1]&3;smd.head=(smd.command[1]>>2)&1;smd.cylinder=smd.command[2];if(smd.cylinder>=80)smd.cylinder=79;smd.last_st0=(unsigned char)(0x20|(smd.head<<2)|smd.drive);smd.irq=1;result(NULL,0,0);return;
    case 0x10:r[0]=0x90;result(r,1,0);return;
    default:r[0]=0x80;result(r,1,0);return;
  }
}
static unsigned char fdc_read(void){
  if(!(smd.dor&4))return 0xff;
  if(smd.fdc_phase==SMD_FDC_RESULT){unsigned char v;if(smd.result_i==0&&!smd.reset_sense_pending)smd.irq=0;v=smd.result_i<smd.result_n?smd.result[smd.result_i++]:0xff;if(smd.result_i>=smd.result_n){smd.result_i=smd.result_n=0;smd.fdc_phase=SMD_FDC_COMMAND;}return v;}
  if(smd.fdc_phase==SMD_FDC_READ){unsigned char v=smd.sector[smd.sector_i++];if(smd.sector_i>=smd.sector_bytes){if(advance()){if(!loadsec())finish(0,0x20,0);}else finish(1,0,0);}return v;}return 0xff;
}
static void fdc_write(unsigned char d){
  if(!(smd.dor&4))return;
  if(smd.fdc_phase==SMD_FDC_WRITE){if(smd.sector_i<sizeof(smd.sector))smd.sector[smd.sector_i++]=d;if(smd.sector_i>=smd.sector_bytes){if(!storesec()){finish(0,smd.disk_writable?0x20:2,0);return;}if(advance()){smd.sector_i=0;memset(smd.sector,0,smd.sector_bytes);}else finish(1,0,0);}return;}
  if(smd.fdc_phase==SMD_FDC_FORMAT){if(smd.format_id_i<smd.format_id_bytes)smd.format_ids[smd.format_id_i++]=d;if(smd.format_id_i>=smd.format_id_bytes){int ok=formattrack();smd.data_c=smd.cylinder;smd.data_h=smd.head;smd.data_r=1;finish(ok,ok?0:0x20,0);}return;}
  if(smd.fdc_phase!=SMD_FDC_COMMAND)return;if(smd.command_n==0){smd.command_expected=clen(d);if(!smd.command_expected)smd.command_expected=1;}if(smd.command_n<sizeof(smd.command))smd.command[smd.command_n++]=d;if(smd.command_n>=smd.command_expected)execfdc();
}

/* ---------------- Z80 bus ---------------- */
static unsigned char busread(unsigned short a){
  unsigned int off;
  if(!smd.active)return 0xff;
  if(smd.mode==1&&a<0xc000){off=a;return smd.dram&&off<smd.dram_bytes?smd.dram[off]:0xff;}
  if(a<0x2000)return smd.bios[a];
  if(a>=0x2000&&a<0x4000){switch(a&0x200f){case 0x2000:return smd.page;case 0x2001:return smd.mode;case 0x2002:return smd.ctrl2;case 0x2003:return smd.ctrl3;case 0x2009:return (unsigned char)(0x1f|(smd.irq?0x80:0)|(smd.cart?0x40:0)|(smd.disk?0:0x20));case 0x200c:return fdc_read();case 0x200d:return msr();case 0x200e:return smd.dcr;default:return 0xff;}}
  if(a>=0x4000&&a<0x8000){if(smd.mode==4)return smd.bram[a-0x4000];if(!smd.cart||!smd.cart_bytes)return 0xff;off=(unsigned int)smd.page*SMD_PAGE_BYTES+(a-0x4000);off=mirror(smd.cart_bytes,off);return smd.cart[MEM_BE2(off)];}
  if(a>=0x8000&&a<0xc000){if(smd.mode==4)return smd.bram[0x4000+(a-0x8000)];if(!smd.dram||!smd.dram_bytes||smd.dram_swapped)return 0xff;off=((unsigned int)smd.page*SMD_PAGE_BYTES+(a-0x8000))%smd.dram_bytes;return smd.dram[off];}
  if(a>=0xc000)return PicoMem.zram[(a-0xc000)&0x1fff];return 0xff;
}
static void buswrite(unsigned int a,unsigned char d);
static void smd_refresh_firmware_windows(void);

static void smd_build_cart_fetch(void){
  unsigned char *dst=(unsigned char *)smd.cart_fetch_words;
  unsigned int i,base=(unsigned int)smd.page*SMD_PAGE_BYTES;
  if(!smd.cart||!smd.cart_bytes){memset(dst,0xff,SMD_PAGE_BYTES);return;}
  for(i=0;i<SMD_PAGE_BYTES;i++){
    unsigned int off=mirror(smd.cart_bytes,base+i);
    dst[i]=smd.cart[MEM_BE2(off)];
  }
}

static void smd_refresh_firmware_windows(void){
  unsigned int off;
  if(!smd.active||smd.mode==1)return;
  if(smd.mode==4){
    z80_map_set(z80_read_map,0x4000,0x7fff,smd.bram,0);
    z80_map_set(z80_write_map,0x4000,0x7fff,buswrite,1);
    z80_map_set(z80_read_map,0x8000,0xbfff,smd.bram+0x4000,0);
    z80_map_set(z80_write_map,0x8000,0xbfff,buswrite,1);
    return;
  }
  smd_build_cart_fetch();
  z80_map_set(z80_read_map,0x4000,0x7fff,smd.cart_fetch_words,0);
  z80_map_set(z80_write_map,0x4000,0x7fff,buswrite,1);
  if(smd.dram&&smd.dram_bytes&&!smd.dram_swapped){
    off=((unsigned int)smd.page*SMD_PAGE_BYTES)%smd.dram_bytes;
    z80_map_set(z80_read_map,0x8000,0xbfff,smd.dram+off,0);
  }else{
    z80_map_set(z80_read_map,0x8000,0xbfff,smd.open_fetch_words,0);
  }
  z80_map_set(z80_write_map,0x8000,0xbfff,buswrite,1);
}

static void buswrite(unsigned int a,unsigned char d){
  unsigned int off;a&=0xffff;if(!smd.active)return;
  if(smd.mode==1&&a<0xc000){if(smd.dram&&a<smd.dram_bytes)smd.dram[a]=d;return;}
  if(a>=0x2000&&a<0x4000){switch(a&0x200f){
    case 0x2000:if(smd.page!=d){smd.page=d;smd_refresh_firmware_windows();}return;
    case 0x2001:if(d<=4){if(d==1||d==2||d==3)smd.pending_mode=d;else{smd.mode=d;smd.pending_mode=0xff;smd_refresh_firmware_windows();}}return;
    case 0x2002:smd.ctrl2=d;return;case 0x2003:smd.ctrl3=d;return;
    case 0x200b:{unsigned char old=smd.dor;smd.dor=d;if(!(d&4))freset(0);else if(!(old&4))freset(1);return;}
    case 0x200c:fdc_write(d);return;case 0x200e:smd.dcr=d;return;default:return;}}
  if(a>=0x4000&&a<0x8000){if(smd.mode==4)smd.bram[a-0x4000]=d;return;}
  if(a>=0x8000&&a<0xc000){if(smd.mode==4){smd.bram[0x4000+(a-0x8000)]=d;return;}if(smd.dram&&smd.dram_bytes&&!smd.dram_swapped){off=((unsigned int)smd.page*SMD_PAGE_BYTES+(a-0x8000))%smd.dram_bytes;smd.dram[off]=d;}return;}
  if(a>=0xc000)PicoMem.zram[(a-0xc000)&0x1fff]=d;
}

void PicoDriveAurora_SmdMemSetupMS(void){
  if(!smd.active){PicoMemSetupMS();return;}
  /* AURORA_SMD_V1_8_DYNAMIC_FETCH_WINDOWS_20260903
   * First install canonical SMS RAM/I/O callbacks, then overlay SMD. Direct
   * maps update both data maps and CZ80 Fetch[]; callbacks remain for MMIO. */
  PicoMemSetupMS();
  if(smd.mode==1){
    if(smd.dram&&smd.dram_bytes>=0xc000u){z80_map_set(z80_read_map,0x0000,0xbfff,smd.dram,0);z80_map_set(z80_write_map,0x0000,0xbfff,smd.dram,0);}
    else{
      z80_map_set(z80_read_map,0x0000,0x3fff,smd.open_fetch_words,0);
      z80_map_set(z80_read_map,0x4000,0x7fff,smd.open_fetch_words,0);
      z80_map_set(z80_read_map,0x8000,0xbfff,smd.open_fetch_words,0);
      z80_map_set(z80_write_map,0x0000,0xbfff,buswrite,1);
    }
    return;
  }
  z80_map_set(z80_read_map,0x0000,0x1fff,smd.bios,0);
  z80_map_set(z80_write_map,0x0000,0x1fff,buswrite,1);
  z80_map_set(z80_read_map,0x2000,0x3fff,busread,1);
  z80_map_set(z80_write_map,0x2000,0x3fff,buswrite,1);
#ifdef _USE_CZ80
  /* Opcode fetch cannot call busread; never leave the old bootstrap Fetch[]. */
  Cz80_Set_Fetch(&CZ80,0x2000,0x3fff,(FPTR)smd.mmio_fetch_words);
#endif
  smd_refresh_firmware_windows();
}

/* ---------------- lifecycle / modes ---------------- */
static void enterbios(void){
  dramz80();smd.running_dram=0;smd.mode=0;smd.pending_mode=0xff;smd.page=0;
  Pico.rom=smd.cart;Pico.romsize=smd.cart_bytes;
  PicoIn.AHW&=~(PAHW_32X|PAHW_PICO|PAHW_MCD|PAHW_SVP|PAHW_GG|PAHW_SG|PAHW_SC);PicoIn.AHW|=PAHW_SMS;
  PicoPowerMS();
  /* PicoPowerMS resets CZ80 before PicoResetMS finishes the SMD overlay. */
  PicoDriveAurora_SmdMemSetupMS();
  z80_reset();
  PicoLoopPrepare(); /* AURORA_SMD_V1_8_DYNAMIC_FETCH_WINDOWS_20260903 */
}
static void entercart(void){if(!smd.cart||!smd.cart_bytes){seterr("SMD mode 2 requested without cartridge");enterbios();return;}smd.mode=2;smd.pending_mode=0xff;smd.running_dram=0;PicoIn.AHW=0;PicoCartInsert(smd.cart,smd.cart_bytes,NULL);PicoLoopPrepare(); /* AURORA_SMD_V1_8_DYNAMIC_FETCH_WINDOWS_20260903 */}
static void enterdram68k(void){unsigned int blocks,bytes;if(!smd.dram||!smd.dram_bytes){seterr("SMD mode 3 requested without DRAM");enterbios();return;}blocks=PicoMem.zram[0x1c00];bytes=blocks?blocks*SMD_PAGE_BYTES:smd.dram_bytes;if(!bytes||bytes>smd.dram_bytes)bytes=smd.dram_bytes;bytes&=~1u;smd.run_bytes=bytes;dram68k();smd.mode=3;smd.pending_mode=0xff;smd.running_dram=1;PicoIn.AHW=0;PicoCartInsert(smd.dram,smd.run_bytes,NULL);PicoLoopPrepare(); /* AURORA_SMD_V1_8_DYNAMIC_FETCH_WINDOWS_20260903 */}
static void enterdramz80(void){dramz80();smd.mode=1;smd.pending_mode=0xff;PicoIn.AHW&=~(PAHW_32X|PAHW_PICO|PAHW_MCD|PAHW_SVP|PAHW_GG|PAHW_SG|PAHW_SC);PicoIn.AHW|=PAHW_SMS;PicoDriveAurora_SmdMemSetupMS();z80_reset();PicoLoopPrepare(); /* AURORA_SMD_V1_8_DYNAMIC_FETCH_WINDOWS_20260903 */}
void PicoDriveAurora_SmdPostFrame(void){unsigned char m;if(!smd.active||smd.pending_mode==0xff)return;m=smd.pending_mode;smd.pending_mode=0xff;if(m==1)enterdramz80();else if(m==2)entercart();else if(m==3)enterdram68k();}
int PicoDriveAurora_SmdStart(const unsigned char*bios,unsigned int bb,unsigned int db,const char*dp){
  unsigned char keep[SMD_BRAM_BYTES];if(!bios||bb!=SMD_BIOS_BYTES)return 0;memcpy(keep,smd.bram,sizeof(keep));PicoDriveAurora_SmdShutdown();memset(&smd,0,sizeof(smd));memcpy(smd.bram,keep,sizeof(keep));memcpy(smd.bios,bios,SMD_BIOS_BYTES);memset(smd.cart_fetch_words,0xff,sizeof(smd.cart_fetch_words));memset(smd.open_fetch_words,0xff,sizeof(smd.open_fetch_words));memset(smd.mmio_fetch_words,0xff,sizeof(smd.mmio_fetch_words));
  if(!db||db>SMD_MAX_DRAM_BYTES)db=SMD_MAX_DRAM_BYTES;db=(db+SMD_PAGE_BYTES-1)&~(SMD_PAGE_BYTES-1);smd.dram=(unsigned char*)calloc(1,db);if(!smd.dram){seterr("not enough EE memory for SMD DRAM");return 0;}
  smd.dram_bytes=db;smd.active=1;smd.pending_mode=0xff;freset(0);if(dp&&*dp&&!mountdisk(dp)){PicoDriveAurora_SmdShutdown();return 0;}enterbios();return 1;
}
void PicoDriveAurora_SmdPrepareUnload(void){if(!smd.active)return;dramz80();if(smd.cart){Pico.rom=smd.cart;Pico.romsize=smd.cart_bytes;}else{Pico.rom=NULL;Pico.romsize=0;}(void)flushdisk();}
void PicoDriveAurora_SmdShutdown(void){if(smd.disk||smd.d88)closedisk();if(smd.dram)free(smd.dram);smd.dram=NULL;smd.dram_bytes=0;smd.cart=NULL;smd.cart_bytes=0;smd.active=0;smd.mode=0;smd.pending_mode=0xff;smd.running_dram=smd.dram_swapped=0;}
int PicoDriveAurora_SmdInsertCartridge(const unsigned char*rom,unsigned int rb){
  unsigned char*loaded=NULL;unsigned int lb=0;if(!smd.active||!rom||!rb||smd.mode!=0)return 0;PicoCartUnload();
  if(PicoCartLoad(NULL,rom,rb,&loaded,&lb,0)!=0||!loaded||!lb){seterr("PicoCartLoad failed while inserting SMD cartridge");smd.cart=NULL;smd.cart_bytes=0;Pico.rom=NULL;Pico.romsize=0;enterbios();return 0;}
  smd.cart=loaded;smd.cart_bytes=lb;Pico.rom=loaded;Pico.romsize=lb;enterbios();return 1;
}
void PicoDriveAurora_SmdEjectCartridge(void){if(!smd.active||smd.mode!=0)return;PicoCartUnload();smd.cart=NULL;smd.cart_bytes=0;Pico.rom=NULL;Pico.romsize=0;enterbios();}
int PicoDriveAurora_SmdSwapDisk(const char*p){return smd.active&&p&&*p?mountdisk(p):0;}
void PicoDriveAurora_SmdPowerCycle(void){if(smd.active)enterbios();}
int PicoDriveAurora_SmdIsActive(void){return smd.active?1:0;}
int PicoDriveAurora_SmdIsFirmwareMode(void){return smd.active&&(smd.mode==0||smd.mode==4);}
int PicoDriveAurora_SmdHasCartridge(void){return smd.cart?1:0;}
int PicoDriveAurora_SmdHasDisk(void){return smd.disk?1:0;}
const char*PicoDriveAurora_SmdDiskPath(void){return smd.disk_path;}
const char*PicoDriveAurora_SmdLastError(void){return smd.error;}
unsigned char*PicoDriveAurora_SmdBatteryRam(void){return smd.bram;}
unsigned int PicoDriveAurora_SmdBatteryRamBytes(void){return smd.active?SMD_BRAM_BYTES:0;}

/* ---------------- private state ---------------- */
unsigned int PicoDriveAurora_SmdStateSize(void){return smd.active&&smd.dram&&smd.dram_bytes?(unsigned int)sizeof(SmdStateHeader)+SMD_BRAM_BYTES+smd.dram_bytes:0;}
static void fillhdr(SmdStateHeader*h){
  memset(h,0,sizeof(*h));h->magic=0x31444d53u;h->version=1;h->total_bytes=PicoDriveAurora_SmdStateSize();h->dram_bytes=smd.dram_bytes;h->cart_bytes=smd.cart_bytes;h->run_bytes=smd.run_bytes;
  h->active=(unsigned char)smd.active;h->mode=smd.mode;h->pending_mode=smd.pending_mode;h->page=smd.page;h->ctrl2=smd.ctrl2;h->ctrl3=smd.ctrl3;h->running_dram=(unsigned char)smd.running_dram;h->dram_swapped=(unsigned char)smd.dram_swapped;
  h->dor=smd.dor;h->dcr=smd.dcr;h->cylinder=smd.cylinder;h->head=smd.head;h->drive=smd.drive;h->irq=smd.irq;h->last_st0=smd.last_st0;h->reset_sense_pending=smd.reset_sense_pending;
  h->fdc_phase=smd.fdc_phase;memcpy(h->command,smd.command,sizeof(h->command));h->command_n=smd.command_n;h->command_expected=smd.command_expected;memcpy(h->result,smd.result,sizeof(h->result));h->result_n=smd.result_n;h->result_i=smd.result_i;
  h->sector_i=smd.sector_i;h->sector_bytes=smd.sector_bytes;h->data_c=smd.data_c;h->data_h=smd.data_h;h->data_r=smd.data_r;h->data_n=smd.data_n;h->data_eot=smd.data_eot;h->data_mt=smd.data_mt;
  memcpy(h->format_ids,smd.format_ids,sizeof(h->format_ids));h->format_id_bytes=smd.format_id_bytes;h->format_id_i=smd.format_id_i;h->format_sc=smd.format_sc;h->format_fill=smd.format_fill;memcpy(h->sector,smd.sector,sizeof(h->sector));
}
int PicoDriveAurora_SmdSaveState(void*dst,unsigned int bytes){SmdStateHeader h;unsigned char*p=(unsigned char*)dst;unsigned int need=PicoDriveAurora_SmdStateSize();if(!dst||!need||bytes<need)return 0;fillhdr(&h);memcpy(p,&h,sizeof(h));p+=sizeof(h);memcpy(p,smd.bram,SMD_BRAM_BYTES);p+=SMD_BRAM_BYTES;memcpy(p,smd.dram,smd.dram_bytes);return (int)need;}
static int readhdr(const void*src,unsigned int bytes,SmdStateHeader*h){if(!src||!h||bytes<sizeof(*h))return 0;memcpy(h,src,sizeof(*h));if(h->magic!=0x31444d53u||h->version!=1||h->total_bytes!=bytes||h->dram_bytes!=smd.dram_bytes||bytes!=sizeof(*h)+SMD_BRAM_BYTES+h->dram_bytes)return 0;return 1;}
int PicoDriveAurora_SmdPrepareLoadState(const void*src,unsigned int bytes){SmdStateHeader h;if(!smd.active||!readhdr(src,bytes,&h))return 0;if(h.mode==3||h.running_dram){Pico.rom=smd.dram;Pico.romsize=h.run_bytes&&h.run_bytes<=smd.dram_bytes?h.run_bytes:smd.dram_bytes;}else{Pico.rom=smd.cart;Pico.romsize=smd.cart_bytes;}return 1;}
int PicoDriveAurora_SmdLoadState(const void*src,unsigned int bytes){
  SmdStateHeader h;const unsigned char*p=(const unsigned char*)src;if(!smd.active||!readhdr(src,bytes,&h))return 0;p+=sizeof(h);memcpy(smd.bram,p,SMD_BRAM_BYTES);p+=SMD_BRAM_BYTES;memcpy(smd.dram,p,smd.dram_bytes);
  smd.mode=h.mode;smd.pending_mode=h.pending_mode;smd.page=h.page;smd.ctrl2=h.ctrl2;smd.ctrl3=h.ctrl3;smd.running_dram=h.running_dram;smd.dram_swapped=h.dram_swapped;smd.run_bytes=h.run_bytes;
  smd.dor=h.dor;smd.dcr=h.dcr;smd.cylinder=h.cylinder;smd.head=h.head;smd.drive=h.drive;smd.irq=h.irq;smd.last_st0=h.last_st0;smd.reset_sense_pending=h.reset_sense_pending;
  smd.fdc_phase=h.fdc_phase;memcpy(smd.command,h.command,sizeof(smd.command));smd.command_n=h.command_n;smd.command_expected=h.command_expected;memcpy(smd.result,h.result,sizeof(smd.result));smd.result_n=h.result_n;smd.result_i=h.result_i;
  smd.sector_i=h.sector_i;smd.sector_bytes=h.sector_bytes;smd.data_c=h.data_c;smd.data_h=h.data_h;smd.data_r=h.data_r;smd.data_n=h.data_n;smd.data_eot=h.data_eot;smd.data_mt=h.data_mt;
  memcpy(smd.format_ids,h.format_ids,sizeof(smd.format_ids));smd.format_id_bytes=h.format_id_bytes;smd.format_id_i=h.format_id_i;smd.format_sc=h.format_sc;smd.format_fill=h.format_fill;memcpy(smd.sector,h.sector,sizeof(smd.sector));if(smd.mode==0||smd.mode==1||smd.mode==4){
#ifdef _USE_CZ80
  unsigned int pc=Cz80_Get_Reg(&CZ80,CZ80_PC);
#endif
  PicoDriveAurora_SmdMemSetupMS();
#ifdef _USE_CZ80
  Cz80_Set_Reg(&CZ80,CZ80_PC,pc);
#endif
  PicoLoopPrepare();
}return 1;
}
