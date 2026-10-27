// The Java bridge.
//  Java -> guest: the 11 Java_* natives below forward into the original code.
//  Guest -> Java: one fake JavaVM/JNIEnv in guest memory whose slots are table thunks.
// Java references and method IDs are 64-bit, so the guest sees small handles instead.
#include <jni.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "rr2.h"

static u32 g_env, g_vm;          // the fake JNIEnv* / JavaVM* the guest holds
thread_local JNIEnv* t_env;      // the real one for this thread
thread_local int t_depth;

// ---- handles ----
static std::mutex g_lock;
static std::vector<jobject> g_objs;  // handle h -> g_objs[h-1]
static std::vector<u32> g_free;
thread_local std::vector<u32> t_locals;  // released when the outermost Java->guest call returns

static u32 obj(jobject o, bool global = false) {
    if (!o) return 0;
    std::lock_guard lk(g_lock);
    u32 h;
    if (!g_free.empty()) h = g_free.back(), g_free.pop_back(), g_objs[h - 1] = o;
    else g_objs.push_back(o), h = u32(g_objs.size());
    if (!global) t_locals.push_back(h);
    return h;
}
static jobject ref(u32 h) {
    if (!h) return nullptr;
    std::lock_guard lk(g_lock);
    return g_objs[h - 1];
}
static void release(u32 h) {
    std::lock_guard lk(g_lock);
    g_objs[h - 1] = nullptr, g_free.push_back(h);
}

struct Method { jmethodID id; std::string sig; bool present; };  // present: frame swap
static std::vector<Method> g_methods;
static std::unordered_map<jmethodID, u32> g_method_handles;

static u32 method(jmethodID id, const char* name, const char* sig) {
    if (!id) return 0;
    std::lock_guard lk(g_lock);
    auto [it, fresh] = g_method_handles.try_emplace(id, u32(g_methods.size() + 1));
    if (fresh) g_methods.push_back({id, sig, !strcmp(name, "EglUpdateDisplay")});
    return it->second;
}
static Method method_at(u32 h) {
    std::lock_guard lk(g_lock);
    return g_methods[h - 1];
}

// ---- direct ByteBuffers: Java's memory is above 4 GB, so the guest gets a mirror ----
struct Mirror { void* host; u8* guest; jlong n; };
static std::unordered_map<void*, Mirror> g_mirrors;
thread_local std::vector<Mirror*> t_dirty;  // copied back when the outermost call returns

static u32 buffer_address(jobject buf) {
    void* host = t_env->GetDirectBufferAddress(buf);
    jlong n = t_env->GetDirectBufferCapacity(buf);
    if (!host) return 0;
    Mirror* m;
    {
        std::lock_guard lk(g_lock);
        m = &g_mirrors[host];
        if (m->n < n) guest_free(m->guest), m->guest = (u8*)guest_malloc(n), m->host = host, m->n = n;
    }
    memcpy(m->guest, host, n);
    t_dirty.push_back(m);
    return gaddr(m->guest);
}

// Every Java->guest entry opens a Scope; leaving the outermost one mirrors JNI's own
// "local refs die when the native method returns" rule.
struct Scope {
    JNIEnv* prev;
    explicit Scope(JNIEnv* e) : prev(t_env) { t_env = e, t_depth++; }
    ~Scope() {
        if (--t_depth == 0) {
            for (Mirror* m : t_dirty) memcpy(m->host, m->guest, m->n);
            for (u32 h : t_locals) release(h);
            t_dirty.clear(), t_locals.clear();
        }
        t_env = prev;
    }
};

// ---- Call<Type>Method[V] ----
// Reads Java args from a guest va_list per the method signature (floats arrive as doubles).
static std::vector<jvalue> va_args(const std::string& sig, u32 ap) {
    VaReader va{nullptr, 0, ap};
    std::vector<jvalue> v;
    for (size_t i = 1; sig[i] != ')'; i++) {
        jvalue x{};
        switch (sig[i]) {
        case 'J': x.j = jlong(va.dword()); break;
        case 'D': x.d = std::bit_cast<double>(va.dword()); break;
        case 'F': x.f = float(std::bit_cast<double>(va.dword())); break;
        case 'L':
        case '[':
            x.l = ref(va.word());
            while (sig[i] == '[') i++;
            if (sig[i] == 'L') i = sig.find(';', i);
            break;
        default: x.i = jint(va.word());  // Z B C S I: little-endian, so the low bytes line up
        }
        v.push_back(x);
    }
    return v;
}

