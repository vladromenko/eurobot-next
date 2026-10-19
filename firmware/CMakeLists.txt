set(_deps ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(FREERTOS_KERNEL_PATH ${_deps}/FreeRTOS-Kernel CACHE PATH "FreeRTOS source")
set(CMSIS_CORE_PATH ${_deps}/CMSIS_5/CMSIS/Core/Include CACHE PATH "CMSIS Core headers")
set(CMSIS_DEVICE_PATH ${_deps}/cmsis_device_f4 CACHE PATH "STM32F4 device source")
foreach(dep ${FREERTOS_KERNEL_PATH}/tasks.c ${CMSIS_CORE_PATH}/core_cm4.h ${CMSIS_DEVICE_PATH}/Include/stm32f407xx.h)
 if(NOT EXISTS ${dep})
  message(FATAL_ERROR "Missing ${dep}; run tools/dependencies.sh")
 endif()
endforeach()
add_library(freertos STATIC ${FREERTOS_KERNEL_PATH}/tasks.c ${FREERTOS_KERNEL_PATH}/queue.c ${FREERTOS_KERNEL_PATH}/list.c ${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM4F/port.c)
target_include_directories(freertos PUBLIC include ${FREERTOS_KERNEL_PATH}/include ${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM4F)
target_compile_options(freertos PRIVATE -Wall -Wextra)
add_executable(eurobot_next.elf src/stm32_board.cpp src/main.cpp src/runtime.cpp ${CMSIS_DEVICE_PATH}/Source/Templates/gcc/startup_stm32f407xx.s)
target_include_directories(eurobot_next.elf PRIVATE ${CMSIS_CORE_PATH} ${CMSIS_DEVICE_PATH}/Include)
target_compile_definitions(eurobot_next.elf PRIVATE STM32F407xx)
set(EUROBOT_BOARD_CONFIG "" CACHE FILEPATH "Reviewed board deployment profile header")
if(EUROBOT_BOARD_CONFIG)
 configure_file(${EUROBOT_BOARD_CONFIG} ${CMAKE_CURRENT_BINARY_DIR}/generated/deployment.hpp COPYONLY)
 target_include_directories(eurobot_next.elf PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
 target_compile_definitions(eurobot_next.elf PRIVATE EUROBOT_DEPLOYMENT_CONFIG=1)
else()
 file(REMOVE ${CMAKE_CURRENT_BINARY_DIR}/generated/deployment.hpp)
endif()
target_link_libraries(eurobot_next.elf PRIVATE eurobot_core freertos m)
target_compile_options(eurobot_next.elf PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(eurobot_next.elf PRIVATE -nostartfiles -T${CMAKE_CURRENT_SOURCE_DIR}/linker/stm32f407vg.ld -Wl,--gc-sections,-Map=${CMAKE_CURRENT_BINARY_DIR}/eurobot_next.map,--print-memory-usage --specs=nano.specs --specs=nosys.specs)
set_property(TARGET eurobot_next.elf APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/linker/stm32f407vg.ld ${PROJECT_SOURCE_DIR}/tools/image_manifest.py ${PROJECT_SOURCE_DIR}/tools/source_fingerprint.py ${PROJECT_SOURCE_DIR}/tools/dependencies.sh ${PROJECT_SOURCE_DIR}/CMakePresets.json)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(_compiler_bin ${CMAKE_C_COMPILER} DIRECTORY)
find_program(ARM_READELF arm-none-eabi-readelf HINTS ${_compiler_bin} REQUIRED)
add_custom_command(TARGET eurobot_next.elf POST_BUILD
 BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/eurobot_next.hex ${CMAKE_CURRENT_BINARY_DIR}/eurobot_next.bin ${CMAKE_CURRENT_BINARY_DIR}/eurobot_next.manifest.json ${CMAKE_CURRENT_BINARY_DIR}/eurobot_next.map
 COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:eurobot_next.elf> ${CMAKE_CURRENT_BINARY_DIR}/eurobot_next.hex
 COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:eurobot_next.elf> ${CMAKE_CURRENT_BINARY_DIR}/eurobot_next.bin
 COMMAND ${CMAKE_SIZE} $<TARGET_FILE:eurobot_next.elf>
 COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/verify_elf.py $<TARGET_FILE:eurobot_next.elf> --readelf ${ARM_READELF}
 COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/image_manifest.py ${CMAKE_CURRENT_BINARY_DIR}/eurobot_next.bin --deployment-source "${EUROBOT_BOARD_CONFIG}"
 VERBATIM)
