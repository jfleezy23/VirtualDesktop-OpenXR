import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('cohort', type=Path, help='Successful runtime cohort directory with result.json, build-source.json and obj/')
parser.add_argument('sources', nargs='+')
parser.add_argument('--installation', type=Path, help='Visual Studio installation root containing VC/ and MSBuild/')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
cohort = args.cohort.resolve(strict=True)
metadata = json.loads((cohort / 'result.json').read_text())
assert metadata['exit_code'] == 0, 'Successful runtime build required'
version = metadata['compiler_version']
assert version in {'14.44.35207', '14.51.36231'}, 'Unexpected compiler cohort'
new = version.startswith('14.51.')
installation = args.installation
if installation is None:
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    selected = subprocess.run([str(vswhere), '-latest', '-products', '*', '-version',
                               '[18.0,19.0)' if new else '[17.0,18.0)', '-requires',
                               'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                              capture_output=True, text=True, check=True)
    if not selected.stdout.strip():
        parser.error('Matching Visual Studio C++ installation was not found; supply --installation.')
    installation = Path(selected.stdout.splitlines()[0])
vcvars = installation / 'VC/Auxiliary/Build/vcvars64.bat'
if not vcvars.is_file():
    parser.error('Visual Studio compiler environment was not found; supply --installation with its installation root.')
objects = cohort / 'obj'
out = cohort / 'tests'
out.mkdir(exist_ok=True)
includes = ['nr-frame-generation/include', 'virtualdesktop-openxr', 'RuntimeConfiguration', 'external/OpenXR-SDK/include', 'external/OpenXR-SDK/src/common',
            'external/OpenXR-MixedReality/Shared', 'external/OpenXR-MixedReality/Shared/XrUtility',
            'external/OpenXR-MixedReality/Shared/SampleShared', 'external/LibOVR/Include', 'external/LibOVR/Include/Extras',
            'external/LibOVR/Shim', 'external/Vulkan-SDK/include', 'external/OpenGL', 'external/fmt/include',
            'external/openvr/headers', 'external/openvr/samples/drivers/drivers/handskeletonsimulation/src',
            'external/openvr/samples/drivers/utils/vrmath', 'external/FidelityFX-FSR/ffx-fsr', 'external/FidelityFX-CAS/ffx-cas',
            'external/cJSON', 'external/d3dx12', 'external/DLSS/include', 'packages/Detours.4.0.1/lib/native/include',
            'packages/Microsoft.Windows.ImplementationLibrary.1.0.220201.1/include',
            'packages/Microsoft.GameInput.2.2.26100.6114/native/include']
libraries = ['dxgi.lib', 'dxguid.lib', 'd3d11.lib', 'd3d12.lib', 'opengl32.lib', 'ntdll.lib', 'kernel32.lib', 'user32.lib',
             'gdi32.lib', 'winspool.lib', 'comdlg32.lib', 'advapi32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib',
             'uuid.lib', 'odbc32.lib', 'odbccp32.lib', 'd3dcompiler.lib']
native_libraries = ['packages/Detours.4.0.1/lib/native/libs/x64/detours.lib',
                    'packages/Microsoft.GameInput.2.2.26100.6114/native/lib/x64/GameInput.lib',
                    'external/Vulkan-SDK/lib/vulkan-1.lib', 'external/DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib']
asan = '-asan-' in cohort.name
def response(path, argv):
    # Windows compiler response files preserve literal paths; avoid a shell command per argument.
    path.write_text('\n'.join('"' + str(arg) + '"' for arg in argv) + '\n', encoding='utf-8')

spec = importlib.util.spec_from_file_location('fg_provenance', repo / 'scripts/fg-build-provenance.py')
provenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provenance)
provenance.verify_receipt(cohort / 'build-source.json', repo, 'runtime')
for source in args.sources:
    before_sources = provenance.snapshot(repo, 'fixture')
    name = Path(source).stem
    source_path = repo / 'tests' / (name + '.cpp')
    assert source_path.is_file(), source_path
    test_object = out / (name + '.obj')
    flags = ['/nologo', '/std:c++17', '/EHsc', '/W3', '/MT', '/O2', '/Gy', '/c', '/fp:precise',
             '/DRUNTIME_NAMESPACE=virtualdesktop_openxr', '/DNDEBUG', '/DUNICODE', '/D_UNICODE',
             '/D_CRT_SECURE_NO_WARNINGS', '/DUSING_GAMEINPUT']
    if new:
        flags += ['/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS', '/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR']
    if asan:
        flags += ['/fsanitize=address', '/Zi', '/Fd' + str(out / (name + '-compile.pdb'))]
    flags += ['/I' + str(repo / item) for item in includes] + ['/I' + str(objects), str(source_path), '/Fo' + str(test_object)]
    response(out / (name + '-cl.rsp'), flags)
    link_args = ['/nologo', '/SUBSYSTEM:CONSOLE', '/DEBUG', '/OUT:' + str(out / (name + '.exe')), str(test_object)]
    if not asan:
        link_args += ['/LTCG']
    # Command-context fixture is intentionally header-only; others use all matched runtime objects.
    header_only = name in {'nr_command_context_regression', 'nr_fence_regression', 'runtime_negotiation_regression', 'runtime_redirect_regression'}
    if not header_only:
        link_args += [str(item) for item in sorted(objects.glob('*.obj'))]
    link_args += libraries + [str(repo / item) for item in native_libraries]
    response(out / (name + '-link.rsp'), link_args)
    batch = out / (name + '-build.cmd')
    batch.write_text('@echo off\ncall "' + str(vcvars) + '" -vcvars_ver=' + version + ' 10.0.26100.0 >nul\n'
                     'if errorlevel 1 exit /b %errorlevel%\ncl.exe @"' + str(out / (name + '-cl.rsp')) + '"\n'
                     'if errorlevel 1 exit /b %errorlevel%\nlink.exe @"' + str(out / (name + '-link.rsp')) + '"\n'
                     'exit /b %errorlevel%\n', encoding='utf-8')
    started = time.monotonic()
    env = os.environ.copy()
    env.pop('_CL_', None)
    env.pop('CL', None)
    with (out / (name + '-build.log')).open('w') as log:
        result = subprocess.run([os.environ.get('COMSPEC', 'C:/Windows/System32/cmd.exe'), '/d', '/c', str(batch)], stdout=log, stderr=subprocess.STDOUT, env=env)
    if result.returncode:
        print((out / (name + '-build.log')).read_text(errors='replace')[-6000:], flush=True)
        raise SystemExit(result.returncode)
    provenance.write_receipt(out / (name + '-source.json'), repo, 'fixture', before_sources, [out / (name + '.exe'), test_object])
    manifest = {'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(), 'compiler': version,
                'cohort': str(cohort), 'exe_sha256': hashlib.sha256((out / (name + '.exe')).read_bytes()).hexdigest(),
                'runtime_objects': {} if header_only else {item.name: hashlib.sha256(item.read_bytes()).hexdigest() for item in objects.glob('*.obj')},
                'seconds': time.monotonic() - started}
    (out / (name + '-provenance.json')).write_text(json.dumps(manifest, indent=2))
    print('BUILT ' + str(cohort) + '/' + name + ' in ' + str(round(manifest['seconds'], 2)) + 's', flush=True)
