cmake_minimum_required(VERSION 3.16)

set(ESPDROP_BASE_VERSION "0.1.0")
if(DEFINED ENV{GITHUB_SHA} AND NOT "$ENV{GITHUB_SHA}" STREQUAL "")
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 ESPDROP_GIT_VERSION)
else()
    execute_process(
        COMMAND git rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE ESPDROP_GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
string(LENGTH "${ESPDROP_GIT_VERSION}" ESPDROP_GIT_VERSION_LENGTH)
if(ESPDROP_GIT_VERSION_LENGTH EQUAL 7 AND
   ESPDROP_GIT_VERSION MATCHES "^[0-9a-fA-F]+$")
    set(PROJECT_VER "${ESPDROP_BASE_VERSION}-${ESPDROP_GIT_VERSION}")
else()
    set(PROJECT_VER "${ESPDROP_BASE_VERSION}")
endif()
set(PROJECT_VER_NUMBER 1)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/core"
    "${CMAKE_CURRENT_LIST_DIR}/tapdrop"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.esp32s3)
# lwIP's default pool of four MLD groups is consumed by its loopback and the
# AWDL link-local solicited-node memberships. Keep explicit capacity for mDNS
# (ff02::fb) and future AirDrop service groups on the second IPv6 interface.
idf_build_set_property(COMPILE_DEFINITIONS "MEMP_NUM_MLD6_GROUP=8" APPEND)
project(espdrop)
