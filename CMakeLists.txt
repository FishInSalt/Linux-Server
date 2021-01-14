cmake_minimum_required(VERSION 3.17)
project(WebServerWithThreadPool)

set(CMAKE_CXX_STANDARD 14)

add_executable(WebServerWithThreadPool main.cpp http_conn.cpp http_conn.h)
include_directories(/home/lz/CLionLib/lock)