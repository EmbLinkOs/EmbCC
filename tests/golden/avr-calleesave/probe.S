; Calls an EmbCC-compiled AVR function with a known value in every register
; avr-gcc's ABI says the callee must preserve -- r2-r17 and Y (r28:r29) --
; and records which came back different: bit n-2 of probe_bad for rn
; (r2..r17), and probe_ybad for Y. Written in assembly so no compiler
; decides what holds what.
;
;   unsigned probe(unsigned (*f)(unsigned), unsigned a);   returns f(a)
    .text
    .global probe
    .type probe, @function
probe:
    push r2
    push r3
    push r4
    push r5
    push r6
    push r7
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push r16
    push r17
    push r28
    push r29
    movw r30, r24               ; Z = f (a word address, as icall wants)
    movw r24, r22               ; its argument
    ldi r26, 162
    mov r2, r26
    ldi r26, 163
    mov r3, r26
    ldi r26, 164
    mov r4, r26
    ldi r26, 165
    mov r5, r26
    ldi r26, 166
    mov r6, r26
    ldi r26, 167
    mov r7, r26
    ldi r26, 168
    mov r8, r26
    ldi r26, 169
    mov r9, r26
    ldi r26, 170
    mov r10, r26
    ldi r26, 171
    mov r11, r26
    ldi r26, 172
    mov r12, r26
    ldi r26, 173
    mov r13, r26
    ldi r26, 174
    mov r14, r26
    ldi r26, 175
    mov r15, r26
    ldi r26, 176
    mov r16, r26
    ldi r26, 177
    mov r17, r26
    ldi r28, 0x5c
    ldi r29, 0x3d
    icall
    sts probe_result, r24
    sts probe_result+1, r25
    clr r24
    clr r25
    ldi r26, 162
    cp r2, r26
    breq 1f
    ori r24, 1
1:
    ldi r26, 163
    cp r3, r26
    breq 1f
    ori r24, 2
1:
    ldi r26, 164
    cp r4, r26
    breq 1f
    ori r24, 4
1:
    ldi r26, 165
    cp r5, r26
    breq 1f
    ori r24, 8
1:
    ldi r26, 166
    cp r6, r26
    breq 1f
    ori r24, 16
1:
    ldi r26, 167
    cp r7, r26
    breq 1f
    ori r24, 32
1:
    ldi r26, 168
    cp r8, r26
    breq 1f
    ori r24, 64
1:
    ldi r26, 169
    cp r9, r26
    breq 1f
    ori r24, 128
1:
    ldi r26, 170
    cp r10, r26
    breq 1f
    ori r25, 1
1:
    ldi r26, 171
    cp r11, r26
    breq 1f
    ori r25, 2
1:
    ldi r26, 172
    cp r12, r26
    breq 1f
    ori r25, 4
1:
    ldi r26, 173
    cp r13, r26
    breq 1f
    ori r25, 8
1:
    ldi r26, 174
    cp r14, r26
    breq 1f
    ori r25, 16
1:
    ldi r26, 175
    cp r15, r26
    breq 1f
    ori r25, 32
1:
    ldi r26, 176
    cp r16, r26
    breq 1f
    ori r25, 64
1:
    ldi r26, 177
    cp r17, r26
    breq 1f
    ori r25, 128
1:
    sts probe_bad, r24
    sts probe_bad+1, r25
    clr r24
    cpi r28, 0x5c
    breq 2f
    ldi r24, 1
2:  cpi r29, 0x3d
    breq 3f
    ldi r24, 1
3:  sts probe_ybad, r24
    pop r29
    pop r28
    pop r17
    pop r16
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop r7
    pop r6
    pop r5
    pop r4
    pop r3
    pop r2
    lds r24, probe_result
    lds r25, probe_result+1
    ret

    .data
    .global probe_bad
    .global probe_ybad
probe_result: .word 0
probe_bad:    .word 0
probe_ybad:   .byte 0
