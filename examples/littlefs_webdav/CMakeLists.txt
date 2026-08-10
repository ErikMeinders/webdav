cmake_minimum_required(VERSION 3.16)

# Pull in the webdav component from the repo root (two levels up from this
# example) without requiring it to be installed via the component registry.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(littlefs_webdav_example)
