#define _XTAL_FREQ                                                             \
  20000000UL // Definește frecvența oscilatorului pentru funcțiile de întârziere
#include "adc.h"
#include "buttons.h" // Include fișierul de antet pentru butoane
#include "config.h" // Include fișierul de configurare pentru definiții și setări
#include "control.h" // Include fișierul de antet pentru control
#include "menu.h"
#include "pwm.h"
#include <xc.h>

#pragma config FOSC = HS  // Oscilator High-Speed (20 MHz)
#pragma config WDTE = OFF // Watchdog Timer dezactivat
#pragma config PWRTE = ON // Power-up Timer activat
#pragma config BOREN = ON // Brown-out Reset activat
#pragma config LVP =                                                           \
    OFF // Low-Voltage Programming dezactivat (RB3 devine I/O digital)
#pragma config CPD = OFF // Data EEPROM Code Protection off
#pragma config WRT = OFF // Flash Write Protection off
#pragma config CP = OFF  // Code Protection off

#define LCD_RS_MASK 0x04u   /* RB2 */
#define LCD_E_MASK 0x08u    /* RB3 */
#define LCD_DATA_MASK 0xF0u /* RB4-RB7 */
#define LCD_CHAR_DEGREE 0u
#define LCD_BAR_CHAR_BASE 1u
#define LCD_BAR_CHAR_CAP 2u
#define LCD_BAR_CHAR_CELL_1 3u
#define LCD_BAR_CHAR_CELL_2 4u
#define LCD_BAR_CHAR_CELL_3 5u

static unsigned char lcd_portb_shadow = 0u;

#define ADC_AVERAGE_SAMPLES 8u
#define ADC_DISCHARGE_AVERAGE_SAMPLES 8u
#define ADC_REFERENCE_MV 5000UL
#define ADC_MAX_COUNT 1023UL

static const unsigned char adc_hardware_channels[ADC_INPUT_COUNT] = {
    0u, 1u, 3u}; /* AN0, AN1, AN3 */

static unsigned int adc_samples[ADC_INPUT_COUNT][ADC_AVERAGE_SAMPLES];
static unsigned int adc_discharge_samples[ADC_DISCHARGE_AVERAGE_SAMPLES];
static unsigned int adc_sample_sums[ADC_INPUT_COUNT];
static unsigned char adc_sample_indices[ADC_INPUT_COUNT];
static unsigned char adc_sample_counts[ADC_INPUT_COUNT];
static unsigned int adc_discharge_sample_sum;
static unsigned char adc_discharge_sample_index;
static unsigned char adc_discharge_sample_count;
unsigned int adc_temperature_raw;
unsigned int adc_temperature_celsius;

void SYSTEM_Init(void) {
  // 1. Configurare pini ADC (RA0/AN0 ... RA3/AN3 ca intrări)
  TRISA = 0x0Fu;

  // 2. Configurare pin PWM (RC2 / CCP1 ca ieșire)
  TRISCbits.TRISC2 = 0;
  PORTCbits.RC5 = 0u;
  TRISCbits.TRISC5 = 0u;

  // 3. Configurare ventilator pe RD1 ca ieșire, oprit la pornire
  PORTDbits.RD1 = 0;
  TRISDbits.TRISD1 = 0;

  // 4. Configurare pini LCD pe PORTB (RS->RB2, E->RB3, D4-D7->RB4-RB7)
  // Setează biții 2 la 7 din TRISB ca ieșiri (0), păstrând RB0 și RB1 liberi
  TRISB &= 0x03; // 0000 0011 -> RB2...RB7 devin ieșiri

  // Setează RD1, RD2 și RD3 ca ieșiri (0 = Output, 1 = Input)
  TRISDbits.TRISD1 = 0; // Ventilator
  TRISDbits.TRISD2 = 0; // LED Verde (RD2)
  TRISDbits.TRISD3 = 0; // LED Roșu (RD3)
}

void PWM_Init(void) {
  // Setează perioada pentru Timer2 (PR2 = 99 pentru ~50 kHz la 20 MHz cu
  // Prescaler 1)
  PR2 = 99;

  // Configurează modulul CCP1 în mod PWM
  CCP1CON = 0x0C;

  // Pornește Timer2 cu prescaler 1:1 (T2CON bit 2 = TMR2ON, biții 1-0 = T2CKPS)
  T2CON = 0x04;
}

void PWM_SetDutyCycle(unsigned int duty) {
  // Valoarea 'duty' este pe 10 biți (0 - 1023)
  if (duty > 1023)
    duty = 1023;

  // Cei 8 biți superiori merg în CCPR1L
  CCPR1L = (unsigned char)(duty >> 2);

  // Cei 2 biți inferiori merg în biții 4 și 5 ai CCP1CON
  CCP1CON = (CCP1CON & 0xCF) | (unsigned char)((duty & 0x03) << 4);
}

static void LCD_WriteNibble(unsigned char nibble) {
  /* Pastreaza RS, scrie nibble-ul pe RB4-RB7 si tine E jos. */
  lcd_portb_shadow &= LCD_RS_MASK;
  lcd_portb_shadow |= (unsigned char)((nibble & 0x0Fu) << 4);

  PORTB = lcd_portb_shadow;
  __delay_us(1);

  lcd_portb_shadow |= LCD_E_MASK;
  PORTB = lcd_portb_shadow;
  __delay_us(1);

  lcd_portb_shadow &= (unsigned char)~LCD_E_MASK;
  PORTB = lcd_portb_shadow;
  __delay_us(1);
}

static void LCD_WriteByte(unsigned char value, unsigned char is_data) {
  if (is_data != 0u) {
    lcd_portb_shadow |= LCD_RS_MASK;
  } else {
    lcd_portb_shadow &= (unsigned char)~LCD_RS_MASK;
  }

  LCD_WriteNibble((unsigned char)(value >> 4));
  LCD_WriteNibble((unsigned char)(value & 0x0Fu));

  /* Clear si Return Home au nevoie de un timp mai lung. */
  if ((is_data == 0u) && ((value == 0x01u) || (value == 0x02u))) {
    __delay_ms(2);
  } else {
    __delay_us(50);
  }
}

void LCD_Command(unsigned char cmd) { LCD_WriteByte(cmd, 0u); }

void LCD_Char(unsigned char dat) { LCD_WriteByte(dat, 1u); }

static void LCD_CreateChar(unsigned char location,
                           const unsigned char *bitmap) {
  unsigned char row;

  LCD_Command((unsigned char)(0x40u | ((location & 0x07u) << 3)));
  for (row = 0u; row < 8u; ++row) {
    LCD_Char((unsigned char)(bitmap[row] & 0x1Fu));
  }
  LCD_Command(0x80u);
}

