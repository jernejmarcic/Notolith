add_library(noteAppC_index STATIC
    Fuzzy.cpp
    Fuzzy.h
    Index.cpp
    Index.h
    Indexer.cpp
    Indexer.h)

set_target_properties(noteAppC_index PROPERTIES AUTOMOC ON)
target_include_directories(noteAppC_index PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noteAppC_index
    PUBLIC Qt6::Core Qt6::Sql noteAppC_markdown
    PRIVATE Qt6::Concurrent noteAppC_vault)
