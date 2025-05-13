if (WHISPER_SDL2)
    set(TARGET_STREAM whisper-stream)
    add_executable(${TARGET_STREAM} stream.cpp)
    
    set(TARGET_STREAM_TEST whisper-stream-test)
    add_executable(${TARGET_STREAM_TEST} test_stream.cpp)

    set(TARGET_ROS2_TEST ros2-whisper-stream)
    add_executable(${TARGET_ROS2_TEST} ros2_stream.cpp)

    # 필수 패키지 찾기
    find_package(rclcpp REQUIRED)
    find_package(std_msgs REQUIRED)
    find_package(Python3 COMPONENTS Development REQUIRED)

    # ROS2 라이브러리 디렉토리 설정
    get_filename_component(ANACONDA_HUMBLE_LIB_DIR "${Python3_LIBRARY_DIRS}" REALPATH)
    
    include(DefaultTargetOptions)

    # 라이브러리 링크
    target_link_libraries(${TARGET_STREAM} PRIVATE common common-sdl whisper ${CMAKE_THREAD_LIBS_INIT})
    target_link_libraries(${TARGET_STREAM_TEST} PRIVATE common common-sdl whisper ${CMAKE_THREAD_LIBS_INIT})

    # ROS2 프로그램 설정
    target_include_directories(${TARGET_ROS2_TEST} PRIVATE ${std_msgs_INCLUDE_DIRS})
    target_link_libraries(${TARGET_ROS2_TEST} PRIVATE
        common common-sdl whisper ${CMAKE_THREAD_LIBS_INIT}
        rclcpp::rclcpp ${std_msgs_LIBRARIES} Python3::Python
    )

    # RPATH 설정
    if(APPLE)
        set_target_properties(${TARGET_ROS2_TEST} PROPERTIES
            INSTALL_RPATH "@loader_path;${ANACONDA_HUMBLE_LIB_DIR}"
            BUILD_RPATH "@loader_path;${ANACONDA_HUMBLE_LIB_DIR}"
            MACOSX_RPATH TRUE
        )
    elseif(UNIX AND NOT APPLE)
        set_target_properties(${TARGET_ROS2_TEST} PROPERTIES
            INSTALL_RPATH "$ORIGIN:${ANACONDA_HUMBLE_LIB_DIR}"
            BUILD_RPATH "$ORIGIN:${ANACONDA_HUMBLE_LIB_DIR}"
        )
    endif()
    
    # 설치
    install(TARGETS ${TARGET_STREAM} ${TARGET_STREAM_TEST} ${TARGET_ROS2_TEST} RUNTIME)
endif()
