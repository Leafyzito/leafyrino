# Building on macOS

Moltorino is built in CI as a universal macOS 13+ app.
Local dev machines for testing are available on Apple Silicon on macOS 13.

## Installing dependencies

1. Install Xcode and Xcode Command Line Utilities
1. Start Xcode, go into Settings -> Locations, and activate your Command Line Tools
1. Install [Homebrew](https://brew.sh/#install)  
   We use this for dependency management on macOS
1. Install all dependencies:  
   `brew install boost openssl@3 rapidjson cmake qt@6 libavif`

## Building

### Building from terminal

1. Open a terminal
1. Go to the project directory where you cloned Chatterino2 & its submodules
1. Create a build directory and go into it:  
   `mkdir build && cd build`
1. Run CMake. To enable Lua plugins in your build add `-DCHATTERINO_PLUGINS=ON` to this command.  
   `cmake -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@6 ..`
1. Build:  
   `make`

Your binary can now be found under `bin/Moltorino7.app/Contents/MacOS/Moltorino7`.

### Other building methods

You can achieve similar results by using an IDE like Qt Creator, although this is undocumented but if you know the IDE you should have no problems applying the terminal instructions to your IDE.
