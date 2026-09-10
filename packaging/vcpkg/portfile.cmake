# vcpkg port for FluxCap. Copy this directory to <vcpkg>/ports/fluxcap or use
# it directly with --overlay-ports=packaging/vcpkg.
#
# REF/SHA512 below pin the upstream tag. Update them when a new tag is cut:
#   vcpkg install fluxcap --overlay-ports=packaging/vcpkg
# prints the expected SHA512 on mismatch; paste it here.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO sxyyds/fluxcap
    REF "v0.1.1"
    SHA512 00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DFLUXCAP_BUILD_EXAMPLES=OFF
        -DFLUXCAP_BUILD_BENCHMARKS=OFF
        -DFLUXCAP_BUILD_TESTS=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    CONFIG_PATH lib/cmake/FluxCap
    PACKAGE_NAME FluxCap
)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
