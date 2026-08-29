set(PROJECT_NAME test_core)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_RCU.cpp
	test_Buffer.cpp
	test_BufferView.cpp
	test_RingBuffer.cpp
	test_LockFreeQueue.cpp
	test_RtFormat.cpp
	test_Logger.cpp
	test_Thread.cpp
	test_WavReader.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::Core
		gtest_main
		gtest
)

tanh_copy_dlls_for_tests(${PROJECT_NAME})

if(CMAKE_CROSSCOMPILING)
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME})
endif()