CMAKE_MINIMUM_REQUIRED(VERSION 3.0)
PROJECT(TinyCoroutine CXX)
INCLUDE_DIRECTORIES(${CMAKE_PROJECT_DIR})
SET(SRC_LIST "test.cpp" "Fiber.cpp" "Scheduler.cpp" "IOManager.cpp" "Timer.cpp" "FdManager.cpp" "Hook.cpp")
ADD_EXECUTABLE(test ${SRC_LIST})
