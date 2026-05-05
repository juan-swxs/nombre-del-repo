idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_event esp_system driver json esp_http_server esp_timer)



target_add_binary_data(${COMPONENT_LIB} "index.html" BINARY)
target_add_binary_data(${COMPONENT_LIB} "style.css" BINARY)
target_add_binary_data(${COMPONENT_LIB} "app.js" BINARY)