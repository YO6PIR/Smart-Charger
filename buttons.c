#include "buttons.h"
#include "config.h" // Include fișierul de configurare pentru definiții și setări
#include <xc.h>

// Definirea pinilor pentru butoane (presupunând logică activă pe 0 / active-low
// cu pull-up)
#define BTN_ESC PORTDbits.RD4
#define BTN_PLUS PORTDbits.RD6
#define BTN_MINUS PORTDbits.RD5
#define BTN_OK PORTDbits.RD7

enum {
  BTN_LOCK_PLUS = 0x01u,
  BTN_LOCK_OK = 0x02u,
  BTN_LOCK_ESC = 0x04u,
  BTN_LOCK_MINUS = 0x08u
};

static unsigned char button_release_lock = 0u;
// Funcție de inițializare a butoanelor (setează pinii ca intrări)
void BUTTONS_Init(void) {
  // Setează biții 4, 5, 6, 7 ai PORTD ca intrări (1)
  TRISD |= 0xF0;
  button_release_lock = 0u;
}

// Funcție de citire cu debounce simplu
ButtonType BUTTONS_Read(void) {
  unsigned char plus_pressed;
  unsigned char ok_pressed;
  unsigned char wait_ticks;
  // Verificăm dacă există un blocaj de eliberare a butoanelor
  if (button_release_lock !=
      0u) { // Dacă există un blocaj, verificăm dacă butoanele au fost eliberate
    if ((button_release_lock & BTN_LOCK_PLUS) != 0u && BTN_PLUS != 0) {
      button_release_lock &= (unsigned char)~BTN_LOCK_PLUS;
    }
    if ((button_release_lock & BTN_LOCK_OK) != 0u &&
        BTN_OK != 0) { // Dacă butonul OK a fost blocat și acum este eliberat,
                       // eliminăm blocajul
      button_release_lock &= (unsigned char)~BTN_LOCK_OK;
    }
    if ((button_release_lock & BTN_LOCK_ESC) != 0u &&
        BTN_ESC != 0) { // Dacă butonul ESC a fost blocat și acum este eliberat,
                        // eliminăm blocajul
      button_release_lock &= (unsigned char)~BTN_LOCK_ESC;
    }
    if ((button_release_lock & BTN_LOCK_MINUS) != 0u &&
        BTN_MINUS != 0) { // Dacă butonul MINUS a fost blocat și acum este
                          // eliberat, eliminăm blocajul
      button_release_lock &= (unsigned char)~BTN_LOCK_MINUS;
    }
    if (button_release_lock !=
        0u) { // Dacă încă există blocaje, nu citim alte butoane
      return BTN_NONE;
    }
  }

  // Detectăm PLUS + OK chiar dacă tastele sunt apăsate la câteva momente
  // distanță una de cealaltă.
  if ((BTN_PLUS == 0) || (BTN_OK == 0)) { //
    __delay_ms(20);
    plus_pressed = (BTN_PLUS == 0);
    ok_pressed = (BTN_OK == 0);
    //  Verificăm dacă ambele butoane sunt apăsate simultan
    if (plus_pressed != 0u || ok_pressed != 0u) {
      for (wait_ticks = 0u; wait_ticks < 20u; ++wait_ticks) {
        if ((BTN_PLUS == 0) && (BTN_OK == 0)) {
          __delay_ms(20);
          if ((BTN_PLUS == 0) && (BTN_OK == 0)) {
            button_release_lock = 0u;
            if (BTN_PLUS == 0) {
              button_release_lock |= BTN_LOCK_PLUS;
            }
            if (BTN_OK == 0) {
              button_release_lock |= BTN_LOCK_OK;
            }
            return BTN_PLUS_OK_PRESSED;
          }
        }
        // Dacă unul dintre butoane a fost eliberat, ieșim din buclă
        if ((plus_pressed != 0u && BTN_PLUS != 0) ||
            (ok_pressed != 0u && BTN_OK != 0)) {
          break;
        }
        __delay_ms(10);
      }
      // Dacă am ieșit din buclă fără să detectăm ambele butoane apăsate,
      // continuăm
      if (plus_pressed != 0u) {
        if (BTN_PLUS == 0) {
          button_release_lock |= BTN_LOCK_PLUS;
        }
        return BTN_PLUS_PRESSED;
      }
      // Dacă am ieșit din buclă fără să detectăm ambele butoane apăsate,
      // continuăm
      if (ok_pressed != 0u) {
        if (BTN_OK == 0) {
          button_release_lock |= BTN_LOCK_OK;
        }
        return BTN_OK_PRESSED;
      }
    }
  }

  // Verificăm butonul ESC (RD4)
  if (BTN_ESC == 0) {
    __delay_ms(20); // Anti-bârnâit
    if (BTN_ESC == 0) {
      button_release_lock |= BTN_LOCK_ESC;
      return BTN_ESC_PRESSED;
    }
  }

  // Verificăm butonul PLUS (RD5)
  if (BTN_PLUS == 0) {
    __delay_ms(20);
    if (BTN_PLUS == 0) {
      button_release_lock |= BTN_LOCK_PLUS;
      return BTN_PLUS_PRESSED;
    }
  }

  // Verificăm butonul MINUS (RD6)
  if (BTN_MINUS == 0) {
    __delay_ms(20);
    if (BTN_MINUS == 0) {
      button_release_lock |= BTN_LOCK_MINUS;
      return BTN_MINUS_PRESSED;
    }
  }

  // Verificăm butonul OK (RD7)
  if (BTN_OK == 0) {
    __delay_ms(20);
    if (BTN_OK == 0) {
      button_release_lock |= BTN_LOCK_OK;
      return BTN_OK_PRESSED;
    }
  }

  return BTN_NONE;
}
