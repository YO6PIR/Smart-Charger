#ifndef CONTROL_H
#define CONTROL_H

typedef enum { CHG_IDLE = 0, CHG_CC, CHG_CV, CHG_DONE } ChargerState;

extern ChargerState charger_state;
extern unsigned int current_duty; // <--- Adaugă această linie
// Funcții pentru controlul încărcării și descărcării
void Control_Run(unsigned long batt_voltage_mV,
                 unsigned long charge_current_mA);
unsigned long Control_GetMinimumStartVoltage_mV(void);
unsigned long Control_GetTargetCVVoltage_mV(void);
unsigned long Control_GetDischargeCutoffVoltage_mV(void);
unsigned char Control_StartDischarge(unsigned long batt_voltage_mV);
void Control_RunDischarge(unsigned long batt_voltage_mV,
                          unsigned long discharge_current_mA);
void Control_StopDischarge(void);
unsigned char Control_IsDischarging(void);

#endif
