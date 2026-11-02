// Coma flotante con instrucciones VFP para reemplazar las rutinas por software del juego
// (__aeabi_fadd, __aeabi_fmul, ...). libgame está compilado sin FPU: cada suma o comparación es
// una llamada a una rutina de decenas de instrucciones. Con VFP el traductor usa una sola
// instrucción del procesador real. El resultado es el mismo (IEEE 754, redondeo al más cercano).
// Convención "softfp": los argumentos y el resultado van en registros enteros, como en las
// rutinas originales. Se regenera con tools/compilar_fastlib.sh → src/fastfloat_blob.h
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int i32;

// Con números subnormales (|x| < 1.2e-38) la suma original redondea distinto que la FPU: en esos
// casos se llama a la rutina original para que el resultado sea idéntico al del juego.
// El host rellena la tabla con la dirección de la rutina original (antes del parche).
__attribute__((used, section(".text.table"))) static void* ff_orig[2] = {0, 0};  // [0]=fadd [1]=dadd
#define ORIG_FADD ((float (*)(float, float))ff_orig[0])
#define ORIG_DADD ((double (*)(double, double))ff_orig[1])

union F { float f; u32 u; };
union D { double d; u64 u; };
static inline int sub32(float x) { union F v = {x}; return (v.u & 0x7f800000u) == 0 && (v.u & 0x7fffffu); }
static inline int sub64(double x) { union D v = {x}; return (v.u & 0x7ff0000000000000ull) == 0 && (v.u & 0xfffffffffffffull); }
static inline float negf(float x) { union F v = {x}; v.u ^= 0x80000000u; return v.f; }
static inline double negd(double x) { union D v = {x}; v.u ^= 0x8000000000000000ull; return v.d; }

float ff_fadd(float a, float b) {
    if (sub32(a) || sub32(b)) return ORIG_FADD(a, b);
    float r = a + b;
    return sub32(r) ? ORIG_FADD(a, b) : r;
}
float ff_fsub(float a, float b) { return ff_fadd(a, negf(b)); }
float ff_frsub(float a, float b) { return ff_fadd(negf(a), b); }
float ff_fmul(float a, float b) { return a * b; }
float ff_fdiv(float a, float b) { return a / b; }
float ff_i2f(i32 a) { return (float)a; }
float ff_ui2f(u32 a) { return (float)a; }
i32 ff_f2iz(float a) { return (i32)a; }
u32 ff_f2uiz(float a) { return (u32)a; }
int ff_fcmpeq(float a, float b) { return a == b; }
int ff_fcmplt(float a, float b) { return a < b; }
int ff_fcmple(float a, float b) { return a <= b; }
int ff_fcmpge(float a, float b) { return a >= b; }
int ff_fcmpgt(float a, float b) { return a > b; }
int ff_fcmpun(float a, float b) { return a != a || b != b; }

double ff_dadd(double a, double b) {
    if (sub64(a) || sub64(b)) return ORIG_DADD(a, b);
    double r = a + b;
    return sub64(r) ? ORIG_DADD(a, b) : r;
}
double ff_dsub(double a, double b) { return ff_dadd(a, negd(b)); }
double ff_drsub(double a, double b) { return ff_dadd(negd(a), b); }
double ff_dmul(double a, double b) { return a * b; }
double ff_ddiv(double a, double b) { return a / b; }
double ff_i2d(i32 a) { return (double)a; }
double ff_ui2d(u32 a) { return (double)a; }
double ff_f2d(float a) { return (double)a; }
float ff_d2f(double a) { return (float)a; }
i32 ff_d2iz(double a) { return (i32)a; }
u32 ff_d2uiz(double a) { return (u32)a; }
int ff_dcmpeq(double a, double b) { return a == b; }
int ff_dcmplt(double a, double b) { return a < b; }
int ff_dcmple(double a, double b) { return a <= b; }
int ff_dcmpge(double a, double b) { return a >= b; }
int ff_dcmpgt(double a, double b) { return a > b; }
int ff_dcmpun(double a, double b) { return a != a || b != b; }
