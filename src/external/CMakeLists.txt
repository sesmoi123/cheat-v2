# Externe : un seul exe, aucune injection, aucun lien avec la DLL.
# Ne depend que de user32 / gdi32 (overlay) et advapi32 (SeDebugPrivilege).
add_executable(RetracExternal WIN32 main.cpp)

target_link_libraries(RetracExternal PRIVATE user32 gdi32 advapi32 shell32)

# Manifest admin : indispensable pour ouvrir le process protege.
target_link_options(RetracExternal PRIVATE "/MANIFESTUAC:level='requireAdministrator'")

set_target_properties(RetracExternal PROPERTIES OUTPUT_NAME "RetracExternal")
