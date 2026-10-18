#!/usr/bin/env bash
# build.sh: собирает @fyxod_cpp из этого каталога. Нужен только Android NDK (r26+), интернет НЕ нужен.
#   NDK=/path/to/android-ndk RELEASE=true bash build.sh      ->  out/@fyxod_cpp_run.sh + out/@fyxod_cpp
set -e
cd "$(dirname "$0")"
NDK="${NDK:-${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-$ANDROID_NDK}}}"
[ -d "$NDK" ] || { echo "NDK not found: set NDK=/path/to/android-ndk (r26+)"; exit 1; }
echo "NDK=$NDK"
BIN=${BIN:-$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin}
CXX=${CXX:-$BIN/aarch64-linux-android24-clang++}
I=$PWD/include
REL=""; [ "${RELEASE:-false}" = "true" ] && REL="-DNOLOG"
HARD="-ffunction-sections -fdata-sections -fvisibility=hidden"
LDH="-Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--build-id=none"
$CXX -shared -fPIC -O2 -std=c++20 $HARD -DPAYLOAD $REL -fno-exceptions -fno-rtti -nostdlib++ \
    cheat.cpp -ldl -llog -lm -Wl,--no-undefined $LDH -o libcheat.so
$BIN/llvm-strip --strip-all libcheat.so
$BIN/llvm-objcopy --remove-section=.comment libcheat.so || true
echo "--- libcheat.so NEEDED (must not contain libc++):"; readelf -d libcheat.so | grep NEEDED
! readelf -d libcheat.so | grep -q 'c++'
# embed libcheat.so into the host, encrypted with a fresh random key per build (no plain .so ships anywhere)
python3 - <<'PYEOF'
import secrets
d = open('libcheat.so', 'rb').read()
M = (1 << 64) - 1
key = secrets.randbits(64) | 1
x = key; out = bytearray()
for b in d:
    x ^= (x << 13) & M; x ^= x >> 7; x ^= (x << 17) & M
    out.append(b ^ ((x >> 24) & 0xFF))
open('payload_blob.h', 'w').write('static const unsigned long long PL_KEY = 0x%016xULL;\nstatic const unsigned long PL_LEN = %d;\nstatic const unsigned char PL_BLOB[] = {%s};\n' % (key, len(d), ','.join(map(str, out))))
PYEOF
CPPS="$I/ImGui/imgui.cpp $I/ImGui/imgui_draw.cpp $I/ImGui/imgui_widgets.cpp $I/ImGui/imgui_tables.cpp \
      $I/ImGui/backends/imgui_impl_android.cpp $I/ImGui/backends/imgui_impl_opengl3.cpp $I/Android_touch/TouchHelperA.cpp"
$CXX -O2 -std=c++20 $HARD -DHOST -DEMBED_PAYLOAD $REL -static-libstdc++ -I. -I$I -I$I/ImGui -I$I/native_surface -I$I/Android_touch \\
    cheat.cpp $CPPS -llog -ldl -landroid -lEGL -lGLESv3 $LDH \\
    -Wl,--dynamic-linker,/system/bin/linker64 \\
    -Wl,--rpath,/system/lib64 \\
    -Wl,--rpath,/vendor/lib64 \\
    -o @fyxod_cpp
$BIN/llvm-strip --strip-all @fyxod_cpp
$BIN/llvm-objcopy --remove-section=.comment @fyxod_cpp || true
# verify no unexpected NEEDED on Android 16
readelf -d @fyxod_cpp | grep NEEDED
echo "--- @fyxod_cpp NEEDED:"; readelf -d @fyxod_cpp | grep NEEDED
ls -l libcheat.so @fyxod_cpp
rm -f libcheat.so payload_blob.h
# ---- pack ----
mkdir -p out
cat > out/@fyxod_cpp_run.sh <<'OXEOF'
#!/system/bin/sh
# @fyxod_cpp_run.sh — готовый билд внутри, Termux/clang НЕ нужны. Только root.
#   sh /data/local/tmp/@fyxod_cpp_run.sh        распаковать, запустить игру, инжект + оверлей (Ctrl-C = выход)
#   sh /data/local/tmp/@fyxod_cpp_run.sh bg     то же, оверлей в фоне (лог: /data/local/tmp/@fyxod_cpp.log)
#   sh /data/local/tmp/@fyxod_cpp_run.sh stop
SELF=$(readlink -f "$0" 2>/dev/null || echo "$0")
[ "$(id -u)" != "0" ] && exec su -c "sh '$SELF' $*"

DIR=/data/local/tmp
PKG="com.catsbit.oxidesurvivalisland"
R='\033[31m' G='\033[32m' C='\033[36m' W='\033[0m'
die(){ printf "${R}[-] %s${W}\n" "$1"; exit 1; }
ok(){  printf "${G}[+] %s${W}\n" "$1"; }
inf(){ printf "${C}[*] %s${W}\n" "$1"; }

[ "$1" = "stop" ] && { for p in $(pidof @fyxod_cpp); do kill "$p"; done; ok "stopped"; exit 0; }

for p in $(pidof @fyxod_cpp); do kill "$p" 2>/dev/null; done; sleep 1
inf "распаковываю в $DIR ..."
sed '1,/^__PAYLOAD__$/d' "$SELF" | base64 -d | tar xf - -C "$DIR" || die "распаковка не удалась (скачай файл заново)"
rm -f "$DIR/libcheat.so"; chmod 755 "$DIR/@fyxod_cpp"

if [ -z "$(pidof "$PKG")" ]; then
    inf "запускаю игру..."
    monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
fi
inf "ждём il2cpp, инжект, оверлей. меню: тап [M] слева сверху"
if [ "$1" = "bg" ]; then
    nohup "$DIR/@fyxod_cpp" >"$DIR/@fyxod_cpp.log" 2>&1 &
    sleep 1; ok "в фоне. лог: $DIR/@fyxod_cpp.log   стоп: sh $SELF stop"; exit 0
fi
LOG=$DIR/@fyxod_cpp.log
{
    echo "== $(date)  android $(getprop ro.build.version.release) sdk $(getprop ro.build.version.sdk) $(getprop ro.product.model) $(getprop ro.product.cpu.abi)"
    "$DIR/@fyxod_cpp"
    RC=$?
    echo "[exit code $RC]"
    if [ "$RC" != 0 ]; then echo "--- logcat crash buffer:"; logcat -d -b crash 2>/dev/null | tail -60; fi
    echo "--- logcat @fyxod_cpp (game side):"; logcat -d -s @fyxod_cpp 2>/dev/null | tail -40
} 2>&1 | tee -i "$LOG"
ok "лог: $LOG"
exit 0
__PAYLOAD__
OXEOF
tar cf - @fyxod_cpp | base64 -w 100 >> out/@fyxod_cpp_run.sh
cp @fyxod_cpp out/
ls -l out
