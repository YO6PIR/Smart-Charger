# PIRCharger

Firmware for a **PIC16F877A-based, single-channel battery charger and
discharger**. PIRCharger measures pack voltage and current, guides the
user through battery setup, and controls separate charge and discharge
PWM outputs. An HD44780-compatible 16×2 LCD, status LEDs, a fan, and a
piezo buzzer provide operating feedback.

> **Safety notice:** This firmware controls battery power hardware.
> Before connecting a battery, verify the analog front end,
> current-sense circuitry, power stage, component ratings, and firmware
> thresholds against your actual circuit. The project does not provide
> per-cell monitoring or cell balancing. Use at your own risk and
> supervise initial testing with a current-limited supply and suitable
> test load.

## Features

-   CC/CV charging control for Li-ion, LiPo, SLA, and Ni-MH battery
    packs.
-   Discharge control using a separate PWM output and a current target
    adjustable from 100 mA to 1000 mA.
-   Pack-voltage-based cell-count estimate for packs of 1--5 cells, with
    manual correction.
-   Battery-voltage calibration using a multimeter; the calibration
    factor is stored in EEPROM.
-   16×2 LCD interface with a custom battery-level bar, operating mode,
    elapsed time, temperature reading, and current.
-   Debounced buttons, status LEDs, piezo-buzzer tones, and automatic
    fan control.
-   ADC rolling averages and a separate low-pass filter for
    LCD-displayed values.
-   Discharge cutoff thresholds and power limiting for the intended 10
    Ω, 5 W discharge resistor.

## Target hardware

  Item                      Configuration
  ------------------------- --------------------------------------------------
  Microcontroller           Microchip PIC16F877A
  Oscillator                20 MHz HS
  Toolchain                 Microchip XC8; project metadata targets XC8 3.10
  Display                   HD44780-compatible 16×2 LCD, 4-bit interface
  Charge PWM                CCP1 on RC2, approximately 50 kHz using Timer2
  Discharge PWM             CCP2 on RC1, using Timer2
  Battery-voltage input     AN0 / RA0
  Charge-current input      AN1 / RA1
  Discharge-current input   AN2 / RA2
  Thermistor analog input   AN3 / RA3
  Buttons                   RD4 (ESC), RD5 (−), RD6 (+), RD7 (OK)
  Status LEDs               RD2 (green), RD3 (red)
  Fan                       RD1
  Piezo buzzer              RC5

### LCD wiring

The LCD operates in 4-bit mode:

  LCD signal   PIC pin
  ------------ ----------
  RS           RB2
  Enable       RB3
  D4--D7       RB4--RB7

### Analog inputs and scaling

-   **Battery voltage (AN0/RA0):** measured through a 56 kΩ / 18 kΩ
    divider. Firmware scales the ADC reading using a 5 V reference and
    the 74/18 ratio, then applies an EEPROM calibration factor.
-   **Charge current (AN1/RA1):** converted using the configured sensor
    scale of 676 mV/A.
-   **Discharge current (AN2/RA2):** averaged over eight samples and
    converted using the configured reference scale of 1.8 mA per ADC
    count.
-   **Thermistor input (AN3/RA3):** averaged over eight samples. The
    reference conversion is `ADC * 53 / 100`, using integer arithmetic.
    The resulting value is displayed, but **temperature-based control is
    not implemented**.

Confirm all scaling constants against the actual circuit before relying
on displayed measurements.

## User operation

After a 1.5-second startup screen, the charger enters its normal
display. The initial software selections are Li-ion, one cell, and a
1000 mA target current. These selections are not saved across power
cycles.

1.  Press **OK** to enter setup.
2.  Select the battery chemistry using **+ / −**: Li-ion, LiPo, SLA, or
    Ni-MH.
3.  Press **OK** to estimate the cell count from pack voltage. The
    display shows a blinking `CelX` indicator. Adjust the estimate with
    **+ / −** if necessary (1--5 cells), then press **OK** to confirm.
4.  Adjust the target current with **+ / −** in 100 mA steps, from 100
    mA to 1000 mA.
