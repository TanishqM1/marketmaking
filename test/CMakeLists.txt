add_executable(test_codec
    codec/test_itch.cpp
)
target_link_libraries(test_codec PRIVATE mm_codec mm_warnings mm_sanitizers)
add_test(NAME codec COMMAND test_codec)

add_executable(test_startup
    app/test_startup.cpp
)
target_link_libraries(test_startup PRIVATE mm_app mm_warnings mm_sanitizers)
add_test(NAME startup COMMAND test_startup)

# order_map is header-only, so this needs no library -- just the include path.
# EXCLUDE_FROM_ALL so the default build stays green until insert/find/erase have
# bodies. Build it explicitly:  cmake --build build --target test_order_map
add_executable(test_order_map EXCLUDE_FROM_ALL
    state/test_order_map.cpp
)
target_link_libraries(test_order_map PRIVATE mm_types mm_warnings mm_sanitizers)
add_test(NAME order_map COMMAND test_order_map)

add_executable(test_book EXCLUDE_FROM_ALL
    state/test_book.cpp
)
target_link_libraries(test_book PRIVATE mm_types mm_warnings mm_sanitizers)
add_test(NAME book COMMAND test_book)
