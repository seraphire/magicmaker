# Stage-1 bring-up: blink the ESP32-S3's onboard RGB LED. No external wiring.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bringup_blink)
