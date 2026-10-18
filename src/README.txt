@fyxod_cpp source (vendored, self-contained)
include/ImGui            imgui v1.93.0 @ c1aaac65e896a45f81f47a4d68f9959e0f86c859 (subset, as used)
include/Android_touch    enenH/AndroidImgui @ 63c9d83620665f87dce26e0a6de9f209b6d69593 (subset) + local patch
include/native_surface   ANativeWindowCreator.h (local)
cheat.cpp                payload + host (one file, -DPAYLOAD / -DHOST)
build.sh                 offline build: NDK=/path/to/android-ndk RELEASE=true bash build.sh
no git clone anywhere. out/@fyxod_cpp_run.sh is the single file to push to the phone.
