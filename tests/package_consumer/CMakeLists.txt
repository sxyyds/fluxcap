cmake_minimum_required(VERSION 3.24)

project(FluxCapPackageConsumer LANGUAGES CXX)

find_package(FluxCap CONFIG REQUIRED)

add_executable(fluxcap_package_consumer main.cpp)
target_link_libraries(fluxcap_package_consumer PRIVATE
    FluxCap::fluxcap FluxCap::gpu)
target_compile_features(fluxcap_package_consumer PRIVATE cxx_std_20)
