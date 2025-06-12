cmake_minimum_required(VERSION 3.15)
project(MovementTracker)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 디버깅: 경로 출력
message(STATUS "CMAKE_TOOLCHAIN_FILE: ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "CMAKE_PREFIX_PATH: ${CMAKE_PREFIX_PATH}")
message(STATUS "VCPKG_TARGET_TRIPLET: ${VCPKG_TARGET_TRIPLET}")

# 수동으로 vcpkg 경로 설정
set(CMAKE_PREFIX_PATH "C:/Users/VICTUS/Desktop/vcpkg/installed/x64-windows" ${CMAKE_PREFIX_PATH})

# MongoDB 드라이버 찾기
find_package(bsoncxx CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)

# 실행파일 생성
add_executable(movement_tracker movement_tracker.cpp)

# MongoDB 라이브러리 링크
target_link_libraries(movement_tracker PRIVATE 
    $<IF:$<TARGET_EXISTS:mongo::bsoncxx_static>,mongo::bsoncxx_static,mongo::bsoncxx_shared>
    $<IF:$<TARGET_EXISTS:mongo::mongocxx_static>,mongo::mongocxx_static,mongo::mongocxx_shared>
)