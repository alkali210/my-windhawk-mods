"""Build locally; --run-tests --test-mode selection-fade exercises native dismissal."""
from pathlib import Path
import argparse
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--windhawk', type=Path, default=Path(os.environ.get('ProgramW6432', r'C:\Program Files')) / 'Windhawk')
parser.add_argument('--tests', action='store_true')
parser.add_argument('--run-tests', action='store_true')
parser.add_argument('--test-mode', choices=['smoke', 'selection-fade'], default='smoke',
                    help='Native menu harness mode used by --run-tests (requires an interactive desktop)')
parser.add_argument('--runtime-dir', type=Path, default=Path(os.environ.get('ProgramData', r'C:\ProgramData')) / 'Windhawk/Engine/Mods/64')
args = parser.parse_args()
compiler = args.windhawk / 'Compiler/bin/clang++.exe'
versions = sorted((args.windhawk / 'Engine').iterdir(), key=lambda p: tuple(int(x) for x in p.name.split('.')))
library = versions[-1] / '64/windhawk.lib'
output = ROOT / 'build'
output.mkdir(exist_ok=True)
compiler_tmp = output / 'tmp'
compiler_tmp.mkdir(exist_ok=True)
os.environ['TMP'] = str(compiler_tmp)
os.environ['TEMP'] = str(compiler_tmp)
common = [str(compiler), '-target', 'x86_64-w64-mingw32', '-std=c++23', '-O2',
          '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
          '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008',
          '-D__USE_MINGW_ANSI_STDIO=0', '-Wall', '-Wextra', '-Werror',
          '-Wno-missing-field-initializers', '-Wno-unused-parameter']
libs = ['-ldwmapi', '-luxtheme', '-lgdi32', '-ladvapi32', '-lcomctl32', '-lmsimg32']
defines = output / 'mod-defines.h'
version = re.search(r'^// @version\s+(\S+)',
                    (ROOT / 'explorer-translucent-menus.wh.cpp').read_text(encoding='utf-8'),
                    re.M).group(1)
defines.write_text('#define WH_MOD\n#define WH_MOD_ID L"local@explorer-translucent-menus"\n'
                   f'#define WH_MOD_VERSION L"{version}"\n', encoding='utf-8')
subprocess.run(common + ['-shared', '-include', str(defines), '-include', 'windhawk_api.h',
               str(ROOT / 'explorer-translucent-menus.wh.cpp'), str(library),
               '-Wl,--export-all-symbols', '-o', str(output / 'explorer-translucent-menus.dll')]
               + libs, check=True)
print('Built production Windhawk DLL (x64).', flush=True)
if args.tests or args.run_tests:
    minhook = ROOT / 'reference/minhook'
    if not (minhook / 'include/MinHook.h').is_file():
        raise SystemExit('Tests need: git clone --depth 1 https://github.com/TsudaKageyu/minhook reference/minhook')
    objects = []
    for source in ['buffer.c', 'hook.c', 'trampoline.c', 'hde/hde64.c']:
        obj = output / (Path(source).stem + '.o')
        subprocess.run([str(compiler), '-target', 'x86_64-w64-mingw32', '-O2', '-x', 'c',
                        '-c', str(minhook / 'src' / source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    subprocess.run(common + [str(ROOT / 'tools/menu-smoke.cpp'), *objects,
                    '-I', str(minhook / 'include'), '-o', str(output / 'menu-smoke.exe')]
                   + libs, check=True)
    print('Built isolated native menu smoke harness (x64).', flush=True)
    subprocess.run(common + [str(ROOT / 'tools/menu-visual.cpp'), *objects,
                    '-I', str(minhook / 'include'), '-o', str(output / 'menu-visual.exe')]
                   + libs, check=True)
    import shutil
    for runtime in ['libc++.whl', 'libunwind.whl']:
        shutil.copy2(args.runtime_dir / runtime, output / runtime)
    print('Built interactive material visual fixture (x64).', flush=True)

if args.run_tests:
    env = os.environ.copy()
    env['PATH'] = str(args.runtime_dir) + os.pathsep + env.get('PATH', '')
    command = [str(output / 'menu-smoke.exe')]
    if args.test_mode == 'selection-fade':
        command.append('--selection-fade')
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True,
                            text=True, errors='replace',
                            timeout=180 if args.test_mode == 'selection-fade' else 60)
    log = result.stdout + result.stderr
    (output / f'{args.test_mode}-test.log').write_text(log, encoding='utf-8')
    print(log)
    result.check_returncode()