// (env, object-or-class, methodID, va_list)
static void call_method(Ctx& c, bool is_static) {
    JNIEnv* e = t_env;
    jobject o = ref(c.arg(1));
    Method m = method_at(c.arg(2));
    if (m.present) on_frame(), overlay_draw();  // frame finished: apply live settings, draw menu
    auto args = va_args(m.sig, c.arg(3));
    const jvalue* a = args.data();
#define CALL(T) (is_static ? e->CallStatic##T##MethodA(jclass(o), m.id, a) : e->Call##T##MethodA(o, m.id, a))
    switch (m.sig[m.sig.find(')') + 1]) {
    case 'V': is_static ? e->CallStaticVoidMethodA(jclass(o), m.id, a) : e->CallVoidMethodA(o, m.id, a); break;
    case 'Z': c.ret(CALL(Boolean)); break;
    case 'I': c.ret(u32(CALL(Int))); break;
    case 'L':
    case '[': c.ret(obj(CALL(Object))); break;
    default: LOG("unsupported return type in %s", m.sig.c_str());
    }
#undef CALL
}

// ---- the JNIEnv slots the game uses (found by scanning its code); others log by number ----
static const std::pair<int, const char*> kSlots[] = {
    {4, "GetVersion"}, {6, "FindClass"}, {17, "ExceptionClear"}, {21, "NewGlobalRef"},
    {22, "DeleteGlobalRef"}, {23, "DeleteLocalRef"}, {29, "NewObjectV"}, {33, "GetMethodID"},
    {35, "CallObjectMethodV"}, {38, "CallBooleanMethodV"}, {50, "CallIntMethodV"},
    {62, "CallVoidMethodV"}, {113, "GetStaticMethodID"}, {130, "CallStaticIntMethodV"},
    {142, "CallStaticVoidMethodV"}, {167, "NewStringUTF"}, {169, "GetStringUTFChars"},
    {170, "ReleaseStringUTFChars"}, {171, "GetArrayLength"}, {176, "NewByteArray"},
    {179, "NewIntArray"}, {184, "GetByteArrayElements"}, {192, "ReleaseByteArrayElements"},
    {211, "SetIntArrayRegion"}, {228, "ExceptionCheck"}, {230, "GetDirectBufferAddress"},
    {231, "GetDirectBufferCapacity"},
};

