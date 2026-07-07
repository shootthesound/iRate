@echo off
rem Build iRate.exe (needs zig on PATH, or: pip install ziglang & use "python -m ziglang c++" / "python -m ziglang rc")
zig rc /:output-format coff irate.rc irate_res.obj
zig c++ -target x86_64-windows-gnu -O2 -municode -DUNICODE -D_UNICODE -w irate.cpp irate_res.obj -o ..\iRate.exe -Wl,--subsystem,windows -lole32 -luser32 -lgdi32 -lshell32 -lshlwapi -lmsimg32
