# IMBridge_CMake
ob_define(PYTHON_DIR "/usr/local/python311")

set(PYTHON_LIB_DIR "${PYTHON_DIR}/lib")

message(STATUS "Set Python dir ${PYTHON_DIR}")

add_library(python_lib INTERFACE)
target_include_directories(
  python_lib INTERFACE
  "${PYTHON_DIR}/include/python3.11"
  "${PYTHON_LIB_DIR}"
  "${PYTHON_LIB_DIR}/python3.11/site-packages/numpy/core/include/")
target_link_libraries(python_lib INTERFACE
    -L/usr/local/python311/lib -lpython3.11 -lpthread -ldl  -lutil -lm  -Xlinker -export-dynamic)