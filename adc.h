#ifndef ADC_H
#define ADC_H
// Definirea numărului de intrări ADC și a indexurilor pentru fiecare intrare
#define ADC_INPUT_COUNT 3u
#define ADC_INPUT_VOLTAGE 0u
#define ADC_INPUT_CHARGE_CURRENT 1u
#define ADC_INPUT_TEMPERATURE 2u
// Variabile globale pentru valorile ADC citite
extern unsigned int adc_temperature_raw;
extern unsigned int adc_temperature_celsius;
// Funcții pentru inițializarea și citirea ADC
void ADC_Init(void);
unsigned int ADC_Read(unsigned char input);
unsigned int ADC_ReadDischargeCurrentRaw(void);
unsigned int ADC_ReadTemperatureRaw(void);
unsigned int ADC_ConvertTemperature_C(unsigned int adc_value);
unsigned long ADC_ConvertVoltage_mV(unsigned int adc_value);
unsigned long ADC_ConvertChargeCurrent_mA(unsigned int adc_value);
unsigned long ADC_ConvertDischargeCurrent_mA(unsigned int adc_value);

#endif
