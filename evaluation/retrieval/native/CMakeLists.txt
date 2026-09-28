cmake_minimum_required(VERSION 3.24)
project(retrieval_native_proof CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same pinned archive the engine links
set(OPENVINO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../external/openvino" CACHE PATH "OpenVINO GenAI archive")
find_package(OpenVINOGenAI REQUIRED PATHS "${OPENVINO_ROOT}/runtime/cmake" NO_DEFAULT_PATH)

add_executable(proof proof.cpp)
target_link_libraries(proof PRIVATE openvino::genai)
