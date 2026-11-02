#!/bin/sh
# Compila guest/fastlib.c y guest/fastfloat.c para ARM y genera src/fastlib_blob.h y src/fastfloat_blob.h.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
# blob <fuente> <cabecera> <espacio de nombres> <prefijo> <opciones de clang>
blob() {
    clang $5 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fPIC -ffunction-sections -c "$1" -o $T/f.o
    ld.lld -shared -Bsymbolic --no-undefined -Ttext=0 -o $T/f.so $T/f.o
    llvm-objcopy -O binary --only-section=.text $T/f.so $T/f.bin
    python3 - "$T" "$1" "$3" "$4" > "$2" <<'PY'
import subprocess, sys
t, src, ns, pre = sys.argv[1:5]
data = open(t + "/f.bin", "rb").read()
syms = {}
for line in subprocess.check_output(["llvm-nm", t + "/f.so"]).decode().splitlines():
    p = line.split()
    if len(p) == 3 and p[2].startswith(pre) and p[1] in "TtDdRr":
        syms[p[2]] = int(p[0], 16)
print("// Generado por tools/compilar_fastlib.sh a partir de %s. No editar a mano." % src)
print("#pragma once")
print("#include <cstdint>")
print("namespace %s {" % ns)
print("const uint8_t kCode[] = {" + ",".join(str(b) for b in data) + "};")
print("struct Sym { const char* name; uint32_t off; };")
print("const Sym kSyms[] = {")
for k, v in sorted(syms.items()):
    print('    {"%s", 0x%x},' % (k, v))
print("};")
print("}  // namespace %s" % ns)
PY
    echo "$2 generado"
}
blob guest/fastlib.c src/fastlib_blob.h fastlib fl_ "--target=thumbv7a-none-eabi -mfloat-abi=soft"
blob guest/fastfloat.c src/fastfloat_blob.h fastfloat ff_ "--target=armv7a-none-eabi -marm -mfloat-abi=softfp -mfpu=vfpv3-d16"
rm -rf $T
