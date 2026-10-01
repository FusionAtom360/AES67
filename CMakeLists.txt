cmake_minimum_required(VERSION 3.20)

project(aes67_speaker_node VERSION 0.1.0 LANGUAGES CXX)

option(AES67_BUILD_TESTS "Build the C++ test harness" ON)

add_library(aes67_audio
    audio/src/audio_packet.cpp
    audio/src/alsa_sink.cpp
    audio/src/alsa_source.cpp
    audio/src/jitter_buffer.cpp
    audio/src/ptp_clock.cpp
    audio/src/playout_controller.cpp
    audio/src/receive_path.cpp
    audio/src/rtp_packet.cpp
    audio/src/sdp_session.cpp
    audio/src/udp_receiver.cpp
    audio/src/rtp_sender.cpp
)
target_compile_features(aes67_audio PUBLIC cxx_std_20)
target_include_directories(aes67_audio PUBLIC audio/include)

if(UNIX)
    find_package(ALSA REQUIRED)
    target_compile_definitions(aes67_audio PRIVATE AES67_HAVE_ALSA)
    target_include_directories(aes67_audio PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(aes67_audio PRIVATE ${ALSA_LIBRARIES})
endif()

add_executable(aes67_speaker_node audio/src/speaker_node.cpp)
target_compile_features(aes67_speaker_node PRIVATE cxx_std_20)
target_link_libraries(aes67_speaker_node PRIVATE aes67_audio)

add_executable(aes67_sender_node audio/src/sender_node.cpp)
target_compile_features(aes67_sender_node PRIVATE cxx_std_20)
target_link_libraries(aes67_sender_node PRIVATE aes67_audio)

if(AES67_BUILD_TESTS)
    enable_testing()
    add_executable(aes67_audio_tests audio/tests/audio_packet_test.cpp)
    target_compile_features(aes67_audio_tests PRIVATE cxx_std_20)
    target_link_libraries(aes67_audio_tests PRIVATE aes67_audio)
    add_test(NAME aes67_audio_tests COMMAND aes67_audio_tests)

    add_executable(aes67_sender_receiver_test audio/tests/sender_receiver_test.cpp)
    target_compile_features(aes67_sender_receiver_test PRIVATE cxx_std_20)
    target_link_libraries(aes67_sender_receiver_test PRIVATE aes67_audio)
    add_test(NAME aes67_sender_receiver_test COMMAND aes67_sender_receiver_test)
    set_tests_properties(aes67_sender_receiver_test PROPERTIES ENVIRONMENT "AES67_TEST_MULTICAST=1")
    add_test(NAME aes67_speaker_help COMMAND aes67_speaker_node --help)
    add_test(NAME aes67_sender_help COMMAND aes67_sender_node --help)
endif()
