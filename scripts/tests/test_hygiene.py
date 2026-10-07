"""Exercise the hygiene CLI against real disposable Git repositories."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "check-hygiene.py"
FORMATTER = shutil.which("clang-format") or str(
    Path(os.environ.get("ProgramFiles", "")) / "LLVM/bin/clang-format.exe"
)


class HygieneTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        self.git("init", "--initial-branch=main")
        self.git("config", "user.name", "Hygiene Test")
        self.git("config", "user.email", "hygiene@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.write(".clang-format", "BasedOnStyle: LLVM\nIndentWidth: 4\n")
        self.write(".gitignore", "bin/\npackages/\n")
        self.write("README.md", "Baseline\n")
        self.commit()
        self.git("checkout", "-b", "topic")

    def git(self, *args, input=None):
        return subprocess.run(
            ["git", "-C", str(self.repo), *args], input=input,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        ).stdout

    def write(self, name, text):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))

    def commit(self):
        self.git("add", "--all")
        self.git("commit", "-m", "test fixture")

    def check(self, *args):
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--repo", str(self.repo),
             "--clang-format", FORMATTER, *args], capture_output=True, text=True,
        )
        return result.returncode, result.stdout + result.stderr

    def assert_passes(self, *args):
        code, output = self.check(*args)
        self.assertEqual(code, 0, output)
        self.assertIn("PASS", output)

    def assert_rejects(self, reason, *args):
        code, output = self.check(*args)
        self.assertNotEqual(code, 0, output)
        self.assertIn(reason, output)

    def test_invalid_base_fails_instead_of_checking_nothing(self):
        self.assert_rejects("base", "--base", "missing-ref")

    def test_clean_tree_uses_main_by_default(self):
        self.assert_passes()

    def test_untracked_text_is_checked(self):
        self.write("new.md", "bad trailing whitespace \n")
        self.assert_rejects("whitespace")

    def test_uncommitted_fix_supersedes_bad_committed_content(self):
        self.write("README.md", "Bad \n")
        self.commit()
        self.write("README.md", "Fixed\n")
        self.assert_passes()

    def test_crlf_checkout_is_not_a_whitespace_defect(self):
        self.write("README.md", "Baseline\r\nAdded\r\n")
        self.assert_passes()

    def test_deleted_file_is_not_read_or_formatted(self):
        self.write("old.cpp", "int  old;\n")
        self.commit()
        self.git("branch", "-f", "main", "HEAD")
        (self.repo / "old.cpp").unlink()
        self.assert_passes()

    def test_renamed_cpp_receives_full_file_format_check(self):
        self.write("old.cpp", "int  old;\n")
        self.commit()
        self.git("branch", "-f", "main", "HEAD")
        self.git("mv", "old.cpp", "new.cpp")
        self.assert_rejects("format: new.cpp")

    def test_new_source_checks_the_actual_full_text(self):
        self.write("new.cpp", "int  value;\n")
        self.assert_rejects("format: new.cpp")
        self.write("new.cpp", "int value;\n")
        self.assert_passes()

    def test_modified_cpp_limits_formatter_to_changed_lines(self):
        self.write("old.cpp", "int  inherited;\n\nint changed;\n")
        self.commit()
        self.git("branch", "-f", "main", "HEAD")
        self.write("old.cpp", "int  inherited;\n\nint clean;\n")
        self.assert_passes()
        self.write("old.cpp", "int  inherited;\n\nint  bad;\n")
        self.assert_rejects("format: old.cpp")

    def test_external_code_is_excluded_only_from_formatting(self):
        self.write("external/vendor.cpp", "int  inherited;\n")
        self.assert_passes()
        self.write("external/vendor.cpp", "int  inherited; \n")
        self.assert_rejects("whitespace")

    def test_case_collision_in_index_fails_even_for_vendor(self):
        blob = self.git("hash-object", "-w", "--stdin", input=b"text\n").strip().decode()
        for name in ("external/Header.h", "external/header.h"):
            self.git("update-index", "--add", "--cacheinfo", f"100644,{blob},{name}")
        self.assert_rejects("case collision")

    def test_generated_artifact_is_rejected_even_when_force_added(self):
        self.write("packages/generated.txt", "not source\n")
        self.git("add", "--force", "packages/generated.txt")
        self.assert_rejects("artifact")

    def test_ignored_build_output_is_not_a_publication_candidate(self):
        self.write("bin/generated.dll", "local output\n")
        self.assert_passes()

    def test_changed_inherited_binary_is_rejected(self):
        (self.repo / "upstream.dll").write_bytes(b"MZ\0baseline")
        self.commit()
        self.git("branch", "-f", "main", "HEAD")
        (self.repo / "upstream.dll").write_bytes(b"MZ\0changed")
        self.assert_rejects("artifact")

    def test_real_formatter_error_cannot_report_success(self):
        self.write("new.cpp", "int value;\n")
        self.write(".clang-format", "InvalidFormatOption: true\n")
        self.assert_rejects("formatter command failed")

    def test_unchanged_inherited_binary_is_allowed_but_new_binary_is_not(self):
        (self.repo / "upstream.dll").write_bytes(b"MZ\0baseline")
        self.commit()
        self.git("branch", "-f", "main", "HEAD")
        self.assert_passes()
        (self.repo / "new.dll").write_bytes(b"MZ\0new")
        self.assert_rejects("artifact")

    def test_binary_without_known_extension_is_rejected(self):
        (self.repo / "payload").write_bytes(b"\0private payload")
        self.assert_rejects("binary")

    def test_secret_screen_reports_category_without_echoing_value(self):
        token = "gh" + "p_" + "a" * 36
        self.write("config.txt", token + "\n")
        code, output = self.check()
        self.assertNotEqual(code, 0, output)
        self.assertIn("secret", output)
        self.assertNotIn(token, output)

    def test_personal_machine_path_is_rejected(self):
        path = "C:" + "/Users/" + "PrivatePerson/Documents/file"
        self.write("config.txt", path + "\n")
        self.assert_rejects("machine path")

    def test_missing_formatter_fails_if_cpp_changed(self):
        self.write("new.cpp", "int value;\n")
        self.assert_rejects("formatter", "--clang-format", str(self.repo / "missing-tool"))

    def test_wrong_formatter_version_is_rejected(self):
        self.write("new.cpp", "int value;\n")
        self.assert_rejects("22.1.1", "--clang-format", sys.executable)


class WorkflowDiagnosticTests(unittest.TestCase):
    """Run the workflow's actual diagnostic gate with controlled compiler logs."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.powershell = shutil.which("pwsh") or shutil.which("powershell")
        self.assertIsNotNone(self.powershell, "PowerShell is required to exercise the Windows CI gate")
        workflow = SCRIPT.parents[1] / ".github/workflows/community-validation.yml"
        lines = workflow.read_text(encoding="utf-8").splitlines(keepends=True)
        # The final workflow step is the standalone comparison payload, not a test copy.
        last_run = max(index for index, line in enumerate(lines) if line.strip() == "run: |")
        self.payload = self.directory / "compare.ps1"
        self.payload.write_text(textwrap.dedent("".join(lines[last_run + 1:])), encoding="utf-8")

    def compare(self, baseline, candidate):
        for name, content in (("baseline", baseline), ("candidate", candidate)):
            folder = self.directory / name / "bin/community/analyze/x64/ReleaseBundle"
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "build.log").write_text(content, encoding="utf-8")
        return subprocess.run(
            [self.powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(self.payload)],
            cwd=self.directory, capture_output=True, text=True,
            env=dict(os.environ, BUILD_PLATFORM="x64", BUILD_CONFIGURATION="ReleaseBundle"),
        )

    def test_location_only_changes_do_not_introduce_diagnostics(self):
        result = self.compare(
            "runtime.cpp(10): warning C6246: Local hides outer at line '8'.: Lines: 8\n",
            "runtime.cpp(30): warning C6246: Local hides outer at line '20'.: Lines: 20\n",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)

    def test_new_diagnostic_fails_the_gate(self):
        result = self.compare("", "runtime.cpp(10): warning C6001: Uninitialized value.\n")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NEW DIAGNOSTIC", result.stdout)

    def test_increased_occurrences_fail_the_gate(self):
        result = self.compare(
            "runtime.cpp(10): warning C6001: Uninitialized value.\n",
            "runtime.cpp(10): warning C6001: Uninitialized value.\nruntime.cpp(20): warning C6001: Uninitialized value.\n",
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NEW DIAGNOSTIC", result.stdout)

    def test_unrecognized_warning_cannot_be_silently_dropped(self):
        result = self.compare("", "unexpected warning C6001 without a source location\n")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Unparsed diagnostic", result.stderr)

    def test_uncoded_msbuild_warning_cannot_be_silently_dropped(self):
        result = self.compare("", "project.vcxproj(42,5): warning : New uncoded MSBuild warning.\n")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Unparsed diagnostic", result.stderr)
        self.assertNotIn("PASS:", result.stdout)


class BuildHelperPathTests(unittest.TestCase):
    """Exercise PowerShell 5.1's real native argument boundary and MSBuild."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.source = cls.directory / "source with spaces"
        project_folder = cls.source / "virtualdesktop-openxr"
        project_folder.mkdir(parents=True)
        cls.helper = SCRIPT.parent / "Build-CommunityRuntime.ps1"
        cls.powershell = shutil.which("powershell.exe")
        if cls.powershell is None:
            raise AssertionError("Windows PowerShell 5.1 is required for native path regression tests")
        vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-find", r"MSBuild\**\Bin\MSBuild.exe"],
            capture_output=True, text=True, check=True,
        )
        cls.msbuild = result.stdout.splitlines()[0]
        # An actual v143 C project provides a small native argv probe, not a mocked launcher.
        (project_folder / "argument-probe.c").write_text(
            '#include <stdio.h>\n#include "commit.h"\n'
            'int main(int argc, char** argv) {\n'
            '    printf("COMMIT:%s\\n", RuntimeCommitHash);\n'
            '    for (int i = 0; i < argc; ++i) printf("ARGV:%s\\n", argv[i]);\n'
            '    return 0;\n}\n', encoding="utf-8",
        )
        cls.project = project_folder / "virtualdesktop-openxr.vcxproj"
        cls.project.write_text(textwrap.dedent("""\
            <Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
              <ItemGroup Label="ProjectConfigurations">
                <ProjectConfiguration Include="ReleaseBundle|x64">
                  <Configuration>ReleaseBundle</Configuration><Platform>x64</Platform>
                </ProjectConfiguration>
              </ItemGroup>
              <PropertyGroup Label="Globals"><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
              <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
              <PropertyGroup Label="Configuration">
                <ConfigurationType>Application</ConfigurationType><PlatformToolset>v143</PlatformToolset>
              </PropertyGroup>
              <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />
              <PropertyGroup><TargetName>argument-probe</TargetName></PropertyGroup>
              <ItemDefinitionGroup>
                <ClCompile><AdditionalIncludeDirectories>$(IntDir)</AdditionalIncludeDirectories></ClCompile>
                <Link><SubSystem>Console</SubSystem></Link>
                <PreBuildEvent><Command>exit /b 77</Command></PreBuildEvent>
                <PostBuildEvent><Command>exit /b 78</Command></PostBuildEvent>
              </ItemDefinitionGroup>
              <ItemGroup><ClCompile Include="argument-probe.c" /></ItemGroup>
              <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />
            </Project>
            """), encoding="utf-8",
        )
        for args in (
            ("init", "--initial-branch=main"),
            ("config", "user.name", "Build Helper Test"),
            ("config", "user.email", "build-helper@example.invalid"),
            ("add", "--all"), ("commit", "-m", "native argument fixture"),
        ):
            subprocess.run(["git", "-C", str(cls.source), *args], capture_output=True, check=True)
        cls.commit = subprocess.run(
            ["git", "-C", str(cls.source), "rev-parse", "HEAD"], capture_output=True, text=True, check=True,
        ).stdout.strip()
        bootstrap = cls.directory / "bootstrap output with spaces"
        objects = bootstrap / "obj"
        objects.mkdir(parents=True)
        (objects / "commit.h").write_text('const char* RuntimeCommitHash = "bootstrap";\n', encoding="ascii")
        result = subprocess.run(
            [cls.msbuild, str(cls.project), "/t:Rebuild", "/nologo", "/v:minimal",
             "/p:Configuration=ReleaseBundle", "/p:Platform=x64",
             f"/p:SolutionDir={cls.source.as_posix()}/", f"/p:OutDir={bootstrap.as_posix()}/",
             f"/p:IntDir={objects.as_posix()}/", "/p:PreBuildEventUseInBuild=false",
             "/p:PostBuildEventUseInBuild=false"], capture_output=True, text=True,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.probe = bootstrap / "argument-probe.exe"
        if not cls.probe.is_file():
            raise AssertionError("Actual MSBuild did not create the native argument probe")

    def invoke_helper(self, msbuild, output):
        return subprocess.run(
            [self.powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(self.helper),
             "-SourceDirectory", str(self.source), "-OutputDirectory", str(output), "-MsBuildPath", str(msbuild)],
            capture_output=True, text=True,
        )

    def test_powershell_51_preserves_native_directory_arguments_with_spaces(self):
        output = self.directory / "probe output with spaces"
        result = self.invoke_helper(self.probe, output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        arguments = [line.removeprefix("ARGV:") for line in result.stdout.splitlines() if line.startswith("ARGV:")]
        self.assertIn(f"/p:SolutionDir={self.source.as_posix()}/", arguments)
        self.assertIn(f"/p:OutDir={output.as_posix()}/", arguments)
        self.assertIn(f"/p:IntDir={(output / 'obj').as_posix()}/", arguments)
        self.assertIn("/p:PreBuildEventUseInBuild=false", arguments)
        self.assertIn("/p:PostBuildEventUseInBuild=false", arguments)

    def test_actual_msbuild_accepts_spaced_source_and_output_directories(self):
        output = self.directory / "actual build output with spaces"
        result = self.invoke_helper(self.msbuild, output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        executable = output / "argument-probe.exe"
        self.assertTrue(executable.is_file(), result.stdout + result.stderr)
        run = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
        self.assertIn(f"COMMIT:{self.commit}", run.stdout)
        self.assertTrue((output / "obj/argument-probe.obj").is_file())
        self.assertTrue((output / "build.log").is_file())


if __name__ == "__main__":
    unittest.main()
