# Compatibility forwarder. googletest now comes from the shared tanh-tooling module
# (cmake/tanh/test-deps.cmake); downstream projects that still do
# `include(${TANH_CMAKE_DIR}/gtest-deps.cmake)` (cosmos-backend) keep working and get
# the same targets plus the old helper name. New code: include the module directly and
# call tanh_fetch_googletest() / tanh_copy_runtime_dlls().
include(${CMAKE_CURRENT_LIST_DIR}/tanh/test-deps.cmake)
tanh_fetch_googletest(VERSION v1.14.0)
if(NOT COMMAND tanh_copy_dlls_for_tests)
    function(tanh_copy_dlls_for_tests target)
        tanh_copy_runtime_dlls(${target})
    endfunction()
endif()
