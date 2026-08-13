# Pharos - top-level ESP-IDF project
#
# Target: Waveshare ESP32-S3-Touch-AMOLED-1.75C (ESP32-S3R8, 8MB PSRAM, 16MB
# flash). Build with ESP-IDF v5.5 or newer:
#
#   idf.py set-target esp32s3
#   idf.py build flash monitor
#
# The detection engines and UI geometry are also buildable and tested with no
# ESP-IDF at all - see test/host. Do that first; it is faster and catches more.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(pharos)
