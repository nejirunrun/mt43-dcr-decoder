cmake_minimum_required(VERSION 3.15)
project(mt43_dcr_decoder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mt43_dcr_decoder STATIC
    src/dcr_report.cpp
    src/dcr_epicenter_table.cpp
    src/dcr_volcano_name_table.cpp
    src/dcr_coastal_region_table.cpp
    src/dcr_flood_region_table.cpp
    src/dcr_local_government_table.cpp
    src/dcr_prevention_table.cpp
)
target_include_directories(mt43_dcr_decoder PUBLIC src)

option(MT43_DCR_BUILD_EXAMPLE "Build the decode_sample example" ON)
if(MT43_DCR_BUILD_EXAMPLE)
    add_executable(decode_sample examples/decode_sample.cpp)
    target_link_libraries(decode_sample PRIVATE mt43_dcr_decoder)
endif()
