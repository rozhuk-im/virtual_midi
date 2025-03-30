
set(VIRTUAL_MIDI_BIN	dev_midi.c
			midi_backend_fluidsynth.c
			virtual_midi.c
			../midi_event.c
			../sys_utils.c)

add_executable(virtual_midi ${VIRTUAL_MIDI_BIN})
set_target_properties(virtual_midi PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(virtual_midi ${CMAKE_REQUIRED_LIBRARIES} ${FLUIDSYNTH_LIBRARIES} ${CMAKE_EXE_LINKER_FLAGS})

install(TARGETS virtual_midi RUNTIME DESTINATION bin)

if (CMAKE_SYSTEM_NAME MATCHES "^.*BSD$|DragonFly")
	install_script("../../freebsd/virtual_midi" "${CMAKE_INSTALL_PREFIX}/etc/rc.d/")
endif()
