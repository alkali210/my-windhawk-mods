"""Build the Windhawk mod and, optionally, its isolated Windows integration test."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--windhawk", type=Path, default=Path(os.environ.get("ProgramW6432", r"C:\Program Files")) / "Windhawk")
parser.add_argument("--minhook", type=Path, default=root / "reference/minhook")
parser.add_argument("--test", action="store_true", help="Build and run the isolated native-dialog test")
parser.add_argument("--host-probe", action="store_true", help="Check compatibility in a real rundll32.exe host")
parser.add_argument("--symbols-dir", type=Path, help="Also verify symbols using a directory containing sysdm.pdb")
parser.add_argument("--sample-dir", type=Path, action="append", default=[],
                    help="Also run the isolated experiment with sysdm.cpl and matching PDB in this directory")
args = parser.parse_args()
if args.sample_dir and (not args.test or not args.symbols_dir):
    parser.error("--sample-dir requires --test and --symbols-dir for the host PDB")

compiler = args.windhawk / "Compiler/bin/clang++.exe"
versions = [p for p in (args.windhawk / "Engine").iterdir() if (p / "64/windhawk.lib").is_file()]
versions.sort(key=lambda p: tuple(map(int, re.findall(r"\d+", p.name))))
if not versions:
    raise SystemExit("Could not find a Windhawk x64 engine library.")
library = versions[-1] / "64/windhawk.lib"
output = root / "build"
output.mkdir(exist_ok=True)
tmp = output / "tmp"
tmp.mkdir(exist_ok=True)
env = os.environ.copy()
env.pop("ENV_VARS_TEST_SAMPLE", None)
env["TMP"] = env["TEMP"] = str(tmp)
common = [str(compiler), "-target", "x86_64-w64-mingw32", "-std=c++23", "-O2",
          "-DUNICODE", "-D_UNICODE", "-DWINVER=0x0A00", "-D_WIN32_WINNT=0x0A00",
          "-D_WIN32_IE=0x0A00", "-DNTDDI_VERSION=0x0A000008",
          "-D__USE_MINGW_ANSI_STDIO=0", "-Wall", "-Wextra", "-Werror",
          "-Wno-unused-parameter", "-Wno-missing-field-initializers"]
source = root / "env-vars-unlimited.wh.cpp"
version = re.search(r"^// @version\s+(\S+)", source.read_text(encoding="utf-8"), re.M).group(1)
defines = output / "mod-defines.h"
defines.write_text('#define WH_MOD\n#define WH_MOD_ID L"local@env-vars-unlimited"\n'
                   f'#define WH_MOD_VERSION L"{version}"\n', encoding="utf-8")
libs = ["-lcomctl32", "-lversion", "-ladvapi32", "-lktmw32", "-lbcrypt"]
subprocess.run(common + ["-shared", "-include", str(defines), "-include", "windhawk_api.h",
                         str(source), str(library), "-Wl,--export-all-symbols", "-o",
                         str(output / "env-vars-unlimited.dll"), *libs], check=True, env=env)
print("Built Windhawk DLL", flush=True)

if args.test or args.host_probe:
    probe_dll = output / "rundll32-probe.dll"
    subprocess.run(common + ["-shared", str(root / "tests/rundll32-probe.cpp"),
                             "-o", str(probe_dll), *libs], check=True, env=env)
    runtime = Path(os.environ.get("ProgramData", r"C:\ProgramData")) / "Windhawk/Engine/Mods/64"
    env["PATH"] = str(runtime) + os.pathsep + env.get("PATH", "")
    probe_log = output / "rundll32-probe.txt"
    probe_log.unlink(missing_ok=True)
    env["ENV_VARS_TEST_OUTPUT"] = str(probe_log)
    rundll = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/rundll32.exe"
    subprocess.run(f'"{rundll}" "{probe_dll}",Probe', check=True, env=env, timeout=30)
    result = probe_log.read_text(encoding="ascii")
    expected = ("pdb=1 sha256=1 supported=1 wrong_module_rejected=1 "
                "wrong_hash_rejected=1 wrong_age_rejected=1 cross_pair_rejected=1")
    if expected not in result:
        raise SystemExit(f"rundll32 host validation failed:\n{result}")
    print("PASS: real rundll32.exe accepts the verified binary and rejects an unrelated DLL", flush=True)

if args.test:
    if args.symbols_dir:
        subprocess.run([os.fspath(Path(os.sys.executable)), str(root / "tests/verify_symbols.py"),
                        str(args.symbols_dir)], check=True, env=env)
    if not (args.minhook / "include/MinHook.h").is_file():
        raise SystemExit("Integration tests need MinHook: pass --minhook <checkout>.")
    objects = []
    for name in ["buffer.c", "hook.c", "trampoline.c", "hde/hde64.c"]:
        obj = output / (Path(name).stem + ".o")
        subprocess.run([str(compiler), "-target", "x86_64-w64-mingw32", "-O2", "-x", "c",
                        "-c", str(args.minhook / "src" / name), "-o", str(obj)], check=True, env=env)
        objects.append(str(obj))
    system_dll = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/sysdm.cpl"
    baseline = hashlib.sha256(system_dll.read_bytes()).hexdigest() == (
        "2057c7910a5768368122e6902b799e2cf437b28658721d9b03b674ac76c10e99"
    )
    if baseline:
        exe = output / "dialog-integration.exe"
        subprocess.run(common + [str(root / "tests/dialog-integration.cpp"), *objects,
                                 "-I", str(args.minhook / "include"), "-o", str(exe),
                                 *libs], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env, timeout=90)
    elif not args.symbols_dir:
        raise SystemExit("This host needs --symbols-dir with its matching PDB for the integration test.")
    else:
        print("Skipping fixed-RVA harness on a different host; running the PDB-resolved harness", flush=True)
    if args.symbols_dir:
        init_dll = output / "rundll32-init-test.dll"
        subprocess.run(common + ["-DENV_VARS_TEST_WINDHAWK_INIT", "-shared",
                                 str(root / "tests/dialog-integration.cpp"), *objects,
                                 "-I", str(args.minhook / "include"), "-o", str(init_dll),
                                 *libs, "-ldbghelp"], check=True, env=env)
        init_log = output / "rundll32-init-test.txt"
        init_log.unlink(missing_ok=True)
        env["ENV_VARS_TEST_OUTPUT"] = str(init_log)
        env["ENV_VARS_TEST_SYMBOLS"] = str(args.symbols_dir.resolve())
        result = subprocess.run(f'"{rundll}" "{init_dll}",RunInitTest sysdm.cpl',
                                check=False, env=env, timeout=90)
        contents = init_log.read_text(encoding="utf-8", errors="replace") if init_log.exists() else ""
        if result.returncode != 0 or "PASS: user and system values" not in contents:
            raise SystemExit(f"rundll32 initialization test failed ({result.returncode}):\n{contents}")
        print("PASS: Wh_ModInit and both native editors in rundll32 using a test Windhawk API", flush=True)

        for sample_dir in args.sample_dir:
            sample_dir = sample_dir.resolve()
            sample = sample_dir / "sysdm.cpl"
            if not sample.is_file() or not (sample_dir / "sysdm.pdb").is_file():
                raise SystemExit(f"Sample directory must contain sysdm.cpl and its matching sysdm.pdb: {sample_dir}")
            sample_env = env.copy()
            sample_env["ENV_VARS_TEST_SAMPLE"] = str(sample)
            sample_env["ENV_VARS_TEST_SYMBOLS"] = str(sample_dir)
            sample_log = output / f"sample-{sample_dir.name}.log"
            sample_log.unlink(missing_ok=True)
            sample_env["ENV_VARS_TEST_OUTPUT"] = str(sample_log)
            result = subprocess.run(f'"{rundll}" "{init_dll}",RunInitTest sysdm.cpl',
                                    check=False, env=sample_env, timeout=90)
            contents = sample_log.read_text(encoding="utf-8", errors="replace") if sample_log.exists() else ""
            if result.returncode != 0 or "PASS: user and system values" not in contents:
                raise SystemExit(f"Sample test failed for {sample_dir} ({result.returncode}):\n{contents}")
            print(f"PASS: isolated sample experiment with {sample}", flush=True)
