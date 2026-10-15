"""Check source discovery with isolated, arbitrarily named CMake projects."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROLES = ("display-api", "display-simulator", "transit-query", "map-query")
OVERRIDE = "CHECK_PROVIDER_SOURCE_DIR"


def bracket(value):
    """Quote a path as a CMake literal, including spaces and backslashes."""
    delimiter = "="
    while "]" + delimiter + "]" in str(value):
        delimiter += "="
    return "[" + delimiter + "[" + str(value) + "]" + delimiter + "]"


def provider(path, role="map-query", target="check_provider"):
    (path / "cmake").mkdir(parents=True)
    (path / "cmake/capabilities.txt").write_text("helper-tool\n  " + role + "  \n")
    (path / "CMakeLists.txt").write_text(
        f"add_library({target} INTERFACE)\n"
        f"set_property(TARGET {target} PROPERTY DISCOVERY_ORIGIN "
        '"${CMAKE_CURRENT_SOURCE_DIR}")\n')
    return path


def dependency(role="map-query", target="check_provider", override=OVERRIDE):
    return (f"add_source_dependency(TARGET {target} CAPABILITY {role}\n"
            f'  DESCRIPTION "test {role} provider" OVERRIDE {override})\n'
            f"get_target_property(found {target} DISCOVERY_ORIGIN)\n"
            f'file(WRITE "${{CMAKE_BINARY_DIR}}/{target}.txt" "${{found}}")\n')


class Case:
    def __init__(self, directory, name, helper, cmake, project_name="project"):
        self.base = directory / name
        self.source = self.base / project_name
        self.build = self.base / "build"
        self.source.mkdir(parents=True)
        self.helper = helper
        self.cmake = cmake
        self.project()

    def project(self, body=None):
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(DiscoveryChecks LANGUAGES NONE)\n"
            f"include({bracket(self.helper)})\n" +
            (dependency() if body is None else body))

    def configure(self, *arguments, environment_roots=None, error=None):
        environment = os.environ.copy()
        environment.pop("SOURCE_DEPENDENCY_ROOTS", None)
        if environment_roots is not None:
            environment["SOURCE_DEPENDENCY_ROOTS"] = environment_roots
        result = subprocess.run(
            [self.cmake, "-S", str(self.source), "-B", str(self.build), *arguments],
            env=environment, text=True, capture_output=True, timeout=20)
        output = result.stdout + result.stderr
        if error is None:
            assert result.returncode == 0, output
        else:
            assert result.returncode != 0, "Configuration unexpectedly succeeded"
            for fragment in error:
                assert " ".join(fragment.split()) in " ".join(output.split()), (fragment, output)
        return output

    def found(self, expected, target="check_provider"):
        actual = (self.build / (target + ".txt")).read_text()
        assert Path(actual).resolve() == expected.resolve(), (actual, str(expected))


def check_renamed_nested(case):
    body = ""
    expected = []
    for index, role in enumerate(ROLES):
        target = "provider_" + str(index)
        path = provider(case.source / "arbitrary components" / f"renamed {index}" / "code",
                        role, target)
        body += dependency(role, target, f"PROVIDER_{index}_SOURCE_DIR")
        expected.append((path, target))
    case.project(body)
    case.configure()
    for path, target in expected:
        case.found(path, target)


def check_declared_roots(case):
    first = provider(case.base / "external collection" / "renamed map")
    second = provider(case.base / "environment collection" / "deep" / "renamed display",
                      "display-api", "display_provider")
    case.project(dependency() + dependency("display-api", "display_provider", "DISPLAY_SOURCE_DIR"))
    # Relative cache roots resolve against the top-level source directory; the
    # environment adds roots instead of replacing the cache's search locations.
    case.configure("-DSOURCE_DEPENDENCY_ROOTS=../external collection",
                   environment_roots=str(second.parent.parent))
    case.found(first)
    case.found(second, "display_provider")
    case.configure("-DSOURCE_DEPENDENCY_ROOTS=../nonexistent collection",
                   error=("Invalid SOURCE_DEPENDENCY_ROOTS",))


def check_missing(case):
    # Names and CMake targets alone do not establish a provider's capability.
    provider(case.source / "looks plausible", "map-query-extra")
    orphan = case.source / "metadata without CMakeLists" / "cmake"
    orphan.mkdir(parents=True)
    (orphan / "capabilities.txt").write_text("map-query\n")
    case.configure(error=("Missing test map-query provider", "SOURCE_DEPENDENCY_ROOTS", OVERRIDE))


def check_ambiguity_and_overrides(case):
    first = provider(case.source / "first choice")
    second = provider(case.source / "unrelated name" / "second choice")
    case.configure(error=("Ambiguous test map-query provider", str(first), str(second), OVERRIDE))
    case.configure(f"-D{OVERRIDE}={second}")
    case.found(second)
    wrong = provider(case.source / "wrong capability", "transit-query")
    case.configure(f"-D{OVERRIDE}={wrong}", error=(f"Invalid {OVERRIDE}", "map-query"))
    missing_cmake = case.source / "only metadata"
    (missing_cmake / "cmake").mkdir(parents=True)
    (missing_cmake / "cmake/capabilities.txt").write_text("map-query\n")
    case.configure(f"-D{OVERRIDE}={missing_cmake}", error=(f"Invalid {OVERRIDE}",))
    # Explicit relative paths work even with other valid providers in the tree.
    case.configure(f"-D{OVERRIDE}=first choice")
    case.found(first)


def check_existing_target(case):
    case.project('add_library(check_provider INTERFACE)\n'
                 'set_property(TARGET check_provider PROPERTY DISCOVERY_ORIGIN "already-defined")\n'
                 + dependency())
    case.configure("-DSOURCE_DEPENDENCY_ROOTS=/missing/discovery/root",
                   f"-D{OVERRIDE}=/missing/discovery/override")
    assert (case.build / "check_provider.txt").read_text() == "already-defined"


def check_no_sibling_fallback(case):
    sibling = provider(case.base / "osm_query_parser")
    case.configure(error=("Missing test map-query provider",))
    case.configure(f"-D{OVERRIDE}={sibling}")
    case.found(sibling)
    case.configure(f"-D{OVERRIDE}=", f"-DSOURCE_DEPENDENCY_ROOTS={sibling}")
    case.found(sibling)


def check_deduplication_and_exclusions(case):
    chosen = provider(case.source / "real source")
    # Existing in-source builds do not turn legitimate source trees into
    # disposable build directories, even when the root has no capability file.
    (case.source / "CMakeCache.txt").write_text("# Existing project configuration\n")
    (chosen / "CMakeCache.txt").write_text("# Existing provider configuration\n")
    provider(case.source / ".hidden" / "another provider")
    generated = case.source / "old build"
    provider(generated / "downloaded copy")
    (generated / "CMakeCache.txt").write_text("# Exclude generated build trees\n")
    roots = [str(case.source), str(chosen), str(case.source / "real source/../real source")]
    try:
        (case.source / "alias").symlink_to(chosen, target_is_directory=True)
        (chosen / "cycle").symlink_to(case.source, target_is_directory=True)
        roots.append(str(case.source / "alias"))
    except (OSError, NotImplementedError):
        print("Symlink creation unavailable; checking duplicate roots without a cycle")
    case.configure("-DSOURCE_DEPENDENCY_ROOTS=" + ";".join(roots),
                   environment_roots=str(chosen))
    case.found(chosen)


def check_moved_provider(case):
    original = provider(case.source / "old checkout" / "nested")
    case.configure()
    case.found(original)
    renamed = case.source / "new checkout name"
    original.parent.rename(renamed)
    current = renamed / "nested"
    case.configure()
    case.found(current)
    # A fresh configure must not reuse an old absolute provider path once that
    # provider is outside every declared search root.
    external = case.base / "moved outside tree"
    renamed.rename(external)
    case.configure(error=("Missing test map-query provider",))
    case.configure(f"-DSOURCE_DEPENDENCY_ROOTS={external}")
    case.found(external / "nested")


def check_stale_override(case):
    original = provider(case.source / "old provider")
    case.configure(f"-D{OVERRIDE}={original}")
    renamed = case.source / "moved provider"
    original.rename(renamed)
    case.configure(error=(f"Invalid {OVERRIDE}",))
    case.configure(f"-D{OVERRIDE}=")
    case.found(renamed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("helper", nargs="?", type=Path,
                        default=Path(__file__).resolve().parents[1] / "cmake/SourceDependencies.cmake")
    parser.add_argument("cmake", nargs="?", default=shutil.which("cmake") or "cmake")
    arguments = parser.parse_args()
    helper = arguments.helper.resolve(strict=True)
    checks = (check_renamed_nested, check_declared_roots, check_missing,
              check_ambiguity_and_overrides, check_existing_target, check_no_sibling_fallback,
              check_deduplication_and_exclusions, check_moved_provider, check_stale_override)
    with tempfile.TemporaryDirectory(prefix="cmake source discovery ") as folder:
        for check in checks:
            project_name = {check_no_sibling_fallback: "espaper32",
                            check_renamed_nested: "source [local]"}.get(check, "project")
            case = Case(Path(folder), check.__name__, helper, arguments.cmake, project_name)
            check(case)
    print(f"CMake source dependency discovery checks passed ({len(checks)} scenarios)")


if __name__ == "__main__":
    main()
