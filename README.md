# ESP8266 Passive Buzzer

The ESP8266 discovers folders in [Robson Couto's Arduino Songs
repository](https://github.com/robsoncouto/arduino-songs), picks a folder at
random, and downloads its Arduino sketch over Wi-Fi. Folders without a
parseable melody sketch are skipped. The player displays the folder name and
current note, then picks another random folder after playback ends. If Wi-Fi
or GitHub is unavailable, it plays the built-in "Neon Dreams" fallback.

The upstream README says Robson Couto wrote the sketches and requests credit
when they are reused. This project credits the source repository.

## Wi-Fi setup

Copy `include/secrets.h.example` to `include/secrets.h` and enter your Wi-Fi
name and password. The local credentials file is ignored by Git.

## Wiring

- Passive buzzer: D1 to buzzer positive, GND to buzzer negative.
- Integrated SSD1306 OLED: SCL=D5, SDA=D6.
