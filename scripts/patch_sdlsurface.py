import os

path = 'android/app/src/main/java/org/libsdl/app/SDLSurface.java'
with open(path, 'r', encoding='utf-8') as f:
    s = f.read()

target = """        if (skip) {
           Log.v("SDL", "Skip .. Surface is not ready.");
           mIsSurfaceReady = false;
           return;
        }"""

replacement = """        if (skip) {
           Log.v("SDL", "Orientation transition detected, but proceeding with surface initialization.");
        }"""

if target in s:
    s = s.replace(target, replacement, 1)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(s)
    print("SDLSurface.java patched successfully!")
else:
    print("Target not found, checking with CRLF...")
    target_crlf = target.replace("\n", "\r\n")
    if target_crlf in s:
        s = s.replace(target_crlf, replacement.replace("\n", "\r\n"), 1)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(s)
        print("SDLSurface.java patched with CRLF successfully!")
    else:
        print("Could not find target!")
