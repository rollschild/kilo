add_executable(kilo)
target_sources(kilo PRIVATE kilo.c)
target_compile_features(kilo PRIVATE c_std_17)
