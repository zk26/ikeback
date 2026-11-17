/*
 * ClassroomPlugin.h - main plugin implementing teacher console, student client
 *                     and behaviour monitoring
 *
 * This file is part of the Ikeback classroom modification.
 */

#pragma once

#include <memory>

#include <QDockWidget>
#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "ClassroomConfiguration.h"
#include "ClassroomMasterDialog.h"
#include "ClassroomProtocol.h"
#include "ConfigurationPagePluginInterface.h"
#include "Feature.h"
#include "FeatureProviderInterface.h"
#include "MessageContext.h"
#include "PluginInterface.h"

class QFile;
class QFileSystemWatcher;
class QProcess;
class QSystemTrayIcon;
class QTimer;
class QUdpSocket;
class ClassroomClientWidget;
class ClassroomLogStore;
class ClassroomMonitor;
class ClassroomRoster;
class ClassroomSettings;
class IkebackServerInterface;

class ClassroomPlugin : public QObject,
						public PluginInterface,
						public FeatureProviderInterface,
						public ConfigurationPagePluginInterface
{
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "io.ikeback.Ikeback.Plugins.Classroom")
	Q_INTERFACES(PluginInterface FeatureProviderInterface ConfigurationPagePluginInterface)
public:
	explicit ClassroomPlugin( QObject* parent = nullptr );
	~ClassroomPlugin() override;

	Plugin::Uid uid() const override { return Classroom::pluginUid(); }
	QVersionNumber version() const override { return QVersionNumber( 1, 0 ); }
	QString name() const override { return QStringLiteral("ikeback"); }
	QString description() const override { return tr( "ikeback - classes, logs, scoring, analysis and control" ); }
	QString vendor() const override { return QStringLiteral("zk26"); }
	QString copyright() const override { return QStringLiteral("ikeback"); }

	const FeatureList& featureList() const override;

	bool controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
						const ComputerControlInterfaceList& computerControlInterfaces ) override;

	bool startFeature( IkebackMasterInterface& master, const Feature& feature,
					   const ComputerControlInterfaceList& computerControlInterfaces ) override;

	void startSessionFeature( IkebackServerInterface& server ) override;
	void sendAsyncFeatureMessages( IkebackServerInterface& server, const MessageContext& messageContext ) override;

	bool handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
							   const FeatureMessage& message ) override;
	bool handleFeatureMessage( IkebackServerInterface& server, const MessageContext& messageContext,
							   const FeatureMessage& message ) override;
	bool handleFeatureMessageFromWorker( IkebackServerInterface& server, const FeatureMessage& message ) override;
	bool handleFeatureMessage( IkebackWorkerInterface& worker, const FeatureMessage& message ) override;

	ConfigurationPage* createConfigurationPage() override;

Q_SIGNALS:
	void eventReceived();

private:
	QString dataDirectory() const;
	ClassroomRoster* roster();
	ClassroomLogStore* store();
	ClassroomSettings* settings();

	void startClient( IkebackServerInterface& server );
	//! Creates the teacher console on demand. Quick action buttons create it
	//! HIDDEN (they only need the parameter widgets); `show` brings it up.
	ClassroomMasterDialog* ensureConsoleDialog( IkebackMasterInterface& master,
											   const ComputerControlInterfaceList& interfaces, bool show );
	void registerContext( const MessageContext& context );
	bool hasActiveContext() const;
	void ensureClientUi( IkebackWorkerInterface& worker );
	void ensureTrayIcon();
	void updateTrayTooltip();
	void reportEvent( const QString& type, const QString& detail, const QString& app );
	void applyWorkerSettings( const QJsonObject& settingsObject );
	QString localIpAddress() const;

	//! Install role marker written by the installer ("student"/"teacher") or
	//! an empty string on unmarked machines. Teacher machines never show the
	//! floating card or its tray icon.
	static QString machineRole();
	//! 5.2.2: student machines broadcast a UDP beacon so the teacher console
	//! discovers them automatically; the Master listens and registers unknown
	//! machines in the builtin directory.
	void startStudentBeacon();
	void startDiscoveryListener();
	void registerDiscoveredHost( const QString& hostName, const QString& ip );

	//! Builds one tool-bar Feature per quick-tool action in the white list, so
	//! the actions sit next to "Screen demo"/"Remote control" instead of being
	//! hidden behind a drop-down.
	void buildQuickActionFeatures();
	static Feature::Uid quickActionUid( int index );

	ClassroomConfiguration m_configuration;
	const Feature m_consoleFeature;
	const Feature m_clientFeature;
	const Feature m_signInFeature;
	const Feature m_signOutFeature;
	const Feature m_quickToolsFeature;
	const Feature m_checkInFeature;
	const Feature m_operationLogFeature;
	FeatureList m_features;
	//! Same order as the white list in buildQuickActionFeatures(); the index is
	//! also the UID suffix.
	QStringList m_quickActionIds;

	std::unique_ptr<ClassroomRoster> m_roster;
	std::unique_ptr<ClassroomLogStore> m_store;
	std::unique_ptr<ClassroomSettings> m_settings;
	QPointer<ClassroomMasterDialog> m_dialog;
	QPointer<QDockWidget> m_consoleDock;

	IkebackServerInterface* m_server = nullptr;
	QVector<MessageContext> m_contexts;
	QVector<FeatureMessage> m_pendingEvents;
	QTimer* m_watchdog = nullptr;
	bool m_restartPending = false;
	bool m_stopAllowed = false;
	//! Consecutive restarts that never produced a working worker. A worker that
	//! cannot start (missing runtime DLL, blocked by security software) used to
	//! be relaunched every 30 s forever, which is what left dozens of
	//! ikeback-worker.exe processes behind.
	int m_restartFailures = 0;

	IkebackWorkerInterface* m_worker = nullptr;
	ClassroomClientWidget* m_clientWidget = nullptr;
	QSystemTrayIcon* m_trayIcon = nullptr;
	int m_trayIconRetries = 0;
	ClassroomMonitor* m_monitor = nullptr;
	QJsonObject m_workerSettings;

	QString m_hostName;
	QString m_ipAddress;
	QString m_className;
	QString m_seat;
	QString m_student;
	QString m_sessionId;
	//! Never holds the bridge key: only whether the teacher has started a lesson.
	bool m_keyConfigured = false;

	//! Student-side presence beacon (UDP broadcast).
	QTimer* m_beaconTimer = nullptr;
	//! Master-side discovery listener for student beacons.
	QUdpSocket* m_discoverySocket = nullptr;
	//! ip -> last beacon time; keeps the directory rewrite frequency low.
	QMap<QString, QDateTime> m_discoveredHosts;
	//! Worker-side staging file for remote software deployment.
	QFile* m_deployFile = nullptr;
	//! Worker-side installer process so its real exit code can be reported.
	QProcess* m_installProcess = nullptr;
	//! Master-side watcher so roster imports from the configurator show up at once.
	QFileSystemWatcher* m_rosterWatcher = nullptr;
	//! ip -> live classroom display name, more reliable than a stale roster IP.
	QMap<QString, QString> m_studentDisplayByIp;
};
