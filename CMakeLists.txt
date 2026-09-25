cmake_minimum_required(VERSION 3.15)
project(WebviewExample LANGUAGES C CXX)

add_executable(webview_example main.cpp)
target_compile_features(webview_example PRIVATE cxx_std_17)

target_link_libraries(webview_example PRIVATE
    ole32
    oleaut32
    user32
    gdi32
    shell32
)

if(WIN32)
    set_target_properties(webview_example PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
