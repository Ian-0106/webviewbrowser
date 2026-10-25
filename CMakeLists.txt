cmake_minimum_required(VERSION 3.15)
project(WebviewExample LANGUAGES C CXX)

add_executable(webview_example main.cpp activity_store.cpp notification_manager.cpp)
target_compile_features(webview_example PRIVATE cxx_std_17)

include(CTest)
if(BUILD_TESTING)
    add_executable(activity_store_test activity_store_test.cpp activity_store.cpp)
    target_compile_features(activity_store_test PRIVATE cxx_std_17)
    target_link_libraries(activity_store_test PRIVATE ole32 shell32)
    add_test(NAME activity_store_test COMMAND activity_store_test)
    add_executable(native_security_test native_security_test.cpp activity_store.cpp notification_manager.cpp)
    target_compile_features(native_security_test PRIVATE cxx_std_17)
    target_link_libraries(native_security_test PRIVATE ole32 oleaut32 user32 gdi32 shell32 comctl32 runtimeobject propsys)
    add_test(NAME native_security_test COMMAND native_security_test)
    add_executable(native_webview_security_smoke native_webview_security_smoke.cpp activity_store.cpp notification_manager.cpp)
    target_compile_features(native_webview_security_smoke PRIVATE cxx_std_17)
    target_link_libraries(native_webview_security_smoke PRIVATE ole32 oleaut32 user32 gdi32 shell32 comctl32 runtimeobject propsys)
    add_test(NAME native_webview_security_smoke COMMAND native_webview_security_smoke)
    set_tests_properties(native_webview_security_smoke PROPERTIES LABELS integration TIMEOUT 60)
    if(MSVC)
        target_compile_options(activity_store_test PRIVATE /UNDEBUG)
        target_compile_options(native_security_test PRIVATE /utf-8 /UNDEBUG)
        target_compile_options(native_webview_security_smoke PRIVATE /utf-8 /UNDEBUG)
    endif()
endif()

if(MSVC)
    target_compile_options(webview_example PRIVATE /utf-8)
    target_compile_options(webview_example PRIVATE /W4)
endif()

target_link_libraries(webview_example PRIVATE
    ole32
    oleaut32
    user32
    gdi32
    shell32
    comctl32
    runtimeobject
    propsys
)

if(WIN32)
    set_target_properties(webview_example PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