5.  Select `CHG` or `DIS` with **+ / −**, then press **OK** to start the
    selected operation.

During charging, **ESC** stops PWM and returns the controller to idle.
During discharge, **ESC** stops the discharge PWM. Pressing **ESC**
during setup backs out of the setup sequence.

From the normal screen, press **+ and OK together** to open voltage
calibration.

## Battery profiles and control thresholds

### Cell-count estimate

The firmware estimates the number of cells by matching pack voltage
against the selected chemistry's per-cell range. LiPo uses the same
range as Li-ion.

  Chemistry         Auto-detection range per cell
  --------------- -------------------------------
  Li-ion / LiPo                     2700--4250 mV
  SLA                               1700--2350 mV
  Ni-MH                             1000--1450 mV

The detector checks counts from one to five and returns the first
matching range. If none matches, it defaults to one cell. Some
pack-voltage ranges overlap, so **always verify and, if needed, correct
the detected cell count before starting**.

### Charging

  -------------------------------------------------------------------------
  Chemistry      CV target per cell   Charge-termination       Start/active
                                                 current       undervoltage
                                                              threshold per
                                                                     cell\*
  -------------- ------------------ -------------------- ------------------
  Li-ion                    4200 mV               100 mA            2000 mV

  LiPo                      4200 mV               100 mA            2500 mV

  SLA                       2350 mV                50 mA            1200 mV

  Ni-MH                     1450 mV               200 mA             500 mV
  -------------------------------------------------------------------------

\*The firmware calculates the start and active undervoltage limit as the
profile's minimum-start value minus 500 mV per cell. A pack below this
limit cannot start charging. If the voltage falls below it during
charging, PWM is disabled and the controller returns to idle.

The charge controller uses two stages:

-   **CC (constant current):** duty cycle is increased by one count when
    measured current is below target and decreased when it exceeds the
    target by more than 50 mA.
-   **CV (constant voltage):** begins when pack voltage reaches the
    total chemistry-specific CV target. PWM is adjusted around a 10 mV
    voltage band.
-   **DONE:** charging completes when measured current reaches or falls
    below the chemistry-specific termination threshold. PWM is turned
    off.

### Discharging

  Chemistry     Discharge cutoff per cell
  ----------- ---------------------------
  Li-ion                          3000 mV
  LiPo                            2500 mV
  SLA                             1666 mV
  Ni-MH                           1000 mV

Discharge current is regulated toward the selected target using CCP2 on
RC1. For the intended **10 Ω, 5 W discharge resistor**, firmware limits
the target based on measured pack voltage:

`maximum current (mA) = 5,000,000 / voltage_mV`

The firmware also reduces PWM if calculated battery power exceeds 5 W.
Pack voltage continues to be sampled while discharge PWM is active;
reaching or falling below the total pack cutoff disables CCP2.

The discharge-current scale and cutoff behavior must be validated
against the actual analog front end and power stage before battery
testing.

## Measurements and voltage calibration

Each used ADC input uses a rolling average of eight samples, including
battery voltage, charge current, and thermistor readings. An additional
1/4 low-pass filter is applied only to values rendered on the LCD; it
does not change the measurements used by the CC/CV controller.

To calibrate battery voltage:

1.  From the normal screen, press **+ and OK together**.
2.  Measure the battery terminals with a multimeter.
3.  Adjust the displayed reference voltage using **+ / −** in 10 mV
    steps.
4.  Press **OK** to save, or **ESC** to cancel.

The firmware calculates the new factor relative to the current factor:

`new factor = current factor * reference voltage / captured voltage`

The factor is stored in EEPROM bytes 0 and 1 and read back to verify the
write. The accepted range is 500--1500. If the result is outside this
range or EEPROM verification fails, the display shows `Cal Error` and
the existing factor is retained. On startup, an erased or out-of-range
factor is replaced with the default value of 1000. The LCD voltage
filter resets when a new factor is applied.

There is no equivalent calibration menu for current measurement.

## Display and indicators

