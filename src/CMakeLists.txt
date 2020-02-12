add_executable(pthread_sync_test main.cpp)
target_compile_options(pthread_sync_test PRIVATE -Wall -Wextra -Wconversion -Wshadow)
target_link_libraries(pthread_sync_test sub::cos_log pthread)