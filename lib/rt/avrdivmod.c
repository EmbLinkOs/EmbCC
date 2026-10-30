/* avr-gcc's division routines, under their own register conventions:
 * what an object compiled by avr-gcc or clang calls for a / and a % --
 * both at once -- so it links against this archive. EmbCC's own code
 * calls the libgcc names in avr.c (__divsi3 ...) and none of these.
 *
 *   __udivmodqi4, __divmodqi4  r24 / r22   -> quotient r24, remainder r25
 *   __udivmodhi4, __divmodhi4  r25:r24 / r23:r22
 *                              -> quotient r23:r22, remainder r25:r24
 *   __udivmodsi4, __divmodsi4  r25:r22 / r21:r18
 *                              -> quotient r21:r18, remainder r25:r22
 *
 * Each is libgcc's restoring shift-and-subtract loop (lib1funcs.S), and
 * each saves the scratch registers it uses, so it changes its results,
 * r0 and the flags and nothing else -- less than libgcc's, which a caller
 * compiled against libgcc's clobbers cannot mind. The 32-bit loop counts
 * in r1, which ends at zero, as libgcc's does: an interrupt handler of
 * EmbCC's clears r1 for itself. A quotient of a division by zero is all
 * ones and its remainder the dividend, as libgcc's are. All weak: a
 * program's own definition wins. */
#if defined(__AVR__)

