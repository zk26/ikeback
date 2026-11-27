include(BuildIkebackPlugin)

build_ikeback_plugin(classroom
	NAME Classroom
	SOURCES
	ClassroomAiClient.cpp
	ClassroomAiClient.h
	ClassroomBridge.cpp
	ClassroomBridge.h
	ClassroomClient.cpp
	ClassroomClient.h
	ClassroomConfiguration.h
	ClassroomConfigurationPage.cpp
	ClassroomConfigurationPage.h
	ClassroomData.cpp
	ClassroomData.h
	ClassroomMasterDialog.cpp
	ClassroomMasterDialog.h
	ClassroomPlugin.cpp
	ClassroomPlugin.h
	ClassroomProtocol.h
	ClassroomScore.cpp
	ClassroomScore.h
	ClassroomScorePanel.cpp
	ClassroomScorePanel.h
	ClassroomShop.cpp
	ClassroomShop.h
	ClassroomShopPanel.cpp
	ClassroomShopPanel.h
	ClassroomSignInDialog.cpp
	ClassroomSignInDialog.h
	ClassroomSeatGrid.cpp
	ClassroomSeatGrid.h
	ClassroomSecrets.cpp
	ClassroomSecrets.h
	ClassroomSystem.cpp
	ClassroomSystem.h
	ClassroomTheme.cpp
	ClassroomTheme.h
	ClassroomTermReport.cpp
	ClassroomTermReport.h
	ClassroomWordCloud.cpp
	ClassroomWordCloud.h
	classroom.qrc
)

if(WIN32)
	# CryptProtectData / CryptUnprotectData used by ClassroomSecrets.
	target_link_libraries(classroom PRIVATE crypt32)
endif()

test_ikeback_plugin(classroom ClassroomSelfTest)
if(WITH_TESTS)
	target_include_directories(ClassroomSelfTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	set_tests_properties(ClassroomSelfTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
	if(WIN32 AND MINGW)
		target_link_options(classroom PRIVATE "-Wl,--export-all-symbols")
	endif()
endif()
