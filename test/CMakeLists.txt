add_executable(test_codec
    codec/test_itch.cpp
)
target_link_libraries(test_codec PRIVATE mm_codec mm_warnings mm_sanitizers)

add_test(NAME codec COMMAND test_codec)
