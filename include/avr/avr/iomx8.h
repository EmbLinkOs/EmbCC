/* EmbCC's register file for the ATmega48A/PA/88A/PA/168A/PA/328/P family,
 * of which EmbCC takes the ATmega328P, ATmega328, ATmega168P and ATmega168
 * (-mmcu=). Included by <avr/io.h>; not meant to be included directly.
 *
 * Every register and bit of the datasheet's "Register Summary", in its
 * order from the top of the I/O space down, under the datasheet's names --
 * which are avr-libc's, so code written for avr-gcc compiles unchanged.
 * Registers 0x00-0x3f are named by their I/O address (_SFR_IO8), the
 * extended I/O registers above data address 0x5f by their data address
 * (_SFR_MEM8). Sixteen-bit registers are also named as a pair (TCNT1 for
 * TCNT1H:TCNT1L); EmbCC orders the two accesses through the TEMP register
 * as the datasheet requires (high byte first when writing, low byte first
 * when reading).
 *
 * tests/golden/avr-io.sh checks every address and bit here against a table
 * taken from the datasheet, and QEMU's arduino-uno models the GPIO ports,
 * the three timers, the USART and the power reduction register at the
 * same addresses. */
#ifndef _AVR_IOMX8_H_
#define _AVR_IOMX8_H_

/* ---- I/O space: in, out, sbi, cbi --------------------------------------- */

#define PINB    _SFR_IO8(0x03)
#define PINB7   7
#define PINB6   6
#define PINB5   5
#define PINB4   4
#define PINB3   3
#define PINB2   2
#define PINB1   1
#define PINB0   0

#define DDRB    _SFR_IO8(0x04)
#define DDB7    7
#define DDB6    6
#define DDB5    5
#define DDB4    4
#define DDB3    3
#define DDB2    2
#define DDB1    1
#define DDB0    0

#define PORTB   _SFR_IO8(0x05)
#define PORTB7  7
#define PORTB6  6
#define PORTB5  5
#define PORTB4  4
#define PORTB3  3
#define PORTB2  2
#define PORTB1  1
#define PORTB0  0

/* Port C has seven pins; PC6 is RESET unless the fuse says otherwise. */
#define PINC    _SFR_IO8(0x06)
#define PINC6   6
#define PINC5   5
#define PINC4   4
#define PINC3   3
#define PINC2   2
#define PINC1   1
#define PINC0   0

#define DDRC    _SFR_IO8(0x07)
#define DDC6    6
#define DDC5    5
#define DDC4    4
#define DDC3    3
#define DDC2    2
#define DDC1    1
#define DDC0    0

#define PORTC   _SFR_IO8(0x08)
#define PORTC6  6
#define PORTC5  5
#define PORTC4  4
#define PORTC3  3
#define PORTC2  2
#define PORTC1  1
#define PORTC0  0

#define PIND    _SFR_IO8(0x09)
#define PIND7   7
#define PIND6   6
#define PIND5   5
#define PIND4   4
#define PIND3   3
#define PIND2   2
#define PIND1   1
#define PIND0   0

#define DDRD    _SFR_IO8(0x0A)
#define DDD7    7
#define DDD6    6
#define DDD5    5
#define DDD4    4
#define DDD3    3
#define DDD2    2
#define DDD1    1
#define DDD0    0

#define PORTD   _SFR_IO8(0x0B)
#define PORTD7  7
#define PORTD6  6
#define PORTD5  5
#define PORTD4  4
#define PORTD3  3
#define PORTD2  2
#define PORTD1  1
#define PORTD0  0

#define TIFR0   _SFR_IO8(0x15)
#define OCF0B   2
#define OCF0A   1
#define TOV0    0

#define TIFR1   _SFR_IO8(0x16)
#define ICF1    5
#define OCF1B   2
#define OCF1A   1
#define TOV1    0

#define TIFR2   _SFR_IO8(0x17)
#define OCF2B   2
#define OCF2A   1
#define TOV2    0

#define PCIFR   _SFR_IO8(0x1B)
#define PCIF2   2
#define PCIF1   1
#define PCIF0   0

#define EIFR    _SFR_IO8(0x1C)
#define INTF1   1
#define INTF0   0

#define EIMSK   _SFR_IO8(0x1D)
#define INT1    1
#define INT0    0

#define GPIOR0  _SFR_IO8(0x1E)

#define EECR    _SFR_IO8(0x1F)
#define EEPM1   5
#define EEPM0   4
#define EERIE   3
#define EEMPE   2
#define EEPE    1
#define EERE    0

#define EEDR    _SFR_IO8(0x20)

