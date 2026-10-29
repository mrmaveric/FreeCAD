# Runs moc from CMake and shows the PATH CMake passes on
file(TO_CMAKE_PATH "$ENV{CONDA_PREFIX}" prefix)
execute_process(COMMAND "${prefix}/Library/lib/qt6/moc.exe" -v
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("  moc started by cmake: ${result} ${out}${err}")
string(LENGTH "$ENV{PATH}" length)
message("  PATH in cmake: ${length} characters")
set(path "$ENV{PATH}")
list(SUBLIST path 0 8 first)
foreach(entry IN LISTS first)
    message("    ${entry}")
endforeach()
