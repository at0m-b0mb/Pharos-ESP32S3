# Deliberately free of ESP-IDF dependencies beyond pharos_core's headers:
# every file here also compiles under test/host/Makefile with a plain cc, and
# every line of it is exercised there before it reaches a radio.
idf_component_register(
    SRCS
        "pharos_census.c"
        "pharos_dot11.c"
        "pharos_report.c"
        "pharos_twin.c"
        "pharos_watch.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core
)