#define EEAR    _SFR_IO16(0x21)
#define EEARL   _SFR_IO8(0x21)
#define EEARH   _SFR_IO8(0x22)
#define EEAR0   0
#define EEAR1   1
#define EEAR2   2
#define EEAR3   3
#define EEAR4   4
#define EEAR5   5
#define EEAR6   6
#define EEAR7   7
#define EEAR8   0
#if E2END > 0x1FF
#define EEAR9   1
#endif

#define GTCCR   _SFR_IO8(0x23)
#define TSM     7
#define PSRASY  1
#define PSRSYNC 0

#define TCCR0A  _SFR_IO8(0x24)
#define COM0A1  7
#define COM0A0  6
#define COM0B1  5
#define COM0B0  4
#define WGM01   1
#define WGM00   0

#define TCCR0B  _SFR_IO8(0x25)
#define FOC0A   7
#define FOC0B   6
#define WGM02   3
#define CS02    2
#define CS01    1
#define CS00    0

#define TCNT0   _SFR_IO8(0x26)
#define OCR0A   _SFR_IO8(0x27)
#define OCR0B   _SFR_IO8(0x28)

#define GPIOR1  _SFR_IO8(0x2A)
#define GPIOR2  _SFR_IO8(0x2B)

#define SPCR    _SFR_IO8(0x2C)
#define SPIE    7
#define SPE     6
#define DORD    5
#define MSTR    4
#define CPOL    3
#define CPHA    2
#define SPR1    1
#define SPR0    0

#define SPSR    _SFR_IO8(0x2D)
#define SPIF    7
#define WCOL    6
#define SPI2X   0

#define SPDR    _SFR_IO8(0x2E)

#define ACSR    _SFR_IO8(0x30)
#define ACD     7
#define ACBG    6
#define ACO     5
#define ACI     4
#define ACIE    3
#define ACIC    2
#define ACIS1   1
#define ACIS0   0

#define SMCR    _SFR_IO8(0x33)
#define SM2     3
#define SM1     2
#define SM0     1
#define SE      0

#define MCUSR   _SFR_IO8(0x34)
#define WDRF    3
#define BORF    2
#define EXTRF   1
#define PORF    0

/* BODS and BODSE, the brown-out detector's sleep disable, are on the
 * picoPower parts (the P in ATmega328P and ATmega168P) alone. */
#define MCUCR   _SFR_IO8(0x35)
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168P__)
#define BODS    6
#define BODSE   5
#endif
#define PUD     4
#define IVSEL   1
#define IVCE    0

/* Bit 0 is SPMEN in the ATmega328P's datasheet and SELFPRGEN in the
 * family's: one bit, both names. */
#define SPMCSR  _SFR_IO8(0x37)
#define SPMIE   7
#define RWWSB   6
#define SIGRD   5
#define RWWSRE  4
#define BLBSET  3
#define PGWRT   2
#define PGERS   1
#define SPMEN   0
#define SELFPRGEN 0

/* SP, SPH:SPL and SREG are <avr/common.h>'s, which <avr/io.h> includes. */

/* ---- extended I/O: data space only (lds, sts) --------------------------- */

#define WDTCSR  _SFR_MEM8(0x60)
#define WDIF    7
#define WDIE    6
#define WDP3    5
#define WDCE    4
#define WDE     3
#define WDP2    2
#define WDP1    1
#define WDP0    0

#define CLKPR   _SFR_MEM8(0x61)
#define CLKPCE  7
#define CLKPS3  3
#define CLKPS2  2
#define CLKPS1  1
#define CLKPS0  0

#define PRR     _SFR_MEM8(0x64)
#define PRTWI   7
#define PRTIM2  6
#define PRTIM0  5
#define PRTIM1  3
#define PRSPI   2
#define PRUSART0 1
#define PRADC   0

#define OSCCAL  _SFR_MEM8(0x66)
#define CAL7    7
#define CAL6    6
#define CAL5    5
#define CAL4    4
#define CAL3    3
#define CAL2    2
#define CAL1    1
#define CAL0    0

#define PCICR   _SFR_MEM8(0x68)
#define PCIE2   2
#define PCIE1   1
#define PCIE0   0

#define EICRA   _SFR_MEM8(0x69)
#define ISC11   3
#define ISC10   2
#define ISC01   1
#define ISC00   0

#define PCMSK0  _SFR_MEM8(0x6B)
#define PCINT7  7
#define PCINT6  6
#define PCINT5  5
#define PCINT4  4
#define PCINT3  3
#define PCINT2  2
#define PCINT1  1
#define PCINT0  0

#define PCMSK1  _SFR_MEM8(0x6C)
#define PCINT14 6
#define PCINT13 5
#define PCINT12 4
#define PCINT11 3
#define PCINT10 2
#define PCINT9  1
#define PCINT8  0

