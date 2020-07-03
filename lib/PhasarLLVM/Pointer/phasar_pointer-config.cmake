set(PHASAR_pointer_COMPONENT_FOUND 1)

list(APPEND
  PHASAR_LLVM_DEPS
  Core
  Support
  Analysis
  Passes)

list(APPEND
  PHASAR_POINTER_DEPS
  utils
)

foreach(dep ${PHASAR_POINTER_DEPS})
  list(APPEND
    PHASAR_NEEDED_LIBS
    phasar::phasar_${dep}
  )
  if(NOT (${PHASAR_${dep}_COMPONENT_FOUND}))
    find_package(phasar COMPONENTS ${dep})
  endif()
endforeach()

list(APPEND
  PHASAR_NEEDED_LIBS
  phasar::phasar_pointer
)
