set(TARGET whisper-cli)
set(TARGET_MIC mic-cli)
add_executable(${TARGET} cli.cpp)
add_executable(${TARGET_MIC} mic_cli.cpp)

include(DefaultTargetOptions)

# Find PortAudio
find_path(PORTAUDIO_INCLUDE_DIR portaudio.h
  PATHS
  /usr/local/include
  /opt/homebrew/include
  /usr/include
)

find_library(PORTAUDIO_LIBRARY
  NAMES portaudio
  PATHS
  /usr/local/lib
  /opt/homebrew/lib
  /usr/lib
)

if(PORTAUDIO_INCLUDE_DIR AND PORTAUDIO_LIBRARY)
  message(STATUS "Found PortAudio: ${PORTAUDIO_LIBRARY}")
else()
  message(FATAL_ERROR "PortAudio not found. Please install with 'brew install portaudio'")
endif()


# Configure targets
target_link_libraries(${TARGET} PRIVATE common whisper ${FFMPEG_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

target_include_directories(${TARGET_MIC} PRIVATE ${PORTAUDIO_INCLUDE_DIR} ${CMAKE_SOURCE_DIR})
target_link_libraries(${TARGET_MIC} PRIVATE 
  common whisper ${FFMPEG_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT} ${PORTAUDIO_LIBRARY})

if(APPLE)
  target_link_libraries(${TARGET_MIC} PRIVATE
    "-framework CoreAudio"
    "-framework AudioToolbox"
  )
endif()

install(TARGETS ${TARGET} RUNTIME)
