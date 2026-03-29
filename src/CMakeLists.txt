set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

# For OSX - don't clear RPATH on install
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

if (MSVC)
    #force std::complex<> typedefs in liquiddsp
    add_definitions(-D_LIBCPP_COMPLEX)

    #enable math definitions in math.h
    add_definitions(-D_USE_MATH_DEFINES)

    #build a graphical application without the console
    option(BUILD_WIN32 "Build win32 app, false for console" TRUE)
    if (BUILD_WIN32)
        set(EXE_ARGS WIN32)
        set(CMAKE_EXE_LINKER_FLAGS "/entry:mainCRTStartup ${CMAKE_EXE_LINKER_FLAGS}")
    endif (BUILD_WIN32)
endif (MSVC)

if (NOT CMAKE_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "-O2")
endif (NOT CMAKE_CXX_FLAGS)

# Static linking options for MinGW
if (MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -mwindows")
endif (MINGW)

# This only works in cmake >3.1
set(CMAKE_CXX_STANDARD 14)

list(APPEND inspectrum_sources 
    abstractsamplesource.cpp
    amplitudedemod.cpp
    cursor.cpp
    cursors.cpp
    main.cpp
    fft.cpp
    frequencydemod.cpp
    mainwindow.cpp
    inputsource.cpp
    phasedemod.cpp
    plot.cpp
    plots.cpp
    plotview.cpp
    samplebuffer.cpp
    samplesource.cpp
    spectrogramcontrols.cpp
    spectrogramplot.cpp
    threshold.cpp
    thresholdplot.cpp
    traceplot.cpp
    tuner.cpp
    tunertransform.cpp
    util.cpp
)

find_package(Qt6 COMPONENTS Core Concurrent Widgets)
if (NOT Qt6_FOUND)
    find_package(Qt5 REQUIRED COMPONENTS Core Concurrent Widgets)
endif()
find_package(FFTW REQUIRED)
find_package(Liquid REQUIRED)

include_directories(
    ${FFTW_INCLUDES}
    ${LIQUID_INCLUDES}
)

add_executable(inspectrum ${EXE_ARGS} ${inspectrum_sources})

if (Qt6_FOUND)
    set(QT_LIBS Qt6::Core Qt6::Widgets Qt6::Concurrent)
else()
    set(QT_LIBS Qt5::Core Qt5::Widgets Qt5::Concurrent)
endif()

target_link_libraries(inspectrum
    ${QT_LIBS}
    ${LIQUID_LIBRARIES}
    ${FFTW_LIBRARIES}
)

# Benchmark and test target (standalone, no GUI)
# Only built when benchmark.cpp exists (kept local, not in repo)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/benchmark.cpp)
    set(bench_sources
        benchmark.cpp
        fft.cpp
        inputsource.cpp
        samplesource.cpp
        abstractsamplesource.cpp
        util.cpp
    )
    add_executable(inspectrum_bench ${bench_sources})
    target_link_libraries(inspectrum_bench
        ${QT_LIBS}
        ${LIQUID_LIBRARIES}
        ${FFTW_LIBRARIES}
    )

    # CTest: regression tests (uses synthetic signal, no external file needed)
    add_test(NAME regression_tests
             COMMAND inspectrum_bench --test-only
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(regression_tests PROPERTIES
        TIMEOUT 60
        LABELS "regression"
    )
endif()

set(INSTALL_DEFAULT_BINDIR "bin" CACHE STRING "Appended to CMAKE_INSTALL_PREFIX")

install(TARGETS inspectrum RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

# Copy required DLLs next to executable after build
if (WIN32)
    # Copy Qt DLLs (including indirect dependencies like Qt5Gui)
    if (Qt6_FOUND)
        set(QT_COPY_LIBS Qt6::Core Qt6::Widgets Qt6::Concurrent Qt6::Gui)
    else()
        set(QT_COPY_LIBS Qt5::Core Qt5::Widgets Qt5::Concurrent Qt5::Gui)
    endif()
    foreach(qt_lib ${QT_COPY_LIBS})
        add_custom_command(TARGET inspectrum POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${qt_lib}>
                $<TARGET_FILE_DIR:inspectrum>
        )
    endforeach()

    # Copy MinGW runtime DLLs
    if (MINGW)
        get_filename_component(MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
        set(MINGW_RUNTIME_DLLS
            libwinpthread-1.dll
            libgcc_s_seh-1.dll
            libstdc++-6.dll
            # Qt transitive dependencies
            libdouble-conversion.dll
            libicuin78.dll
            libicuuc78.dll
            libicudt78.dll
            libpcre2-16-0.dll
            libpcre2-8-0.dll
            libpng16-16.dll
            zlib1.dll
            libzstd.dll
            libharfbuzz-0.dll
            libmd4c.dll
            libfreetype-6.dll
            libglib-2.0-0.dll
            libgraphite2.dll
            libbrotlidec.dll
            libbrotlicommon.dll
            libbz2-1.dll
            libintl-8.dll
            libiconv-2.dll
        )
        foreach(dll ${MINGW_RUNTIME_DLLS})
            find_file(${dll}_PATH ${dll} HINTS ${MINGW_BIN_DIR})
            if (${dll}_PATH)
                add_custom_command(TARGET inspectrum POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${${dll}_PATH}"
                        $<TARGET_FILE_DIR:inspectrum>
                )
            endif()
        endforeach()
    endif()

    # Copy Qt platform plugin
    if (Qt6_FOUND)
        get_target_property(_qt_core_loc Qt6::Core LOCATION)
        get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
        set(QT_PLUGIN_DIR "${_qt_bin_dir}/../share/qt6/plugins")
    else()
        get_target_property(_qt_core_loc Qt5::Core LOCATION)
        get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
        set(QT_PLUGIN_DIR "${_qt_bin_dir}/../share/qt5/plugins")
    endif()
    add_custom_command(TARGET inspectrum POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:inspectrum>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_PLUGIN_DIR}/platforms/qwindows.dll"
            "$<TARGET_FILE_DIR:inspectrum>/platforms/"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:inspectrum>/styles"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QT_PLUGIN_DIR}/styles/qwindowsvistastyle.dll"
            "$<TARGET_FILE_DIR:inspectrum>/styles/"
    )

    # Copy FFTW DLL if using shared library
    if (FFTW_LIBRARIES MATCHES "dll")
        get_filename_component(FFTW_LIB_DIR "${FFTW_LIBRARIES}" DIRECTORY)
        find_file(FFTW_DLL NAMES libfftw3f-3.dll HINTS "${FFTW_LIB_DIR}/../bin")
        if (FFTW_DLL)
            add_custom_command(TARGET inspectrum POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FFTW_DLL}"
                    $<TARGET_FILE_DIR:inspectrum>
            )
        endif()
    endif()

    # Copy liquid-dsp DLL if using shared library
    if (LIQUID_LIBRARIES MATCHES "dll")
        get_filename_component(LIQUID_LIB_DIR "${LIQUID_LIBRARIES}" DIRECTORY)
        find_file(LIQUID_DLL NAMES libliquid.dll HINTS "${LIQUID_LIB_DIR}/../bin" "${LIQUID_LIB_DIR}")
        if (LIQUID_DLL)
            add_custom_command(TARGET inspectrum POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LIQUID_DLL}"
                    $<TARGET_FILE_DIR:inspectrum>
            )
        endif()
    endif()
endif()

# Create uninstall target
configure_file(
    ${PROJECT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
@ONLY)

add_custom_target(uninstall
    ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
)
