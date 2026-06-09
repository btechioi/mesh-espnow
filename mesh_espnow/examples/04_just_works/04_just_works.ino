/*
  04_just_works: One-binary mesh firmware for every ESP32 board

  Flash the same binary on every board. Power on 2+ boards.
  Nodes auto-discover and elect a root automatically.

  This .ino file is a thin wrapper around the main source.
  All logic lives in 04_just_works.c (same directory).
*/

#include "04_just_works.c"
