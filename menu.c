#include "menu.h"
#include "buttons.h"
#include "control.h"
#include "pwm.h"
#include <xc.h>

// Adresele în EEPROM pentru factorul de calibrare (salvăm un uint16_t pe 2
// octeți)
#define EEPROM_CAL_ADDR_L 0x00
#define EEPROM_CAL_ADDR_H 0x01

#define TARGET_CURRENT_MIN_MA 100u
#define TARGET_CURRENT_MAX_MA 1000u
#define TARGET_CURRENT_STEP_MA 100u
#define INTELLISENSE_MAX_CELLS 5u
#define CHEM_UNSUPPORTED 0xFFu
#define VOLTAGE_CAL_FACTOR_MIN 500u
#define VOLTAGE_CAL_FACTOR_MAX 1500u

// Definirea variabilelor globale pentru calibrare
unsigned int voltage_cal_factor = 1000;
unsigned long cal_actual_mV = 0;
unsigned long cal_change_mV = 0;

// Funcție nativă pentru citirea unui byte din EEPROM-ul PIC16F877A
unsigned char EEPROM_Read(unsigned char address) {
  EEADR = address;
  EECON1bits.EEPGD = 0; // Selectează memoria de date EEPROM (nu Flash)
  EECON1bits.RD = 1;    // Pornește citirea
  return EEDATA;        // Returnează datele citite
}

// Funcție nativă pentru scrierea unui byte în EEPROM-ul PIC16F877A (cu secvența
// de siguranță)
void EEPROM_Write(unsigned char address, unsigned char data) {
  EEADR = address;
  EEDATA = data;
  EECON1bits.EEPGD = 0; // Selectează memoria de date EEPROM
  EECON1bits.WREN = 1;  // Permite scrierea

  // Secvența obligatorie de deblocare Microchip
  unsigned char int_status = INTCONbits.GIE;
  INTCONbits.GIE = 0; // Dezactivează întreruperile temporar
  EECON2 = 0x55;
  EECON2 = 0xAA;
  EECON1bits.WR = 1; // Inițiază ciclul de scriere

  // Așteaptă finalizarea scrierii în EEPROM
  while (EECON1bits.WR)
    ;

  EECON1bits.WREN = 0;         // Blochează din nou scrierile accidental
  INTCONbits.GIE = int_status; // Restaurează starea întreruperilor
}

// Funcție de citire a factorului din EEPROM la pornire (folosește funcția
// nativă corectă)
void EEPROM_LoadCalibration(void) {
  unsigned char low = EEPROM_Read(EEPROM_CAL_ADDR_L);
  unsigned char high = EEPROM_Read(EEPROM_CAL_ADDR_H);
  voltage_cal_factor = ((unsigned int)high << 8) | low;

  // Dacă EEPROM-ul este neșters (valoare 0xFFFF) sau invalid, setăm implicit
  // 1000
  if (voltage_cal_factor == 0xFFFF || voltage_cal_factor == 0 ||
      voltage_cal_factor > VOLTAGE_CAL_FACTOR_MAX ||
      voltage_cal_factor < VOLTAGE_CAL_FACTOR_MIN) {
    voltage_cal_factor = 1000;
  }
}

// Funcție de salvare a factorului în EEPROM la apăsarea OK (folosește funcția
// nativă corectă)
unsigned char EEPROM_SaveCalibration(void) {
  EEPROM_Write(EEPROM_CAL_ADDR_L, (unsigned char)(voltage_cal_factor & 0xFF));
  EEPROM_Write(EEPROM_CAL_ADDR_H, (unsigned char)(voltage_cal_factor >> 8));
  return (unsigned char)(
      (EEPROM_Read(EEPROM_CAL_ADDR_L) ==
       (unsigned char)(voltage_cal_factor & 0xFF)) &&
      (EEPROM_Read(EEPROM_CAL_ADDR_H) ==
       (unsigned char)(voltage_cal_factor >> 8)));
}

// Definirea variabilelor globale ale meniului
BatteryType current_battery_type = BATT_LIION;
unsigned char cell_count = 1; // Val implicită: 1 celulă (1-5)
unsigned int target_current_mA = TARGET_CURRENT_MAX_MA;
UIState ui_state = UI_NORMAL;
OperationMode operation_mode = OPERATION_CHARGE;
CalibrationMessage calibration_message = CALIBRATION_MESSAGE_NONE;

// Numele chimiei ocupă exact 4 caractere pe LCD.
const char *battery_names[] = {"LiIo", "LiPo", "SLA ", "NiMh"};

