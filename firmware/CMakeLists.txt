# Walt Disney World inspired MagicBand Reader - ESP32-S3 firmware
# Stopgap build: button-triggered until the PN532 NFC reader arrives.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(magic_band_reader)
