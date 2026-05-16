"""Conan 2 recipe for migris-labs/fsw-core."""

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class MigrisFswCoreConan(ConanFile):
    name = "migris-fsw-core"
    version = "0.1.0"
    license = "Apache-2.0"
    author = "Migris <max.giacalone@icloud.com>"
    description = (
        "Migris platform — flight-software framework "
        "(HAL, PUS, FDIR, mode mgr, scheduler)"
    )
    url = "https://github.com/migris-labs/fsw-core"
    topics = ("migris", "cubesat", "flight-software", "zephyr", "space", "newspace")

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_tests": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_tests": True,
    }

    exports_sources = (
        "CMakeLists.txt",
        "CMakePresets.json",
        "cmake/*",
        "include/*",
        "src/*",
        "lib/*",
        "tests/*",
        "LICENSE",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def build_requirements(self):
        if self.options.with_tests:
            self.test_requires("gtest/1.14.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["MIGRIS_FSW_BUILD_TESTS"] = bool(self.options.with_tests)
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if self.options.with_tests:
            cmake.test()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["fsw-core"]
        self.cpp_info.set_property("cmake_target_name", "migris::fsw-core")
        self.cpp_info.set_property("cmake_file_name", "migris-fsw-core")
