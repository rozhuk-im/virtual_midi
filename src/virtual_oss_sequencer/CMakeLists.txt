
set(VIRTUAL_OSS_SEQUENCER_BIN	dev_oss_sequencer.c
				virtual_oss_sequencer.c
				../midi_event.c
				../sys_utils.c)

add_executable(virtual_oss_sequencer ${VIRTUAL_OSS_SEQUENCER_BIN})
set_target_properties(virtual_oss_sequencer PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(virtual_oss_sequencer ${CMAKE_REQUIRED_LIBRARIES} ${CMAKE_EXE_LINKER_FLAGS})

install(TARGETS virtual_oss_sequencer RUNTIME DESTINATION bin)

if (CMAKE_SYSTEM_NAME MATCHES "^.*BSD$|DragonFly")
	install_script("../../freebsd/virtual_oss_sequencer" "${CMAKE_INSTALL_PREFIX}/etc/rc.d/")
endif()
