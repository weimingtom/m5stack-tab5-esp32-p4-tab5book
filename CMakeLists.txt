# Tab5 Book Reader - ESP-IDF project
cmake_minimum_required(VERSION 3.5)

set(COMPONENTS main) # "Trim" the build. Include the minimal set of components; main and anything it depends on.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5book)
