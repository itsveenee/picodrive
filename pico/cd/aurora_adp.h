/* AURORA_ADP_VN7: CDDA IMA ADPCM, formato próprio AURADP1.
 * Cada setor lógico CDDA é independente: 596 bytes -> 2352 bytes PCM.
 * 64-byte header + 596*N bytes; 588 pares stereo/sector; 44.1kHz.
 * Não é o formato Q2MU de Quake. Usar somente em tracks AUDIO.
 * Funções static: zero alocação para faixas tradicionais.
 */
#ifndef AURORA_CD_ADP1_H
#define AURORA_CD_ADP1_H
#include <stdint.h>
#include <string.h>
#define AURORA_ADP_HEADER 64u
#define AURORA_ADP_PACKED 596u
#define AURORA_ADP_PCM 2352u
static inline uint32_t aurora_adp_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int aurora_adp_parse(const uint8_t *h, uint64_t file_size,
                             uint32_t *sectors) {
  static const uint8_t magic[8] = { 'A','U','R','A','D','P','1',0 };
  uint32_t n;
  if (memcmp(h, magic, 8) || aurora_adp_le32(h+8)!=1 ||
      aurora_adp_le32(h+12)!=44100 || aurora_adp_le32(h+16)!=2 ||
      aurora_adp_le32(h+20)!=588 || aurora_adp_le32(h+24)!=596 ||
      aurora_adp_le32(h+32)!=0) return 0;
  n = aurora_adp_le32(h+28);
  if (n==0 || n>300000 || file_size!=64ULL+596ULL*(uint64_t)n) return 0;
  *sectors=n; return 1;
}
static inline int aurora_adp_step(int i) {
  static const int steps[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
    34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,
    544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
    1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,
    4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,
    11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767
  };
  return steps[i];
}
static inline int aurora_adp_sample(int code, int *predictor, int *index) {
  static const int itab[8] = {-1,-1,-1,-1,2,4,6,8};
  int step=aurora_adp_step(*index);
  int diff=step>>3;
  if(code&1) diff+=step>>2;
  if(code&2) diff+=step>>1;
  if(code&4) diff+=step;
  if(code&8) diff=-diff;
  *predictor+=diff;
  if(*predictor>32767) *predictor=32767;
  if(*predictor< -32768) *predictor=-32768;
  *index+=itab[code&7];
  if(*index<0) *index=0;
  if(*index>88) *index=88;
  return *predictor;
}
static inline void aurora_adp_decode(const uint8_t *src, uint8_t *dst) {
  int pred[2], idx[2], i, j;
  pred[0]=(int16_t)((uint16_t)src[0] | ((uint16_t)src[1]<<8));
  idx[0]=src[2];
  pred[1]=(int16_t)((uint16_t)src[4] | ((uint16_t)src[5]<<8));
  idx[1]=src[6];
  if(idx[0]>88 || idx[1]>88 || src[3]!=0 || src[7]!=0) {
    memset(dst,0,AURORA_ADP_PCM); return;
  }
  for(i=0;i<588;i++) {
    int codes=src[8+i], v;
    for(j=0;j<2;j++) {
      v=aurora_adp_sample(j ? (codes>>4) : (codes&15),&pred[j],&idx[j]);
      dst[4*i+j*2]=(uint8_t)v;
      dst[4*i+j*2+1]=(uint8_t)((uint16_t)v>>8);
    }
  }
}
#endif /* AURORA_CD_ADP1_H */
