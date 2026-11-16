add_library(camstream-common-log STATIC src/log.c)
add_library(camstream::common-log ALIAS camstream-common-log)
set_target_properties(camstream-common-log PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(camstream-common-log PUBLIC c_std_11)
target_compile_options(camstream-common-log PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(camstream-common-log PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(TARGETS camstream-common-log ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
