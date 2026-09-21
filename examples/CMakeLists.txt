# WebRTC page demo (needs libdatachannel; excluded from ctest)
if(HAS_WEBRTC)
    add_executable(demo_webrtc_page webrtc_page/demo.c)
    target_link_libraries(demo_webrtc_page PRIVATE zff-plugins zff-core pthread)
    set_target_properties(demo_webrtc_page PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/webrtc_page)
endif()
