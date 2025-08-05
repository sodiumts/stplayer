cmake_minimum_required(VERSION 3.20.0)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BOARD blackpill_f411ce)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stplayer)

target_sources(app PRIVATE src/main.c)
