if(NOT EXISTS "/home/kai422/workspace/Oct_HEVC_MV_Decoder/cmake-build-debug-remote/install_manifest.txt")
  message(FATAL_ERROR "Cannot find install manifest: /home/kai422/workspace/Oct_HEVC_MV_Decoder/cmake-build-debug-remote/install_manifest.txt")
endif(NOT EXISTS "/home/kai422/workspace/Oct_HEVC_MV_Decoder/cmake-build-debug-remote/install_manifest.txt")

file(READ "/home/kai422/workspace/Oct_HEVC_MV_Decoder/cmake-build-debug-remote/install_manifest.txt" files)
string(REGEX REPLACE "\n" ";" files "${files}")
foreach(file ${files})
  message(STATUS "Uninstalling $ENV{DESTDIR}${file}")
  if(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
    exec_program(
      "/home/kai422/cmake-3.17.3-Linux-x86_64/bin/cmake" ARGS "-E remove \"$ENV{DESTDIR}${file}\""
      OUTPUT_VARIABLE rm_out
      RETURN_VALUE rm_retval
      )
    if(NOT "${rm_retval}" STREQUAL 0)
      message(FATAL_ERROR "Problem when removing $ENV{DESTDIR}${file}")
    endif(NOT "${rm_retval}" STREQUAL 0)
  else(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
    message(STATUS "File $ENV{DESTDIR}${file} does not exist.")
  endif(IS_SYMLINK "$ENV{DESTDIR}${file}" OR EXISTS "$ENV{DESTDIR}${file}")
endforeach(file)
