cmake_minimum_required(VERSION 3.5)
project(inspectrum VERSION 0.5.4 LANGUAGES CXX)
enable_testing()

# Pass version to source code
add_definitions(
    -DAPP_VERSION="${PROJECT_VERSION}-rc0"
    -DAPP_NAME="inspectrum ng"
)

add_subdirectory(src)