#define E t_env
static Shims jni_shims = {
    {"jni:GetVersion", [](Ctx& c) { c.ret(JNI_VERSION_1_6); }},
    {"jni:FindClass", [](Ctx& c) { c.ret(obj(E->FindClass(ptr<char>(c.arg(1))))); }},
    {"jni:ExceptionCheck", [](Ctx& c) { c.ret(E->ExceptionCheck()); }},
    {"jni:ExceptionClear", [](Ctx&) { E->ExceptionClear(); }},
    {"jni:NewGlobalRef", [](Ctx& c) { c.ret(obj(E->NewGlobalRef(ref(c.arg(1))), true)); }},
    {"jni:DeleteGlobalRef", [](Ctx& c) { if (u32 h = c.arg(1)) E->DeleteGlobalRef(ref(h)), release(h); }},
    {"jni:DeleteLocalRef", [](Ctx& c) {
         if (u32 h = c.arg(1)) E->DeleteLocalRef(ref(h)), std::erase(t_locals, h), release(h);
     }},
    {"jni:GetMethodID", [](Ctx& c) {
         auto sig = ptr<char>(c.arg(3));
         c.ret(method(E->GetMethodID(jclass(ref(c.arg(1))), ptr<char>(c.arg(2)), sig), ptr<char>(c.arg(2)), sig));
     }},
    {"jni:GetStaticMethodID", [](Ctx& c) {
         auto sig = ptr<char>(c.arg(3));
         c.ret(method(E->GetStaticMethodID(jclass(ref(c.arg(1))), ptr<char>(c.arg(2)), sig), ptr<char>(c.arg(2)), sig));
     }},
    {"jni:CallObjectMethodV", [](Ctx& c) { call_method(c, false); }},
    {"jni:CallBooleanMethodV", [](Ctx& c) { call_method(c, false); }},
    {"jni:CallIntMethodV", [](Ctx& c) { call_method(c, false); }},
    {"jni:CallVoidMethodV", [](Ctx& c) { call_method(c, false); }},
    {"jni:CallStaticIntMethodV", [](Ctx& c) { call_method(c, true); }},
    {"jni:CallStaticVoidMethodV", [](Ctx& c) { call_method(c, true); }},
    {"jni:NewObjectV", [](Ctx& c) {
         Method m = method_at(c.arg(2));
         c.ret(obj(E->NewObjectA(jclass(ref(c.arg(1))), m.id, va_args(m.sig, c.arg(3)).data())));
     }},
    {"jni:NewStringUTF", [](Ctx& c) { c.ret(obj(E->NewStringUTF(ptr<char>(c.arg(1))))); }},
    {"jni:GetStringUTFChars", [](Ctx& c) {
         auto s = jstring(ref(c.arg(1)));
         const char* host = E->GetStringUTFChars(s, nullptr);
         size_t n = strlen(host) + 1;
         void* g = guest_malloc(n);
         memcpy(g, host, n), E->ReleaseStringUTFChars(s, host);
         if (c.arg(2)) *ptr<u8>(c.arg(2)) = JNI_TRUE;
         c.ret(gaddr(g));
     }},
    {"jni:ReleaseStringUTFChars", [](Ctx& c) { guest_free(ptr(c.arg(2))); }},
    {"jni:GetArrayLength", [](Ctx& c) { c.ret(E->GetArrayLength(jarray(ref(c.arg(1))))); }},
    {"jni:NewByteArray", [](Ctx& c) { c.ret(obj(E->NewByteArray(jint(c.arg(1))))); }},
    {"jni:NewIntArray", [](Ctx& c) { c.ret(obj(E->NewIntArray(jint(c.arg(1))))); }},
    {"jni:GetByteArrayElements", [](Ctx& c) {
         auto a = jbyteArray(ref(c.arg(1)));
         jint n = E->GetArrayLength(a);
         void* g = guest_malloc(n);
         E->GetByteArrayRegion(a, 0, n, (jbyte*)g);
         if (c.arg(2)) *ptr<u8>(c.arg(2)) = JNI_TRUE;
         c.ret(gaddr(g));
     }},
    {"jni:ReleaseByteArrayElements", [](Ctx& c) {
         auto a = jbyteArray(ref(c.arg(1)));
         if (c.arg(3) != JNI_ABORT) E->SetByteArrayRegion(a, 0, E->GetArrayLength(a), ptr<jbyte>(c.arg(2)));
         if (c.arg(3) != JNI_COMMIT) guest_free(ptr(c.arg(2)));
     }},
    {"jni:SetIntArrayRegion", [](Ctx& c) {
         E->SetIntArrayRegion(jintArray(ref(c.arg(1))), jint(c.arg(2)), jint(c.arg(3)), ptr<jint>(c.arg(4)));
     }},
    {"jni:GetDirectBufferAddress", [](Ctx& c) { c.ret(buffer_address(ref(c.arg(1)))); }},
    {"jni:GetDirectBufferCapacity", [](Ctx& c) { c.ret(u32(E->GetDirectBufferCapacity(ref(c.arg(1))))); }},
    // JavaVM: every guest thread is already a Java thread inside a Scope
    {"vm:GetEnv", [](Ctx& c) { *ptr<u32>(c.arg(1)) = g_env, c.ret(JNI_OK); }},
    {"vm:AttachCurrentThread", [](Ctx& c) { *ptr<u32>(c.arg(1)) = g_env, c.ret(JNI_OK); }},
    {"vm:DetachCurrentThread", [](Ctx& c) { c.ret(JNI_OK); }},
};
#undef E

