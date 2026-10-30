cmake_minimum_required(VERSION 3.10)
project(meostar)

# Thêm thư mục Dobby
add_subdirectory(Dobby)

find_library(log-lib log)

# Tạo thư viện meostar
add_library(meostar SHARED libmeostar.cpp)

# QUAN TRỌNG: Liên kết với dobby_static thay vì dobby
target_link_libraries(meostar dobby_static ${log-lib})
