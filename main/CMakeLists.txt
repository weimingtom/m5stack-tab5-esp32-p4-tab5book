idf_component_register(
    SRCS
        "app_main.c"
        "reader/rmem.c"
        "reader/text.c"
        "reader/doc.c"
        "reader/txt.c"
        "reader/zip.c"
        "reader/epub.c"
        "reader/pdf.c"
        "reader/pdf_text.c"
        "storage/library.c"
        "ui/ui.c"
        "ui/ui_reader.c"
    INCLUDE_DIRS "." "reader" "storage" "ui"
    REQUIRES m5stack_tab5 nvs_flash book_font espressif__zlib fatfs esp_driver_sdmmc
)
