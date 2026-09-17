# ports/

Platform ports and packaging for targets the core library does not
build for out of the box.

Each port carries its own toolchain file and any platform glue the
freestanding core needs; the library sources themselves stay portable
C11 with no external dependencies.

Planned subdirectories: `arduino/`, `esp32/`, `android/`, `windows/`.
