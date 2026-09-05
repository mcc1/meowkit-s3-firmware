"""
PlatformIO pre-build extra script.
Converts boot_animation.gif → splash_gif_data.S  (GNU AS .incbin, instant)
                       → splash_gif.h       (tiny extern declarations)

Result: GIF bytes land in Flash RODATA with zero C compilation overhead.
"""
Import("env")  # noqa: F821  (PlatformIO SCons magic)
import os

project_dir = env.subst("$PROJECT_DIR")
gif_path    = os.path.join(project_dir, "boot_animation.gif")
splash_dir  = os.path.join(project_dir, "src", "splash")

# Keep the generated assembly path-independent. The include path is used by
# GNU AS to resolve the .incbin filename from the project root.
env.Append(ASFLAGS=[f"-I{project_dir}"], ASPPFLAGS=[f"-I{project_dir}"])

asm_out = os.path.join(splash_dir, "splash_gif_data.S")
hdr_out = os.path.join(splash_dir, "splash_gif.h")

gif_exists = os.path.isfile(gif_path)
gif_size   = os.path.getsize(gif_path) if gif_exists else 0
gif_name   = "boot_animation.gif"

# ── build output ──────────────────────────────────────────────────────────────
if gif_exists:
    asm_body = (
        "    .section .rodata\n"
        "    .balign 4\n"
        "    .global splash_gif_data\n"
        "splash_gif_data:\n"
        f'    .incbin "{gif_name}"\n'
        "    .balign 4\n"
    )
    hdr_body = (
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        "#ifdef __cplusplus\n"
        'extern "C" {\n'
        "#endif\n"
        "extern const uint8_t splash_gif_data[];\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n\n"
        f"static const size_t splash_gif_len = {gif_size}u;\n"
    )
    print(f"[embed_gif] {gif_name}  ({gif_size/1024:.1f} KB) → .incbin in Flash RODATA")

else:
    # No GIF present — emit a zero-length symbol so the project still compiles.
    # SplashScreen::show() checks splash_gif_len == 0 and shows text fallback.
    asm_body = (
        "    .section .rodata\n"
        "    .balign 4\n"
        "    .global splash_gif_data\n"
        "splash_gif_data:\n"
    )
    hdr_body = (
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        "#ifdef __cplusplus\n"
        'extern "C" {\n'
        "#endif\n"
        "extern const uint8_t splash_gif_data[];\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n\n"
        "static const size_t splash_gif_len = 0u;  // boot_animation.gif not found\n"
    )
    print("[embed_gif] boot_animation.gif not found — splash will use text fallback")

# Write only when content changed (avoids needless recompilation)
for path, body in ((asm_out, asm_body), (hdr_out, hdr_body)):
    existing = open(path).read() if os.path.isfile(path) else ""
    if existing != body:
        with open(path, "w") as f:
            f.write(body)
