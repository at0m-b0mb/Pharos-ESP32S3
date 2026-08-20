# Deliberately free of ESP-IDF dependencies beyond pharos_core's headers:
# every file here also compiles under test/host/Makefile with a plain cc, and
# every line of it is exercised there before it reaches a radio.
#
# Keep this list in step with test/host/Makefile. A source present in one and
# missing from the other is the classic way for a firmware link to break long
# after the host tests went green.
idf_component_register(
    SRCS
        "pharos_acoustic.c"
        "pharos_aegis.c"
        "pharos_census.c"
        "pharos_chain.c"
        "pharos_console.c"
        "pharos_dot11.c"
        "pharos_flood.c"
        "pharos_harvest.c"
        "pharos_karma.c"
        "pharos_locate.c"
        "pharos_opsec.c"
        "pharos_survey.c"
        "pharos_tower.c"
        "pharos_power.c"
        "pharos_probe.c"
        "pharos_range.c"
        "pharos_report.c"
        "pharos_rival.c"
        "pharos_sentinel.c"
        "pharos_sha256.c"
        "pharos_squall.c"
        "pharos_twin.c"
        "pharos_vigil.c"
        "pharos_watch.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core
)
