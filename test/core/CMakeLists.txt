set(PROJECT_NAME test_core)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_RCU.cpp
	test_Buffer.cpp
	test_BufferView.cpp
	test_BufferOperations.cpp
	test_RingBuffer.cpp
	test_LockFreeQueue.cpp
	test_SpscRingBuffer.cpp
	test_TripleBuffer.cpp
	test_SeqLock.cpp
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

tanh_copy_runtime_dlls(${PROJECT_NAME})

if(CMAKE_CROSSCOMPILING)
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME})
endif()

# An executable of its own: it replaces the global operator new and operator delete to
# count the allocations of a call and to make the n-th one fail, which no other test
# should run under. The static runtimes of ThreadSanitizer, LeakSanitizer and
# MemorySanitizer define these operators themselves and do not link with a second
# definition, so those builds go without it; AddressSanitizer (which finds the leaks
# too), UBSan and RealtimeSanitizer builds have it.
if(NOT (TANH_WITH_TSAN OR TANH_WITH_LSAN OR TANH_WITH_MSAN))
	add_executable(test_core_allocation)

	target_sources(test_core_allocation PRIVATE
		test_RCUAllocation.cpp
	)

	target_link_libraries(test_core_allocation
		PRIVATE
			tanh::Core
			gtest_main
			gtest
	)

	tanh_copy_runtime_dlls(test_core_allocation)

	if(CMAKE_CROSSCOMPILING)
		gtest_add_tests(TARGET test_core_allocation)
	else()
		gtest_discover_tests(test_core_allocation)
	endif()
endif()