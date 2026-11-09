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
parser.add_argument('--output-directory', type=Path, help='Fixture output directory (default: cohort/tests)')
parser.add_argument('--shader-directory', type=Path, help='Generated headers for the spatial/native shader fixtures')
parser.add_argument('--build-runtime', action='store_true', help='Build a fresh matched ReleaseBundle cohort first')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fg_provenance', repo / 'scripts/fg-build-provenance.py')
provenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provenance)
if args.build_runtime:
    if args.installation is None:
        parser.error('--build-runtime requires --installation.')
    if args.cohort.exists() and any(args.cohort.iterdir()):
        parser.error('--build-runtime requires a fresh empty cohort directory.')
    args.cohort.mkdir(parents=True, exist_ok=True)
    before = provenance.snapshot(repo, 'runtime')
    command = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
               str(repo / 'scripts/Build-CommunityRuntime.ps1'), '-Configuration', 'ReleaseBundle',
               '-OutputDirectory', str(args.cohort.resolve()), '-MsBuildPath',
               str(args.installation / 'MSBuild/Current/Bin/MSBuild.exe'), '-VCToolsVersion', '14.51.36231']
    with (args.cohort / 'driver.log').open('w') as log:
        built = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    (args.cohort / 'result.json').write_text(json.dumps({'exit_code': built.returncode,
        'compiler_version': '14.51.36231', 'command': command}, indent=2))
    if built.returncode:
        print((args.cohort / 'driver.log').read_text(errors='replace')[-6000:], flush=True)
        raise SystemExit(built.returncode)
    provenance.write_receipt(args.cohort / 'build-source.json', repo, 'runtime', before,
                             [*sorted((args.cohort / 'obj').glob('*.obj')), args.cohort / 'virtualdesktop-openxr.dll'])
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
out = args.output_directory.resolve() if args.output_directory else cohort / 'tests'
out.mkdir(parents=True, exist_ok=True)
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
             'uuid.lib', 'odbc32.lib', 'odbccp32.lib', 'd3dcompiler.lib', 'bcrypt.lib']
native_libraries = ['packages/Detours.4.0.1/lib/native/libs/x64/detours.lib',
                    'packages/Microsoft.GameInput.2.2.26100.6114/native/lib/x64/GameInput.lib',
                    'external/Vulkan-SDK/lib/vulkan-1.lib', 'external/DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib']
asan = '-asan-' in cohort.name
def response(path, argv):
    # Windows compiler response files preserve literal paths; avoid a shell command per argument.
    path.write_text('\n'.join('"' + str(arg) + '"' for arg in argv) + '\n', encoding='utf-8')

