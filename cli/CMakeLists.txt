add_executable(qtsvfs main.cpp)
target_link_libraries(qtsvfs PRIVATE qtsvfs_core)

if(MSVC)
    target_compile_options(qtsvfs PRIVATE /W4 /utf-8)
    target_compile_definitions(qtsvfs PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
