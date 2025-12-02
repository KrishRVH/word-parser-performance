cmake_minimum_required(VERSION 3.16)
project(word_parser_performance C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(wordcount_c wordcount.c)
