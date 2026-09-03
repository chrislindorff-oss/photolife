# Unit tests. Each links the core library and runs under ctest.
find_package(Qt6 6.4 REQUIRED COMPONENTS Test)

function(photolife_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE photolife_core Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

photolife_test(tst_database)
photolife_test(tst_settings)
