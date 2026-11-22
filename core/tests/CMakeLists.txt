# Core tests.  Plain executables registered with CTest -- no test framework
# dependency, because the core is meant to build on a bare toolchain.

add_executable(mp3_smoke_test smoke_test.cpp)
target_link_libraries(mp3_smoke_test PRIVATE mp3_core)
target_compile_options(mp3_smoke_test PRIVATE ${MP3_WARNING_FLAGS})

add_test(NAME core.smoke COMMAND mp3_smoke_test)

# The null-output test renders for ~300 ms of wall clock; give it room on a
# loaded CI machine without hiding a genuine hang.
set_tests_properties(core.smoke PROPERTIES TIMEOUT 60)
