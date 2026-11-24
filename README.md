# Paragon

A personal-use 64-bit native port of the Android game *Zenonia* (Gamevil, 2010), for owners of the original APK.
The original only ships 32-bit ARMv5 native code (`lib/armeabi/libmerkava.so`) and cannot run on 64-bit-only phones.

**This repository contains no game code or data.** It holds only the tools. You supply your own APK at build time,
and everything derived from it is written to `work/` and `generated/`, which are git-ignored.

## Approach: static recompilation

1. **Reference harness** (`tools/harness/`): runs the original 32-bit library in a CPU emulator (Unicorn) with
   stubbed libc, OpenGL ES 1.x and JNI. It is the test oracle for everything below.
2. **Recompiler**: translates every function of the ARMv5TE/Thumb library into C (capstone-based), keeping a 32-bit
   guest address space (guest pointer = host base + offset), so data layout stays identical.
3. **Runtime**: host implementations of the 67 imported functions (libc, GLES1, log), a JNI bridge that forwards to
   the real JNIEnv, and a fix for the Dalvik-only array access in `MC_knlGetResource` (reads `jbyteArray + 0x10`).
4. **Android build**: the generated C plus runtime is compiled with the NDK for `arm64-v8a` (and other ABIs) and
   packaged with the original Java layer into a new APK.
5. **Improvements**: replace functions with readable C over time; widescreen, touch controls, modern saves.

## Milestones

- [x] M1: original library boots in the harness and loads assets
- [ ] M2: harness renders the title screen to a PNG (software GLES1)
- [ ] M3: recompiler output runs on x86-64 Linux and matches the harness frame by frame
- [ ] M4: playable arm64 APK on a 64-bit-only phone
- [ ] M5: widescreen and other improvements

## Facts about the original library (v1.2.0)

- `.text` about 435 KB, Thumb-1 with some ARM, about 3,800 named functions (not stripped)
- Imports: libc basics, `__cxa_*` guards/unwind, about 27 GLES 1.x calls, `__android_log_print`
- 16 JNI exports on `com.gamevil.nexus2.Natives`; Java callbacks `isAssetExist`, `readAssete`
