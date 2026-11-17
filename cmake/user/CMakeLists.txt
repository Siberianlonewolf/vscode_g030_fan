set(USER_Include_Dirs

    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/show/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/function/
    
)

set(USER_Application_Src

    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/show/tft.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/show/show.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/show/Font_color.c

    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/function/control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/function/fan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/function/key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/function/Sampling.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Code/function/adc_input.c

)

target_include_directories(stm32cubemx INTERFACE ${USER_Include_Dirs})

target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_Application_Src})