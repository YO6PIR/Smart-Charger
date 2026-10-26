#include "control.h"
#include "buttons.h"
#include "menu.h" // Necesar pentru current_battery_type și cell_count
#include "pwm.h"
#include <xc.h>
// În control.c, la început (fără 'static'):

ChargerState charger_state = CHG_IDLE;
// În control.c, la început (fără 'static'):
unsigned int current_duty = 0;

#define FAN_CURRENT_LIMIT_MA                                                   \
  500UL // Pragul de curent pentru a activa ventilatorul (500 mA)
#define DISCHARGE_MAX_POWER_UW 5000000UL
#define FAN_OFF_DELAY_LOOPS 34u      /* 34 x bucla de minimum 30 ms = ~1 s */
#define RED_LED_LONG_PHASE_LOOPS 33u // 33 x bucla de minimum 30 ms = ~1 s
#define RED_LED_SHORT_PHASE_LOOPS 7u // 7 x bucla de minimum 30 ms = ~210 ms

static unsigned char fan_is_on =
    0u; // Variabilă pentru a urmări starea ventilatorului (pornit/oprit)
static unsigned char fan_off_pending =
    0u; // Variabilă pentru a urmări dacă ventilatorul trebuie să fie oprit după
        // o perioadă de timp
static unsigned char fan_off_elapsed_loops =
    0u; // Contor pentru a urmări câte bucle au trecut de la momentul în care
        // ventilatorul ar trebui să fie oprit
static unsigned char charge_blink_counter =
    0u; // Contor pentru a gestiona clipirea LED-ului de încărcare
static unsigned char charge_led_initialized =
    0u; // Variabilă pentru a urmări dacă LED-ul de încărcare a fost inițializat

// Structură pentru parametrii chimici ai fiecărui tip de baterie (valori per
// celulă)
typedef struct {
  unsigned long min_start_mV_per_cell;
  unsigned long cv_voltage_mV_per_cell;
  unsigned long cutoff_current_mA;
  unsigned long discharge_cutoff_mV_per_cell;
} BatteryProfile;

// Tabela corespunde ordinii din enum-ul BatteryType (Li-ion, LiPo, SLA, Ni-MH)
static const BatteryProfile profiles[] = {
    {2500, 4200, 100, 3000}, // BATT_LIION
    {3000, 4200, 100, 2500}, // BATT_LIPO
    {1700, 2350, 50, 1666},  // BATT_SLA
    {1000, 1450, 200, 1000}  // BATT_NIMH
};
// Variabile pentru gestionarea descărcării
static unsigned char discharge_active = 0u;
static unsigned char discharge_duty = 0u;
static unsigned char discharge_blink_counter = 0u;
static unsigned long discharge_target_current_mA = 0UL;

static unsigned long Control_GetPowerLimitedDischargeCurrent_mA(
    unsigned long batt_voltage_mV) {
  unsigned long power_limited_current_mA;

  if (batt_voltage_mV == 0UL) {
    return 0UL;
  }

  /* V(mV) * I(mA) is power in microwatts. */
  power_limited_current_mA =
      DISCHARGE_MAX_POWER_UW / batt_voltage_mV;
  if (power_limited_current_mA > (unsigned long)target_current_mA) {
    return (unsigned long)target_current_mA;
  }

  return power_limited_current_mA;
}

