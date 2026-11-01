# Pen input test (docs/INK-DESIGN.md section 10).
# A developer tool: logs tablet events and draws raw polylines, nothing else.
qt_add_executable(noteAppC-inkspike main.cpp)
target_link_libraries(noteAppC-inkspike PRIVATE Qt6::Widgets)
