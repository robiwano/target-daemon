# Build instructions

## Prerequisites

### Build tools
- CMake v3.0 or later (http://www.cmake.org)

### Libraries
- PIGPIO library (http://abyz.co.uk/rpi/pigpio/index.html).


#### Instructions
Use CMake to generate build files on your chosen platform, currently tested on Windows (with Visual Studio generators) and Raspbian Linux (Unix Makefiles generator). Then use the generated build files to build the daemon.

To run on Raspbian, install the PIGPIO library according to its instructions.

