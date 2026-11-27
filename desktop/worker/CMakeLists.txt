include(BuildIkebackApplication)

build_ikeback_application(ikeback-worker
	NAME Worker
	WINDOWS_ICON ${CMAKE_CURRENT_SOURCE_DIR}/../master/data/ikeback-master.ico
	SOURCES
	src/FeatureWorkerManagerConnection.cpp
	src/FeatureWorkerManagerConnection.h
	src/main.cpp
	src/IkebackWorker.cpp
	src/IkebackWorker.h
	)
