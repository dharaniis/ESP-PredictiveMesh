# ESP-PredictiveMesh
# Stage 1: Basic ESP-NOW point-to-point communication between two ESP32 nodes.
#
# Top-level CMakeLists for the project. The actual component (main) is
# defined in the main/ subdirectory.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_predictive_mesh)
