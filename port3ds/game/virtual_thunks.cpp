// Bodies for the virtual methods of src/'s local stand-in classes.
//
// A few matched Render()s call a member object's virtual through a local
// shadow class that only exists to give the call its slot number:
//
//     struct Foo { virtual void M0(int); ... virtual void M5(int); };
//     ((Container *)this)->obj1.M5(0);           // src/_ZN10BulletBill6RenderEv.cpp
//
// mwccarm dispatches that through the object's vtable, like the ROM does. GCC
// resolves a call on an object (not a pointer or reference) statically, to
// Foo::M5, which doesn't exist. These provide it, dispatching through slot 5 of
// the object's real vtable like the ROM.
typedef int (*Slot)(void *self, int arg);

static inline int via_slot5(void *self, int arg)
{
    return (*reinterpret_cast<Slot **>(self))[5](self, arg);
}

#define VIRTUAL_THUNK(cname, itanium)                                         \
    extern "C" int cname(void *self, int arg) __asm__(itanium);               \
    extern "C" int cname(void *self, int arg) { return via_slot5(self, arg); }

VIRTUAL_THUNK(vthunk_Foo_M5, "_ZN3Foo2M5Ei")              // BulletBill::Render
VIRTUAL_THUNK(vthunk_Sub_m5, "_ZN3Sub2m5EP5Thing")        // PowerStar::Render
VIRTUAL_THUNK(vthunk_Sub_m, "_ZN3Sub1mEP3Arg")            // Skeeter::Render
VIRTUAL_THUNK(vthunk_RenderSub_method5, "_ZN9RenderSub7method5Ei")  // Scuttlebug::Render
VIRTUAL_THUNK(vthunk_Base_vfunc, "_ZN4Base5vfuncEi")      // daTrsTrap_c::Render
