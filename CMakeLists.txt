cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# By default, the examples are standalone projects.
# To build them, cd into examples/<name>/ and run:
#   idf.py set-target esp32
#   idf.py build
#
# The mesh_espnow component is automatically picked up via EXTRA_COMPONENT_DIRS.

project(mesh_espnow)
