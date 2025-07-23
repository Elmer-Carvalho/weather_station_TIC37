# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.1.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD pico_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)
set(FREERTOS_KERNEL_PATH "C:/Users/Elmer Carvalho/Documents/Projetos-DEV/EmbarcaTech/EmbarcaTech_Fase_2/FreeRTOS-Kernel")
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

project(weather_station C CXX ASM)

pico_sdk_init()

include_directories(${CMAKE_SOURCE_DIR}/lib)

add_executable(${PROJECT_NAME} 
    weather_station.c 
    lib/ssd1306.c 
    lib/bmp280.c 
    lib/aht20.c
)

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/lib/pio_matrix.pio)

pico_set_program_name(${PROJECT_NAME} "weather_station")
pico_set_program_version(${PROJECT_NAME} "0.1")

pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_enable_stdio_usb(${PROJECT_NAME} 1)

target_link_libraries(${PROJECT_NAME}
        pico_stdlib
        FreeRTOS-Kernel        # Kernel do FreeRTOS
        FreeRTOS-Kernel-Heap4  # Gerenciador de memoria
        hardware_pwm           # PWM do RP2040
        hardware_clocks        # Clock do RP2040
        hardware_i2c           # I2C do RP2040
        hardware_pio
        pico_cyw43_arch_lwip_threadsafe_background
)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(${PROJECT_NAME} 
        
        )

pico_add_extra_outputs(${PROJECT_NAME})