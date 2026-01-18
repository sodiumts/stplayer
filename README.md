# STPlayer Opus player
Zephyr RTOS based Opus audio player on an STM32U585
## Goals
- High quality headphone DAC & amp (TLV320DAC3100)
- Low power usage
- Removable SD card
- Somewhat cheap to produce
- Oled screen for displaying information (SH1122)
- ~Could have usb msc for easily updating music library~(scrapped due to lack of High-speed USB on STM microcontrollers :/)
- Custom designed enclosure + PCB inspired by the Sony NW-500 series of players
## Current progress
Decoding and playback of opus files from the SD card is working (though requires dynamic buffer filling for < 60ms packet opus files)
Display is working, next goal is to make it read the songs, and store some sort of lookup table on the SD card.