-   **Top LCD row:** chemistry label, pack voltage, and elapsed time.
    Voltage is formatted with two decimal places below 10 V and one
    decimal place at 10 V or above. Labels are `LiIo`, `NiMh`, `SLA`,
    and `LiPo`.
-   **Bottom LCD row:** operation label (`CHG` / `DIS`), a
    five-character battery bar, temperature, and current. The battery
    bar has nine fill levels, scaled between the selected chemistry's
    discharge cutoff and total CV target.
-   **Current display:** shows charge current while charging and
    discharge current while discharging. While editing the target
    current, the current field blinks the selected target.
-   **Elapsed timer:** runs during active CC/CV charging and discharge.
    It displays `MM:SS` until one hour, then `HH:MM`, and saturates at
    `99:59`. It resets when ESC is pressed and is not retained through
    power loss.
-   **LEDs:** green indicates an idle pack voltage above 700 mV and
    remains on in DONE. During charging, red flashes approximately one
    second on / 200 ms off. During discharge, red flashes approximately
    200 ms on / one second off. Green is off during either active
    operation.
-   **Fan:** turns on during discharge and when active charging current
    exceeds 500 mA. After the fan-on condition ends, shutdown is delayed
    by 34 main-loop iterations (nominally about one second).
-   **Buzzer:** compile-time defaults in `config.h` are 1680 Hz, 20 ms
    short tones, 250 ms long tones, and an 80 ms gap between tones.
    Startup plays three short tones; +/− use short tones; menu
    confirmations and calibration save use a long tone.

The buzzer is driven from RC5 and toggled by the Timer0 interrupt.

## Source files

  -----------------------------------------------------------------------
  File                                Responsibility
  ----------------------------------- -----------------------------------
  `pirCharger.c`                      Firmware entry point, board setup,
                                      ADC acquisition/conversion, LCD
                                      driver and rendering, buzzer
                                      patterns, Timer0/Timer1 interrupt
                                      handling, and main loop

  `adc.h`                             ADC input identifiers and public
                                      read/conversion interfaces,
                                      including thermistor readings

  `control.c`, `control.h`            Charger states, CC/CV control,
                                      chemistry profiles, discharge
                                      current regulation/cutoff and CCP2
                                      PWM, voltage safety check, status
                                      LEDs, and fan control

  `menu.c`, `menu.h`                  Chemistry/cell/current selection,
                                      cell-count detection, voltage
                                      calibration, and EEPROM access

  `buttons.c`, `buttons.h`            Button mapping, debounce, and
                                      PLUS+OK combination detection

  `pwm.h`                             Public interface for CCP1 PWM
                                      initialization and duty updates

  `config.h`                          Compile-time buzzer frequency and
                                      tone durations

  `main.c`                            Empty placeholder in the current
                                      project; the actual `main()`
                                      function is in `pirCharger.c`
  -----------------------------------------------------------------------

## Building

The project targets the **PIC16F877A** and its project metadata
specifies **Microchip XC8 3.10**. The project-specific `user.cmake`
requests `-Os` optimization to reduce program-space usage; depending on
the installed license, XC8 may report that it falls back to optimization
level 2.

Build using the project's existing build configuration and installed XC8
toolchain. Generated firmware artifacts are placed under:

`out/PIRCharger/`

The production image is:

`default.hex`

Check the compiler output and memory summary after building. Do not
assume that a HEX file is suitable for your hardware until it has been
tested with the intended board and power stage.

## Limitations

-   The firmware operates on the **battery pack as a whole**; it does
    not monitor individual cells or perform cell balancing.
-   Cell-count detection is a pack-voltage estimate, not a substitute
    for checking the actual battery configuration.
-   The thermistor reading is displayed only; no temperature-based
    shutdown or regulation is implemented.
-   There is no battery-management communication interface.
-   Current measurement has no user calibration workflow.
-   The elapsed timer is a session timer, not a real-time clock, and is
    not retained through power loss.
-   Charging and discharging depend on the analog front end,
    current-sense circuit, power stage, and battery profile matching the
    hardware.

## License

MIT License
