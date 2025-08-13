# STPlayer Opus player
Zephyr RTOS based Opus audio player. 
## Goals
- High quality headphone DAC & amp 
- Low power usage
- Removable SD card
- Somewhat cheap to produce
- Oled screen for displaying information (SH1122)
- Could have usb msc for easily updating music library
## Current progress
Decoding and playback of opus files is working (though requires dynamic buffer filling for < 60ms packet opus files)
