@echo off
rem No-op stub for the PostBuildEvent declared in voxlap/voxlap.vcxproj.
rem The original upstream build invoked an installer here; this fork has no
rem install step. Kept as a stub so msbuild's post-build command succeeds
rem without modifying the vcxproj (which stays untouched until Stage 1
rem replaces the whole build system with CMake).
exit /b 0
