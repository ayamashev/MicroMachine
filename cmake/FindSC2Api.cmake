# - Try to find SC2Api
# Once done, this will define:
#
# SC2Api_FOUND - system has SC2Api.
# SC2Api_INCLUDE_DIRS - the SC2Api include directories.
# SC2Api_LIBRARIES - link these to use SC2Api.
#
# The following libraries variables are provided:
#  SC2Api_CIVETWEB_LIB, SC2Api_PROTOBUF_LIB,
#  SC2Api_SC2API_LIB, SC2Api_SC2LIB_LIB, SC2Api_SC2PROTOCOL_LIB,
#  SC2Api_SC2UTILS_LIB

# Find main Api headers.
find_path(SC2Api_INCLUDE_DIR
    NAMES
        "sc2api/sc2_api.h"
        "sc2utils/sc2_manage_process.h"
    PATHS
        "/opt/local/include"
        "/usr/local/include"
        "/usr/include"
        "/var/lib/jenkins/workspace/MicroMachine/api/include"
)

# Find autogenerated Protobuf Api headers.
find_path(SC2Api_Proto_INCLUDE_DIR
    NAMES
        "s2clientprotocol/sc2api.pb.h"
    PATHS
        "/opt/local/include"
        "/usr/local/include"
        "/usr/include"
        "/var/lib/jenkins/workspace/MicroMachine/api/lib"
)

# Find Protobuf headers.
find_path(SC2Api_Protobuf_INCLUDE_DIR
    NAMES
        "google/protobuf/stubs/common.h"
    PATHS
        "${SC2Api_INCLUDE_DIR}/sc2api"
    NO_DEFAULT_PATH
)

# Put all the headers together.
set(SC2Api_INCLUDE_DIRS
    "${SC2Api_INCLUDE_DIR}"
    "${SC2Api_Proto_INCLUDE_DIR}"
    "${SC2Api_Protobuf_INCLUDE_DIR}"
)

set(SC2Api_LIBRARIES "")

# Search for SC2Api libraries.
foreach(COMPONENT sc2api sc2lib sc2utils sc2protocol civetweb protobuf)
    string(TOUPPER ${COMPONENT} UPPERCOMPONENT)

    find_library(SC2Api_${UPPERCOMPONENT}_LIB
        NAMES
            "${COMPONENT}"
        PATHS
            "/opt/local/lib"
            "/usr/local/lib"
        PATH_SUFFIXES
            "sc2api"
        NO_DEFAULT_PATH
    )

    if(SC2Api_${UPPERCOMPONENT}_LIB)
        mark_as_advanced(SC2Api_${UPPERCOMPONENT}_LIB)
        list(APPEND SC2Api_LIBRARIES "${SC2Api_${UPPERCOMPONENT}_LIB}")
    else()
        message(STATUS ${COMPONENT} " not found!")
        set(SC2Api_FOUND FALSE)
    endif()
endforeach()

mark_as_advanced(SC2Api_INCLUDE_DIRS SC2Api_LIBRARIES)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SC2Api DEFAULT_MSG SC2Api_INCLUDE_DIRS SC2Api_LIBRARIES)
