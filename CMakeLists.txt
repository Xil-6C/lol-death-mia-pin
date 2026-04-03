cmake_minimum_required(VERSION 3.20)
project(lol-death-mia-pin VERSION 1.0.0 LANGUAGES C)

# --- Paths ---
set(OBS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs-studio"
    CACHE PATH "Path to OBS Studio source (for headers)")
set(OBS_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs-lib"
    CACHE PATH "Path to obs.lib import library")
set(OBS_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs-deps"
    CACHE PATH "Path to obs-deps (FFmpeg headers + libs)")

# --- Plugin shared library ---
add_library(${PROJECT_NAME} MODULE
    src/plugin-main.c
    src/mia-overlay.c
    src/lol-api-client.c
    src/animation-controller.c
    src/pcm-decode.c
    src/pcm-decode.h
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PROJECT_VERSION="${PROJECT_VERSION}"
)

# --- Include paths ---
target_include_directories(${PROJECT_NAME} PRIVATE
    "${OBS_SOURCE_DIR}/libobs"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs-config"
    "${OBS_DEPS_DIR}/include"
)

# --- Link obs.lib + FFmpeg + curl + jansson ---
target_link_libraries(${PROJECT_NAME} PRIVATE
    "${OBS_LIB_DIR}/obs.lib"
    "${OBS_DEPS_DIR}/lib/libcurl_imp.lib"
    "${OBS_DEPS_DIR}/lib/jansson.lib"
    "${OBS_DEPS_DIR}/lib/avformat.lib"
    "${OBS_DEPS_DIR}/lib/avcodec.lib"
    "${OBS_DEPS_DIR}/lib/avutil.lib"
    "${OBS_DEPS_DIR}/lib/swresample.lib"
)

# --- Output ---
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "lol-death-mia-pin"
)

# --- Install (copy to OBS plugins folder) ---
set(OBS_PLUGIN_DIR "" CACHE PATH "OBS plugins directory for install")
set(OBS_DATA_DIR "" CACHE PATH "OBS data directory for install")
if(OBS_PLUGIN_DIR)
    install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION "${OBS_PLUGIN_DIR}"
        RUNTIME DESTINATION "${OBS_PLUGIN_DIR}"
    )
endif()
if(OBS_DATA_DIR)
    install(DIRECTORY data/
        DESTINATION "${OBS_DATA_DIR}/obs-plugins/${PROJECT_NAME}"
    )
endif()
