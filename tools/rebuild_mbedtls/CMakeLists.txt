# Minimales ESP-IDF Projekt zum Rebuild von mbedTLS mit Software-AES
cmake_minimum_required(VERSION 3.16)

set(IDF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../esp-idf")
set(EXTRA_COMPONENT_DIRS)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mbedtls_rebuild)