// Creează o căsuță cu trei indicatori pentru bara de progres.
static void LCD_UpdateBarChar(unsigned char location,
                              unsigned char active_sticks) {
  static const unsigned char stick_masks[3] = {0x10u, 0x04u, 0x01u};
  unsigned char bitmap[8];
  unsigned char row;
  unsigned char stick;

  for (row = 0u; row < 8u; ++row) {
    if ((row == 0u) || (row == 7u)) {
      bitmap[row] = 0x1Fu;
    } else {
      bitmap[row] = 0u;
      if ((row >= 2u) && (row <= 5u)) {
        for (stick = 0u; stick < active_sticks; ++stick) {
          bitmap[row] |= stick_masks[stick];
        }
      }
    }
  }

  LCD_CreateChar(
      location,
      bitmap); // Creează caracterul personalizat pe locația specificată
}

// Afișează un șir de caractere pe LCD.
void LCD_String(const char *s) {
  if (s == 0) {
    return;
  }

  while (*s != '\0') {
    LCD_Char((unsigned char)*s);
    ++s;
  }
}

//  Setează cursorul LCD la poziția specificată (rând și coloană).
void LCD_SetCursor(unsigned char row, unsigned char col) {
  unsigned char address;

  if (row > 1u) {
    row = 1u;
  }
  if (col > 15u) {
    col = 15u;
  }

  address = (row == 0u) ? 0x00u : 0x40u;
  LCD_Command((unsigned char)(0x80u | (address + col)));
}

//  Initializează LCD-ul și creează caracterele personalizate necesare.
void LCD_Init(void) {
  static const unsigned char degree_glyph[8] = {0x04u, 0x0Au, 0x04u, 0x00u,
                                                0x00u, 0x00u, 0x00u, 0x00u};
  static const unsigned char battery_base_glyph[8] = {32u, 1u, 1u, 1u,
                                                      1u, 1u, 1u, 32u};
  static const unsigned char battery_cap_glyph[8] = {32u, 16u, 24u, 24u,
                                                     24u, 24u, 16u, 32u};

  /* RB0-RB1 intrari; RB2-RB7 iesiri. */
  TRISB = 0x03u;

  lcd_portb_shadow = 0u;
  PORTB = lcd_portb_shadow;

  __delay_ms(40);

  /* Secventa HD44780 pentru trecerea in modul 4 biti. */
  LCD_WriteNibble(0x03u);
  __delay_ms(5);

  LCD_WriteNibble(0x03u);
  __delay_us(150);

  LCD_WriteNibble(0x03u);
  __delay_us(150);

  LCD_WriteNibble(0x02u);
  __delay_us(150);

  LCD_Command(0x28u); /* 4 biti, 2 randuri, font 5x8 */
  LCD_Command(0x08u); /* Afisaj oprit temporar */
  LCD_Command(0x01u); /* Sterge afisajul */
  LCD_Command(0x06u); /* Incrementare cursor, fara deplasare */
  LCD_Command(0x0Cu); /* Afisaj pornit, cursor ascuns */
  LCD_CreateChar(LCD_CHAR_DEGREE, degree_glyph);
  LCD_CreateChar(LCD_BAR_CHAR_BASE, battery_base_glyph);
  LCD_CreateChar(LCD_BAR_CHAR_CAP, battery_cap_glyph);
}

/* Implementate in modulele SYSTEM si PWM ale proiectului. */
void SYSTEM_Init(void);

/* Citirile ADC normale si AN2 folosesc medii mobile de 8 esantioane. */
void ADC_Init(void) {
  unsigned char channel;
  unsigned char sample;

  TRISA |= 0x0Fu;
  ADCON1 = 0x86u; /* AN0-AN3 analogice, rezultat aliniat la dreapta. */
  ADCON0 = 0x81u; /* Fosc/32, canal AN0, modul ADC pornit. */

  for (channel = 0u; channel < ADC_INPUT_COUNT; ++channel) {
    adc_sample_sums[channel] = 0u;
    adc_sample_indices[channel] = 0u;
    adc_sample_counts[channel] = 0u;
    for (sample = 0u; sample < ADC_AVERAGE_SAMPLES; ++sample) {
      adc_samples[channel][sample] = 0u;
    }
  }
  adc_discharge_sample_sum = 0u;
  adc_discharge_sample_index = 0u;
  adc_discharge_sample_count = 0u;
  for (sample = 0u; sample < ADC_DISCHARGE_AVERAGE_SAMPLES; ++sample) {
    adc_discharge_samples[sample] = 0u;
  }
}

//  Citeste valoarea ADC de pe canalul specificat si returneaza valoarea medie.
unsigned int ADC_Read(unsigned char channel) {
  unsigned int adc_value;
  unsigned char sample_index;

  if (channel >= ADC_INPUT_COUNT) {
    return 0u;
  }

  ADCON0 =
      (unsigned char)((ADCON0 & 0xC7u) |
                      (unsigned char)(adc_hardware_channels[channel] << 3));
  __delay_us(20); /* Timp de achizitie dupa schimbarea canalului ADC. */

  ADCON0bits.GO_nDONE = 1u;
  while (ADCON0bits.GO_nDONE != 0u) {
  }

  adc_value = (unsigned int)(((unsigned int)ADRESH << 8) | ADRESL);

  // Actualizare bufferul de esantioane si suma pentru medierea mobila
  if (adc_sample_counts[channel] ==
      0u) { // Prima citire, initializeaza bufferul de esantioane
    unsigned char sample;
    adc_sample_sums[channel] = adc_value * ADC_AVERAGE_SAMPLES;
    for (sample = 0u; sample < ADC_AVERAGE_SAMPLES; ++sample) {
      adc_samples[channel][sample] = adc_value;
    }
    adc_sample_counts[channel] = ADC_AVERAGE_SAMPLES;
  } else { // Citire ulterioara, actualizeaza bufferul de esantioane
    sample_index = adc_sample_indices[channel];
    adc_sample_sums[channel] -= adc_samples[channel][sample_index];
    adc_samples[channel][sample_index] = adc_value;
    adc_sample_sums[channel] += adc_value;
    ++sample_index;
    if (sample_index >= ADC_AVERAGE_SAMPLES) {
      sample_index = 0u;
    }
    adc_sample_indices[channel] = sample_index;
  }

  return adc_sample_sums[channel] / ADC_AVERAGE_SAMPLES;
}
// Citeste valoarea ADC de pe canalul de descarcare si returneaza valoarea
// medie.
unsigned int ADC_ReadDischargeCurrentRaw(void) {
  unsigned int adc_value; // Valoarea ADC citita
  unsigned char
      sample_index; // Indexul esantionului curent in bufferul de esantioane

  ADCON0 = (unsigned char)((ADCON0 & 0xC7u) |
                           (2u << 3)); // Selecteaza canalul AN2 pentru citirea
                                       // curentului de descarcare
  __delay_us(20);
  ADCON0bits.GO_nDONE = 1u;           // Porneste conversia ADC
  while (ADCON0bits.GO_nDONE != 0u) { // Asteapta finalizarea conversiei
  }
  adc_value = (unsigned int)(((unsigned int)ADRESH << 8) |
                             ADRESL); // Combina cei doi octeti pentru a obtine
                                      // valoarea ADC completa

  if (adc_discharge_sample_count ==
      0u) { // Prima citire, initializeaza bufferul de esantioane
    unsigned char sample;
    adc_discharge_sample_sum = adc_value * ADC_DISCHARGE_AVERAGE_SAMPLES;
    for (sample = 0u; sample < ADC_DISCHARGE_AVERAGE_SAMPLES; ++sample) {
      adc_discharge_samples[sample] = adc_value;
    }
    adc_discharge_sample_count = ADC_DISCHARGE_AVERAGE_SAMPLES;
  } else { // Citire ulterioara, actualizeaza bufferul de esantioane
    sample_index = adc_discharge_sample_index;
    adc_discharge_sample_sum -= adc_discharge_samples[sample_index];
    adc_discharge_samples[sample_index] = adc_value;
    adc_discharge_sample_sum += adc_value;
    ++sample_index;
    if (sample_index >= ADC_DISCHARGE_AVERAGE_SAMPLES) {
      sample_index = 0u;
    }
    adc_discharge_sample_index =
        sample_index; // Actualizeaza indexul esantionului curent
  }

  return adc_discharge_sample_sum / ADC_DISCHARGE_AVERAGE_SAMPLES;
}