__asm__(
".text\n"

".weak __udivmodqi4\n"
".type __udivmodqi4, @function\n"
"__udivmodqi4:\n"
"    push r23\n"
"    sub r25, r25\n"                /* the remainder, and C */
"    ldi r23, 9\n"
"    rjmp .Luqi_ep\n"
".Luqi_loop:\n"
"    rol r25\n"
"    cp r25, r22\n"
"    brcs .Luqi_ep\n"
"    sub r25, r22\n"
".Luqi_ep:\n"
"    rol r24\n"
"    dec r23\n"
"    brne .Luqi_loop\n"
"    com r24\n"                     /* C was the complement of each bit */
"    pop r23\n"
"    ret\n"
".size __udivmodqi4, .-__udivmodqi4\n"

".weak __divmodqi4\n"
".type __divmodqi4, @function\n"
"__divmodqi4:\n"
"    push r22\n"
"    bst r24, 7\n"                  /* T: the dividend's sign */
"    mov r0, r24\n"
"    eor r0, r22\n"                 /* r0.7: the quotient's */
"    sbrc r24, 7\n"
"    neg r24\n"
"    sbrc r22, 7\n"
"    neg r22\n"
"    rcall __udivmodqi4\n"
"    brtc .Lsqi_q\n"
"    neg r25\n"                     /* the remainder has the dividend's sign */
".Lsqi_q:\n"
"    sbrc r0, 7\n"
"    neg r24\n"
"    pop r22\n"
"    ret\n"
".size __divmodqi4, .-__divmodqi4\n"

".weak __udivmodhi4\n"
".type __udivmodhi4, @function\n"
"__udivmodhi4:\n"
"    push r21\n"
"    push r26\n"
"    push r27\n"
"    sub r26, r26\n"
"    sub r27, r27\n"
"    ldi r21, 17\n"
"    rjmp .Luhi_ep\n"
".Luhi_loop:\n"
"    rol r26\n"
"    rol r27\n"
"    cp r26, r22\n"
"    cpc r27, r23\n"
"    brcs .Luhi_ep\n"
"    sub r26, r22\n"
"    sbc r27, r23\n"
".Luhi_ep:\n"
"    rol r24\n"
"    rol r25\n"
"    dec r21\n"
"    brne .Luhi_loop\n"
"    com r24\n"
"    com r25\n"
"    movw r22, r24\n"               /* the quotient */
"    movw r24, r26\n"               /* the remainder */
"    pop r27\n"
"    pop r26\n"
"    pop r21\n"
"    ret\n"
".size __udivmodhi4, .-__udivmodhi4\n"

".weak __divmodhi4\n"
".type __divmodhi4, @function\n"
"__divmodhi4:\n"
"    bst r25, 7\n"                  /* T: the dividend's sign */
"    mov r0, r23\n"
"    brtc .Lshi_d\n"
"    com r0\n"                      /* r0.7: the quotient's */
"    rcall .Lshi_neg1\n"
".Lshi_d:\n"
"    sbrc r23, 7\n"
"    rcall .Lshi_neg2\n"
"    rcall __udivmodhi4\n"
"    sbrc r0, 7\n"
"    rcall .Lshi_neg2\n"
"    brtc .Lshi_ret\n"
".Lshi_neg1:\n"                    /* r25:r24 = -r25:r24 */
"    com r25\n"
"    neg r24\n"
"    sbci r25, 0xff\n"
"    ret\n"
".Lshi_neg2:\n"                    /* r23:r22 = -r23:r22 */
"    com r23\n"
"    neg r22\n"
"    sbci r23, 0xff\n"
".Lshi_ret:\n"
"    ret\n"
".size __divmodhi4, .-__divmodhi4\n"

".weak __udivmodsi4\n"
".type __udivmodsi4, @function\n"
"__udivmodsi4:\n"
"    push r26\n"
"    push r27\n"
"    push r30\n"
"    push r31\n"
"    ldi r26, 33\n"
"    mov r1, r26\n"                 /* the count, in r1: 0 again at the end */
"    sub r26, r26\n"
"    sub r27, r27\n"
"    movw r30, r26\n"
"    rjmp .Lusi_ep\n"
".Lusi_loop:\n"
"    rol r26\n"
"    rol r27\n"
"    rol r30\n"
"    rol r31\n"
"    cp r26, r18\n"
"    cpc r27, r19\n"
"    cpc r30, r20\n"
"    cpc r31, r21\n"
"    brcs .Lusi_ep\n"
"    sub r26, r18\n"
"    sbc r27, r19\n"
"    sbc r30, r20\n"
"    sbc r31, r21\n"
".Lusi_ep:\n"
"    rol r22\n"
"    rol r23\n"
"    rol r24\n"
"    rol r25\n"
"    dec r1\n"
"    brne .Lusi_loop\n"
"    com r22\n"
"    com r23\n"
"    com r24\n"
"    com r25\n"
"    movw r18, r22\n"               /* the quotient */
"    movw r20, r24\n"
"    movw r22, r26\n"               /* the remainder */
"    movw r24, r30\n"
"    pop r31\n"
"    pop r30\n"
"    pop r27\n"
"    pop r26\n"
"    ret\n"
".size __udivmodsi4, .-__udivmodsi4\n"

".weak __divmodsi4\n"
".type __divmodsi4, @function\n"
"__divmodsi4:\n"
"    mov r0, r21\n"
"    bst r25, 7\n"                  /* T: the dividend's sign */
"    brtc .Lssi_d\n"
"    com r0\n"                      /* r0.7: the quotient's */
"    rcall .Lssi_neg1\n"
".Lssi_d:\n"
"    sbrc r21, 7\n"
"    rcall .Lssi_neg2\n"
"    rcall __udivmodsi4\n"
"    sbrc r0, 7\n"
"    rcall .Lssi_neg2\n"
"    brtc .Lssi_ret\n"
".Lssi_neg1:\n"                    /* r25:r22 = -r25:r22 */
"    com r25\n"
"    com r24\n"
"    com r23\n"
"    neg r22\n"
"    sbci r23, 0xff\n"
"    sbci r24, 0xff\n"
"    sbci r25, 0xff\n"
"    ret\n"
".Lssi_neg2:\n"                    /* r21:r18 = -r21:r18 */
"    com r21\n"
"    com r20\n"
"    com r19\n"
"    neg r18\n"
"    sbci r19, 0xff\n"
"    sbci r20, 0xff\n"
"    sbci r21, 0xff\n"
".Lssi_ret:\n"
"    ret\n"
".size __divmodsi4, .-__divmodsi4\n"
);

#endif
