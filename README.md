# B(l)utter
Flutter Mobile Application Reverse Engineering Tool by Compiling Dart AOT Runtime

Currently the application supports Android libapp.so (arm64) and, on this branch, iOS `App` (Mach-O arm64). See [iOS Usage](#ios-usage).
Also the application is currently work only against recent Dart versions.

For high priority missing features, see [TODO](#todo)


## Environment Setup
This application uses C++20 Formatting library. It requires very recent C++ compiler such as g++>=13, Clang>=16.

I recommend using Linux OS (only tested on Deiban sid/trixie) because it is easy to setup.

### Debian Unstable (gcc 13)
**_NOTE:_**
Use ONLY Debian/Ubuntu version that provides gcc>=13 from its own main repository.
Using ported gcc to old Debian/Ubuntu version does not work.

- Install build tools and depenencies
```
apt install python3-pyelftools python3-requests git cmake ninja-build \
    build-essential pkg-config libicu-dev libcapstone-dev
```

### Windows
- Install git and python 3
- Install latest Visual Studio with "Desktop development with C++" and "C++ CMake tools"
- Install required libraries (libcapstone and libicu4c)
```
python scripts\init_env_win.py
```
- Start "x64 Native Tools Command Prompt"

### macOS Sequoia
- Install XCode
- Install required tools
```
brew install cmake ninja pkg-config icu4c capstone
pip3 install pyelftools requests
```

### macOS Ventura and Sonoma (clang 16)
- Install XCode
- Install clang 16 and required tools
```
brew install llvm@16 cmake ninja pkg-config icu4c capstone
pip3 install pyelftools requests
```

## Usage
Extract "lib" directory from apk file
```
python3 blutter.py path/to/app/lib/arm64-v8a out_dir
```
The blutter.py will automatically detect the Dart version from the flutter engine and call executable of blutter to get the information from libapp.so.

If the blutter executable for required Dart version does not exists, the script will automatically checkout Dart source code and compiling it.

## iOS Usage
This branch adds support for iOS Flutter apps (arm64 Mach-O, thin or FAT/universal).

1. Get the decrypted `.ipa` and unzip it (an `.ipa` is a zip file). App Store binaries are FairPlay-encrypted, so you must dump a decrypted copy from a jailbroken device first.
2. Copy the two Flutter binaries into one directory:
```
mkdir ios_in
cp Payload/<Name>.app/Frameworks/App.framework/App ios_in/App
cp Payload/<Name>.app/Frameworks/Flutter.framework/Flutter ios_in/Flutter
```
3. Run blutter on that directory:
```
python3 blutter.py ios_in out_dir
```
blutter detects the Dart version from `Flutter` and picks the iOS build (`blutter_dartvm<ver>_ios_arm64`). On the first run for each Dart version it fetches and compiles the Dart SDK, which takes a while and needs internet access.

Notes:
- Only arm64 is supported. Apps without compressed pointers get a separate `_no-compressed-ptrs` build automatically.
- Direct `.ipa` input is not supported yet; extract the files as above.
- Building needs recent Xcode Command Line Tools. If the build fails with an "outdated Command Line Tools" error, update them in System Settings → Software Update.

## Update
You can use ```git pull``` to update and run blutter.py with ```--rebuild``` option to force rebuild the executable
```
python3 blutter.py path/to/app/lib/arm64-v8a out_dir --rebuild
```

## Output files
- **asm/\*** libapp assemblies with symbols
- **blutter_frida.js** the frida script template for the target application
- **objs.txt** complete (nested) dump of Object from Object Pool
- **pp.txt** all Dart objects in Object Pool


## Directories
- **bin** contains blutter executables for each Dart version in "blutter_dartvm\<ver\>\_\<os\>\_\<arch\>" format
- **blutter** contains source code. need building against Dart VM library
- **build** contains building projects which can be deleted after finishing the build process
- **dartsdk** contains checkout of Dart Runtime which can be deleted after finishing the build process
- **external** contains 3rd party libraries for Windows only
- **packages** contains the static libraries of Dart Runtime
- **scripts** contains python scripts for getting/building Dart


## Generating Visual Studio Solution for Development
I use Visual Studio to delevlop Blutter on Windows. ```--vs-sln``` options can be used to generate a Visual Studio solution.
```
python blutter.py path\to\lib\arm64-v8a build\vs --vs-sln
```

## TODO
- More code analysis
  - Function arguments and return type
  - Some psuedo code for code pattern
- Generate better Frida script
  - More internal classes
  - Object modification
- Obfuscated app (still missing many functions)
- Reading iOS binary
- Input as apk or ipa