unsigned int
ADC_ReadTemperatureRaw(void) { // Citeste valoarea ADC de pe canalul de
                               // temperatura si returneaza valoarea medie.
  return ADC_Read(ADC_INPUT_TEMPERATURE);
}

unsigned int ADC_ConvertTemperature_C(
    unsigned int
        adc_value) { // Converteste valoarea ADC in temperatura in grade Celsius
  return (unsigned int)(((unsigned long)adc_value * 53UL) / 100UL);
}

// Converteste valoarea ADC in tensiune in milivolti, aplicand factorul de
// etalonare citit din EEPROM.
unsigned long ADC_ConvertVoltage_mV(unsigned int adc_value) {
  unsigned long adc_voltage_mV;
  unsigned long raw_voltage_mV;

  // 1. Calculul de bază al tensiunii pe pinul ADC
  adc_voltage_mV =
      ((unsigned long)adc_value * ADC_REFERENCE_MV) / ADC_MAX_COUNT;

  /* Divizorul 56k/18k: tensiune baterie = Vadc * (56k + 18k) / 18k. */
  raw_voltage_mV = (adc_voltage_mV * 74UL) / 18UL;

  // 2. Aplicarea factorului de etalonare citit din EEPROM (ex: 1000 = 1.000)
  unsigned long calibrated_voltage_mV =
      (raw_voltage_mV * (unsigned long)voltage_cal_factor) / 1000UL;

  return calibrated_voltage_mV;
}

// Converteste valoarea ADC in curent de incarcare in miliamperi, aplicand
// factorul de etalonare citit din EEPROM.
unsigned long ADC_ConvertChargeCurrent_mA(unsigned int adc_value) {
  unsigned long adc_voltage_mV;

  adc_voltage_mV =
      ((unsigned long)adc_value * ADC_REFERENCE_MV) / ADC_MAX_COUNT;
  /* Vadc = I * 0.1 ohm * 6.76; rezultatul este in mA. */
  return (adc_voltage_mV * 1000UL) / 676UL;
}

// Converteste valoarea ADC in curent de descarcare in miliamperi, aplicand
// factorul de etalonare citit din EEPROM.
unsigned long ADC_ConvertDischargeCurrent_mA(unsigned int adc_value) {
  return ((unsigned long)adc_value * 18UL) / 10UL;
}

#define APP_TMR1_PRELOAD_H                                                     \
  0xC2u // Preload pentru Timer1 pentru a genera un tick de 25 ms la Fosc=20
        // MHz, Fosc/4, prescaler 1:8
#define APP_TMR1_PRELOAD_L                                                     \
  0xF7u // Preload pentru Timer1 pentru a genera un tick de 25 ms la Fosc=20
        // MHz, Fosc/4, prescaler 1:8
#define APP_TIMER_TICKS_PER_SECOND                                             \
  40u // Numărul de tick-uri de 25 ms într-o secundă (1000 ms / 25 ms = 40)

#define APP_BUZZER_TIMER_PRESCALER                                             \
  32UL // Prescaler pentru Timer0 pentru a genera tonul buzzer-ului la frecvența
       // dorită
#define APP_BUZZER_TIMER_COUNTS                                                \
  ((_XTAL_FREQ +                                                               \
    (8UL * APP_BUZZER_TIMER_PRESCALER * BUZZER_FREQUENCY_HZ) / 2UL) /          \
   (8UL * APP_BUZZER_TIMER_PRESCALER * BUZZER_FREQUENCY_HZ))
#define APP_BUZZER_TMR0_PRELOAD (256UL - APP_BUZZER_TIMER_COUNTS)
#define APP_BUZZER_HALF_TICKS(duration_ms)                                     \
  ((2UL * BUZZER_FREQUENCY_HZ * (duration_ms) + 999UL) / 1000UL)
#define APP_BUZZER_QUEUE_SIZE 4u

enum { // Tipuri de pattern pentru buzzer
  APP_BUZZER_PATTERN_SHORT = 1u,
  APP_BUZZER_PATTERN_LONG,
  APP_BUZZER_PATTERN_EXTRA_LONG,
  APP_BUZZER_PATTERN_BOOT
};

enum { // Stările interne ale buzzer-ului
  APP_BUZZER_STATE_IDLE = 0u,
  APP_BUZZER_STATE_TONE,
  APP_BUZZER_STATE_GAP
};

// Variabile volatile pentru a fi utilizate în ISR și în bucla principală
static volatile unsigned char app_lcd_tick_pending = 0u;
static volatile unsigned char charge_timer_running = 0u;
static volatile unsigned char charge_timer_subsecond_ticks = 0u;
static volatile unsigned char charge_elapsed_hours = 0u;
static volatile unsigned char charge_elapsed_minutes = 0u;
static volatile unsigned char charge_elapsed_seconds = 0u;
static volatile unsigned char app_buzzer_queue[APP_BUZZER_QUEUE_SIZE];
static volatile unsigned char app_buzzer_queue_head = 0u;
static volatile unsigned char app_buzzer_queue_tail = 0u;
static volatile unsigned char app_buzzer_queue_count = 0u;
static volatile unsigned char app_buzzer_state = APP_BUZZER_STATE_IDLE;
static volatile unsigned char app_buzzer_beeps_remaining = 0u;
static volatile unsigned int app_buzzer_tone_ticks = 0u;
static volatile unsigned int app_buzzer_gap_ticks = 0u;
static volatile unsigned int app_buzzer_segment_ticks_left = 0u;