// Funcție internă pentru a seta duty cycle-ul PWM pentru descărcare
unsigned long Control_GetMinimumStartVoltage_mV(void) {
  unsigned long min_start_mV_per_cell =
      profiles[current_battery_type].min_start_mV_per_cell;

  return (min_start_mV_per_cell - 500UL) * (unsigned long)cell_count;
}
// Funcție pentru a obține tensiunea țintă de încărcare CV în milivolți
unsigned long Control_GetTargetCVVoltage_mV(void) {
  return profiles[current_battery_type].cv_voltage_mV_per_cell *
         (unsigned long)cell_count;
}
// Funcție pentru a obține tensiunea de oprire a descărcării în milivolți
unsigned long Control_GetDischargeCutoffVoltage_mV(void) {
  return profiles[current_battery_type].discharge_cutoff_mV_per_cell *
         (unsigned long)cell_count;
}
// Funcție internă pentru a seta duty cycle-ul PWM pentru descărcare
static void Control_SetDischargePwmDuty(unsigned char duty) {
  CCPR2L = (unsigned char)(duty >> 2);
  CCP2CON =
      (unsigned char)((CCP2CON & 0xCFu) | (unsigned char)((duty & 0x03u) << 4));
}
// Funcție pentru a începe procesul de descărcare
unsigned char Control_StartDischarge(unsigned long batt_voltage_mV) {
  if ((discharge_active != 0u) ||
      ((charger_state != CHG_IDLE) && (charger_state != CHG_DONE)) ||
      (batt_voltage_mV <= Control_GetDischargeCutoffVoltage_mV())) {
    return 0u;
  }
  // Configurăm PWM pentru descărcare
  PWM_SetDutyCycle(0u);
  TRISCbits.TRISC1 = 0u;
  PR2 = 0x4Du;
  T2CON = 0x05u;
  CCPR2L = 0u;
  CCP2CON = 0x0Cu;
  discharge_duty = 0u;
  discharge_target_current_mA =
      Control_GetPowerLimitedDischargeCurrent_mA(batt_voltage_mV);
  discharge_blink_counter = 0u;
  PORTDbits.RD3 = 1u;
  PORTDbits.RD2 = 0u;
  discharge_active = 1u;
  return 1u;
}
// Funcție pentru a opri procesul de descărcare
void Control_StopDischarge(void) {
  Control_SetDischargePwmDuty(0u);
  CCP2CON = 0u;
  PORTCbits.RC1 = 0u;
  discharge_duty = 0u;
  discharge_blink_counter = 0u;
  discharge_target_current_mA = 0UL;
  PORTDbits.RD3 = 0u;
  discharge_active = 0u;
}
// Funcție pentru a verifica dacă descărcarea este activă
unsigned char Control_IsDischarging(void) { return discharge_active; }
// Funcție pentru a rula logica de control a încărcării și descărcării
void Control_RunDischarge(unsigned long batt_voltage_mV,
                          unsigned long discharge_current_mA) {
  if (discharge_active == 0u) {
    return;
  }
  // Protecție critică: Dacă tensiunea bateriei scade sub pragul de oprire,
  // oprim descărcarea
  if (batt_voltage_mV <= Control_GetDischargeCutoffVoltage_mV()) {
    Control_StopDischarge();
    return;
  }
  discharge_target_current_mA =
      Control_GetPowerLimitedDischargeCurrent_mA(batt_voltage_mV);

  /* Reduce PWM immediately if measured battery power exceeds the resistor
   * rating; the current regulator then holds the power-limited target. */
  if (discharge_current_mA > 0UL) {
    unsigned long measured_power_uW =
        batt_voltage_mV * discharge_current_mA;
    if (measured_power_uW > DISCHARGE_MAX_POWER_UW) {
      discharge_duty = (unsigned char)(
          ((unsigned long)discharge_duty * DISCHARGE_MAX_POWER_UW) /
          measured_power_uW);
      Control_SetDischargePwmDuty(discharge_duty);
      return;
    }
  }
  // LED-ul Verde (RD2) stă stins în timpul descărcării active
  PORTDbits.RD2 = 0u;
  ++discharge_blink_counter;
  if (discharge_blink_counter >= (PORTDbits.RD3 != 0u
                                      ? RED_LED_SHORT_PHASE_LOOPS
                                      : RED_LED_LONG_PHASE_LOOPS)) {
    discharge_blink_counter = 0u;
    PORTDbits.RD3 = (unsigned char)!PORTDbits.RD3;
  }
  // Algoritmul de control al descărcării bazat pe curentul țintă
  if (discharge_current_mA < discharge_target_current_mA) {
    if (discharge_duty < 255u) {
      ++discharge_duty;
    }
  } else if (discharge_current_mA > discharge_target_current_mA) {
    if (discharge_duty > 0u) {
      --discharge_duty;
    }
  }

  Control_SetDischargePwmDuty(discharge_duty);
}
// Funcție internă pentru a actualiza starea ventilatorului în funcție de
// curentul activ
static void Control_UpdateFan(unsigned long active_current_mA) {
  if (active_current_mA > FAN_CURRENT_LIMIT_MA) {
    PORTDbits.RD1 = 1;
    fan_is_on = 1u;
    fan_off_pending = 0u;
    fan_off_elapsed_loops = 0u;
  } else if (fan_is_on != 0u) {
    if (fan_off_pending == 0u) {
      fan_off_pending = 1u;
      fan_off_elapsed_loops = 0u;
    } else {
      if (fan_off_elapsed_loops < FAN_OFF_DELAY_LOOPS) {
        ++fan_off_elapsed_loops;
      }

      if (fan_off_elapsed_loops >= FAN_OFF_DELAY_LOOPS) {
        PORTDbits.RD1 = 0;
        fan_is_on = 0u;
        fan_off_pending = 0u;
        fan_off_elapsed_loops = 0u;
      }
    }
  } else {
    PORTDbits.RD1 = 0;
    fan_off_pending = 0u;
    fan_off_elapsed_loops = 0u;
  }
}
// Funcție pentru a rula logica de control a încărcării și descărcării
void Control_Run(unsigned long batt_voltage_mV,
                 unsigned long charge_current_mA) {

  // 1. Calculul pragurilor dinamice în funcție de meniu
  unsigned long target_cv_mV = Control_GetTargetCVVoltage_mV();
  unsigned long minimum_run_voltage_mV = Control_GetMinimumStartVoltage_mV();
  unsigned long cutoff_current =
      profiles[current_battery_type].cutoff_current_mA;

  unsigned long active_current_mA =
      (discharge_active != 0u)
          ? (FAN_CURRENT_LIMIT_MA + 1UL)
          : (((charger_state == CHG_CC) || (charger_state == CHG_CV))
                 ? charge_current_mA
                 : 0UL);
  Control_UpdateFan(active_current_mA);

  // --- PROTECȚIE CRITICĂ: Dacă bateria este deconectată brusc în timpul
  // încărcării ---
  if ((charger_state == CHG_CC || charger_state == CHG_CV) &&
      (batt_voltage_mV < minimum_run_voltage_mV)) {
    charger_state = CHG_IDLE;
    current_duty = 0;
    PWM_SetDutyCycle(0);
    return; // Iese imediat din funcție și revine în repaus curat!
  }
  // --- LOGICA PRINCIPALĂ DE CONTROL ---
  switch (charger_state) {
  case CHG_IDLE: // Încărcătorul este în repaus, fără încărcare activă
    PWM_SetDutyCycle(0);

    charge_blink_counter = 0u;
    charge_led_initialized = 0u;
    if (discharge_active == 0u) {
      PORTDbits.RD3 = 0;

      // LED Verde (RD2): Aprins dacă e baterie cuplată (> 700 mV), stins altfel
      if (batt_voltage_mV > 700) {
        PORTDbits.RD2 = 1; // Baterie detectată la borne
      } else {
        PORTDbits.RD2 = 0; // Nicio baterie / Gol
      }
    } else {
      PORTDbits.RD2 = 0;
    }
    break;

  case CHG_CC: // Constant Current
  case CHG_CV: // Constant Voltage (Încărcare activă)

    if (charge_led_initialized == 0u) {
      PORTDbits.RD3 = 1u;
      charge_blink_counter = 0u;
      charge_led_initialized = 1u;
    } else {
      ++charge_blink_counter;
      if (charge_blink_counter >= (PORTDbits.RD3 != 0u
                                       ? RED_LED_LONG_PHASE_LOOPS
                                       : RED_LED_SHORT_PHASE_LOOPS)) {
        charge_blink_counter = 0u;
        PORTDbits.RD3 = (unsigned char)!PORTDbits.RD3;
      }
    }

    // LED-ul Verde (RD2) stă stins în timpul încărcării active
    PORTDbits.RD2 = 0;

    // Algoritmul de CC / CV bazat pe pragurile dinamice:
    if (charger_state == CHG_CC) {
      if (batt_voltage_mV >= target_cv_mV) {
        charger_state = CHG_CV;
        break;
      }
      if (charge_current_mA < (unsigned long)target_current_mA) {
        if (current_duty < 1023)
          current_duty++;
      } else if (charge_current_mA >
                 ((unsigned long)target_current_mA + 50UL)) {
        if (current_duty > 0)
          current_duty--;
      }
    } else { // CHG_CV
      if (charge_current_mA <=
          cutoff_current) { // Sub pragul specific de tăiere -> Terminat!
        charger_state = CHG_DONE;
        current_duty = 0;
        PWM_SetDutyCycle(0);
        break;
      }

      // Reglaj fin în jurul tensiunii țintă calculate (target_cv_mV)
      if (batt_voltage_mV > (target_cv_mV + 10)) {
        if (current_duty > 0)
          current_duty--;
      } else if (batt_voltage_mV < (target_cv_mV - 10)) {
        if (current_duty < 1023)
          current_duty++;
      }
    }

    PWM_SetDutyCycle(current_duty);
    break;

  case CHG_DONE: // Încărcare finalizată cu succes
    PWM_SetDutyCycle(0);

    charge_blink_counter = 0u;
    charge_led_initialized = 0u;
    if (discharge_active == 0u) {
      // Semnalizarea finală:
      PORTDbits.RD3 = 0; // Se stinge ROȘU pe RD3
      PORTDbits.RD2 = 1; // Se aprinde VERDE pe RD2 continuu
    } else {
      PORTDbits.RD2 = 0u;
    }
    break;
  }
}
