/* AURORA_PD_R5900_GOOD_ACCURATE_V6_20260822
 *
 * Small, deliberately isolated subset of PicoDrive's existing
 * platform/libretro/ps2/Draw_mips_r5900.s.
 *
 * We do NOT link the full legacy R5900 draw object because it exports old
 * DrawLayer/DrawWindow/sprite symbols that would replace or collide with the
 * current C renderer. These three routines are leaf transformations only:
 *   - palette-index line -> RGB555 line;
 *   - palette-index line -> RGB555 line (6-bit indices);
 *   - in-place byte OR/copy used by Good's Sonic palette mode.
 *
 * The instruction sequences are the same R5900 algorithms already carried by
 * PicoDrive; the symbols are renamed so they cannot alter unrelated paths.
 */

#if !defined(PS2) || !defined(_EE)
#error "aurora_r5900_finalize.c is PS2 EE-only"
#endif

__asm__(
".set noreorder\n"
".set noat\n"
".text\n"
".align 4\n"

".global aurora_r5900_clut\n"
".ent aurora_r5900_clut\n"
"aurora_r5900_clut:\n"
"1:\n"
"    lbu     $t0, 0($a1)\n"
"    lbu     $t1, 1($a1)\n"
"    lbu     $t2, 2($a1)\n"
"    lbu     $t3, 3($a1)\n"
"    sll     $t0, $t0, 1\n"
"    sll     $t1, $t1, 1\n"
"    sll     $t2, $t2, 1\n"
"    sll     $t3, $t3, 1\n"
"    addu    $t0, $t0, $a2\n"
"    addu    $t1, $t1, $a2\n"
"    addu    $t2, $t2, $a2\n"
"    addu    $t3, $t3, $a2\n"
"    lhu     $t0, 0($t0)\n"
"    lhu     $t1, 0($t1)\n"
"    lhu     $t2, 0($t2)\n"
"    lhu     $t3, 0($t3)\n"
"    sll     $t1, $t1, 16\n"
"    or      $t0, $t0, $t1\n"
"    sll     $t3, $t3, 16\n"
"    or      $t2, $t2, $t3\n"
"    sw      $t0, 0($a0)\n"
"    sw      $t2, 4($a0)\n"
"    addiu   $a0, $a0, 8\n"
"    addiu   $a3, $a3, -4\n"
"    bnez    $a3, 1b\n"
"    addiu   $a1, $a1, 4\n"
"    jr      $ra\n"
"    nop\n"
".end aurora_r5900_clut\n"

".align 4\n"
".global aurora_r5900_clut_6bit\n"
".ent aurora_r5900_clut_6bit\n"
"aurora_r5900_clut_6bit:\n"
"2:\n"
"    lw      $t0, 0($a1)\n"
"    # AURORA_V8_4_2_R5900_ASM_ABI_20260823\n"
"    # Current ps2dev EE ABI names these scratch GPRs a4..a7.\n"
"    sll     $a4, $t0, 1\n"
"    andi    $a4, $a4, 0x7e\n"
"    srl     $a5, $t0, 7\n"
"    andi    $a5, $a5, 0x7e\n"
"    srl     $a6, $t0, 15\n"
"    andi    $a6, $a6, 0x7e\n"
"    srl     $a7, $t0, 23\n"
"    andi    $a7, $a7, 0x7e\n"
"    addu    $t0, $a4, $a2\n"
"    addu    $t1, $a5, $a2\n"
"    addu    $t2, $a6, $a2\n"
"    addu    $t3, $a7, $a2\n"
"    lhu     $t0, 0($t0)\n"
"    lhu     $t1, 0($t1)\n"
"    lhu     $t2, 0($t2)\n"
"    lhu     $t3, 0($t3)\n"
"    sll     $t1, $t1, 16\n"
"    or      $t0, $t0, $t1\n"
"    sll     $t3, $t3, 16\n"
"    or      $t2, $t2, $t3\n"
"    sw      $t0, 0($a0)\n"
"    sw      $t2, 4($a0)\n"
"    addiu   $a0, $a0, 8\n"
"    addiu   $a3, $a3, -4\n"
"    bnez    $a3, 2b\n"
"    addiu   $a1, $a1, 4\n"
"    jr      $ra\n"
"    nop\n"
".end aurora_r5900_clut_6bit\n"

".align 4\n"
".global aurora_r5900_blockcpy_or\n"
".ent aurora_r5900_blockcpy_or\n"
"aurora_r5900_blockcpy_or:\n"
"    sll     $t0, $a3, 8\n"
"    or      $a3, $a3, $t0\n"
"    # Modern R5900 GAS requires PCPYLD's three operands.\n"
"    pcpyld  $a3, $a3, $a3\n"
"    pcpyh   $a3, $a3\n"
"3:\n"
"    ld      $t0, 8($a1)\n"
"    ld      $t1, 0($a1)\n"
"    pcpyld  $t0, $t0, $t1\n"
"    por     $t0, $t0, $a3\n"
"    sq      $t0, 0($a0)\n"
"    addiu   $a2, $a2, -16\n"
"    addiu   $a0, $a0, 16\n"
"    bgtz    $a2, 3b\n"
"    addiu   $a1, $a1, 16\n"
"    jr      $ra\n"
"    nop\n"
".end aurora_r5900_blockcpy_or\n"

".set at\n"
".set reorder\n"
);
