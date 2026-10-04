# Copyright 2026 CEMAXECUTER LLC

idf_component_register(
    SRCS "main.c" "halow_link.c" "console.c" "web.c" "radio.c" "spectrum.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_netif esp_event esp_driver_uart esp_http_server esp_phy esp_hw_support
             esp_timer heap json
)

# The page, gzipped into flash and served as is.
set(web_dir "${CMAKE_CURRENT_SOURCE_DIR}/../web")
set(gz_dir "${CMAKE_CURRENT_BINARY_DIR}/web")
file(MAKE_DIRECTORY "${gz_dir}")
foreach(name index.html app.js render.js style.css icon.svg)
    add_custom_command(
        OUTPUT "${gz_dir}/${name}.gz"
        COMMAND gzip -9 -n -c "${web_dir}/${name}" > "${gz_dir}/${name}.gz"
        DEPENDS "${web_dir}/${name}")
    target_add_binary_data(${COMPONENT_LIB} "${gz_dir}/${name}.gz" BINARY)
endforeach()
