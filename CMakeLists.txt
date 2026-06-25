cmake_minimum_required(VERSION 3.22)
project(Atlas VERSION 1.1.0 LANGUAGES C CXX OBJC OBJCXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../JUCE JUCE)

juce_add_plugin(Atlas
  COMPANY_NAME "Alessio"
  BUNDLE_ID "plugins.alessio.atlas"
  PLUGIN_MANUFACTURER_CODE Alss
  PLUGIN_CODE Atls
  FORMATS AU VST3
  PRODUCT_NAME "Atlas"
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(Atlas)

target_sources(Atlas PRIVATE
  src/unity_build/common.cpp
  src/unity_build/synthesis.cpp
  src/unity_build/plugin.cpp)

target_include_directories(Atlas PRIVATE
  src/common
  src/common/wavetable
  src/interface/look_and_feel
  src/plugin
  src/synthesis/synth_engine
  src/synthesis/effects
  src/synthesis/filters
  src/synthesis/framework
  src/synthesis/lookups
  src/synthesis/modulators
  src/synthesis/modules
  src/synthesis/producers
  src/synthesis/utilities
  third_party
  third_party/concurrentqueue
  third_party/json)

target_compile_definitions(Atlas PRIVATE
  VITAL_ACCESSIBLE_UI=1
  HEADLESS=0
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_DSP_USE_SHARED_FFTW=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

# Vital's DSP unity build is designed around SIMD arithmetic. The original
# production build enables fast floating-point transforms for this code, but
# applying that flag to JUCE itself would weaken JUCE's NaN/Inf handling.
set_source_files_properties(src/unity_build/synthesis.cpp PROPERTIES
  COMPILE_OPTIONS "$<$<CONFIG:Release>:-ffast-math>")

target_link_libraries(Atlas PRIVATE
  juce::juce_audio_utils
  juce::juce_dsp
  juce::juce_gui_extra
  PUBLIC
  juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags)
