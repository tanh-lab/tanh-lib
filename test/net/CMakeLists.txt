set(PROJECT_NAME test_net)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

# Sha256 is platform-independent and worth covering on every compiler — MSVC
# included, since it exercises the big-endian packing and the stream reads.
target_sources(${PROJECT_NAME} PRIVATE
	test_Sha256.cpp
)

# The transfer tests drive TestHttpServer, which is POSIX sockets. Porting it to
# Winsock would be writing Windows socket code to test a Windows HTTP backend
# that does not exist yet: HttpClient reports Unsupported there, so every one of
# these tests would GTEST_SKIP. They come back with the WinHTTP backend.
if(NOT WIN32)
	target_sources(${PROJECT_NAME} PRIVATE
		test_HttpClient.cpp
		test_AssetStore.cpp
	)
endif()

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::Net
		gtest_main
		gtest
)

tanh_copy_runtime_dlls(${PROJECT_NAME})

if(CMAKE_CROSSCOMPILING)
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME})
endif()
