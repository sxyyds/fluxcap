cmake_minimum_required(VERSION 3.24)

project(FluxCap
    VERSION 0.1.0
    DESCRIPTION "Low-latency Windows screen capture with a stable C ABI"
    LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "FluxCap currently supports Windows only")
endif()

option(BUILD_SHARED_LIBS "Build FluxCap as a shared library" OFF)
option(FLUXCAP_BUILD_EXAMPLES "Build examples" ON)
option(FLUXCAP_BUILD_BENCHMARKS "Build benchmarks" ON)
option(FLUXCAP_BUILD_TESTS "Build tests" ON)
option(FLUXCAP_BUILD_GPU "Build the Windows Graphics Capture GPU pipeline" ON)

add_library(fluxcap
    src/dirty_tracker.cpp
    src/fluxcap.cpp)
add_library(FluxCap::fluxcap ALIAS fluxcap)

target_compile_features(fluxcap PUBLIC cxx_std_20)
target_include_directories(fluxcap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(fluxcap
    PRIVATE
        FLUXCAP_EXPORTS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(fluxcap PUBLIC FLUXCAP_SHARED)
endif()

target_link_libraries(fluxcap PRIVATE user32 gdi32)

if(MSVC)
    target_compile_options(fluxcap PRIVATE /W4 /permissive- /EHsc /utf-8)
    set_property(TARGET fluxcap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set_property(TARGET fluxcap PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    target_compile_options(fluxcap PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FLUXCAP_BUILD_GPU)
    add_library(fluxcap_gpu
        src/gpu/async_gpu_pipeline.cpp
        src/gpu/bitstream_metadata.cpp
        src/gpu/desktop_duplication_capture.cpp
        src/gpu/desktop_duplication_controller.cpp
        src/gpu/gdi_capture.cpp
        src/gpu/gpu_crop.cpp
        src/gpu/gpu_encoder.cpp
        src/gpu/gpu_move_inference.cpp
        src/gpu/gpu_transform.cpp
        src/gpu/image_encoder.cpp
        src/gpu/pipeline_epoch_supervisor.cpp
        src/gpu/recoverable_gpu_pipeline.cpp
        src/gpu/session_event_monitor.cpp
        src/gpu/shared_frame_bus.cpp
        src/gpu/shared_texture.cpp
        src/gpu/wgc_capture.cpp
        src/gpu/wgc_dxgi_bridge.cpp
        src/gpu/win32_cursor_shape.cpp)
    add_library(FluxCap::gpu ALIAS fluxcap_gpu)
    set_property(TARGET fluxcap_gpu PROPERTY EXPORT_NAME gpu)
    target_compile_features(fluxcap_gpu PUBLIC cxx_std_20)
    target_include_directories(fluxcap_gpu
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
    target_compile_definitions(fluxcap_gpu PRIVATE
        FLUXCAP_GPU_EXPORTS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00)
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(fluxcap_gpu PUBLIC FLUXCAP_GPU_SHARED)
    endif()
    target_link_libraries(fluxcap_gpu
        PUBLIC d3d11
        PRIVATE
            dxgi
            d3dcompiler
            dwmapi
            gdi32
            mf
            mfplat
            mfuuid
            ole32
            user32
            windowsapp
            windowscodecs
            wtsapi32)
    if(MSVC)
        target_compile_options(fluxcap_gpu PRIVATE /W4 /permissive- /EHsc /utf-8)
        set_property(TARGET fluxcap_gpu PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(fluxcap_gpu PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(FLUXCAP_BUILD_EXAMPLES)
    add_executable(fluxcap_grab examples/grab.cpp)
    target_link_libraries(fluxcap_grab PRIVATE FluxCap::fluxcap)
    target_compile_features(fluxcap_grab PRIVATE cxx_std_20)

    if(FLUXCAP_BUILD_GPU)
        add_executable(fluxcap_gpu_pipeline examples/gpu_pipeline.cpp)
        target_link_libraries(fluxcap_gpu_pipeline PRIVATE
            FluxCap::gpu user32 windowsapp)
        target_compile_features(fluxcap_gpu_pipeline PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_pipeline PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_pipeline PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_gpu_preview examples/gpu_preview.cpp)
        target_link_libraries(fluxcap_gpu_preview PRIVATE
            FluxCap::gpu user32 windowsapp dxgi d2d1 dwrite)
        target_compile_features(fluxcap_gpu_preview PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_preview PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_preview PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_resilient_monitor_capture
            examples/resilient_monitor_capture.cpp)
        target_link_libraries(fluxcap_resilient_monitor_capture PRIVATE
            FluxCap::gpu)
        target_compile_features(fluxcap_resilient_monitor_capture PRIVATE
            cxx_std_20)
        target_compile_definitions(fluxcap_resilient_monitor_capture PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_resilient_monitor_capture PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
    endif()
endif()

if(FLUXCAP_BUILD_BENCHMARKS)
    add_executable(fluxcap_bench benchmarks/benchmark.cpp)
    target_link_libraries(fluxcap_bench PRIVATE FluxCap::fluxcap user32 gdi32)
    target_compile_features(fluxcap_bench PRIVATE cxx_std_20)
    target_compile_definitions(fluxcap_bench PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    if(MSVC)
        target_compile_options(fluxcap_bench PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()

    if(FLUXCAP_BUILD_GPU)
        add_executable(fluxcap_gpu_bench benchmarks/gpu_pipeline_benchmark.cpp)
        target_link_libraries(fluxcap_gpu_bench PRIVATE
            FluxCap::gpu user32 windowsapp d3d11 dxgi)
        target_compile_features(fluxcap_gpu_bench PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_bench PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_gpu_throughput_bench
            benchmarks/gpu_throughput_benchmark.cpp)
        target_link_libraries(fluxcap_gpu_throughput_bench PRIVATE FluxCap::gpu)
        target_compile_features(fluxcap_gpu_throughput_bench PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_throughput_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_throughput_bench PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_gpu_crop_bench
            benchmarks/gpu_crop_benchmark.cpp)
        target_link_libraries(fluxcap_gpu_crop_bench PRIVATE FluxCap::gpu)
        target_compile_features(fluxcap_gpu_crop_bench PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_crop_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_crop_bench PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_gpu_ingress_bench
            benchmarks/gpu_ingress_benchmark.cpp)
        target_link_libraries(fluxcap_gpu_ingress_bench PRIVATE d3d11)
        target_compile_features(fluxcap_gpu_ingress_bench PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_ingress_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_ingress_bench PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_gpu_shared_bus_bench
            benchmarks/gpu_shared_bus_benchmark.cpp)
        target_link_libraries(fluxcap_gpu_shared_bus_bench PRIVATE
            FluxCap::gpu d3d11 dxgi)
        target_compile_features(fluxcap_gpu_shared_bus_bench PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_shared_bus_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_shared_bus_bench PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_gpu_wgc_bus_bench
            benchmarks/gpu_wgc_bus_benchmark.cpp)
        target_link_libraries(fluxcap_gpu_wgc_bus_bench PRIVATE
            FluxCap::gpu user32 windowsapp d3d11 dxgi)
        target_compile_features(fluxcap_gpu_wgc_bus_bench PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_wgc_bus_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_wgc_bus_bench PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_gpu_bus_soak
            benchmarks/gpu_bus_soak.cpp)
        target_link_libraries(fluxcap_gpu_bus_soak PRIVATE
            FluxCap::gpu user32 windowsapp d3d11 dxgi psapi)
        target_compile_features(fluxcap_gpu_bus_soak PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_bus_soak PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_bus_soak PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()

        add_executable(fluxcap_gpu_dd_resilience_bench
            benchmarks/gpu_dd_resilience_benchmark.cpp)
        target_link_libraries(fluxcap_gpu_dd_resilience_bench PRIVATE
            FluxCap::gpu d3d11 dxgi bcrypt)
        target_compile_features(fluxcap_gpu_dd_resilience_bench PRIVATE
            cxx_std_20)
        target_compile_definitions(fluxcap_gpu_dd_resilience_bench PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_dd_resilience_bench PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
    endif()
endif()

if(FLUXCAP_BUILD_TESTS)
    enable_testing()

    add_executable(fluxcap_dirty_tests
        tests/dirty_tracker_tests.cpp
        src/dirty_tracker.cpp)
    target_include_directories(fluxcap_dirty_tests PRIVATE src include)
    target_compile_features(fluxcap_dirty_tests PRIVATE cxx_std_20)
    target_compile_definitions(fluxcap_dirty_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(fluxcap_dirty_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
    add_test(NAME fluxcap_dirty_tests COMMAND fluxcap_dirty_tests)

    add_executable(fluxcap_capture_tests tests/capture_tests.cpp)
    target_link_libraries(fluxcap_capture_tests PRIVATE FluxCap::fluxcap)
    target_compile_features(fluxcap_capture_tests PRIVATE cxx_std_20)
    add_test(NAME fluxcap_capture_tests COMMAND fluxcap_capture_tests)

    add_executable(fluxcap_c_api_tests tests/c_api_tests.c)
    target_link_libraries(fluxcap_c_api_tests PRIVATE FluxCap::fluxcap)
    target_compile_features(fluxcap_c_api_tests PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(fluxcap_c_api_tests PRIVATE /W4)
    endif()
    add_test(NAME fluxcap_c_api_tests COMMAND fluxcap_c_api_tests)

    if(FLUXCAP_BUILD_GPU)
        add_executable(fluxcap_gpu_capture_tests tests/gpu_capture_tests.cpp)
        target_include_directories(fluxcap_gpu_capture_tests PRIVATE src/gpu)
        target_link_libraries(fluxcap_gpu_capture_tests PRIVATE
            FluxCap::gpu user32 gdi32 dwmapi dxgi windowsapp)
        target_compile_features(fluxcap_gpu_capture_tests PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_capture_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_capture_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_gpu_capture_tests COMMAND fluxcap_gpu_capture_tests)

        add_executable(fluxcap_gpu_move_inference_tests
            tests/gpu_move_inference_tests.cpp
            src/gpu/gpu_move_inference.cpp)
        target_include_directories(fluxcap_gpu_move_inference_tests PRIVATE
            include src/gpu)
        target_link_libraries(fluxcap_gpu_move_inference_tests PRIVATE
            d3d11 d3dcompiler dxgi)
        target_compile_features(fluxcap_gpu_move_inference_tests PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_move_inference_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_move_inference_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_gpu_move_inference_tests
            COMMAND fluxcap_gpu_move_inference_tests)

        add_executable(fluxcap_gpu_device_failure_tests
            tests/gpu_device_failure_tests.cpp)
        target_include_directories(fluxcap_gpu_device_failure_tests PRIVATE
            include src/gpu)
        target_link_libraries(fluxcap_gpu_device_failure_tests PRIVATE d3d11)
        target_compile_features(fluxcap_gpu_device_failure_tests PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_gpu_device_failure_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_gpu_device_failure_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_gpu_device_failure_tests
            COMMAND fluxcap_gpu_device_failure_tests)

        add_executable(fluxcap_bitstream_metadata_tests
            tests/bitstream_metadata_tests.cpp)
        target_link_libraries(fluxcap_bitstream_metadata_tests PRIVATE
            FluxCap::gpu)
        target_compile_features(fluxcap_bitstream_metadata_tests PRIVATE
            cxx_std_20)
        target_compile_definitions(fluxcap_bitstream_metadata_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_bitstream_metadata_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_bitstream_metadata_tests
            COMMAND fluxcap_bitstream_metadata_tests)

        add_executable(fluxcap_side_data_geometry_tests
            tests/side_data_geometry_tests.cpp)
        target_include_directories(fluxcap_side_data_geometry_tests PRIVATE
            include src/gpu)
        target_link_libraries(fluxcap_side_data_geometry_tests PRIVATE
            d3d11)
        target_compile_features(fluxcap_side_data_geometry_tests PRIVATE
            cxx_std_20)
        target_compile_definitions(fluxcap_side_data_geometry_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_side_data_geometry_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_side_data_geometry_tests
            COMMAND fluxcap_side_data_geometry_tests)

        add_executable(fluxcap_shared_frame_bus_tests
            tests/shared_frame_bus_tests.cpp)
        target_link_libraries(fluxcap_shared_frame_bus_tests PRIVATE
            FluxCap::gpu d3d11 dxgi)
        target_compile_features(fluxcap_shared_frame_bus_tests PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_shared_frame_bus_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_shared_frame_bus_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_shared_frame_bus_tests
            COMMAND fluxcap_shared_frame_bus_tests)

        add_executable(fluxcap_pipeline_epoch_tests
            tests/pipeline_epoch_supervisor_tests.cpp
            src/gpu/pipeline_epoch_supervisor.cpp)
        target_include_directories(fluxcap_pipeline_epoch_tests PRIVATE src/gpu)
        target_compile_features(fluxcap_pipeline_epoch_tests PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_pipeline_epoch_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_pipeline_epoch_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_pipeline_epoch_tests
            COMMAND fluxcap_pipeline_epoch_tests)

        add_executable(fluxcap_recoverable_gpu_pipeline_tests
            tests/recoverable_gpu_pipeline_tests.cpp)
        target_include_directories(fluxcap_recoverable_gpu_pipeline_tests PRIVATE
            src/gpu)
        target_link_libraries(fluxcap_recoverable_gpu_pipeline_tests PRIVATE
            FluxCap::gpu)
        target_compile_features(fluxcap_recoverable_gpu_pipeline_tests PRIVATE
            cxx_std_20)
        target_compile_definitions(fluxcap_recoverable_gpu_pipeline_tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_recoverable_gpu_pipeline_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_recoverable_gpu_pipeline_tests
            COMMAND fluxcap_recoverable_gpu_pipeline_tests)

        add_executable(fluxcap_desktop_duplication_policy_tests
            tests/desktop_duplication_policy_tests.cpp)
        target_include_directories(fluxcap_desktop_duplication_policy_tests
            PRIVATE include src)
        target_link_libraries(fluxcap_desktop_duplication_policy_tests PRIVATE
            d3d11)
        target_compile_features(fluxcap_desktop_duplication_policy_tests
            PRIVATE cxx_std_20)
        target_compile_definitions(fluxcap_desktop_duplication_policy_tests
            PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
        if(MSVC)
            target_compile_options(fluxcap_desktop_duplication_policy_tests
                PRIVATE /W4 /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME fluxcap_desktop_duplication_policy_tests
            COMMAND fluxcap_desktop_duplication_policy_tests)

        find_program(FLUXCAP_WINDOWS_POWERSHELL_EXECUTABLE powershell.exe)
        if(FLUXCAP_WINDOWS_POWERSHELL_EXECUTABLE)
            add_test(NAME fluxcap_gpu_copy_qualification_self_test
                COMMAND ${FLUXCAP_WINDOWS_POWERSHELL_EXECUTABLE}
                    -NoProfile
                    -ExecutionPolicy Bypass
                    -File
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/qualification/Test-FluxCapQualificationSelfTest.ps1)
        endif()
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS fluxcap
    EXPORT FluxCapTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(FLUXCAP_BUILD_GPU)
    install(TARGETS fluxcap_gpu
        EXPORT FluxCapTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT FluxCapTargets
    FILE FluxCapTargets.cmake
    NAMESPACE FluxCap::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FluxCap)

configure_package_config_file(
    cmake/FluxCapConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/FluxCapConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FluxCap)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/FluxCapConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/FluxCapConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/FluxCapConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FluxCap)
