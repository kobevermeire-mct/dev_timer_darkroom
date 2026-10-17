# dev_timer_darkroom
Arduino firmware for a three-channel darkroom timer with TM1637 displays, rotary encoders, and a piezo buzzer with adjustable volume.

Darkroom Multi-Timer is firmware for an ATmega32U4 board (Arduino Pro Micro / Leonardo) that runs three independent timers, for example for development, stop, and fixer. 

Each timer has its own 4-digit display, a rotary encoder to set the time, and a push button to start, stop, or save the setting to EEPROM.
Brightness and volume are adjustable with a double-click on the button, and all settings persist across power cycles.
When a timer finishes, it beeps three times and the display flashes "00:00".


Operation
//  - Turn the encoder to set the time (only when the timer is NOT running).
//  - Click to START the timer. Click whilst it is running = stop & reset.
//  - Press and hold (>=1.2s) whilst the timer is idle = save the set time to EEPROM.
//  - When a timer expires: 3 beeps + the display flashes “00:00”.
