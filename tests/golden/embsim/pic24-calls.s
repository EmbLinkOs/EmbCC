; pic24-calls.s -- calls, returns, an interrupt and a trap, for
; tests/golden/embsim-pic24.sh's analyses of the PIC24 core: --profile,
; --stack-report, --coverage, --fault-report and the GDB server.
; Assembled by pic24-asm.awk, made an ELF file by pic24-elf.py (its labels
; functions, its .equs symbols: __SP_init and __SPLIM_init say where the
; stack is, as XC16's link does).
;
;   - start calls outer twice (CALL, RCALL) and leaf once; outer pushes
;     three words and calls leaf, which pushes two: the deepest the
;     stack goes is 0x1200 + 18 (outer's return address and three words,
;     leaf's return address and two);
;   - Timer1 interrupts twice while start waits;
;   - a word read at an odd address is an address error trap, its
;     handler clears INTCON1.ADDRERR and returns past it, to a last call
;     of leaf; then RESET.
        .equ __SP_init, 0x1200
        .equ __SPLIM_init, 0x1ff0
        .equ SPLIM, 0x20
        .equ INTCON1, 0x80
        .equ IFS0, 0x88
        .equ IEC0, 0x98
        .equ PR1, 0x192
        .equ T1CON, 0x194
        .equ COUNT, 0x1000
        .org 0
        goto start
        .org 0x6
        .word addrerr                   ; the address error trap's vector
        .org 0x1a
        .word t1isr                     ; Timer1's (interrupt 3)
        .org 0x200
start:  mov #0x1200, w15
        mov #0x1ff0, w0
        mov w0, SPLIM
        mov #0x1111, w8                 ; for the debugger to read
        mov #0x2222, w9
        call outer
        rcall outer
        rcall leaf
        mov #0, w0                      ; Timer1's interrupt, twice
        mov w0, COUNT
        mov #100, w0
        mov w0, PR1
        bset IEC0, #3
        mov #0x8000, w0
        mov w0, T1CON
.Lwait: mov COUNT, w0
        cp w0, #2
        bra lt, .Lwait
        mov #0, w0
        mov w0, T1CON
        bclr IEC0, #3
        mov #0x1001, w1                 ; an address error
        .word 0x780011                  ; mov [w1], w0
.Lafter: rcall leaf
        reset
outer:  push w0
        push w1
        push w2
        rcall leaf
        pop w2
        pop w1
        pop w0
        return
leaf:   push w4
        push w5
        pop w5
        pop w4
        return
t1isr:  push w0
        mov COUNT, w0
        add #1, w0
        mov w0, COUNT
        bclr IFS0, #3
        pop w0
        .word 0x064000                  ; retfie
addrerr: bclr INTCON1, #3
        .word 0x064000                  ; retfie: on to .Lafter
