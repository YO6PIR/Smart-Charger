#ifndef MENU_H
#define MENU_H
// Definirea tipurilor de baterii
typedef enum {
  BATT_LIION = 0,
  BATT_LIPO,
  BATT_SLA,
  BATT_NIMH,
  BATT_TYPES_TOTAL
} BatteryType;
// Definirea stărilor meniului
typedef enum {
  UI_NORMAL = 0,
  UI_EDIT_TYPE,
  UI_EDIT_CELLS,
  UI_EDIT_CURRENT,
  UI_EDIT_MODE,
  UI_CALIBRATION // Starea de etalonare
} UIState;
// Definirea modurilor de operare pentru încărcare și descărcare
typedef enum { OPERATION_CHARGE = 0, OPERATION_DISCHARGE } OperationMode;
// Mesaje pentru starea de calibrare
typedef enum {
  CALIBRATION_MESSAGE_NONE = 0,
  CALIBRATION_MESSAGE_SAVED,
  CALIBRATION_MESSAGE_ERROR
} CalibrationMessage;
// Variabile globale pentru starea meniului și setările bateriei
extern BatteryType current_battery_type;
extern unsigned char cell_count;
extern unsigned int target_current_mA;
extern UIState ui_state;
extern OperationMode operation_mode;
extern CalibrationMessage calibration_message;
extern const char *battery_names[];
// Definirea tipurilor de chimie pentru baterii
#define CHEM_LI_ION 0u
#define CHEM_NIMH 1u
#define CHEM_SLA 2u

// Variabile și funcții globale pentru calibrare
extern unsigned int voltage_cal_factor;
extern unsigned long cal_actual_mV;
extern unsigned long cal_change_mV;

void Menu_Init(void);
void Menu_HandleInput(unsigned char action, unsigned long v_batt_mV);
unsigned char IntelliSense_DetectCells(unsigned char chemistry,
                                       unsigned int voltage_mV);
void EEPROM_LoadCalibration(void);
unsigned char EEPROM_SaveCalibration(void);

#endif
