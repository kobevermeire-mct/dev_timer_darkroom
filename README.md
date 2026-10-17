# dev_timer_darkroom
Arduino firmware for a three-channel darkroom timer with TM1637 displays, rotary encoders, and a piezo buzzer with adjustable volume.

Darkroom Multi-Timer is firmware for an ATmega32U4 board (Arduino Pro Micro / Leonardo) that runs three independent timers, for example for development, stop, and fixer. 

Each timer has its own 4-digit display, a rotary encoder to set the time, and a push button to start, stop, or save the setting to EEPROM.
Brightness and volume are adjustable with a double-click on the button, and all settings persist across power cycles.
When a timer finishes, it beeps three times and the display flashes "00:00".


## Operation
- Turn the encoder to set the time (only when the timer is NOT running).
- Click to START the timer. Click whilst it is running = stop & reset.
- Press and hold (>=1.2s) whilst the timer is idle = save the set time to EEPROM.
- When a timer expires: 3 beeps + the display flashes “00:00”.
- Double-click the DEV encoder  -> brightness mode (“br 1”..“br 8”)
  - Turn = adjust brightness in real time (all 3 displays).
  - Click = save to EEPROM and return to normal
- Double-click the STOP encoder -> volume mode (“uo 0”..“uo 8”, 0 = mute).
  - Turn = adjust volume (short test beep at each step).
  - Click = save to EEPROM and return to normal

## Dependencies & Licenties
* **TM1637Display** door Avishay Orpaz – Gelicentieerd onder de [GNU LGPL v3.0](https://github.com/avishayorpaz/arduino-tm1637/blob/master/LICENSE).
* **Encoder** door Paul Stoffregen – Gelicentieerd onder de [MIT License](https://github.com/PaulStoffregen/Encoder/blob/master/LICENSE).
