Give you a set of apis to dev applications can runs on Virtpass platform.

VirtPass is an abstract layer that can be implemented by emulator to provides:
- Android ndk-like abis: GameActivity lifecycle API, Looper API, sensors API, Input API, ANativeWindow API,
User mode
- EGL/GLES apis


VirtPass ABI 进化时严格跟随 Android NDK 的 ABI 规范，根据情况使用合适大小的子集。