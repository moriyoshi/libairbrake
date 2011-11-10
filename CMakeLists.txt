cmake_minimum_required(VERSION 2.5)
project(airbrake)
include(${CMAKE_ROOT}/Modules/CheckIncludeFile.cmake)
include(${CMAKE_ROOT}/Modules/CheckLibraryExists.cmake)
option(BUILD_SHARED_LIBS "building shared libraries" ON)
set(AIRBRAKE_VERSION_MAJOR 0)
set(AIRBRAKE_VERSION_MINOR 0)
set(AIRBRAKE_VERSION_REVISION 0)
configure_file(
   "${PROJECT_SOURCE_DIR}/airbrake.h.in"
   "${PROJECT_SOURCE_DIR}/airbrake.h"
)
add_library(airbrake airbrake.c)
find_package(CURL)
find_package(LibXml2)
include_directories(${CURL_INCLUDE_DIR} ${LIBXML2_INCLUDE_DIR})
target_link_libraries(airbrake ${CURL_LIBRARIES} ${LIBXML2_LIBRARIES})
set_target_properties(airbrake
PROPERTIES
    SOVERSION ${AIRBRAKE_VERSION_MAJOR}.${AIRBRAKE_VERSION_MINOR}
)

add_executable(testclient testclient.c)
target_link_libraries(testclient airbrake)
install(FILES airbrake.h DESTINATION include)
install(TARGETS airbrake LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
