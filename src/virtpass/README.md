# Virtpass
Give you a set of apis to dev applications can runs on Virtpass platform.

VirtPass is an abstract layer conception that can be implemented by emulator to provide bridged apis:
- Android ndk-like abis: GameActivity lifecycle API, Looper API, sensors API, Input API, ANativeWindow API,
User mode
- EGL/GLES apis（已嵌入未测试）
- Other apis


stub headers [text](include)
win host [text](src/virtpass/win32-host)

android host [text](src/virtpass/android-host)


## 扩充应遵循
VirtPass ABI 中NDK部分，在进化时必须严格跟随 Android NDK 的 ABI 签名，可以根据情况仅使用某类的子集。