void jni_init(JavaVM* vm) {
    u32* fns = (u32*)guest_malloc(233 * 4);
    for (int i = 0; i < 233; i++) fns[i] = thunk_for(("jni slot " + std::to_string(i)).c_str());
    for (auto [i, name] : kSlots) fns[i] = thunk_for((std::string("jni:") + name).c_str());
    u32* env = (u32*)guest_malloc(4);
    *env = gaddr(fns);
    g_env = gaddr(env);

    const char* vm_slots[] = {"vm slot 0", "vm slot 1", "vm slot 2", "vm:DestroyJavaVM",
                              "vm:AttachCurrentThread", "vm:DetachCurrentThread", "vm:GetEnv",
                              "vm:AttachCurrentThread"};
    u32* vfns = (u32*)guest_malloc(sizeof(vm_slots) / sizeof(*vm_slots) * 4);
    for (u32 i = 0; i < 8; i++) vfns[i] = thunk_for(vm_slots[i]);
    u32* jvm = (u32*)guest_malloc(4);
    *jvm = gaddr(vfns);
    g_vm = gaddr(jvm);

    JNIEnv* e;
    vm->GetEnv((void**)&e, JNI_VERSION_1_6);
    Scope s(e);
    if (u32 f = guest_sym("JNI_OnLoad")) LOG("guest JNI_OnLoad -> 0x%x", call(f, g_vm, 0));
}

// ---- Java -> guest ----
#define ENTER static const u32 fn = guest_sym(__func__); Scope scope(env)
#define EXPORT extern "C" JNIEXPORT

EXPORT jint Java_com_polarbit_fuse_Jni_OnCreate(JNIEnv* env, jclass c, jstring a, jstring b) {
    ENTER; return jint(call(fn, g_env, obj(c), obj(a), obj(b)));
}
EXPORT jint Java_com_polarbit_fuse_Jni_OnDestroy(JNIEnv* env, jclass c) {
    ENTER; return jint(call(fn, g_env, obj(c)));
}
EXPORT jint Java_com_polarbit_fuse_Jni_OnEvent(JNIEnv* env, jclass c, jint a, jint b, jint d, jint e, jint f) {
    ENTER;
    run_game_thread_queue();                                    // menu actions; no-op off the game thread
    if (a == 3) overlay_surface(d, e);                          // surfaceChanged: (3, 0, w, h, 0)
    if (a == 1 && b == 1 && overlay_touch(d, e, f)) return 1;  // touch: (1, 1, x, y, action|id)
    if (a == 1 && b == 0 && !remap_key(d)) return 1;           // key: (1, 0, keycode, 0, down)
    return jint(call(fn, g_env, obj(c), a, b, d, e, f));
}
EXPORT jint Java_com_polarbit_fuse_Jni_OnEventMessage(JNIEnv* env, jclass c, jint a, jint b, jint d, jint e, jstring s) {
    ENTER; return jint(call(fn, g_env, obj(c), a, b, d, e, obj(s)));
}
EXPORT jint Java_com_polarbit_fuse_Jni_OnEventMessage2(JNIEnv* env, jclass c, jint a, jint b, jint d, jstring s, jstring t) {
    ENTER; return jint(call(fn, g_env, obj(c), a, b, d, obj(s), obj(t)));
}
EXPORT jint Java_com_polarbit_fuse_Jni_OnEventMessage3(JNIEnv* env, jclass c, jint a, jint b, jint d, jint e, jobject buf, jint n) {
    ENTER; return jint(call(fn, g_env, obj(c), a, b, d, e, obj(buf), n));
}
EXPORT void Java_com_polarbit_fuse_Jni_AudioMix(JNIEnv* env, jclass c, jobject buf, jint n) {
    ENTER; call(fn, g_env, obj(c), obj(buf), n);
}
EXPORT jstring Java_com_polarbit_fuse_Jni_FuseDecrypt(JNIEnv* env, jclass c, jbyteArray a) {
    ENTER; return jstring(ref(call(fn, g_env, obj(c), obj(a))));
}
EXPORT jbyteArray Java_com_polarbit_fuse_Jni_FuseEncrypt(JNIEnv* env, jclass c, jstring s) {
    ENTER; return jbyteArray(ref(call(fn, g_env, obj(c), obj(s))));
}
EXPORT void Java_com_polarbit_fuse_MainTask_FuseOnInit(JNIEnv* env, jclass c, jobject a, jobject b, jobject d,
                                                     jobject e, jobject f, jobject g) {
    ENTER; call(fn, g_env, obj(c), obj(a), obj(b), obj(d), obj(e), obj(f), obj(g));
}
EXPORT jboolean Java_com_polarbit_fuse_MainTask_processTouchpadAsPointer(JNIEnv* env, jclass c, jint a, jobject v, jboolean z) {
    ENTER; return jboolean(call(fn, g_env, obj(c), a, obj(v), z));
}
