# C pur : aucune dépendance ESP-IDF (règle d'or, CLAUDE.md §4).
idf_component_register(SRCS "rls.c" "profile.c" "stats.c" "tariff.c" "flows.c" "sanity.c"
                       INCLUDE_DIRS "include")
