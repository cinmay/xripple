cmake_minimum_required(VERSION 3.10)
project(xripple C)

# Use C99
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find the X11 package
find_package(X11 REQUIRED)

# Add the executable (assumes your source is xrp.c)
add_executable(xripple xrp.c)

# Include the X11 header directory
target_include_directories(xripple PRIVATE ${X11_INCLUDE_DIR})

# Link against X11 and math libraries
target_link_libraries(xripple ${X11_LIBRARIES} m)
