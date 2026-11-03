# C pur : aucune dépendance ESP-IDF (règle d'or, CLAUDE.md §4).
idf_component_register(SRCS "flows.c" "sanity.c"
                       INCLUDE_DIRS "include")