// Funcție pentru inițializarea buzzer-ului și a cozii de sunete
static void APP_BuzzerInit(void) {
  unsigned char index;

  /* Timer0: Fosc/4, prescaler 1:32; pastreaza setarile RBPU/INTEDG/T0SE. */
  OPTION_REG = (unsigned char)((OPTION_REG & 0xD0u) | 0x04u);
  TMR0 = (unsigned char)APP_BUZZER_TMR0_PRELOAD;
  INTCONbits.T0IE = 0u;
  INTCONbits.T0IF = 0u;
  // Initializează coada de sunete și starea buzzer-ului
  for (index = 0u; index < APP_BUZZER_QUEUE_SIZE; ++index) {
    app_buzzer_queue[index] = 0u;
  }
  app_buzzer_queue_head = 0u;
  app_buzzer_queue_tail = 0u;
  app_buzzer_queue_count = 0u;
  app_buzzer_state = APP_BUZZER_STATE_IDLE;
  app_buzzer_beeps_remaining = 0u;
  app_buzzer_tone_ticks = 0u;
  app_buzzer_gap_ticks = 0u;
  app_buzzer_segment_ticks_left = 0u;
  PORTCbits.RC5 = 0u;
}
// Funcție pentru a adăuga un pattern de sunet în coada buzzer-ului
static void APP_BuzzerRequest(unsigned char pattern) {
  unsigned char interrupt_state = INTCONbits.GIE;

  INTCONbits.GIE = 0u;
  if (app_buzzer_queue_count < APP_BUZZER_QUEUE_SIZE) {
    app_buzzer_queue[app_buzzer_queue_head] = pattern;
    app_buzzer_queue_head = (unsigned char)((app_buzzer_queue_head + 1u) &
                                            (APP_BUZZER_QUEUE_SIZE - 1u));
    ++app_buzzer_queue_count;
  }

  if (app_buzzer_state == APP_BUZZER_STATE_IDLE) {
    TMR0 = (unsigned char)APP_BUZZER_TMR0_PRELOAD;
    INTCONbits.T0IF = 0u;
    INTCONbits.T0IE = 1u;
  }
  INTCONbits.GIE = interrupt_state;
}
// Funcție pentru a gestiona tick-urile timerului buzzer-ului și a produce
// sunetele corespunzătoare
static void APP_BuzzerTimerTick(void) {
  unsigned char pattern;

  if (app_buzzer_state ==
      APP_BUZZER_STATE_IDLE) { // Dacă buzzer-ul este inactiv, încarcă următorul
                               // pattern din coadă
    goto load_next_pattern;
  }

  if (app_buzzer_segment_ticks_left >
      0u) { // Dacă există tick-uri rămase pentru segmentul curent, decrementați
            // contorul
    --app_buzzer_segment_ticks_left;
  }

  if (app_buzzer_segment_ticks_left !=
      0u) { // Dacă există tick-uri rămase pentru segmentul curent, comutați
            // starea buzzer-ului
    if (app_buzzer_state == APP_BUZZER_STATE_TONE) {
      PORTCbits.RC5 = (unsigned char)!PORTCbits.RC5;
    }
    return;
  }
  //  Dacă segmentul curent s-a terminat, treceți la următorul segment sau
  //  încărcați următorul pattern din coadă
  if (app_buzzer_state == APP_BUZZER_STATE_TONE) {
    PORTCbits.RC5 = 0u;
    if (app_buzzer_beeps_remaining >
        1u) { // Dacă mai există beep-uri rămase, treceți la starea de pauză și
              // setați tick-urile pentru pauză
      --app_buzzer_beeps_remaining;
      app_buzzer_state = APP_BUZZER_STATE_GAP;
      app_buzzer_segment_ticks_left = app_buzzer_gap_ticks;
    } else { // Dacă nu mai există beep-uri rămase, treceți la starea inactivă
             // și încărcați următorul pattern din coadă
      app_buzzer_state = APP_BUZZER_STATE_IDLE;
      goto load_next_pattern;
    }
  } else if (app_buzzer_state ==
             APP_BUZZER_STATE_GAP) { // Dacă buzzer-ul este în starea de pauză,
                                     // treceți la starea de ton și setați
                                     // tick-urile pentru ton
    app_buzzer_state = APP_BUZZER_STATE_TONE;
    app_buzzer_segment_ticks_left = app_buzzer_tone_ticks;
    PORTCbits.RC5 = 1u;
  }
  return;
// Eticheta pentru a încărca următorul pattern din coada buzzer-ului
load_next_pattern:
  if (app_buzzer_queue_count == 0u) {
    app_buzzer_state = APP_BUZZER_STATE_IDLE;
    app_buzzer_beeps_remaining = 0u;
    app_buzzer_segment_ticks_left = 0u;
    PORTCbits.RC5 = 0u;
    INTCONbits.T0IE = 0u;
    return;
  }
  // Încarcă următorul pattern din coadă și setează starea buzzer-ului și
  // tick-urile corespunzătoare
  pattern = app_buzzer_queue[app_buzzer_queue_tail];
  app_buzzer_queue_tail = (unsigned char)((app_buzzer_queue_tail + 1u) &
                                          (APP_BUZZER_QUEUE_SIZE - 1u));
  --app_buzzer_queue_count;
  app_buzzer_gap_ticks = 0u;

  if (pattern ==
      APP_BUZZER_PATTERN_LONG) { // Dacă pattern-ul este de tip "long", setează
                                 // numărul de beep-uri rămase și tick-urile
                                 // pentru ton
    app_buzzer_beeps_remaining = 1u;
    app_buzzer_tone_ticks =
        (unsigned int)APP_BUZZER_HALF_TICKS(BUZZER_LONG_BEEP_MS);
  } else if (pattern ==
             APP_BUZZER_PATTERN_EXTRA_LONG) { // Dacă pattern-ul este de tip
                                              // "extra long", setează numărul
                                              // de beep-uri rămase și
                                              // tick-urile pentru ton
    app_buzzer_beeps_remaining = 1u;
    app_buzzer_tone_ticks =
        (unsigned int)APP_BUZZER_HALF_TICKS(2UL * BUZZER_LONG_BEEP_MS);
  } else if (pattern ==
             APP_BUZZER_PATTERN_BOOT) { // Dacă pattern-ul este de tip "boot",
                                        // setează numărul de beep-uri rămase și
                                        // tick-urile pentru ton și pauză
    app_buzzer_beeps_remaining = 3u;
    app_buzzer_tone_ticks =
        (unsigned int)APP_BUZZER_HALF_TICKS(BUZZER_SHORT_BEEP_MS);
    app_buzzer_gap_ticks =
        (unsigned int)APP_BUZZER_HALF_TICKS(BUZZER_BOOT_GAP_MS);
  } else { // Dacă pattern-ul este de tip "short" sau necunoscut, setează
           // numărul de beep-uri rămase și tick-urile pentru ton
    app_buzzer_beeps_remaining = 1u;
    app_buzzer_tone_ticks =
        (unsigned int)APP_BUZZER_HALF_TICKS(BUZZER_SHORT_BEEP_MS);
  }

  app_buzzer_state = APP_BUZZER_STATE_TONE;
  app_buzzer_segment_ticks_left = app_buzzer_tone_ticks;
  PORTCbits.RC5 = 1u;
  TMR0 = (unsigned char)APP_BUZZER_TMR0_PRELOAD;
  INTCONbits.T0IF = 0u;
  INTCONbits.T0IE = 1u;
}
// Funcție pentru a afișa tensiunea pe LCD în formatul "X.XXV"
static void
LCD_PrintVoltage(unsigned long voltage_mV) { // Afișează tensiunea în formatul
                                             // "X.XXV" sau "XX.XV"
  unsigned long whole; // Partea întreagă a tensiunii în volți
  unsigned char
      decimals; // Partea zecimală a tensiunii în volți (două zecimale)

  whole = voltage_mV / 1000UL; // Obține partea întreagă a tensiunii în volți
  // Afișează tensiunea în formatul "X.XXV" sau "XX.XV" în funcție de valoarea
  // întregă
  if (whole < 10UL) { // Dacă partea întreagă este mai mică de 10, afișează
                      // formatul "X.XXV"
    LCD_Char((unsigned char)('0' + whole));
    LCD_Char('.');
    decimals = (unsigned char)((voltage_mV % 1000UL) / 10UL);
    LCD_Char((unsigned char)('0' + (decimals / 10u)));
    LCD_Char((unsigned char)('0' + (decimals % 10u)));
  } else { // Dacă partea întreagă este mai mare sau egală cu 10, afișează
           // formatul "XX.XV"
    if (whole > 99UL) {
      whole = 99UL;
      decimals = 9u;
    } else { // Dacă partea întreagă este între 10 și 99, afișează formatul
             // "XX.XV"
      decimals = (unsigned char)((voltage_mV % 1000UL) / 100UL);
    }
    LCD_Char((unsigned char)('0' + (whole / 10UL)));
    LCD_Char((unsigned char)('0' + (whole % 10UL)));
    LCD_Char('.');
    LCD_Char((unsigned char)('0' + decimals));
  }
  LCD_Char('V');
}
// Funcție pentru a afișa o bară de progres pe LCD care reprezintă tensiunea
// curentă în raport cu valorile minime și țintă
static void LCD_PrintVoltageBar(unsigned long voltage_mV,
                                unsigned long minimum_mV,
                                unsigned long target_cv_mV) {
  static unsigned char previous_level = 0xFFu;
  unsigned char level = 0u;
  unsigned char segment;
  unsigned char segment_level;
  static const unsigned char glyph_locations[3] = {
      LCD_BAR_CHAR_CELL_1, LCD_BAR_CHAR_CELL_2, LCD_BAR_CHAR_CELL_3};
  // Calculează nivelul de progres pe baza tensiunii curente, a valorii minime
  // și a valorii țintă
  if (target_cv_mV > minimum_mV) {
    if (voltage_mV < minimum_mV) {
      level = 0u;
    } else if (voltage_mV >= target_cv_mV) {
      level = 9u;
    } else {
      level = (unsigned char)(1UL + (((voltage_mV - minimum_mV) * 8UL) /
                                     (target_cv_mV - minimum_mV)));
    }
  }
  // Actualizează bara de progres doar dacă nivelul s-a schimbat față de nivelul
  // anterior
  if (level != previous_level) {
    unsigned char interrupt_state = INTCONbits.GIE;

    /* Evita depasirea stivei hardware PIC16 in timpul scrierii adanci CGRAM. */
    INTCONbits.GIE = 0u;
    for (segment = 0u; segment < 3u; ++segment) {
      segment_level =
          (level > (unsigned char)(segment * 3u))
              ? (unsigned char)(level - (unsigned char)(segment * 3u))
              : 0u;
      if (segment_level > 3u) {
        segment_level = 3u;
      }
      LCD_UpdateBarChar(glyph_locations[segment], segment_level);
    }

    previous_level = level;
    INTCONbits.GIE = interrupt_state;
  }
  // Afișează baza, cele trei celule de nivel și capul bateriei de la coloana 3.
  LCD_SetCursor(1u, 3u);
  LCD_Char(LCD_BAR_CHAR_BASE);
  for (segment = 0u; segment < 3u; ++segment) {
    LCD_Char(glyph_locations[segment]);
  }
  LCD_Char(LCD_BAR_CHAR_CAP);
}
// Funcție pentru a afișa curentul pe LCD în formatul "X.XXA"
static void LCD_PrintCurrent(unsigned long current_mA) {
  unsigned char hundredths;

  if (current_mA >
      9999UL) { // Dacă curentul depășește 9.999 A, afișează "9.99A"
    current_mA = 9999UL;
  }
  hundredths = (unsigned char)((current_mA % 1000UL) /
                               10UL); // Obține partea zecimală a curentului în
                                      // sute de miliamperi

  LCD_Char(
      (unsigned char)('0' + (current_mA /
                             1000UL))); // Afișează cifra unităților de amperi
  LCD_Char('.');                        // Afișează separatorul zecimal
  LCD_Char((unsigned char)('0' + (hundredths /
                                  10u))); // Afișează partea zecimală a
                                          // curentului în zeci de miliamperi
  LCD_Char((unsigned char)('0' + (hundredths %
                                  10u))); // Afișează partea zecimală a
                                          // curentului în sute de miliamperi
  LCD_Char('A');
}
// Funcție pentru a afișa o valoare în milivolți pe LCD, cu un format de 5 cifre
static void LCD_Print5DigitMillivolts(unsigned long value_mV) {
  unsigned long divisor = 10000UL;
  unsigned char digit;
  unsigned char leading = 1u;

  value_mV = (value_mV / 10UL) * 10UL;

  if (value_mV >
      99999UL) { // Dacă valoarea depășește 99.999 mV, afișează "*****"
    LCD_String(
        "*****"); // Afișează un mesaj de eroare pentru depășirea valorii maxime
    return;
  }
  // Afișează fiecare cifră a valorii în milivolți, gestionând zerourile de
  // început
  while (divisor > 0UL) {
    digit = (unsigned char)(value_mV / divisor);
    value_mV %= divisor;
    if ((digit != 0u) || (leading == 0u) || (divisor == 1UL)) {
      LCD_Char((unsigned char)('0' + digit));
      leading = 0u;
    } else {
      LCD_Char(' ');
    }
    divisor /= 10UL;
  }
}