unsigned char IntelliSense_DetectCells(unsigned char chemistry,
                                       unsigned int voltage_mV) {
  unsigned long min_per_cell_mV;
  unsigned long max_per_cell_mV;
  unsigned char detected_cells = 1u;
  unsigned char cells;
  // Determină plaja de tensiune per celulă în funcție de chimia bateriei
  switch (chemistry) {
  case CHEM_LI_ION:
    min_per_cell_mV = 2700UL;
    max_per_cell_mV = 4250UL;
    break;

  case CHEM_NIMH:
    min_per_cell_mV = 1000UL;
    max_per_cell_mV = 1450UL;
    break;

  case CHEM_SLA:
    min_per_cell_mV = 1700UL;
    max_per_cell_mV = 2350UL;
    break;

  default:
    return detected_cells;
  }
  // Caută numărul de celule care se încadrează în plaja de tensiune
  for (cells = 1u; cells <= INTELLISENSE_MAX_CELLS; ++cells) {
    unsigned long min_total_mV = (unsigned long)cells * min_per_cell_mV;
    unsigned long max_total_mV = (unsigned long)cells * max_per_cell_mV;
    // Dacă tensiunea citită se încadrează în plaja pentru acest număr de
    // celule,
    if (((unsigned long)voltage_mV >= min_total_mV) &&
        ((unsigned long)voltage_mV <= max_total_mV)) {
      return cells;
    }
  }

  return detected_cells;
}
// Funcție internă pentru a determina chimia bateriei pe baza tipului selectat
static unsigned char Menu_GetIntelliSenseChemistry(void) {
  switch (current_battery_type) {
  case BATT_LIION:
  case BATT_LIPO:
    /* LiPo foloseste aceeasi plaja de detectie ca Li-ion (4.2 V/celula). */
    return CHEM_LI_ION;

  case BATT_NIMH:
    return CHEM_NIMH;

  case BATT_SLA:
    return CHEM_SLA;

  default:
    return CHEM_UNSUPPORTED;
  }
}
// Funcție pentru a afișa timpul scurs pe LCD în formatul "MM:SS" sau "HH:MM"
void Menu_Init(void) {
  current_battery_type = BATT_LIION;
  cell_count = 1;
  target_current_mA = TARGET_CURRENT_MAX_MA;
  ui_state = UI_NORMAL;
  operation_mode = OPERATION_CHARGE;
  EEPROM_LoadCalibration(); // Încărcăm factorul de calibrare la pornire
}
// Funcție pentru a gestiona inputul de la butoane și a modifica starea meniului
void Menu_HandleInput(unsigned char action, unsigned long v_batt_mV) {
  if (Control_IsDischarging() != 0u) {
    if (action == BTN_ESC_PRESSED) {
      Control_StopDischarge();
    }
    return;
  }

  // Meniul de configurare este activ doar când încărcătorul este în IDLE sau
  // DONE
  if (charger_state == CHG_IDLE || charger_state == CHG_DONE) {

    switch (ui_state) {

    case UI_NORMAL:
      // Apăsare simultană PLUS + OK pentru a intra în calibrare
      if (action == BTN_PLUS_OK_PRESSED) {
        ui_state = UI_CALIBRATION;
        calibration_message = CALIBRATION_MESSAGE_NONE;
        cal_actual_mV = v_batt_mV; // Înghețăm valoarea actuală citită la borne
        cal_change_mV =
            cal_actual_mV; // Pornim valoarea de schimb de la cea actuală

      } else if (action == BTN_OK_PRESSED) {
        ui_state = UI_EDIT_TYPE;
      }
      break;

    case UI_CALIBRATION:
      // În ecranul de calibrare, tastele + și - modifică valoarea Change cu pas
      // de 10mV
      if (action == BTN_PLUS_PRESSED) {
        cal_change_mV += 10;
      } else if (action == BTN_MINUS_PRESSED) {
        if (cal_change_mV >= 10)
          cal_change_mV -= 10;
      } else if (action == BTN_OK_PRESSED) {
        unsigned int previous_factor = voltage_cal_factor;

        if (cal_actual_mV > 0) {
          unsigned long new_factor =
              (cal_change_mV * (unsigned long)previous_factor) /
              cal_actual_mV;

          if ((new_factor >= VOLTAGE_CAL_FACTOR_MIN) &&
              (new_factor <= VOLTAGE_CAL_FACTOR_MAX)) {
            voltage_cal_factor = (unsigned int)new_factor;
            if (EEPROM_SaveCalibration() != 0u) {
              calibration_message = CALIBRATION_MESSAGE_SAVED;
            } else {
              voltage_cal_factor = previous_factor;
              calibration_message = CALIBRATION_MESSAGE_ERROR;
            }
          } else {
            calibration_message = CALIBRATION_MESSAGE_ERROR;
          }
        } else {
          calibration_message = CALIBRATION_MESSAGE_ERROR;
        }
        ui_state = UI_NORMAL; // Ieșim în meniul normal
      } else if (action == BTN_ESC_PRESSED) {
        // ESC anulează tot, NU salvează și NU modifică nimic!
        ui_state = UI_NORMAL;
      }
      break;

    // --- STAREA 1: Editare Tip Baterie ---
    case UI_EDIT_TYPE:
      if (action == BTN_PLUS_PRESSED) {
        current_battery_type++;
        if (current_battery_type >= BATT_TYPES_TOTAL) {
          current_battery_type = BATT_LIION; // Ciclic
        }
      } else if (action == BTN_MINUS_PRESSED) {
        if (current_battery_type == 0) {
          current_battery_type = BATT_TYPES_TOTAL - 1;
        } else {
          current_battery_type--;
        }
      } else if (action == BTN_OK_PRESSED) {
        /* v_batt_mV was sampled and converted earlier in this loop. */
        cell_count = IntelliSense_DetectCells(Menu_GetIntelliSenseChemistry(),
                                              (unsigned int)v_batt_mV);
        ui_state = UI_EDIT_CELLS;
      } else if (action == BTN_ESC_PRESSED) {
        ui_state = UI_NORMAL; // Anulează și revine
      }
      break;

    // --- STAREA 2: Editare Număr Celule ---
    case UI_EDIT_CELLS:
      if (action == BTN_PLUS_PRESSED) {
        if (cell_count < 5)
          cell_count++; // Maxim 5 celule
      } else if (action == BTN_MINUS_PRESSED) {
        if (cell_count > 1)
          cell_count--; // Minim 1 celulă
      } else if (action == BTN_OK_PRESSED) {
        ui_state = UI_EDIT_CURRENT;
      } else if (action == BTN_ESC_PRESSED) {
        ui_state = UI_EDIT_TYPE; // Revine la editarea tipului
      }
      break;

    // --- STAREA 4: Editare Curent Tinta ---
    case UI_EDIT_CURRENT:
      if (action == BTN_PLUS_PRESSED) {
        if (target_current_mA <=
            (TARGET_CURRENT_MAX_MA - TARGET_CURRENT_STEP_MA)) {
          target_current_mA += TARGET_CURRENT_STEP_MA;
        }
      } else if (action == BTN_MINUS_PRESSED) {
        if (target_current_mA >=
            (TARGET_CURRENT_MIN_MA + TARGET_CURRENT_STEP_MA)) {
          target_current_mA -= TARGET_CURRENT_STEP_MA;
        }
      } else if (action == BTN_OK_PRESSED) {
        ui_state = UI_EDIT_MODE;
      } else if (action == BTN_ESC_PRESSED) {
        ui_state = UI_EDIT_CELLS;
      }
      break;
      // --- STAREA 5: Editare Mod Operare ---
    case UI_EDIT_MODE:
      if ((action == BTN_PLUS_PRESSED) || (action == BTN_MINUS_PRESSED)) {
        operation_mode = (operation_mode == OPERATION_CHARGE)
                             ? OPERATION_DISCHARGE
                             : OPERATION_CHARGE;
      } else if (action == BTN_OK_PRESSED) {
        if (operation_mode == OPERATION_DISCHARGE) {
          if (Control_StartDischarge(v_batt_mV) != 0u) {
            ui_state = UI_NORMAL;
          }
        } else {
          ui_state = UI_NORMAL;
          if (v_batt_mV > Control_GetMinimumStartVoltage_mV()) {
            PWM_SetDutyCycle(0u);
            PWM_Init();
            charger_state = CHG_CC;
            current_duty = 20;
            PWM_SetDutyCycle(current_duty);
          }
        }
      } else if (action == BTN_ESC_PRESSED) {
        ui_state = UI_EDIT_CURRENT;
      }
      break;
    }
  } else {
    // Dacă încărcătorul este în timpul încărcării (CC / CV), tasta ESC anulează
    // tot
    if (action == BTN_ESC_PRESSED) {
      charger_state = CHG_IDLE;
      current_duty = 0;
      PWM_SetDutyCycle(0);
      ui_state = UI_NORMAL;
    }
  }
}