#define PCMSK2  _SFR_MEM8(0x6D)
#define PCINT23 7
#define PCINT22 6
#define PCINT21 5
#define PCINT20 4
#define PCINT19 3
#define PCINT18 2
#define PCINT17 1
#define PCINT16 0

#define TIMSK0  _SFR_MEM8(0x6E)
#define OCIE0B  2
#define OCIE0A  1
#define TOIE0   0

#define TIMSK1  _SFR_MEM8(0x6F)
#define ICIE1   5
#define OCIE1B  2
#define OCIE1A  1
#define TOIE1   0

#define TIMSK2  _SFR_MEM8(0x70)
#define OCIE2B  2
#define OCIE2A  1
#define TOIE2   0

/* The conversion result, as a pair (ADC, or ADCW) and as its bytes. */
#ifndef __ASSEMBLER__
#define ADC     _SFR_MEM16(0x78)
#endif
#define ADCW    _SFR_MEM16(0x78)
#define ADCL    _SFR_MEM8(0x78)
#define ADCH    _SFR_MEM8(0x79)

#define ADCSRA  _SFR_MEM8(0x7A)
#define ADEN    7
#define ADSC    6
#define ADATE   5
#define ADIF    4
#define ADIE    3
#define ADPS2   2
#define ADPS1   1
#define ADPS0   0

#define ADCSRB  _SFR_MEM8(0x7B)
#define ACME    6
#define ADTS2   2
#define ADTS1   1
#define ADTS0   0

#define ADMUX   _SFR_MEM8(0x7C)
#define REFS1   7
#define REFS0   6
#define ADLAR   5
#define MUX3    3
#define MUX2    2
#define MUX1    1
#define MUX0    0

#define DIDR0   _SFR_MEM8(0x7E)
#define ADC5D   5
#define ADC4D   4
#define ADC3D   3
#define ADC2D   2
#define ADC1D   1
#define ADC0D   0

#define DIDR1   _SFR_MEM8(0x7F)
#define AIN1D   1
#define AIN0D   0

#define TCCR1A  _SFR_MEM8(0x80)
#define COM1A1  7
#define COM1A0  6
#define COM1B1  5
#define COM1B0  4
#define WGM11   1
#define WGM10   0

#define TCCR1B  _SFR_MEM8(0x81)
#define ICNC1   7
#define ICES1   6
#define WGM13   4
#define WGM12   3
#define CS12    2
#define CS11    1
#define CS10    0

#define TCCR1C  _SFR_MEM8(0x82)
#define FOC1A   7
#define FOC1B   6

#define TCNT1   _SFR_MEM16(0x84)
#define TCNT1L  _SFR_MEM8(0x84)
#define TCNT1H  _SFR_MEM8(0x85)

#define ICR1    _SFR_MEM16(0x86)
#define ICR1L   _SFR_MEM8(0x86)
#define ICR1H   _SFR_MEM8(0x87)

#define OCR1A   _SFR_MEM16(0x88)
#define OCR1AL  _SFR_MEM8(0x88)
#define OCR1AH  _SFR_MEM8(0x89)

#define OCR1B   _SFR_MEM16(0x8A)
#define OCR1BL  _SFR_MEM8(0x8A)
#define OCR1BH  _SFR_MEM8(0x8B)

#define TCCR2A  _SFR_MEM8(0xB0)
#define COM2A1  7
#define COM2A0  6
#define COM2B1  5
#define COM2B0  4
#define WGM21   1
#define WGM20   0

#define TCCR2B  _SFR_MEM8(0xB1)
#define FOC2A   7
#define FOC2B   6
#define WGM22   3
#define CS22    2
#define CS21    1
#define CS20    0

#define TCNT2   _SFR_MEM8(0xB2)
#define OCR2A   _SFR_MEM8(0xB3)
#define OCR2B   _SFR_MEM8(0xB4)

#define ASSR    _SFR_MEM8(0xB6)
#define EXCLK   6
#define AS2     5
#define TCN2UB  4
#define OCR2AUB 3
#define OCR2BUB 2
#define TCR2AUB 1
#define TCR2BUB 0

#define TWBR    _SFR_MEM8(0xB8)

#define TWSR    _SFR_MEM8(0xB9)
#define TWS7    7
#define TWS6    6
#define TWS5    5
#define TWS4    4
#define TWS3    3
#define TWPS1   1
#define TWPS0   0

#define TWAR    _SFR_MEM8(0xBA)
#define TWA6    7
#define TWA5    6
#define TWA4    5
#define TWA3    4
#define TWA2    3
#define TWA1    2
#define TWA0    1
#define TWGCE   0

#define TWDR    _SFR_MEM8(0xBB)

#define TWCR    _SFR_MEM8(0xBC)
#define TWINT   7
#define TWEA    6
#define TWSTA   5
#define TWSTO   4
#define TWWC    3
#define TWEN    2
#define TWIE    0