/* Timer1 interrupt: tick de 25 ms la Fosc=20 MHz, Fosc/4, prescaler 1:8. */
static void APP_Timer1Init(void) {
  T1CON = 0x30u; /* Timer1 oprit, clock intern, prescaler 1:8. */
  PIE1bits.TMR1IE = 0u;
  TMR1H = APP_TMR1_PRELOAD_H;
  TMR1L = APP_TMR1_PRELOAD_L;
  PIR1bits.TMR1IF = 0u;
  app_lcd_tick_pending = 0u;
  charge_timer_running = 0u;
  charge_timer_subsecond_ticks = 0u;
  charge_elapsed_hours = 0u;
  charge_elapsed_minutes = 0u;
  charge_elapsed_seconds = 0u;
  T1CONbits.TMR1ON = 1u;
  PIE1bits.TMR1IE = 1u;
  INTCONbits.PEIE = 1u;
  INTCONbits.GIE = 1u;
}
// Funcție de întrerupere pentru Timer1 și Timer0, gestionând tick-urile și
void __interrupt() APP_Timer1ISR(void) {
  if ((PIE1bits.TMR1IE != 0u) && (PIR1bits.TMR1IF != 0u)) {
    TMR1H = APP_TMR1_PRELOAD_H;
    TMR1L = APP_TMR1_PRELOAD_L;
    PIR1bits.TMR1IF = 0u;
    app_lcd_tick_pending = 1u;

    if (charge_timer_running != 0u) {
      ++charge_timer_subsecond_ticks;
      if (charge_timer_subsecond_ticks >= APP_TIMER_TICKS_PER_SECOND) {
        charge_timer_subsecond_ticks = 0u;

        if (charge_elapsed_seconds < 59u) {
          ++charge_elapsed_seconds;
        } else if (charge_elapsed_minutes < 59u) {
          charge_elapsed_seconds = 0u;
          ++charge_elapsed_minutes;
        } else if (charge_elapsed_hours < 99u) {
          charge_elapsed_seconds = 0u;
          charge_elapsed_minutes = 0u;
          ++charge_elapsed_hours;
        } else {
          charge_elapsed_seconds = 0u;
          charge_elapsed_minutes = 59u;
        }
      }
    }
  }

  if ((INTCONbits.T0IE != 0u) && (INTCONbits.T0IF != 0u)) {
    TMR0 = (unsigned char)APP_BUZZER_TMR0_PRELOAD;
    INTCONbits.T0IF = 0u;
    APP_BuzzerTimerTick();
  }
}
// Funcție pentru a sincroniza starea timerului de încărcare cu starea curentă a
static void APP_ChargeTimerSync(void) {
  unsigned char should_run =
      ((charger_state == CHG_CC) || (charger_state == CHG_CV) ||
       (Control_IsDischarging() != 0u))
          ? 1u
          : 0u;
  unsigned char interrupt_state = INTCONbits.GIE;

  INTCONbits.GIE = 0u;
  if (should_run != 0u) {
    if (charge_timer_running == 0u) {
      charge_elapsed_hours = 0u;
      charge_elapsed_minutes = 0u;
      charge_elapsed_seconds = 0u;
      charge_timer_subsecond_ticks = 0u;
      charge_timer_running = 1u;
    }
  } else if (charge_timer_running != 0u) {
    charge_timer_running = 0u;
    charge_timer_subsecond_ticks = 0u;
  }
  INTCONbits.GIE = interrupt_state;
}
// Funcție pentru a reseta timerul de încărcare și a șterge valorile de timp
static void APP_ChargeTimerReset(void) {
  unsigned char interrupt_state = INTCONbits.GIE;

  INTCONbits.GIE = 0u;
  charge_timer_running = 0u;
  charge_timer_subsecond_ticks = 0u;
  charge_elapsed_hours = 0u;
  charge_elapsed_minutes = 0u;
  charge_elapsed_seconds = 0u;
  INTCONbits.GIE = interrupt_state;
}
// Funcție pentru a verifica dacă a trecut un tick de 25 ms și a reseta starea
static unsigned char APP_Timer1PeriodElapsed(void) {
  unsigned char tick_pending;
  unsigned char interrupt_state = INTCONbits.GIE;

  INTCONbits.GIE = 0u;
  tick_pending = app_lcd_tick_pending;
  app_lcd_tick_pending = 0u;
  INTCONbits.GIE = interrupt_state;
  return tick_pending;
}
// Funcție pentru a afișa timpul scurs pe LCD în formatul "HH:MM:SS" sau "MM:SS"
static void APP_LCD_PrintElapsedTime(void) {
  unsigned char hours;
  unsigned char minutes;
  unsigned char seconds;
  unsigned char interrupt_state = INTCONbits.GIE;
  unsigned char buzzer_interrupt_enabled = INTCONbits.T0IE;

  INTCONbits.GIE = 0u;          // Dezactivează întreruperile pentru a preveni
                                // modificările în timpul citirii valorilor
  hours = charge_elapsed_hours; // Salvează valorile curente ale orelor,
                                // minutelor și secundelor
  minutes = charge_elapsed_minutes; // Salvează valorile curente ale orelor,
                                    // minutelor și secundelor
  seconds = charge_elapsed_seconds; // Salvează valorile curente ale orelor,
                                    // minutelor și secundelor
  INTCONbits.T0IE = 0u;
  INTCONbits.GIE = interrupt_state;

  /* LCD_SetCursor ha un apel imbricat in plus fata de restul scrierilor. */
  LCD_SetCursor(0u, 11u); // Setează cursorul pe rândul 0, coloana 11 pentru a
                          // afișa timpul scurs
  INTCONbits.T0IE = buzzer_interrupt_enabled;
  if (hours ==
      0u) { // Dacă nu au trecut ore, afișează timpul în formatul "MM:SS"
    LCD_Char((unsigned char)('0' + (minutes / 10u)));
    LCD_Char((unsigned char)('0' + (minutes % 10u)));
    LCD_Char(':');
    LCD_Char((unsigned char)('0' + (seconds / 10u)));
    LCD_Char((unsigned char)('0' + (seconds % 10u)));
  } else { // Dacă au trecut ore, afișează timpul în formatul "HH:MM"
    LCD_Char((unsigned char)('0' + (hours / 10u)));
    LCD_Char((unsigned char)('0' + (hours % 10u)));
    LCD_Char((seconds & 0x01u) == 0u ? ':' : ' ');
    LCD_Char((unsigned char)('0' + (minutes / 10u)));
    LCD_Char((unsigned char)('0' + (minutes % 10u)));
  }
}

