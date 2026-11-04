# C pur : aucune dépendance ESP-IDF (règle d'or, CLAUDE.md §4).
idf_component_register(SRCS "flows.c" "sanity.c" "tariff.c" "stats.c" "profile.c" "rls.c" "models.c"
                            "waterheater.c" "planner.c" "regulator.c"
                       INCLUDE_DIRS "include")
