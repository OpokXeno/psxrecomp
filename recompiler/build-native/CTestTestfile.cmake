# CMake generated Testfile for 
# Source directory: F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler
# Build directory: F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/build-native
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[recompiler_patch_test]=] "F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/build-native/recompiler_patch_test.exe")
set_tests_properties([=[recompiler_patch_test]=] PROPERTIES  _BACKTRACE_TRIPLES "F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/CMakeLists.txt;269;add_test;F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/CMakeLists.txt;0;")
add_test([=[recompiler_patch_cxx17_header_test]=] "F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/build-native/recompiler_patch_cxx17_header_test.exe")
set_tests_properties([=[recompiler_patch_cxx17_header_test]=] PROPERTIES  _BACKTRACE_TRIPLES "F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/CMakeLists.txt;279;add_test;F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/CMakeLists.txt;0;")
add_test([=[overlay_cross_page_delay_codegen]=] "C:/Users/Matthew/AppData/Local/Programs/Python/Python312/python.exe" "F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/tests/test_overlay_cross_page_delay_codegen.py" "--recompiler" "F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/build-native/psxrecomp-game.exe")
set_tests_properties([=[overlay_cross_page_delay_codegen]=] PROPERTIES  _BACKTRACE_TRIPLES "F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/CMakeLists.txt;284;add_test;F:/Projects/psxrecomp/_wt-interp-hotpath-master/recompiler/CMakeLists.txt;0;")
subdirs("lib/fmt")