/* Filtru doar pentru LCD; valorile folosite de control raman nemodificate. */
static unsigned long APP_FilterDisplayValue(unsigned long sample,
                                            unsigned long *filtered_value,
                                            unsigned char *initialized) {
  unsigned long difference;
  unsigned long step;
  // Daca filtrul nu a fost initializat, seteaza valoarea filtrata la prima
  // esantionare
  if (*initialized == 0u) { // Daca filtrul nu a fost initializat, seteaza
                            // valoarea filtrata la prima esantionare
    *filtered_value = sample;
    *initialized = 1u;
  } else if (sample > *filtered_value) { // Daca esantionul este mai mare decat
                                         // valoarea filtrata, calculeaza
                                         // diferenta si pasul de actualizare
    difference = sample - *filtered_value;
    step = difference >> 2;
    if (step != 0UL) { // Daca pasul de actualizare este diferit de zero,
                       // actualizeaza valoarea filtrata
      *filtered_value += step;
    }
  } else if (sample < *filtered_value) { // Daca esantionul este mai mic decat
                                         // valoarea filtrata, calculeaza
                                         // diferenta si pasul de actualizare
    difference = *filtered_value - sample;
    step = difference >> 2;
    if (step != 0UL) {
      *filtered_value -= step;
    }
  }

  return *filtered_value;
}
// Funcție pentru a afișa măsurătorile pe LCD, inclusiv tensiunea, curentul și
// temperatura
static void APP_LCD_ShowMeasurements(unsigned long voltage_mV,
                                     unsigned long charge_current_mA,
                                     unsigned long discharge_current_mA,
                                     unsigned int temperature_celsius) {
  static CalibrationMessage displayed_message = CALIBRATION_MESSAGE_NONE;
  static unsigned char message_ticks = 0u;
  static unsigned char calibration_screen_drawn = 0u;
  static unsigned long displayed_actual_mV = 0xFFFFFFFFUL;
  static unsigned long displayed_change_mV = 0xFFFFFFFFUL;
  static unsigned long filtered_voltage_mV = 0UL;
  static unsigned long filtered_current_mA = 0UL;
  static unsigned char voltage_filter_initialized = 0u;
  static unsigned char current_filter_initialized = 0u;
  static unsigned char filtered_current_is_discharge = 0xFFu;
  static unsigned int displayed_voltage_cal_factor = 0xFFFFu;
  unsigned char show_discharge_current = Control_IsDischarging();
  unsigned long display_current_mA;

  if (displayed_voltage_cal_factor != voltage_cal_factor) {
    voltage_filter_initialized = 0u;
    displayed_voltage_cal_factor = voltage_cal_factor;
  }

  // Afișează mesajul de calibrare dacă există unul activ
  if (calibration_message != CALIBRATION_MESSAGE_NONE) {
    if (displayed_message != calibration_message) {
      LCD_Command(0x01u);
      LCD_SetCursor(0u, 4u);
      if (calibration_message == CALIBRATION_MESSAGE_SAVED) {
        LCD_String("Saved!");
      } else {
        LCD_String("Cal Error");
      }
      displayed_message = calibration_message;
      message_ticks = 0u;
    }
    // Dacă mesajul de calibrare a fost afișat pentru o perioadă suficientă,
    // resetează
    ++message_ticks;
    if (message_ticks >= 40u) {
      calibration_message = CALIBRATION_MESSAGE_NONE;
      displayed_message = CALIBRATION_MESSAGE_NONE;
      message_ticks = 0u;
      calibration_screen_drawn = 0u;
    }
    return;
  }
  // Dacă nu există un mesaj de calibrare activ, resetează starea afișajului
  if (displayed_message != CALIBRATION_MESSAGE_NONE) {
    displayed_message = CALIBRATION_MESSAGE_NONE;
    calibration_screen_drawn = 0u;
  }
  // Dacă starea UI este de calibrare, afișează valorile de calibrare pe ecran
  if (ui_state == UI_CALIBRATION) {
    if ((calibration_screen_drawn == 0u) ||
        (displayed_actual_mV != cal_actual_mV) ||
        (displayed_change_mV != cal_change_mV)) {
      LCD_SetCursor(0u, 0u);
      LCD_String("Actual:");
      LCD_Print5DigitMillivolts(cal_actual_mV);
      LCD_String("mV  ");

      LCD_SetCursor(1u, 0u);
      LCD_String("Change:");
      LCD_Print5DigitMillivolts(cal_change_mV);
      LCD_String("mV  ");

      displayed_actual_mV = cal_actual_mV;
      displayed_change_mV = cal_change_mV;
      calibration_screen_drawn = 1u;
    }
    return;
  }

  calibration_screen_drawn = 0u; // Resetare stare ecran calibrare
  // Afișează tensiunea și curentul filtrate pe ecran
  display_current_mA =
      (show_discharge_current != 0u) ? discharge_current_mA : charge_current_mA;
  if (filtered_current_is_discharge != show_discharge_current) {
    current_filter_initialized = 0u;
    filtered_current_is_discharge = show_discharge_current;
  }
  // Aplicați filtrul pentru a obține valori mai stabile pentru afișaj
  voltage_mV = APP_FilterDisplayValue(voltage_mV, &filtered_voltage_mV,
                                      &voltage_filter_initialized);
  display_current_mA = APP_FilterDisplayValue(
      display_current_mA, &filtered_current_mA, &current_filter_initialized);

  // Timer1 furnizeaza tick-uri de aproximativ 25 ms.
  static unsigned char blink_ticks = 0u;
  static unsigned char show_blink = 1;

  ++blink_ticks; // Incrementarea contorului de tick-uri pentru a gestiona
                 // clipirea
  if (blink_ticks >= (show_blink != 0u ? 20u : 8u)) {
    blink_ticks = 0u;
    show_blink = !show_blink;
  }

  // ==========================================
  // R0: chimie sau CelX clipeste la selectia celulelor, apoi tensiunea/timpul.
  // ==========================================
  LCD_SetCursor(0u, 0u);
  if (ui_state ==
      UI_EDIT_CELLS) { // Dacă starea UI este de editare a celulelor, afișează
                       // numărul de celule selectat
    if (show_blink !=
        0u) { // Dacă este timpul să afișeze numărul de celule, afișează "CelX"
      LCD_String("Cel");
      LCD_Char((unsigned char)(cell_count + '0'));
    } else { // Dacă este timpul să ascundă numărul de celule, afișează spații
             // goale pentru a crea efectul de clipire
      LCD_String("    ");
    }
  } else if ((ui_state == UI_EDIT_TYPE) &&
             (show_blink ==
              0u)) { // Dacă starea UI este de editare a tipului de baterie și
                     // este timpul să ascundă tipul, afișează spații goale
                     // pentru a crea efectul de clipire
    LCD_String("    ");
  } else { //  Dacă nu este în starea de editare a celulelor sau a tipului de
           //  baterie, afișează numele tipului de baterie curent selectat
    LCD_String((char *)battery_names[current_battery_type]);
  }

  LCD_SetCursor(0u, 4u);
  LCD_Char(' ');
  LCD_PrintVoltage(voltage_mV);
  LCD_SetCursor(0u, 10u);
  LCD_Char(' ');
  APP_LCD_PrintElapsedTime();

  LCD_SetCursor(1u, 0u);
  if ((ui_state == UI_EDIT_MODE) &&
      (show_blink == 0u)) { // Dacă starea UI este de editare a modului de
                            // operare și este timpul să
    LCD_String("   ");
  } else { // Afișează modul de operare curent (încărcare sau descărcare)
    LCD_String((operation_mode == OPERATION_DISCHARGE) ? "DIS" : "CHG");
  }
  LCD_PrintVoltageBar(voltage_mV, Control_GetDischargeCutoffVoltage_mV(),
                      Control_GetTargetCVVoltage_mV());
  if (temperature_celsius >
      99u) { // Dacă temperatura depășește 99°C, afișează 99°C pentru a evita
             // depășirea limitelor de afișare
    temperature_celsius = 99u;
  }
  LCD_Char((unsigned char)('0' + (temperature_celsius / 10u)));
  LCD_Char((unsigned char)('0' + (temperature_celsius % 10u)));
  LCD_Char(LCD_CHAR_DEGREE);
  // Afișează temperatura în grade Celsius, folosind simbolul de grad.
  if (ui_state == UI_EDIT_CURRENT) {
    if (show_blink !=
        0u) { // Dacă starea UI este de editare a curentului și este timpul să
              // afișeze curentul țintă, afișează curentul țintă
      LCD_PrintCurrent((unsigned long)target_current_mA);
    } else { // Dacă este timpul să ascundă curentul țintă, afișează spații
             // goale pentru a crea efectul de clipire
      LCD_String("     ");
    }
  } else { // Dacă nu este în starea de editare a curentului, afișează curentul
           // curent (încărcare sau descărcare)
    LCD_PrintCurrent(display_current_mA);
  }
}
//  Funcția principală
int main(void) {
  unsigned int adc_voltage;
  unsigned int adc_charge_current;
  unsigned int adc_discharge_current;
  unsigned long voltage_mV;
  unsigned long charge_current_mA;
  ChargerState state_before_control; // Starea încărcătorului înainte de rularea
                                     // controlului
  ChargerState charger_state_before_input; // Starea încărcătorului înainte de
                                           // procesarea inputului de la butoane
  CalibrationMessage
      calibration_message_before_input; // Mesajul de calibrare înainte de
                                        // procesarea inputului de la butoane

  SYSTEM_Init(); // Inițializează sistemul (configurare oscilator, pini, etc.)
  APP_BuzzerInit(); // Inițializează buzzer-ul pentru semnalizare audio
  ADC_Init();       // Inițializează modulul ADC pentru citirea tensiunii și
                    // curentului
  EEPROM_LoadCalibration(); // Încarcă datele de calibrare din EEPROM
  BUTTONS_Init(); // Inițializează butoanele pentru interfața cu utilizatorul
  LCD_Init();     // Inițializează afișajul LCD pentru afișarea informațiilor
  Menu_Init(); // Inițializează meniul pentru selecția tipului de baterie și a
               // celulelor
  APP_Timer1Init(); // Inițializează Timer1 pentru gestionarea tick-urilor și a
                    // temporizării
  APP_BuzzerRequest(APP_BUZZER_PATTERN_BOOT); // Solicita un pattern de buzzer
                                              // pentru semnalizarea pornirii
  // Afișează un mesaj de bun venit pe LCD pentru 1,5 secunde
  LCD_Command(0x01u);
  LCD_SetCursor(0u, 1u);
  LCD_String("Smart Charger");
  LCD_SetCursor(1u, 1u);
  LCD_String("YO6PIR - 2026");
  __delay_ms(1500);
  LCD_Command(0x01u);
  // Afișează un mesaj de pornire pe LCD pentru 1,5 secunde
  while (1) {

    // 1. ACHIZIȚIE HARDWARE (Citire ADC)
    adc_voltage = ADC_Read(ADC_INPUT_VOLTAGE); // Citire tensiune baterie
    adc_charge_current =
        ADC_Read(ADC_INPUT_CHARGE_CURRENT); // Citire curent de încărcare
    adc_discharge_current = 0u; // Inițializare curent de descărcare la 0
    if (Control_IsDischarging() !=
        0u) { // Dacă este activă descărcarea, citire curent de descărcare
      adc_discharge_current = ADC_ReadDischargeCurrentRaw();
    }
    adc_temperature_raw = ADC_ReadTemperatureRaw(); // Citire temperatură
    adc_temperature_celsius = ADC_ConvertTemperature_C(
        adc_temperature_raw); // Conversie temperatură în grade Celsius

    unsigned long voltage_mV =
        ADC_ConvertVoltage_mV(adc_voltage); // Conversie tensiune în milivolți
    unsigned long charge_current_mA = ADC_ConvertChargeCurrent_mA(
        adc_charge_current); // Conversie curent de încărcare în miliamperi
    unsigned long discharge_current_mA =
        (Control_IsDischarging() != 0u)
            ? ADC_ConvertDischargeCurrent_mA(adc_discharge_current)
            : 0UL;

    // Alias-uri pentru compatibilitate cu restul codului
    unsigned long v_batt_mV = voltage_mV;
    unsigned long i_chg_mA = charge_current_mA;

    // 2. CITIRE BUTOANE CENTRAL
    ButtonType action =
        BUTTONS_Read(); // Citire acțiune de la butoane (OK, ESC, +, -)
    charger_state_before_input =
        charger_state; // Salvează starea încărcătorului înainte de procesarea
                       // inputului de la butoane
    calibration_message_before_input =
        calibration_message; // Salvează mesajul de calibrare înainte de
                             // procesarea inputului de la butoane
    if ((action == BTN_PLUS_PRESSED) ||
        (action == BTN_MINUS_PRESSED)) { // Dacă a fost apăsat butonul + sau -,
                                         // solicită un ton scurt de buzzer
      APP_BuzzerRequest(APP_BUZZER_PATTERN_SHORT);
    }

    // 3. GESTIONARE MENIU (Se ocupă de selecția tipului de baterie și
    // celulelor cu OK/+/_/-)
    Menu_HandleInput((unsigned char)action, v_batt_mV);
    if ((action == BTN_OK_PRESSED) && (charger_state_before_input != CHG_CC) &&
        (charger_state_before_input != CHG_CV) && (charger_state == CHG_CC)) {
      APP_BuzzerRequest(APP_BUZZER_PATTERN_EXTRA_LONG);
    } else if ((action == BTN_ESC_PRESSED) &&
               ((charger_state_before_input == CHG_CC) ||
                (charger_state_before_input == CHG_CV))) {
      APP_BuzzerRequest(APP_BUZZER_PATTERN_EXTRA_LONG);
    } else if ((action == BTN_OK_PRESSED) || (action == BTN_ESC_PRESSED) ||
               (action == BTN_PLUS_OK_PRESSED) ||
               ((calibration_message_before_input !=
                 CALIBRATION_MESSAGE_SAVED) &&
                (calibration_message == CALIBRATION_MESSAGE_SAVED))) {
      /* Salvarea confirmata produce acelasi ton lung ca OK-ul de meniu. */
      APP_BuzzerRequest(APP_BUZZER_PATTERN_LONG);
    }

    // 4. Oprire manuală (ESC); pornirea este gestionată de meniul complet.
    if (action == BTN_ESC_PRESSED) {
      charger_state = CHG_IDLE; // Forțează starea de repaus
      current_duty = 0;         // Resetează duty cycle
      PWM_SetDutyCycle(0);      // Oprește PWM-ul fizic
      APP_ChargeTimerReset();
    }

    APP_ChargeTimerSync();

    // 5. RULĂM MAȘINA DE STĂRI AUTOMATĂ CC/CV
    state_before_control = charger_state; // Salvează starea încărcătorului
                                          // înainte de rularea controlului
    Control_Run(v_batt_mV, i_chg_mA);     // Rulăm controlul de încărcare CC/CV
    Control_RunDischarge(
        v_batt_mV,
        discharge_current_mA); // Rulăm controlul de descărcare dacă este activ
    APP_ChargeTimerSync(); // Sincronizăm timerul de încărcare cu starea curentă
                           // a controlului
    // 6. Semnalizare audio la trecerea de la CC/CV la DONE sau IDLE (dacă nu a
    if (((state_before_control == CHG_CC) ||
         (state_before_control == CHG_CV)) &&
        ((charger_state == CHG_DONE) ||
         ((charger_state == CHG_IDLE) && (action != BTN_ESC_PRESSED)))) {
      APP_BuzzerRequest(APP_BUZZER_PATTERN_EXTRA_LONG);
    }

    // 6. Actualizare LCD la fiecare tick Timer1 de aproximativ 25 ms.
    if (APP_Timer1PeriodElapsed() != 0u) {
      APP_LCD_ShowMeasurements(voltage_mV, charge_current_mA,
                               discharge_current_mA, adc_temperature_celsius);
    }
    __delay_ms(30);
  }
  return 0;
}
