Merge with mimalloc v3.1.5.  Version integrated into Python as v2.1.2.

To see changes made to mimalloc sources:

    $ git log Include/internal/mimalloc Objects/mimalloc

Short list of changes:

    b525e31b7fc50 gh-134875: Fix mimallc build error for the old compilers (gh-134994)
    c600310663277 gh-134586: mark `_mi_assert_fail` as `noreturn`, `cold` and `throw` (#134624)
    317c496223972 gh-129748: Update mimalloc to use atomic store for mi_block_set_nextx (#134238)
    03f6c8e239723 gh-131675: Fix `mi_atomic_yield` in mimalloc on 32-bit ARM (gh-131784)
    feda9aa73ab95 gh-125444: Fix illegal instruction for older Arm architectures (#125574)
    4a6b1f179667e gh-123826: Fix unused function warnings in mimalloc on NetBSD (#123827)
    9017b95ff2dcf Fix typos (#123775)
    9e108b8719752 Fix typos in docs, error messages and comments (#123336)
    d061ffea7b408 gh-123022: Fix crash with  `Py_Initialize` in background thread (#123052)
    d005f2c1861db gh-121731: Fix mimalloc compile error on GNU/Hurd (#121732)
    31873bea47102 gh-121487: Fix deprecation warning for ATOMIC_VAR_INIT in mimalloc (gh-121488)
    0153fd094019b Fix typos in comments (#120821)
    6a97929a5ad76 Fix typos in comments (#120188)
    71cc0651e7904 gh-116984: Make mimalloc header includes relative to the current file (#118808)
    3fe03ccea6112 gh-117755: Fix mimalloc for huge allocation on s390x (#117809)
    2067da25796ea gh-117547: Fix mimalloc compile error on OpenBSD (#117548)
    c012c8ab7bb72 gh-115103: Delay reuse of mimalloc pages that store PyObjects (#115435)
    72714c0266ce6 gh-115103: Enable internal mimalloc assertions in debug builds (#116343)
    e7ba6e9dbe543 chore: fix typos (#116345)
    d7ddd90308324 gh-115491: Fix Clang compiler warning (#116153)
    cc82e33af978d gh-115491: Keep some fields valid across allocations (free-threading) (#115573)
    326119d3731f7 gh-112529: Use _PyThread_Id() in mimalloc in free-threaded build (#115488)
    31633f4473966 gh-115184: Fix refleak tracking issues in free-threaded build (#115188)
    412920a41efc6 gh-112532: Improve mimalloc page visiting (#114133)
    10d3f04aec745 gh-112808: Fix mimalloc build on Solaris (#112809)
    ae968d1032616 gh-112806: Remove unused function warnings during mimalloc build on Solaris (#>
    0b7476080b58e gh-112532: Tag mimalloc heaps and pages (#113742)
    fcb3c2a444709 gh-112532: Isolate abandoned segments by interpreter (#113717)
    acf3bcc886198 gh-112532: Use separate mimalloc heaps for GC objects (gh-113263)
    9afb0e1606cad gh-112027: Don't print mimalloc warning after mmap() call (gh-113372)
    0ff6368519ed7 gh-111906: Fix warnings during mimalloc build on FreeBSD (#111907)
    794dff2fb1d9e gh-111544: Fix mimalloc build on AIX (#111593)
    801741ff81556 gh-90815: Fix mimalloc atomic.h on Windows arm64 (#111527)
    1673e44017436 gh-90815: Fix mimalloc build on WASI (#111524)
    05f2f0ac92afa gh-90815: Add mimalloc memory allocator (#109914)

These are likely the difficult ones to merge:

- acf3bcc886198 gh-112532: Use separate mimalloc heaps for GC objects,
- fcb3c2a444709 gh-112532: Isolate abandoned segments by interpreter (#113717),
- 0b7476080b58e gh-112532: Tag mimalloc heaps and pages (#113742),
- 412920a41efc6 gh-112532: Improve mimalloc page visiting (#114133)