#define TWAMR   _SFR_MEM8(0xBD)
#define TWAM6   7
#define TWAM5   6
#define TWAM4   5
#define TWAM3   4
#define TWAM2   3
#define TWAM1   2
#define TWAM0   1

#define UCSR0A  _SFR_MEM8(0xC0)
#define RXC0    7
#define TXC0    6
#define UDRE0   5
#define FE0     4
#define DOR0    3
#define UPE0    2
#define U2X0    1
#define MPCM0   0

#define UCSR0B  _SFR_MEM8(0xC1)
#define RXCIE0  7
#define TXCIE0  6
#define UDRIE0  5
#define RXEN0   4
#define TXEN0   3
#define UCSZ02  2
#define RXB80   1
#define TXB80   0

/* In master SPI mode bits 2 and 1 are UDORD0 and UCPHA0. */
#define UCSR0C  _SFR_MEM8(0xC2)
#define UMSEL01 7
#define UMSEL00 6
#define UPM01   5
#define UPM00   4
#define USBS0   3
#define UCSZ01  2
#define UDORD0  2
#define UCSZ00  1
#define UCPHA0  1
#define UCPOL0  0

#define UBRR0   _SFR_MEM16(0xC4)
#define UBRR0L  _SFR_MEM8(0xC4)
#define UBRR0H  _SFR_MEM8(0xC5)

#define UDR0    _SFR_MEM8(0xC6)

/* ---- pin names: the bit of a pin in its PORTx, DDRx and PINx ------------ */

#define PB7 7
#define PB6 6
#define PB5 5
#define PB4 4
#define PB3 3
#define PB2 2
#define PB1 1
#define PB0 0

#define PC6 6
#define PC5 5
#define PC4 4
#define PC3 3
#define PC2 2
#define PC1 1
#define PC0 0

#define PD7 7
#define PD6 6
#define PD5 5
#define PD4 4
#define PD3 3
#define PD2 2
#define PD1 1
#define PD0 0

/* ---- interrupt vectors: the handler of each, by its datasheet number ---- */

#define INT0_vect_num         1
#define INT0_vect             _VECTOR(1)
#define INT1_vect_num         2
#define INT1_vect             _VECTOR(2)
#define PCINT0_vect_num       3
#define PCINT0_vect           _VECTOR(3)
#define PCINT1_vect_num       4
#define PCINT1_vect           _VECTOR(4)
#define PCINT2_vect_num       5
#define PCINT2_vect           _VECTOR(5)
#define WDT_vect_num          6
#define WDT_vect              _VECTOR(6)
#define TIMER2_COMPA_vect_num 7
#define TIMER2_COMPA_vect     _VECTOR(7)
#define TIMER2_COMPB_vect_num 8
#define TIMER2_COMPB_vect     _VECTOR(8)
#define TIMER2_OVF_vect_num   9
#define TIMER2_OVF_vect       _VECTOR(9)
#define TIMER1_CAPT_vect_num  10
#define TIMER1_CAPT_vect      _VECTOR(10)
#define TIMER1_COMPA_vect_num 11
#define TIMER1_COMPA_vect     _VECTOR(11)
#define TIMER1_COMPB_vect_num 12
#define TIMER1_COMPB_vect     _VECTOR(12)
#define TIMER1_OVF_vect_num   13
#define TIMER1_OVF_vect       _VECTOR(13)
#define TIMER0_COMPA_vect_num 14
#define TIMER0_COMPA_vect     _VECTOR(14)
#define TIMER0_COMPB_vect_num 15
#define TIMER0_COMPB_vect     _VECTOR(15)
#define TIMER0_OVF_vect_num   16
#define TIMER0_OVF_vect       _VECTOR(16)
#define SPI_STC_vect_num      17
#define SPI_STC_vect          _VECTOR(17)
#define USART_RX_vect_num     18
#define USART_RX_vect         _VECTOR(18)
#define USART_UDRE_vect_num   19
#define USART_UDRE_vect       _VECTOR(19)
#define USART_TX_vect_num     20
#define USART_TX_vect         _VECTOR(20)
#define ADC_vect_num          21
#define ADC_vect              _VECTOR(21)
#define EE_READY_vect_num     22
#define EE_READY_vect         _VECTOR(22)
#define ANALOG_COMP_vect_num  23
#define ANALOG_COMP_vect      _VECTOR(23)
#define TWI_vect_num          24
#define TWI_vect              _VECTOR(24)
#define SPM_READY_vect_num    25
#define SPM_READY_vect        _VECTOR(25)

/* 26 vectors (reset among them) of two words each: a jmp. */
#define _VECTORS_SIZE (26 * 4)

#endif /* _AVR_IOMX8_H_ */
