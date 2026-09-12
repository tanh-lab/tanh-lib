set(PROJECT_NAME test_net)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_Sha256.cpp
	test_HttpClient.cpp
	test_AssetStore.cpp
)

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
