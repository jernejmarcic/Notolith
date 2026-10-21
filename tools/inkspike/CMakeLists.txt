# Task 0 pen spike (docs/superpowers/specs/2026-10-21-ink-core-design.md section 11).
# A developer tool: logs tablet events and draws raw polylines, nothing else.
qt_add_executable(noteAppC-inkspike main.cpp)
target_link_libraries(noteAppC-inkspike PRIVATE Qt6::Widgets)
