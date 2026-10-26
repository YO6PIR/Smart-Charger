#ifndef BUTTONS_H
#define BUTTONS_H

// Definim tipul de date pentru starea butoanelor
// În buttons.h, în interiorul enum-ului ButtonType:
typedef enum {
  BTN_NONE = 0,
  BTN_PLUS_PRESSED,
  BTN_MINUS_PRESSED,
  BTN_OK_PRESSED,
  BTN_ESC_PRESSED,
  BTN_PLUS_OK_PRESSED
} ButtonType;

// Prototipurile funcțiilor
void BUTTONS_Init(void);
ButtonType BUTTONS_Read(void);

#endif
