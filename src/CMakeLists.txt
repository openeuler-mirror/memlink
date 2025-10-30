cmake_minimum_required(VERSION 3.12.1)
project(memlinkd)

set(CMAKE_SKIP_BUILD_RPATH TRUE CACHE BOOL "" FORCE)
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
set(MEMLINKD_SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/util.c
        ${CMAKE_CURRENT_SOURCE_DIR}/log.c
        ${CMAKE_CURRENT_SOURCE_DIR}/libvirt_helper.c
        ${CMAKE_CURRENT_SOURCE_DIR}/memlink_driver.c
        ${CMAKE_CURRENT_SOURCE_DIR}/memlink_core.c)

set(INCLUDE_DIRS ${CMAKE_SOURCE_DIR})
if (FIT_FOR_AARCH64 STREQUAL "TRUE")
    set(COMPILE_OPTIONS -g -Werror -Wall -fstack-protector-strong -fPIE -O2 -fstack-check -ftrapv -Wformat=2 -Wshadow -DFOR_AARCH64)
else ()
    set(COMPILE_OPTIONS -g -Werror -Wall -fstack-protector-strong -fPIE -O2 -fstack-check -ftrapv -Wformat=2 -Wshadow)
endif ()
set(EXE_LINK_OPTIONS "-Wl,-z,relro,-z,now,-z,noexecstack -pie -fvisibility=hidden")
set(MEMLINKD_LIBS virt boundscheck pthread)

add_executable(memlinkd ${MEMLINKD_SRC_FILES})
target_include_directories(memlinkd PRIVATE ${INCLUDE_DIRS})
target_compile_options(memlinkd PRIVATE ${COMPILE_OPTIONS})
target_compile_definitions(memlinkd PRIVATE _FORTIFY_SOURCE=2)
set_target_properties(memlinkd PROPERTIES LINK_FLAGS ${EXE_LINK_OPTIONS})
target_link_libraries(memlinkd PRIVATE ${MEMLINKD_LIBS})