provenance.verify_receipt(cohort / 'build-source.json', repo, 'runtime')
for source in args.sources:
    before_sources = provenance.snapshot(repo, 'fixture')
    name = Path(source).stem
    source_path = repo / 'tests' / (name + '.cpp')
    assert source_path.is_file(), source_path
    shader_fixture = name in {'nr_spatial_shader_regression', 'nr_native_guard_regression'}
    if shader_fixture and args.shader_directory is None:
        parser.error('Spatial/native shader fixtures require --shader-directory.')
    shader_headers = []
    if shader_fixture:
        shader_headers = [args.shader_directory.resolve(strict=True) / (shader + '.h') for shader in
                          ('NrSpatialDownsampleCS', 'NrSpatialResolvePS', 'NrNativeResolvePS', 'FullScreenQuadVS')]
        assert all(path.is_file() for path in shader_headers), 'All four generated shader headers are required'
    test_object = out / (name + '.obj')
    flags = ['/nologo', '/std:c++17', '/EHsc', '/W4' if shader_fixture else '/W3', '/MT', '/O2', '/Gy', '/c', '/fp:precise',
             '/DRUNTIME_NAMESPACE=virtualdesktop_openxr', '/DNDEBUG', '/DUNICODE', '/D_UNICODE',
             '/D_CRT_SECURE_NO_WARNINGS', '/DUSING_GAMEINPUT', '/DVDXR_FG_PRODUCER_FENCE=1']
    if name in {'nr_fg_runtime_regression', 'nr_spatial_runtime_regression', 'nr_input_toggle_regression',
                'runtime_delivery_regression', 'native_runtime_delivery_probe', 'controlled_runtime_delivery_probe',
                'nr_fg_prebegin_overlap_regression', 'nr_fg_prebegin_fast_begin_regression',
                'nr_fg_prebegin_failure_regression', 'nr_fg_prebegin_cancel_regression',
                'nr_fg_copy_ready_poison_regression', 'nr_fg_pending_current_copy_regression'}:
        flags += ['/DVDXR_NR_SPATIAL_CANDIDATE=1']
    if new:
        flags += ['/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS', '/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR']
    if asan:
        flags += ['/fsanitize=address', '/Zi', '/Fd' + str(out / (name + '-compile.pdb'))]
    if shader_fixture:
        flags += ['/WX', '/DNOMINMAX', '/DNR_SPATIAL_TEST_OBSERVER', '/I' + str(args.shader_directory.resolve())]
    flags += ['/I' + str(repo / item) for item in includes] + ['/I' + str(objects)]
    response(out / (name + '-cl.rsp'), flags + [str(source_path), '/Fo' + str(test_object)])
    extra_objects = []
    helper_commands = ''

    def compile_helper(label, source, definitions=()):
        global helper_commands
        helper_object = out / (name + '-' + label + '.obj')
        helper_response = out / (name + '-' + label + '-cl.rsp')
        response(helper_response, flags + list(definitions) + [str(repo / source), '/Fo' + str(helper_object)])
        extra_objects.append(helper_object)
        helper_commands += 'cl.exe @"' + str(helper_response) + '"\nif errorlevel 1 exit /b %errorlevel%\n'

    if shader_fixture:
        compile_helper('spatial-observer', 'virtualdesktop-openxr/NrSpatialPipeline.cpp')
    if name == 'nr_fg_early_dispatch_regression':
        compile_helper('context-methods', 'tests/context_method_addresses.cpp')
    if name in {'fg_delivery_isolation_regression', 'fg_delivery_dependencies_regression',
                'delivery_transport_regression', 'nr_fg_copy_ready_poison_regression',
                'nr_fg_pending_current_copy_regression'}:
        compile_helper('delivery-context-slots', 'tests/delivery_context_slots.cpp')
    if name == 'fg_delivery_dependencies_regression':
        compile_helper('delivery-queue-slots', 'tests/delivery_queue_slots.cpp')
    # Only isolated SDK fixtures replace system.obj. Native probes retain the real SDK loader.
    private_system = name in {'controlled_runtime_delivery_probe', 'nr_fg_prebegin_overlap_regression',
                             'nr_fg_prebegin_fast_begin_regression', 'nr_fg_prebegin_failure_regression',
                             'nr_fg_prebegin_cancel_regression', 'nr_fg_copy_ready_poison_regression',
                             'nr_fg_pending_current_copy_regression', 'fg_delivery_isolation_regression',
                             'fg_delivery_dependencies_regression', 'delivery_transport_regression',
                             'nr_fg_runtime_regression', 'nr_spatial_runtime_regression',
                             'nr_input_toggle_regression', 'runtime_delivery_regression'}
    if private_system:
        compile_helper('private-system', 'virtualdesktop-openxr/system.cpp', ['/DLOAD_LOCAL_LIBOVR'])
    link_args = ['/nologo', '/SUBSYSTEM:CONSOLE', '/DEBUG', '/OUT:' + str(out / (name + '.exe')), str(test_object), *map(str, extra_objects)]
    if not asan:
        link_args += ['/LTCG']
    # Command-context fixture is intentionally header-only; others use all matched runtime objects.
    header_only = name in {'nr_command_context_regression', 'nr_fence_regression', 'runtime_negotiation_regression', 'runtime_redirect_regression'}
    runtime_objects = []
    if not header_only:
        runtime_objects = [item for item in sorted(objects.glob('*.obj'))
                           if not (private_system and item.name == 'system.obj')
                           and not (shader_fixture and item.name == 'NrSpatialPipeline.obj')]
        link_args += list(map(str, runtime_objects))
    link_args += libraries + [str(repo / item) for item in native_libraries]
    response(out / (name + '-link.rsp'), link_args)
    batch = out / (name + '-build.cmd')
    batch.write_text('@echo off\ncall "' + str(vcvars) + '" -vcvars_ver=' + version + ' 10.0.26100.0 >nul\n'
                     'if errorlevel 1 exit /b %errorlevel%\n' + helper_commands + 'cl.exe @"' + str(out / (name + '-cl.rsp')) + '"\n'
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
    provenance.write_receipt(out / (name + '-source.json'), repo, 'fixture', before_sources, [out / (name + '.exe'), test_object, *extra_objects])
    manifest = {'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(), 'compiler': version,
                'cohort': str(cohort), 'exe_sha256': hashlib.sha256((out / (name + '.exe')).read_bytes()).hexdigest(),
                'runtime_objects': {item.name: hashlib.sha256(item.read_bytes()).hexdigest() for item in runtime_objects},
                'helper_objects': {item.name: hashlib.sha256(item.read_bytes()).hexdigest() for item in extra_objects},
                'shader_headers': {str(item): hashlib.sha256(item.read_bytes()).hexdigest() for item in shader_headers},
                'seconds': time.monotonic() - started}
    (out / (name + '-provenance.json')).write_text(json.dumps(manifest, indent=2))
    print('BUILT ' + str(cohort) + '/' + name + ' in ' + str(round(manifest['seconds'], 2)) + 's', flush=True)
