/*
 * ClassroomPlugin.cpp - main plugin implementing teacher console, student client
 *                      and behaviour monitoring
 *
 * This file is part of the Ikeback classroom modification.
 */

#include <QtGlobal>

#include <QAbstractSocket>
#include <QAction>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QDockWidget>
#include <QMainWindow>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QFileSystemWatcher>
#include <QDialogButtonBox>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QIcon>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QUuid>

#include "ClassroomAiClient.h"
#include "ClassroomClient.h"
#include "ClassroomConfigurationPage.h"
#include "ClassroomData.h"
#include "ClassroomMasterDialog.h"
#include "ClassroomPlugin.h"
#include "ClassroomProtocol.h"
#include "ClassroomScore.h"
#include "ClassroomSecrets.h"
#include "ClassroomShop.h"
#include "ClassroomSignInDialog.h"
#include "ClassroomSystem.h"
#include "ComputerControlInterface.h"
#include "FeatureWorkerManager.h"
#include "Filesystem.h"
#include "NetworkObject.h"
#include "NetworkObjectDirectory.h"
#include "NetworkObjectDirectoryManager.h"
#include "PlatformCoreFunctions.h"
#include "PlatformFilesystemFunctions.h"
#include "PlatformPluginInterface.h"
#include "IkebackCore.h"
#include "IkebackMasterInterface.h"
#include "IkebackServerInterface.h"
#include "IkebackWorkerInterface.h"

// windows.h may be pulled in transitively AFTER ClassroomProtocol.h, which
// re-defines `ReportEvent` as ReportEventW (Win32 event-log API). Command::
// ReportEvent is an enum member, not that API - drop the macro (see the same
// guard at the enum declaration in ClassroomProtocol.h).
#ifdef ReportEvent
#undef ReportEvent
#endif

using namespace Classroom;

//! Give up automatic worker restarts after this many consecutive failures.
static constexpr int MaxWorkerRestartFailures = 5;

static void persistAdminKeyHash( const QJsonObject& settingsObject );
static QJsonObject withLocalStudentProtection( QJsonObject settingsObject );

static QString portableAppsRoot()
{
#ifdef Q_OS_WIN
	return QStringLiteral("C:/IkebackApps");
#else
	return QStandardPaths::writableLocation( QStandardPaths::AppDataLocation ) + QStringLiteral("/IkebackApps");
#endif
}

static bool safeRelativePath( const QString& value )
{
	if( value.isEmpty() || value.contains( QLatin1Char(':') ) || QDir::isAbsolutePath( value ) ) { return false; }
	const auto native = QDir::fromNativeSeparators( value );
	if( native.startsWith( QLatin1Char('/') ) || native.startsWith( QLatin1Char('\\') ) ) { return false; }
	for( const auto& part : native.split( QLatin1Char('/') ) )
	{
		if( part.isEmpty() || part == QStringLiteral("." ) || part == QStringLiteral(".." ) ) { return false; }
	}
	return true;
}

static bool safePackageFolder( const QString& value )
{
	return value.isEmpty() == false && value != QStringLiteral("." ) && value != QStringLiteral(".." ) &&
		value.contains( QRegularExpression( QStringLiteral("[<>:\"/\\\\|?*]") ) ) == false;
}

static QString deploymentStateDirectory()
{
	return QDir( QStandardPaths::writableLocation( QStandardPaths::AppLocalDataLocation ) ).
		filePath( QStringLiteral("ikeback/portable-deployments") );
}


static bool pathWithoutLinks( const QString& path )
{
	QFileInfo info( path );
#ifdef Q_OS_WIN
	const auto sensitivity = Qt::CaseInsensitive;
#else
	const auto sensitivity = Qt::CaseSensitive;
#endif
	while( true )
	{
		if( info.isSymLink() )
		{
			return false;
		}
		if( info.exists() )
		{
			const auto canonical = info.canonicalFilePath();
			if( canonical.isEmpty() || canonical.compare( info.absoluteFilePath(), sensitivity ) != 0 )
			{
				return false;
			}
		}
		const auto parent = info.absoluteDir().absolutePath();
		if( parent == info.absoluteFilePath() )
		{
			return true;
		}
		info.setFile( parent );
	}
}

static bool ensureDeploymentStateDirectory()
{
	const auto path = deploymentStateDirectory();
	if( pathWithoutLinks( path ) == false ) { return false; }
	const bool existed = QDir( path ).exists();
	if( QDir().mkpath( path ) == false ) { return false; }
#ifdef Q_OS_WIN
	if( existed == false && IkebackCore::platform().filesystemFunctions().setFileOwnerGroupPermissions(
				path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) == false )
	{
		return false;
	}
#else
	Q_UNUSED( existed );
#endif
	return true;
}


static QByteArray fileSha256( const QString& path )
{
	QFile file( path );
	if( file.open( QIODevice::ReadOnly ) == false ) { return {}; }
	QCryptographicHash hash( QCryptographicHash::Sha256 );
	while( file.atEnd() == false )
	{
		const auto chunk = file.read( 1024 * 1024 );
		if( chunk.isEmpty() && file.error() != QFileDevice::NoError ) { return {}; }
		hash.addData( chunk );
	}
	return hash.result().toHex();
}

static QString desktopShortcutPath( const QString& name )
{
	const auto shortcutName = QFileInfo( name ).fileName();
	return QDir( QStandardPaths::writableLocation( QStandardPaths::DesktopLocation ) ).filePath(
		shortcutName.endsWith( QStringLiteral(".lnk"), Qt::CaseInsensitive ) ? shortcutName : shortcutName + QStringLiteral(".lnk") );
}

IMPLEMENT_CONFIG_PROXY(ClassroomConfiguration)


ClassroomPlugin::ClassroomPlugin( QObject* parent ) :
	QObject( parent ),
	m_configuration( &IkebackCore::config() ),
	m_consoleFeature( Feature( QStringLiteral("ClassroomConsole"),
							   Feature::Flag::Action | Feature::Flag::Master,
							   consoleFeatureUid(), Feature::Uid(),
							   Classroom::productName(), {},
							   tr( "开始上课：进入课堂模式并发起全班签到" ),
							   QStringLiteral(":/classroom/icons/class.png") ) ),
	m_clientFeature( Feature( QStringLiteral("ClassroomClient"),
							  Feature::Flag::Session | Feature::Flag::Service | Feature::Flag::Worker,
							  clientFeatureUid(), Feature::Uid(),
							  tr( "Classroom client" ), {}, {} ) ),
	// Name + seat sign-in replaces Ikeback's username/password logon buttons.
	// Action|Master only (like the console feature): no Worker flag, so no
	// extra worker process; startFeature() never calls prepareLogon().
	m_signInFeature( Feature( QStringLiteral("ClassroomSignIn"),
							  Feature::Flag::Action | Feature::Flag::Master,
							  signInFeatureUid(), Feature::Uid(),
							  tr( "Sign in" ), {},
							  tr( "Sign in a student by name and seat number" ),
							  QStringLiteral(":/classroom/icons/check-in.png") ) ),
	m_signOutFeature( Feature( QStringLiteral("ClassroomSignOut"),
							   Feature::Flag::Action | Feature::Flag::Master,
							   signOutFeatureUid(), Feature::Uid(),
							   tr( "Sign out" ), {},
							   tr( "Sign out the current student" ),
							   QStringLiteral(":/core/edit-delete.png") ) ),
	// 5.2.0: the quick tools and check-in actions are first-class tool-bar
	// buttons. They only open UI in the Master process - no Worker flag, so no
	// extra ikeback Worker process is created.
	m_quickToolsFeature( Feature( QStringLiteral("ClassroomQuickTools"),
								  Feature::Flag::Action | Feature::Flag::Master,
								  quickToolsFeatureUid(), Feature::Uid(),
								  tr( "常用工具" ), {},
								  tr( "打开工具参数页：广播、UWF、网关、肃静、禁用与评分等设置" ),
								  QStringLiteral(":/core/icon22.png") ) ),
								  m_checkInFeature( Feature( QStringLiteral("ClassroomCheckIn"),
								   Feature::Flag::Action | Feature::Flag::Master,
								   checkInFeatureUid(), Feature::Uid(),
								   tr( "签到" ), {},
								   tr( "在所有学生机上开始签到" ),
								   QStringLiteral(":/classroom/icons/check-in.png") ) ),
	// 5.2.2: operation log button - answers "did my protection/UWF command
	// actually succeed?" without digging through the behaviour log.
	m_operationLogFeature( Feature( QStringLiteral("ClassroomOperationLog"),
									Feature::Flag::Action | Feature::Flag::Master,
									operationLogFeatureUid(), Feature::Uid(),
									tr( "操作记录" ), {},
									tr( "查看教师操作的执行结果（保护、UWF、静音、远程安装等）" ),
									QStringLiteral(":/core/document-edit.png") ) ),
	m_endLessonFeature( Feature( QStringLiteral("ClassroomEndLesson"),
								 Feature::Flag::Action | Feature::Flag::Master,
								 endLessonFeatureUid(), Feature::Uid(),
								 tr( "结束上课" ), {},
								 tr( "结束当前课堂并返回监控模式" ),
								 QStringLiteral(":/core/edit-delete.png") ) ),
	// iKeback / 注销 / 常用工具 不再注册为可用功能，命令行和快捷键也无法触发。
	m_features( { m_clientFeature, m_consoleFeature, m_checkInFeature, m_operationLogFeature, m_endLessonFeature } ),
	m_hostName( QHostInfo::localHostName() )
{
	m_ipAddress = localIpAddress();
	buildQuickActionFeatures();

	if( IkebackCore::component() == IkebackCore::Component::Master )
	{
		// Remote-access window titles show the roster name of the student who
		// sits at the machine (Q4), not the raw computer directory entry.
		IkebackCore::setComputerNameResolver(
			[this]( const QString& hostName, const QString& hostAddress ) {
				const auto ip = hostAddress.trimmed();
				// 优先使用学生刚上报的实时身份，名册 IP 过期时也不会退回目录名。
				const auto liveName = m_studentDisplayByIp.value( ip );
				if( liveName.isEmpty() == false ) { return liveName; }
				const auto liveByHost = m_studentDisplayByIp.value( hostName.trimmed() );
				if( liveByHost.isEmpty() == false ) { return liveByHost; }
				for( const auto& classValue : roster()->toJsonArray() )
				{
					for( const auto& studentValue : classValue.toObject().value( QStringLiteral("students") ).toArray() )
					{
						const auto student = studentValue.toObject();
						const auto studentIp = student.value( QStringLiteral("ip") ).toString();
						if( studentIp.isEmpty() || ( studentIp != ip && studentIp != hostName ) ) { continue; }
						const auto label = QStringLiteral("%1 %2")
							.arg( student.value( QStringLiteral("seat") ).toString(),
								  student.value( QStringLiteral("name") ).toString() ).trimmed();
						return label.isEmpty() ? QString{} : QStringLiteral("座位 %1").arg( label );
					}
				}
				// 本机（教师机）没有名册条目时不再显示 Administrator/主机名。
				if( hostName.compare( m_hostName, Qt::CaseInsensitive ) == 0 ||
					( ip.isEmpty() == false && ip == localIpAddress() ) )
				{
					return QStringLiteral("教师机");
				}
				return QString{};
			} );

		// 配置器里批量导入名册后，教师端无需重启即可看到新名单。
		m_rosterWatcher = new QFileSystemWatcher( this );
		const auto rosterPath = roster()->filePath();
		QDir().mkpath( QFileInfo( rosterPath ).absolutePath() );
		if( QFileInfo::exists( rosterPath ) == false ) { roster()->save(); }
		m_rosterWatcher->addPath( rosterPath );
		connect( m_rosterWatcher, &QFileSystemWatcher::fileChanged, this,
				 [this]( const QString& path ) {
					 if( m_rosterWatcher->files().contains( path ) == false )
					 {
						 m_rosterWatcher->addPath( path );
					 }
					 roster()->load();
					 if( m_dialog ) { m_dialog->reloadRoster(); }
				 } );

		startDiscoveryListener();
	}
}


Feature::Uid ClassroomPlugin::quickActionUid( int index )
{
	// Kept in its own UUID range so quick-action buttons can never collide with
	// the protocol features declared in ClassroomProtocol.h.
	return Feature::Uid{ QStringLiteral("d3f1c0de-7a41-4b8e-9c14-%1" )
							 .arg( index, 12, 10, QLatin1Char('0') ) };
}


void ClassroomPlugin::buildQuickActionFeatures()
{
	// Every entry becomes a first-class button on the Ikeback Master tool bar,
	// right next to "Screen demo" and "Remote control". The tool bar is allowed
	// to wrap onto a second row, so the pairs stay side by side.
	struct QuickButton
	{
		const char* id;
		const char* name;
		const char* tooltip;
		const char* icon;
	};

	// One button per function: paired "on"/"off" entries were replaced by a
	// single toggle (see ClassroomMasterDialog::*toggle actions). Broadcast is
	// NOT repeated here - the upstream "屏幕演示" / "远程控制" buttons already
	// do that.
	static const QuickButton buttons[] = {
		{ "protection.toggle", "保护", "点一下切换：学生能否退出 iKeback 卡片",
		  ":/classroom/icons/protection.png" },
		{ "uwf.toggle", "UWF", "点一下切换 Windows 写保护（装软件前先关闭）",
		  ":/classroom/icons/lock.png" },
		{ "gateway.disable", "禁用网关", "断开学生机外网访问（保留教师机）",
		  ":/classroom/icons/gateway-disable.png" },
		{ "gateway.enable", "启用网关", "恢复学生机外网访问",
		  ":/classroom/icons/gateway-enable.png" },
		{ "silence.toggle", "肃静页", "点一下显示或收起肃静页面",
		  ":/classroom/icons/silence.png" },
		{ "appblock.toggle", "禁用应用", "添加关键字，命中即拦截程序",
		  ":/classroom/icons/block-app.png" },
		{ "webblock.toggle", "禁用网址", "添加关键字，命中即拦截网址",
		  ":/classroom/icons/block-web.png" },
		{ "mute.toggle", "静音", "点一下切换学生机静音",
		  ":/classroom/icons/mute.png" },
		{ "score.p1", "全部加分", "给全班每人加分（填写分数与原因）",
		  ":/classroom/icons/plus.png" },
		{ "score.m1", "全部减分", "给全班每人减分（填写分数与原因）",
		  ":/classroom/icons/minus.png" },
		{ "remote.install", "远程装软件", "选择安装包后下发到学生机安装",
		  ":/classroom/icons/remote-install.png" },
		{ "batch.programs", "批量打开程序", "一次填写多个学生机程序路径并批量下发",
		  ":/core/document-open.png" },
		{ "batch.websites", "批量打开网页", "一次填写多个网址并批量下发",
		  ":/core/document-open.png" },
	};

	int index = 0;
	for( const auto& button : buttons )
	{
		const auto id = QString::fromLatin1( button.id );
		const auto uid = quickActionUid( index );
		m_quickActionIds.append( id );
		const auto label = QCoreApplication::translate( "ClassroomPlugin", button.name );
		m_features.append( Feature( id,
									Feature::Flag::Action | Feature::Flag::Master,
									uid, Feature::Uid(),
									label, {},
									QCoreApplication::translate( "ClassroomPlugin", button.tooltip ),
									QString::fromLatin1( button.icon ) ) );
		++index;
	}
}


ClassroomPlugin::~ClassroomPlugin()
{
	if( m_trayIcon )
	{
		delete m_trayIcon;
	}
	m_trayIcon = nullptr;
	delete m_clientWidget;
	m_clientWidget = nullptr;
}


void ClassroomPlugin::ensureTrayIcon()
{
	// The ikeback worker always runs in the interactive user session, so it
	// can own the tray icon itself. Ikeback's built-in SystemTrayIcon feature is
	// a separate worker process which is exactly what we want to avoid here.
	if( m_trayIcon ) { return; }
	if( QSystemTrayIcon::isSystemTrayAvailable() == false )
	{
		// Explorer's tray may not be ready when the worker starts. Retry a few
		// times instead of silently giving up on the icon forever.
		if( m_trayIconRetries < 5 )
		{
			++m_trayIconRetries;
			QTimer::singleShot( 3000, this, [this]() { ensureTrayIcon(); } );
		}
		return;
	}

	m_trayIcon = new QSystemTrayIcon( QIcon( QStringLiteral(":/core/icon32.png") ), this );
	m_trayIcon->setToolTip( Classroom::productName() );

	connect( m_trayIcon, &QSystemTrayIcon::activated, this,
			 [this]( QSystemTrayIcon::ActivationReason reason ) {
				 if( reason != QSystemTrayIcon::Context && m_clientWidget )
				 {
					 m_clientWidget->reactivate();
				 }
			 } );
	m_trayIcon->show();
	updateTrayTooltip();
}


void ClassroomPlugin::updateTrayTooltip()
{
	if( m_trayIcon == nullptr ) { return; }
	QString text = Classroom::productName();
	if( m_student.isEmpty() == false )
	{
		text += QStringLiteral("\n%1 · %2 %3").arg( m_className, tr( "座位" ), m_seat );
		text += QStringLiteral("\n%1").arg( m_student );
	}
	m_trayIcon->setToolTip( text );
}


QString ClassroomPlugin::localIpAddress() const
{
	for( const auto& address : QNetworkInterface::allAddresses() )
	{
		if( address.isLoopback() == false && address.protocol() == QAbstractSocket::IPv4Protocol )
		{
			return address.toString();
		}
	}
	return QHostAddress( QHostAddress::LocalHost ).toString();
}


QString ClassroomPlugin::machineRole()
{
#ifdef Q_OS_WIN
	QSettings machine( QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\ikeback"),
					   QSettings::Registry64Format );
	return machine.value( QStringLiteral("InstallRole") ).toString();
#else
	return {};
#endif
}


void ClassroomPlugin::startStudentBeacon()
{
	if( m_beaconTimer != nullptr ) { return; }

	m_beaconTimer = new QTimer( this );
	connect( m_beaconTimer, &QTimer::timeout, this, [this]() {
		const auto payload = QStringLiteral("%1:%2:%3")
								.arg( Classroom::studentBeaconMagic(), m_hostName, m_ipAddress ).toUtf8();
		QUdpSocket socket;
		socket.writeDatagram( payload, QHostAddress::Broadcast, Classroom::studentBeaconPort() );
	} );
	m_beaconTimer->start( 5000 );
}


void ClassroomPlugin::startDiscoveryListener()
{
	if( m_discoverySocket != nullptr ) { return; }

	m_discoverySocket = new QUdpSocket( this );
	if( m_discoverySocket->bind( QHostAddress::AnyIPv4, Classroom::studentBeaconPort(),
								 QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint ) == false )
	{
		vWarning() << "ikeback: student discovery socket could not bind"
				   << Classroom::studentBeaconPort() << m_discoverySocket->errorString();
		m_discoverySocket->deleteLater();
		m_discoverySocket = nullptr;
		return;
	}

	connect( m_discoverySocket, &QUdpSocket::readyRead, this, [this]() {
		while( m_discoverySocket->hasPendingDatagrams() )
		{
			QByteArray datagram;
			datagram.resize( static_cast<int>( m_discoverySocket->pendingDatagramSize() ) );
			QHostAddress sender;
			m_discoverySocket->readDatagram( datagram.data(), datagram.size(), &sender );
			const auto text = QString::fromUtf8( datagram );
			if( text.startsWith( Classroom::studentBeaconMagic() + QLatin1Char(':') ) == false ) { continue; }
			const auto parts = text.split( QLatin1Char(':') );
			const auto hostName = parts.value( 2 );
			const auto ip = parts.value( 3 ).isEmpty() ? sender.toString() : parts.value( 3 );
			registerDiscoveredHost( hostName, ip );
		}
	} );
}


void ClassroomPlugin::registerDiscoveredHost( const QString& hostName, const QString& ip )
{
	const auto sanitizedIp = Classroom::sanitizeUnicastIp( ip );
	if( sanitizedIp.isEmpty() ) { return; }

	// Throttle: a machine that keeps beaconing must not rewrite the directory
	// every five seconds.
	const auto now = QDateTime::currentDateTime();
	const auto lastSeen = m_discoveredHosts.value( sanitizedIp );
	if( lastSeen.isValid() && lastSeen.secsTo( now ) < 60 ) { return; }
	m_discoveredHosts.insert( sanitizedIp, now );

	auto& configuration = IkebackCore::config();
	if( configuration.isStoreWritable() == false ) { return; }

	auto objects = configuration.value( QStringLiteral("NetworkObjects"), QStringLiteral("BuiltinDirectory"),
										QJsonArray() ).toJsonArray();

	for( const auto& value : objects )
	{
		const auto object = value.toObject();
		if( object.value( QStringLiteral("Type") ).toInt() == static_cast<int>( NetworkObject::Type::Host ) &&
			object.value( QStringLiteral("HostAddress") ).toString() == sanitizedIp )
		{
			return; // already known
		}
	}

	// Collect all IPs so a rename does not duplicate entries.
	QSet<QString> knownIps;
	for( const auto& value : objects )
	{
		const auto object = value.toObject();
		if( object.value( QStringLiteral("Type") ).toInt() == static_cast<int>( NetworkObject::Type::Host ) )
		{
			knownIps.insert( object.value( QStringLiteral("HostAddress") ).toString() );
		}
	}
	if( knownIps.contains( sanitizedIp ) ) { return; }

	static const auto locationName = QStringLiteral("自动发现");
	QUuid locationUid;
	for( const auto& value : objects )
	{
		const auto object = value.toObject();
		if( object.value( QStringLiteral("Type") ).toInt() == static_cast<int>( NetworkObject::Type::Location ) &&
			object.value( QStringLiteral("Name") ).toString() == locationName )
		{
			locationUid = QUuid( object.value( QStringLiteral("Uid") ).toString() );
		}
	}
	if( locationUid.isNull() )
	{
		locationUid = QUuid::createUuid();
		objects.append( NetworkObject( NetworkObject::Type::Location, locationName, {}, {}, {},
									   locationUid, QUuid() ).toJson() );
	}

	const auto displayName = hostName.isEmpty() ? sanitizedIp : hostName;
	objects.append( NetworkObject( NetworkObject::Type::Host, displayName, sanitizedIp, {}, {},
								   QUuid::createUuid(), locationUid ).toJson() );
	configuration.setValue( QStringLiteral("NetworkObjects"), objects, QStringLiteral("BuiltinDirectory") );
	configuration.flushStore();
	if( auto* directory = IkebackCore::networkObjectDirectoryManager().configuredDirectory() )
	{
		directory->update();
	}
	vDebug() << "ikeback: auto-discovered student machine" << displayName << sanitizedIp;
}


QString ClassroomPlugin::dataDirectory() const
{
	return IkebackCore::filesystem().expandPath( m_configuration.dataDirectory() );
}


ClassroomRoster* ClassroomPlugin::roster()
{
	if( m_roster == nullptr ) { m_roster.reset( new ClassroomRoster( dataDirectory() ) ); }
	return m_roster.get();
}


ClassroomLogStore* ClassroomPlugin::store()
{
	if( m_store == nullptr ) { m_store.reset( new ClassroomLogStore( dataDirectory(), QStringLiteral("teacher") ) ); }
	return m_store.get();
}


ClassroomSettings* ClassroomPlugin::settings()
{
	if( m_settings == nullptr ) { m_settings.reset( new ClassroomSettings( dataDirectory(), m_configuration ) ); }
	return m_settings.get();
}


const FeatureList& ClassroomPlugin::featureList() const
{
	return m_features;
}


ConfigurationPage* ClassroomPlugin::createConfigurationPage()
{
	return new ClassroomConfigurationPage( m_configuration );
}


bool ClassroomPlugin::controlFeature( Feature::Uid featureUid, Operation operation, const QVariantMap& arguments,
									  const ComputerControlInterfaceList& computerControlInterfaces )
{
	Q_UNUSED(arguments)
	if( featureUid != clientFeatureUid() ) { return false; }

	auto message = Classroom::message( operation == Operation::Stop ? Command::StopClient : Command::StartClient );
	if( operation != Operation::Stop )
	{
		message.addArgument( Argument::SettingsJson,
							 QString::fromUtf8( QJsonDocument( ClassroomSettings::forStudents(
								 ClassroomSettings::fromConfiguration( m_configuration ) ) ).
												toJson( QJsonDocument::Compact ) ) );
		// The lesson key is generated into settings.json (DPAPI protected), not
		// into the registry-backed Ikeback configuration. Check both so a student
		// card still knows a lesson is active after a fresh console restart.
		message.addArgument( Argument::KeyConfigured,
							 m_configuration.sessionKey().isEmpty() == false ||
							 settings()->object().value( QStringLiteral("sessionKey") ).toString().isEmpty() == false );
	}
	sendFeatureMessage( message, computerControlInterfaces );
	return true;
}


bool ClassroomPlugin::startFeature( IkebackMasterInterface& master, const Feature& feature,
									const ComputerControlInterfaceList& computerControlInterfaces )
{
	// ---------------------------------------------------------------- sign-in
	if( feature.uid() == signInFeatureUid() )
	{
		const auto classNames = roster()->classNames();
		const auto currentClass = settings()->object().value( QStringLiteral("className") ).toString();
		ClassroomSignInDialog dialog( classNames, currentClass, roster()->toJsonArray(), master.mainWindow() );
		if( dialog.exec() != QDialog::Accepted ) { return true; }

		auto message = Classroom::message( Command::SetIdentity );
		message.addArgument( Argument::ClassName, dialog.className() );
		message.addArgument( Argument::Seat, dialog.seat() );
		message.addArgument( Argument::Student, dialog.studentName() );
		message.addArgument( Argument::SessionId,
							 QDateTime::currentDateTime().toString( QStringLiteral("yyyyMMdd-HHmmss") ) );
		sendFeatureMessage( message, computerControlInterfaces );

		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.className = dialog.className();
		event.seat = dialog.seat();
		event.student = dialog.studentName();
		event.type = EventType::assign();
		event.detail = QStringLiteral("signed in by name and seat");
		store()->append( event );
		return true;
	}

	// --------------------------------------------------------------- sign-out
	if( feature.uid() == signOutFeatureUid() )
	{
		auto message = Classroom::message( Command::ForceLogout );
		message.addArgument( Argument::Reason, tr( "signed out by teacher" ) );
		sendFeatureMessage( message, computerControlInterfaces );

		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.type = EventType::logout();
		event.detail = tr( "signed out by teacher" );
		store()->append( event );
		return true;
	}

	// --------------------------------------------------------------- end lesson
	if( feature.uid() == endLessonFeatureUid() )
	{
		if( m_dialog ) { m_dialog->endLesson(); }
		master.setClassroomMode( false );
		return true;
	}

	// ------------------------------------------- first-class quick action buttons
	for( int index = 0; index < m_quickActionIds.size(); ++index )
	{
		if( feature.uid() != quickActionUid( index ) ) { continue; }
		// The parameter widgets (silence text, blocked apps, ...) live in the
		// console, so make sure it exists - but HIDDEN: a one-click action must
		// not pop up the whole console window (5.2.2 UX fix).
		if( ensureConsoleDialog( master, computerControlInterfaces, false ) == nullptr ) { return true; }
		m_dialog->runQuickTool( m_quickActionIds.at( index ), computerControlInterfaces );
		return true;
	}

	// ------------------------------------------- operation log (5.2.2)
	if( feature.uid() == operationLogFeatureUid() )
	{
		if( ensureConsoleDialog( master, computerControlInterfaces, false ) == nullptr ) { return true; }
		m_dialog->openOperationLog();
		return true;
	}

	// ------------------------------------------- tool bar quick tools / check-in
	if( feature.uid() == quickToolsFeatureUid() || feature.uid() == checkInFeatureUid() )
	{
		if( ensureConsoleDialog( master, computerControlInterfaces,
								 feature.uid() == quickToolsFeatureUid() ) == nullptr ) { return true; }

		if( feature.uid() == checkInFeatureUid() )
		{
			m_dialog->startCheckInFromToolBar();
			return true;
		}

		// 5.2.2: "常用工具" opens the console on the Quick tools tab instead of
		// the old nested drop-down menu which was impossible to operate.
		m_dialog->showQuickToolsTab();
		return true;
	}

	if( feature.uid() != consoleFeatureUid() ) { return false; }

	if( ensureConsoleDialog( master, computerControlInterfaces, false ) == nullptr ) { return true; }
	m_dialog->beginLesson();
	master.setClassroomMode( true );
	return true;
}


ClassroomMasterDialog* ClassroomPlugin::ensureConsoleDialog( IkebackMasterInterface& master,
															const ComputerControlInterfaceList& computerControlInterfaces,
															bool show )
{
	// The console is embedded in the Master's central stack. Quick actions may
	// create it without switching the visible page; starting a lesson does.
	if( m_dialog.isNull() )
	{
		m_dialog = new ClassroomMasterDialog( master, m_configuration, *roster(), *store(), *settings(),
											  master.mainWindow() );
		m_dialog->setWindowTitle( Classroom::productName() );
		m_dialog->restoreProtectionStates( m_studentProtectionByAddress );
		master.setClassroomPage( m_dialog.data() );
		connect( m_dialog.data(), &ClassroomMasterDialog::hideRequested, m_dialog.data(), [this]() {
			if( m_dialog ) { m_dialog->hide(); }
		} );
	}

	if( show )
	{
		master.setClassroomMode( true );
		m_dialog->show();
		m_dialog->raise();

		// One message, one session-worker wake-up. The old three-message burst
		// re-armed the watchdog and repeatedly called startUnmanagedSessionWorker()
		// while the previous worker was still being torn down (5s kill timer),
		// which is what produced a dozen ikeback-worker.exe processes.
		auto stateMessage = Classroom::message( Command::ApplySession );
		stateMessage.addArgument( Argument::SettingsJson,
								  QString::fromUtf8( QJsonDocument( ClassroomSettings::forStudents( settings()->object() ) ).
													 toJson( QJsonDocument::Compact ) ) );
		stateMessage.addArgument( Argument::RosterJson, roster()->toJson() );
		stateMessage.addArgument( Argument::KeyConfigured,
								  settings()->object().value( QStringLiteral("sessionKey") ).toString().isEmpty() == false );
		stateMessage.addArgument( Argument::SessionId,
								  QDateTime::currentDateTime().toString( QStringLiteral("yyyyMMdd-HHmmss") ) );
		sendFeatureMessage( stateMessage, computerControlInterfaces );
	}

	return m_dialog.data();
}


void ClassroomPlugin::startClient( IkebackServerInterface& server )
{
	auto settingsObject = ClassroomSettings::forStudents(
		ClassroomSettings::fromConfiguration( m_configuration ) );
	settingsObject = withLocalStudentProtection( settingsObject );
	persistAdminKeyHash( settingsObject );

	auto message = Classroom::message( Command::StartClient );
	message.addArgument( Argument::SettingsJson,
						 QString::fromUtf8( QJsonDocument( settingsObject ).
											toJson( QJsonDocument::Compact ) ) );
	message.addArgument( Argument::KeyConfigured, m_configuration.sessionKey().isEmpty() == false );
	message.addArgument( Argument::SessionId,
						 QDateTime::currentDateTime().toString( QStringLiteral("yyyyMMdd-HHmmss") ) );
	server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );
}


void ClassroomPlugin::startSessionFeature( IkebackServerInterface& server )
{
	if( IkebackCore::component() != IkebackCore::Component::Server || m_configuration.enabled() == false )
	{
		return;
	}

	// Teacher machines run the service for their own monitoring but never show
	// the student card. The install-role marker is written by the installer.
	const auto role = machineRole();
	if( role == QStringLiteral("teacher") )
	{
		return;
	}

	m_server = &server;
	startStudentBeacon();
	startClient( server );

	m_watchdog = new QTimer( this );
	// 30s matches FeatureWorkerManager's authentication timeout. Polling every
	// 5s used to re-enter startClient() while the previous worker was still
	// authenticating, multiplying ikeback-worker.exe processes.
	m_watchdog->setInterval( 30000 );
	connect( m_watchdog, &QTimer::timeout, this, [this]() {
		if( m_stopAllowed || m_server == nullptr || m_configuration.enabled() == false ) { return; }
		if( m_server->featureWorkerManager().isWorkerRunning( clientFeatureUid() ) )
		{
			m_restartPending = false;
			m_restartFailures = 0;
			return;
		}
		// A worker that was asked to start but never registered is a failed
		// start, not one still in flight: give up waiting after three watchdog
		// periods so the retry below actually happens.
		if( m_restartPending )
		{
			if( m_restartPendingSince.isValid() && m_restartPendingSince.elapsed() < WorkerStartTimeoutMs ) { return; }
			m_restartPending = false;
		}

		if( m_restartFailures >= MaxWorkerRestartFailures )
		{
			// Stop the loop instead of spraying processes: a worker that never
			// authenticates is a broken install, not a transient failure, and
			// the next teacher message re-arms this watchdog.
			if( m_watchdog ) { m_watchdog->stop(); }
			vWarning() << "ikeback: student worker failed to start" << m_restartFailures
					   << "times - stopping automatic restarts until the next teacher action";
			return;
		}

		++m_restartFailures;
		startClient( *m_server );
		m_restartPending = true;
		m_restartPendingSince.restart();
	} );
	m_watchdog->start();
}


void ClassroomPlugin::registerContext( const MessageContext& context )
{
	if( context.ioDevice() == nullptr ) { return; }
	for( auto it = m_contexts.begin(); it != m_contexts.end(); )
	{
		if( it->ioDevice() == nullptr || it->ioDevice() == context.ioDevice() )
		{
			it = m_contexts.erase( it );
		}
		else
		{
			++it;
		}
	}
	m_contexts.prepend( context );
}


bool ClassroomPlugin::hasActiveContext() const
{
	for( const auto& context : m_contexts )
	{
		if( context.ioDevice() ) { return true; }
	}
	return false;
}


void ClassroomPlugin::sendAsyncFeatureMessages( IkebackServerInterface& server, const MessageContext& messageContext )
{
	registerContext( messageContext );
	for( const auto& pending : m_pendingEvents )
	{
		for( const auto& context : m_contexts )
		{
			if( context.ioDevice() ) { server.sendFeatureMessageReply( context, pending ); }
		}
	}
	m_pendingEvents.clear();
}


bool ClassroomPlugin::handleFeatureMessage( IkebackServerInterface& server, const MessageContext& messageContext,
											const FeatureMessage& message )
{
	if( message.featureUid() != clientFeatureUid() ) { return false; }

	registerContext( messageContext );
	if( m_pendingEvents.isEmpty() == false )
	{
		for( const auto& pending : m_pendingEvents )
		{
			for( const auto& context : m_contexts )
			{
				if( context.ioDevice() ) { server.sendFeatureMessageReply( context, pending ); }
			}
		}
		m_pendingEvents.clear();
	}

	const auto command = message.command<Command>();
	// Server-side (SYSTEM): persist the super-admin key digest so the
	// configurator can gate itself on student machines (Q6).
	if( command == Command::StartClient || command == Command::SetSettings || command == Command::ApplySession )
	{
		persistAdminKeyHash( QJsonDocument::fromJson(
			message.argument( Argument::SettingsJson ).toString().toUtf8() ).object() );
	}
#ifdef Q_OS_WIN
	if( command == Command::SetExitProtection )
	{
		QSettings machine( QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\ikeback\\Classroom"),
						   QSettings::Registry64Format );
		machine.setValue( QStringLiteral("ExitProtection"), message.argument( Argument::Enabled ).toBool() );
		machine.sync();
	}
	if( command == Command::SetUninstallProtection )
	{
		QSettings machine( QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\ikeback\\Student"),
						   QSettings::Registry64Format );
		const auto protect = message.argument( Argument::Enabled ).toBool();
		if( protect == false ||
			ClassroomSecrets::adminKeyHashConfigured( machine.value( QStringLiteral("AdminKeyHash") ).toString() ) )
		{
			machine.setValue( QStringLiteral("UninstallProtection"), protect );
			machine.sync();
		}
	}
#endif
	if( command == Command::StopClient )
	{
		m_stopAllowed = true;
		if( m_watchdog ) { m_watchdog->stop(); }
	}
	else
	{
		m_stopAllowed = false;
		// A teacher action re-arms the watchdog even after it gave up.
		m_restartFailures = 0;
		m_restartPending = false;
		if( m_watchdog && m_watchdog->isActive() == false ) { m_watchdog->start(); }
	}

	// StopClient may only be delivered to an EXISTING worker. Forwarding it
	// unconditionally used to start a brand new session worker just so that
	// worker could hide the card again.
	if( command != Command::StopClient ||
		server.featureWorkerManager().isWorkerRunning( clientFeatureUid() ) )
	{
		server.featureWorkerManager().sendMessageToUnmanagedSessionWorker( message );
	}
	return true;
}


bool ClassroomPlugin::handleFeatureMessageFromWorker( IkebackServerInterface& server, const FeatureMessage& message )
{
	if( message.featureUid() != clientFeatureUid() ) { return false; }

	switch( message.command<Command>() )
	{
	case Command::ReportEvent:
	case Command::ReportIdentity:
	case Command::InstallResult:
	case Command::DeployPackageResult:
	case Command::QueryScore:
	case Command::CallTeacher:
	case Command::CheckInReply:
	case Command::UwfStatusReport:
	case Command::ProtectionStateReport:
	case Command::AiPrompt:
	case Command::ShopQuery:
	case Command::ShopRedeem:
	case Command::ShopBid:
	case Command::ShopImageQuery:
		if( hasActiveContext() )
		{
			for( const auto& context : m_contexts )
			{
				if( context.ioDevice() ) { server.sendFeatureMessageReply( context, message ); }
			}
		}
		else
		{
			if( m_pendingEvents.size() >= 5000 ) { m_pendingEvents.removeFirst(); }
			m_pendingEvents.append( message );
		}
		return true;

	case Command::RequestQuit:
		m_stopAllowed = true;
		if( m_watchdog ) { m_watchdog->stop(); }
		QTimer::singleShot( 0, this, [&server]() {
			server.featureWorkerManager().stopWorker( clientFeatureUid() );
		} );
		return true;

	default:
		return false;
	}
}


static QString shopMachineAddress( const ComputerControlInterface::Pointer& controlInterface );

bool ClassroomPlugin::handleFeatureMessage( ComputerControlInterface::Pointer computerControlInterface,
											const FeatureMessage& message )
{
	if( message.featureUid() != clientFeatureUid() ) { return false; }

	const auto command = message.command<Command>();
	if( command == Command::ProtectionStateReport )
	{
		const auto enabled = message.argument( Argument::Enabled ).toBool();
		const auto ip = message.argument( Argument::Ip ).toString();
		const auto host = message.argument( Argument::Host ).toString();
		for( const auto& key : QStringList{ shopMachineAddress( computerControlInterface ), ip, host } )
		{
			if( key.trimmed().isEmpty() == false ) { m_studentProtectionByAddress.insert( key.trimmed(), enabled ); }
		}
		if( m_dialog ) { m_dialog->recordProtectionState( ip, enabled ); }
		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.ip = ip;
		event.host = host;
		event.type = EventType::protection();
		event.detail = enabled ? QStringLiteral("student protection enabled") : QStringLiteral("student protection disabled");
		store()->append( event );
		Q_EMIT eventReceived();
		return true;
	}
	if( command == Command::ShopQuery || command == Command::ShopRedeem ||
		command == Command::ShopBid || command == Command::ShopImageQuery )
	{
		handleShopRequest( message, computerControlInterface );
		return true;
	}
	if( command == Command::QueryScore )
	{
		const auto className = message.argument( Argument::ClassName ).toString();
		const auto seat = message.argument( Argument::Seat ).toString();
		const auto student = message.argument( Argument::Student ).toString();
		ClassroomScoreBook book( dataDirectory() );
		book.load( className );
		auto reply = Classroom::message( Command::ScoreReply );
		reply.addArgument( Argument::RichText, book.studentHtml( seat, student ) );
		if( computerControlInterface ) { computerControlInterface->sendFeatureMessage( reply ); }
		return true;
	}

	if( command == Command::AiPrompt )
	{
		// The teacher machine owns the model endpoint and API key. Students only
		// send a prompt + short history over the existing Ikeback feature channel.
		const auto payload = QJsonDocument::fromJson(
			message.argument( Argument::AiPayload ).toString().toUtf8() ).object();
		QVariantList history;
		const auto items = payload.value( QStringLiteral("history") ).toArray();
		for( int index = 0; index < qMin( 20, static_cast<int>( items.size() ) ); ++index )
		{
			const auto item = items.at( index ).toObject();
			QVariantMap entry;
			entry.insert( QStringLiteral("role"), item.value( QStringLiteral("role") ).toString() );
			entry.insert( QStringLiteral("content"), item.value( QStringLiteral("content") ).toString().left( 4000 ) );
			history.append( entry );
		}

		auto* client = new ClassroomAiClient( this );
		ClassroomAiClient::Settings fallback;
		client->setSettings( ClassroomAiClient::Settings::fromJson( settings()->object(), fallback ) );
		connect( client, &ClassroomAiClient::finished, this,
				 [this, client, computerControlInterface]( bool success, const QString& answer ) {
					 client->deleteLater();
					 auto reply = Classroom::message( Command::AiReply );
					 reply.addArgument( Argument::AiSuccess, success )
						  .addArgument( Argument::AiAnswer, answer.left( 8000 ) );
					 if( computerControlInterface ) { computerControlInterface->sendFeatureMessage( reply ); }
				 } );
		client->chat( history, payload.value( QStringLiteral("prompt") ).toString().left( 4000 ) );

		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.type = Classroom::EventType::ai();
		event.detail = QStringLiteral("AI prompt proxied by the teacher machine");
		event.ip = message.argument( Argument::Ip ).toString();
		event.host = message.argument( Argument::Host ).toString();
		store()->append( event );
		return true;
	}

	if( command == Command::DeployPackageResult )
	{
		const auto ok = message.argument( Argument::Result ).toString() == QStringLiteral("ok");
		const auto phase = message.argument( Argument::DeployPhase ).toString();
		const auto ip = message.argument( Argument::Ip ).toString();
		const auto detail = message.argument( Argument::Detail ).toString();
		if( m_dialog )
		{
			m_dialog->recordPackageResult( ip, phase, ok, detail,
										   message.argument( Argument::DeploymentId ).toString() );
		}
		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.ip = ip;
		event.host = message.argument( Argument::Host ).toString();
		event.type = EventType::install();
		event.detail = QStringLiteral("deployment %1 %2: %3" ).arg( phase, ok ? QStringLiteral("ok") : QStringLiteral("failed"), detail );
		store()->append( event );
		Q_EMIT eventReceived();
		return true;
	}

	if( command == Command::InstallResult )
	{
		const auto ok = message.argument( Argument::Result ).toString() == QStringLiteral("ok");
		if( m_dialog )
		{
			m_dialog->recordInstallResult( message.argument( Argument::Ip ).toString(),
										   ok, message.argument( Argument::Detail ).toString() );
		}
		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.className = message.argument( Argument::ClassName ).toString();
		event.seat = message.argument( Argument::Seat ).toString();
		event.student = message.argument( Argument::Student ).toString();
		event.ip = message.argument( Argument::Ip ).toString();
		event.host = message.argument( Argument::Host ).toString();
		event.type = EventType::install();
		event.detail = QStringLiteral("install result %1: %2 (%3)")
						   .arg( ok ? QStringLiteral("ok") : QStringLiteral("failed"),
								 message.argument( Argument::SoftwarePath ).toString(),
								 message.argument( Argument::Detail ).toString() );
		store()->append( event );
		Q_EMIT eventReceived();
		return true;
	}

	// 学生自助签到后主动上报身份，教师端名单/座位表立即同步。
	if( command == Command::ReportIdentity )
	{
		// FeatureMessage 的参数是 QVariant（没有默认值重载），必须直接 toBool()。
		const auto loggedIn = message.argument( Argument::Enabled ).toBool();
		const auto className = message.argument( Argument::ClassName ).toString().trimmed();
		const auto seat = message.argument( Argument::Seat ).toString().trimmed();
		const auto student = message.argument( Argument::Student ).toString().trimmed();
		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.className = className;
		event.seat = seat;
		event.student = student;
		event.ip = message.argument( Argument::Ip ).toString();
		event.host = message.argument( Argument::Host ).toString();
		event.type = loggedIn ? EventType::assign() : EventType::logout();
		event.detail = loggedIn ? QStringLiteral("student self signed in")
								: QStringLiteral("student signed out");
		const auto display = loggedIn ?
			QStringLiteral("座位 %1 %2").arg( event.seat, event.student ).trimmed() : QString{};
		const auto identity = QVector<QString>{ className, seat, student };
		for( const auto& key : QStringList{ shopMachineAddress( computerControlInterface ), event.ip, event.host } )
		{
			if( key.trimmed().isEmpty() ) { continue; }
			if( loggedIn ) { m_studentDisplayByIp.insert( key.trimmed(), display ); }
			else { m_studentDisplayByIp.remove( key.trimmed() ); }
			if( loggedIn ) { m_studentIdentityByAddress.insert( key.trimmed(), identity ); }
			else { m_studentIdentityByAddress.remove( key.trimmed() ); }
		}
		store()->append( event );
		if( m_dialog )
		{
			m_dialog->recordStudentIdentity( event.className, event.seat, event.student, loggedIn );
		}
		Q_EMIT eventReceived();
		return true;
	}

	if( command == Command::CheckInReply )
	{
		const auto seat = message.argument( Argument::Seat ).toString();
		const auto typedName = message.argument( Argument::CheckInName ).toString();
		const auto className = message.argument( Argument::ClassName ).toString();

		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.className = className;
		event.seat = seat;
		event.student = message.argument( Argument::Student ).toString();
		event.ip = message.argument( Argument::Ip ).toString();
		event.host = message.argument( Argument::Host ).toString();
		event.type = EventType::checkIn();
		event.detail = typedName;
		store()->append( event );
		if( m_dialog ) { m_dialog->recordCheckIn( className, seat, typedName ); }
		Q_EMIT eventReceived();
		return true;
	}

	if( command == Command::CallTeacher )
	{
		ClassroomEvent event;
		event.timestamp = QDateTime::currentDateTime();
		event.className = message.argument( Argument::ClassName ).toString();
		event.seat = message.argument( Argument::Seat ).toString();
		event.student = message.argument( Argument::Student ).toString();
		event.ip = message.argument( Argument::Ip ).toString();
		event.host = message.argument( Argument::Host ).toString();
		event.type = EventType::call();
		event.detail = message.argument( Argument::Message ).toString();
		store()->append( event );
		if( m_dialog )
		{
			m_dialog->showCall( event.ip, event.seat, event.student );
		}
		Q_EMIT eventReceived();
		return true;
	}

	if( command != Command::ReportEvent ) { return false; }

	ClassroomEvent event;
	event.timestamp = QDateTime::fromString( message.argument( Argument::Timestamp ).toString(), Qt::ISODateWithMs );
	if( event.timestamp.isValid() == false ) { event.timestamp = QDateTime::currentDateTime(); }
	event.sessionId = message.argument( Argument::SessionId ).toString();
	event.className = message.argument( Argument::ClassName ).toString();
	event.seat = message.argument( Argument::Seat ).toString();
	event.student = message.argument( Argument::Student ).toString();
	event.type = message.argument( Argument::EventType ).toString();
	event.detail = message.argument( Argument::Detail ).toString();
	event.app = message.argument( Argument::App ).toString();
	event.host = message.argument( Argument::Host ).toString();
	event.ip = message.argument( Argument::Ip ).toString();

	if( computerControlInterface )
	{
		const auto host = computerControlInterface->computer().hostName();
		event.host = host.isEmpty() ? computerControlInterface->computer().displayName() : host;
		event.ip = host.isEmpty() ? computerControlInterface->computer().hostAddress().toString() : host;
	}

	store()->append( event );
	Q_EMIT eventReceived();
	return true;
}


//! Address of the student machine behind a control interface - the same
//! identifier the console uses to match roster entries.
static QString shopMachineAddress( const ComputerControlInterface::Pointer& controlInterface )
{
	if( controlInterface.isNull() ) { return {}; }
	const auto address = controlInterface->computer().hostAddress().toString().trimmed();
	return address.isEmpty() ? controlInterface->computer().hostName().trimmed() : address;
}


//! The shop acts as the student signed in on that machine, exactly like score
//! queries and teacher calls: the seat/name pair must exist in the roster of
//! the class. Unknown identities never see a balance and can neither redeem
//! nor bid.
static bool shopIdentityKnown( ClassroomRoster& roster, const QString& className,
							   const QString& seat, const QString& student )
{
	if( seat.isEmpty() || student.isEmpty() ) { return false; }
	const auto* rosterStudent = roster.findStudent( className, seat );
	return rosterStudent != nullptr && rosterStudent->name == student;
}


//! Master-side shop request handler (runs in the ikeback Master process).
//! Everything is validated against the teacher machine's own books; student
//! machines never write shop data.
void ClassroomPlugin::handleShopRequest( const FeatureMessage& message,
										 const ComputerControlInterface::Pointer& computerControlInterface )
{
	const auto command = message.command<Command>();
	const auto className = message.argument( Argument::ClassName ).toString().trimmed();
	const auto seat = message.argument( Argument::Seat ).toString().trimmed();
	const auto student = message.argument( Argument::Student ).toString().trimmed();
	const auto machine = computerControlInterface ?
		computerControlInterface->computer().displayName() : message.argument( Argument::Host ).toString();

	// The catalog is always that of the class the signed-in student belongs
	// to; an unknown or empty class simply has an empty shop.
	ClassroomShopBook book( dataDirectory() );
	if( book.load( className ) == false )
	{
		const auto action = command == Command::ShopBid ? Classroom::ShopAction::bid() :
			( ( command == Command::ShopQuery || command == Command::ShopImageQuery ) ?
			  Classroom::ShopAction::query() : Classroom::ShopAction::redeem() );
		auto reply = Classroom::message( Command::ShopReply );
		reply.addArgument( Argument::ShopAction, action )
			 .addArgument( Argument::ClassName, className )
			 .addArgument( Argument::Result, Classroom::ShopAction::failed() )
			 .addArgument( Argument::Message, tr( "商城数据加载失败，请联系老师检查数据文件。" ) )
			 .addArgument( Argument::TotalScore, -1 );
		if( computerControlInterface ) { computerControlInterface->sendFeatureMessage( reply ); }
		return;
	}

	// Identity follows the same rule as the floating card's sign-in: seat and
	// name must be a student of that class in the roster. Without a valid
	// identity a student may look, but never sees a balance, redeem or bid.
	bool identityOk = shopIdentityKnown( *roster(), className, seat, student );
	if( identityOk && computerControlInterface )
	{
		const auto reported = m_studentIdentityByAddress.value( shopMachineAddress( computerControlInterface ) );
		identityOk = reported.size() == 3 && reported.at( 0 ) == className &&
					 reported.at( 1 ) == seat && reported.at( 2 ) == student;
	}
	else
	{
		identityOk = false;
	}

	const auto send = [&computerControlInterface]( const FeatureMessage& reply ) {
		if( computerControlInterface ) { computerControlInterface->sendFeatureMessage( reply ); }
	};
	const auto catalogText = [&book]() {
		return QJsonDocument( book.catalogJson() ).toJson( QJsonDocument::Compact );
	};

	// --------------------------------------------------------- product image
	if( command == Command::ShopImageQuery )
	{
		const auto productId = message.argument( Argument::ProductId ).toString();
		const auto imageId = message.argument( Argument::ShopImageId ).toString();
		const auto* product = book.product( productId );
		const bool imageOk = identityOk && product != nullptr && product->imageId == imageId &&
							 product->image.isEmpty() == false;
		auto reply = Classroom::message( Command::ShopImageReply );
		reply.addArgument( Argument::ClassName, className )
			 .addArgument( Argument::ProductId, productId )
			 .addArgument( Argument::ShopImageId, imageId )
			 .addArgument( Argument::Result, imageOk ? Classroom::ShopAction::ok()
													: Classroom::ShopAction::failed() )
			 .addArgument( Argument::Message, imageOk ? QString{} : tr( "商品图片不存在或身份已失效。" ) )
			 .addArgument( Argument::ShopImageData, imageOk ? product->image : QByteArray{} );
		send( reply );
		return;
	}

	// ------------------------------------------------------------ shop query
	if( command == Command::ShopQuery )
	{
		auto reply = Classroom::message( Command::ShopState );
		reply.addArgument( Argument::ClassName, className )
			 .addArgument( Argument::ShopJson, identityOk ? catalogText() : QByteArrayLiteral("{}") )
			 .addArgument( Argument::TotalScore, identityOk ? book.balance( seat ) : -1 );
		send( reply );
		return;
	}

	// -------------------------------------------------------- redeem / bid
	QString errorText;
	auto action = Classroom::ShopAction::redeem();
	QString item;
	if( identityOk == false )
	{
		errorText = tr( "请先用座位号和姓名签到，再使用班级商场。" );
	}
	else if( command == Command::ShopRedeem )
	{
		const auto productId = message.argument( Argument::ProductId ).toString();
		if( const auto* product = book.product( productId ) ) { item = product->name; }
		errorText = book.redeem( productId, seat, student, book.balance( seat ),
			message.argument( Argument::OperationId ).toString() );
	}
	else if( command == Command::ShopBid )
	{
		action = Classroom::ShopAction::bid();
		const auto auctionId = message.argument( Argument::AuctionId ).toString();
		bool amountOk = false;
		const auto amount = message.argument( Argument::Amount ).toInt( &amountOk );
		if( const auto* auction = book.auction( auctionId ) ) { item = auction->name; }
		if( amountOk == false )
		{
			errorText = tr( "出价无效。" );
		}
		else
		{
			errorText = book.placeBid( auctionId, amount, seat, student, book.balance( seat ) );
			if( errorText.isEmpty() ) { item = QStringLiteral("%1（%2 分）").arg( item ).arg( amount ); }
		}
	}
	else
	{
		return;
	}

	const auto ok = errorText.isEmpty();
	auto reply = Classroom::message( Command::ShopReply );
	reply.addArgument( Argument::ShopAction, action )
		 .addArgument( Argument::ClassName, className )
		 .addArgument( Argument::Result, ok ? Classroom::ShopAction::ok()
											: Classroom::ShopAction::failed() )
		 .addArgument( Argument::Message,
					   ok ? ( action == Classroom::ShopAction::bid() ?
								  tr( "出价成功，结束时价高者得。" ) :
								  tr( "兑换成功，请等待老师发货。" ) )
						  : errorText )
		 .addArgument( Argument::TotalScore, identityOk ? book.balance( seat ) : -1 )
		 // The catalog is built AFTER the operation, so the student at once
		 // sees the new stock / leading bid.
		 .addArgument( Argument::ShopJson, identityOk ? catalogText() : QByteArrayLiteral("{}") );
	send( reply );

	// Audit trail for every attempt - failed ones show who tried what.
	ClassroomEvent event;
	event.timestamp = QDateTime::currentDateTime();
	event.className = className;
	event.seat = seat;
	event.student = student;
	event.host = machine;
	event.ip = shopMachineAddress( computerControlInterface );
	event.type = action == Classroom::ShopAction::bid() ? EventType::auction() : EventType::shop();
	event.detail = ok ?
		QStringLiteral( "%1 %2" ).arg( action == Classroom::ShopAction::bid() ?
			QStringLiteral( "bid on" ) : QStringLiteral( "redeemed" ), item ) :
		QStringLiteral( "rejected %1: %2" ).arg( item, errorText );
	store()->append( event );

	if( ok )
	{
		// Everyone else's catalog changed (stock, leading bid, bid list).
		broadcastShopState( className );
		if( m_dialog )
		{
			m_dialog->refreshShopPanel();
			if( action == Classroom::ShopAction::redeem() ) { m_dialog->refreshScoresAfterShop(); }
		}
	}
	Q_EMIT eventReceived();
}


void ClassroomPlugin::broadcastShopState( const QString& className )
{
	// The console owns the list of all student control interfaces. It is
	// created on the first console open and then keeps living (hidden), which
	// is fine: a shop without products cannot exist before that anyway.
	if( m_dialog ) { m_dialog->broadcastShopState( className ); }
}


void ClassroomPlugin::ensureClientUi( IkebackWorkerInterface& worker )
{
	if( m_clientWidget ) { return; }
	// The floating card is student-only (Q2): teacher installs carry the
	// "teacher" role marker written by the installer.
	if( machineRole() == QStringLiteral("teacher") ) { return; }

	m_worker = &worker;
	m_clientWidget = new ClassroomClientWidget;
	m_monitor = new ClassroomMonitor( m_clientWidget );
	ensureTrayIcon();

	connect( m_clientWidget, &ClassroomClientWidget::eventOccurred, this,
			 [this]( const QString& type, const QString& detail, const QString& app ) {
				 reportEvent( type, detail, app );
			 } );
	connect( m_clientWidget, &ClassroomClientWidget::identityChanged, this,
			 [this]( const QString& seat, const QString& student, bool loggedIn ) {
				 m_seat = seat;
				 m_student = student;
				 if( loggedIn == false ) { m_sessionId.clear(); }
				 updateTrayTooltip();
				 // 教师端直接看到学生自助签到结果。
				 if( m_worker == nullptr ) { return; }
				 auto report = Classroom::message( Command::ReportIdentity );
				 report.addArgument( Argument::ClassName, m_className )
					   .addArgument( Argument::Seat, seat )
					   .addArgument( Argument::Student, student )
					   .addArgument( Argument::Enabled, loggedIn )
					   .addArgument( Argument::Host, m_hostName )
					   .addArgument( Argument::Ip, m_ipAddress );
				 m_worker->sendFeatureMessageReply( report );
			 } );
	connect( m_clientWidget, &ClassroomClientWidget::quitAuthorized, this, [this]() {
		if( m_worker ) { m_worker->sendFeatureMessageReply( Classroom::message( Command::RequestQuit ) ); }
	} );
	connect( m_clientWidget, &ClassroomClientWidget::scoreRequested, this, [this]() {
		if( m_worker == nullptr ) { return; }
		auto query = Classroom::message( Command::QueryScore );
		query.addArgument( Argument::ClassName, m_className )
			 .addArgument( Argument::Seat, m_seat )
			 .addArgument( Argument::Student, m_student )
			 .addArgument( Argument::Host, m_hostName )
			 .addArgument( Argument::Ip, m_ipAddress );
		m_worker->sendFeatureMessageReply( query );
	} );
	connect( m_clientWidget, &ClassroomClientWidget::shopRequested, this, [this]() {
		if( m_worker == nullptr ) { return; }
		auto query = Classroom::message( Command::ShopQuery );
		query.addArgument( Argument::ClassName, m_className )
			 .addArgument( Argument::Seat, m_seat )
			 .addArgument( Argument::Student, m_student )
			 .addArgument( Argument::Host, m_hostName )
			 .addArgument( Argument::Ip, m_ipAddress );
		m_worker->sendFeatureMessageReply( query );
	} );
	connect( m_clientWidget, &ClassroomClientWidget::shopRedeemRequested, this,
			 [this]( const QString& productId ) {
				 if( m_worker == nullptr ) { return; }
				 auto redeem = Classroom::message( Command::ShopRedeem );
				 redeem.addArgument( Argument::ClassName, m_className )
					   .addArgument( Argument::Seat, m_seat )
					   .addArgument( Argument::Student, m_student )
					   .addArgument( Argument::Host, m_hostName )
					   .addArgument( Argument::Ip, m_ipAddress )
					   .addArgument( Argument::ProductId, productId )
					   .addArgument( Argument::OperationId, QUuid::createUuid().toString( QUuid::WithoutBraces ) );
				 m_worker->sendFeatureMessageReply( redeem );
			 } );
	connect( m_clientWidget, &ClassroomClientWidget::shopBidRequested, this,
			 [this]( const QString& auctionId, int amount ) {
				 if( m_worker == nullptr ) { return; }
				 auto bid = Classroom::message( Command::ShopBid );
				 bid.addArgument( Argument::ClassName, m_className )
					.addArgument( Argument::Seat, m_seat )
					.addArgument( Argument::Student, m_student )
					.addArgument( Argument::Host, m_hostName )
					.addArgument( Argument::Ip, m_ipAddress )
					.addArgument( Argument::AuctionId, auctionId )
					.addArgument( Argument::Amount, amount );
				 m_worker->sendFeatureMessageReply( bid );
			 } );
	connect( m_clientWidget, &ClassroomClientWidget::shopImageRequested, this,
			 [this]( const QString& productId, const QString& imageId ) {
				 if( m_worker == nullptr ) { return; }
				 auto query = Classroom::message( Command::ShopImageQuery );
				 query.addArgument( Argument::ClassName, m_className )
					  .addArgument( Argument::Seat, m_seat )
					  .addArgument( Argument::Student, m_student )
					  .addArgument( Argument::Host, m_hostName )
					  .addArgument( Argument::Ip, m_ipAddress )
					  .addArgument( Argument::ProductId, productId )
					  .addArgument( Argument::ShopImageId, imageId );
				 m_worker->sendFeatureMessageReply( query );
			 } );
	connect( m_clientWidget, &ClassroomClientWidget::aiPromptRequested, this,
			 [this]( const QString& prompt, const QVariantList& history ) {
				 if( m_worker == nullptr ) { return; }
				 QJsonArray items;
				 for( const auto& value : history )
				 {
					 const auto entry = value.toMap();
					 items.append( QJsonObject{
						 { QStringLiteral("role"), entry.value( QStringLiteral("role") ).toString() },
						 { QStringLiteral("content"), entry.value( QStringLiteral("content") ).toString().left( 4000 ) } } );
				 }
				 auto request = Classroom::message( Command::AiPrompt );
				 request.addArgument( Argument::AiPayload,
									  QString::fromUtf8( QJsonDocument( QJsonObject{
										  { QStringLiteral("prompt"), prompt },
										  { QStringLiteral("history"), items } } ).toJson( QJsonDocument::Compact ) ) )
						.addArgument( Argument::Ip, m_ipAddress )
						.addArgument( Argument::Host, m_hostName );
				 m_worker->sendFeatureMessageReply( request );
			 } );
	connect( m_clientWidget, &ClassroomClientWidget::checkInSubmitted, this, [this]( const QString& name ) {
		if( m_worker == nullptr ) { return; }
		auto reply = Classroom::message( Command::CheckInReply );
		reply.addArgument( Argument::ClassName, m_className )
			.addArgument( Argument::Seat, m_seat )
			.addArgument( Argument::Student, m_student )
			.addArgument( Argument::CheckInName, name )
			.addArgument( Argument::Host, m_hostName )
			.addArgument( Argument::Ip, m_ipAddress );
		m_worker->sendFeatureMessageReply( reply );
		reportEvent( EventType::checkIn(), name, {} );
	} );
	connect( m_clientWidget, &ClassroomClientWidget::callTeacherRequested, this, [this]() {
		if( m_worker == nullptr ) { return; }
		auto call = Classroom::message( Command::CallTeacher );
		call.addArgument( Argument::ClassName, m_className )
			.addArgument( Argument::Seat, m_seat )
			.addArgument( Argument::Student, m_student )
			.addArgument( Argument::Host, m_hostName )
			.addArgument( Argument::Ip, m_ipAddress )
			.addArgument( Argument::Message, tr( "Student is calling the teacher" ) );
		m_worker->sendFeatureMessageReply( call );
	} );
	connect( m_monitor, &ClassroomMonitor::activity, this,
			 [this]( const QString& app, const QString& title, bool browser ) {
				 reportEvent( browser ? EventType::web() : EventType::window(),
							  title.isEmpty() ? app : title, app );
			 } );
	connect( m_monitor, &ClassroomMonitor::blockedActivity, this,
			 [this]( const QString& app, const QString& title, const QString& rule, bool browser ) {
				 reportEvent( browser ? EventType::webBlock() : EventType::appBlock(),
							  QStringLiteral("fuzzy rule '%1' matched: %2").arg( rule, title ), app );
#ifdef Q_OS_WIN
				 // 应用规则命中才终止进程；网址规则已在学生端关闭当前标签页，
				 // 不再 taskkill 整个浏览器，避免误杀其他标签页。
				 if( browser == false && app.size() >= 2 && rule.size() >= 2 )
				 {
					 const auto image = app.contains( QLatin1Char('.') ) ? app : app + QStringLiteral(".exe");
					 QProcess::startDetached( QStringLiteral("taskkill"),
											 QStringList{ QStringLiteral("/F"), QStringLiteral("/IM"), image } );
				 }
#endif
			 } );
}


static QJsonObject withLocalStudentProtection( QJsonObject settingsObject )
{
#ifdef Q_OS_WIN
	QSettings user( QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\ikeback\\Classroom"),
					QSettings::Registry64Format );
	if( user.contains( QStringLiteral("ExitProtection") ) )
	{
		settingsObject.insert( Classroom::SettingKey::exitProtection(),
							   user.value( QStringLiteral("ExitProtection") ).toBool() );
	}
	if( user.contains( QStringLiteral("ExitAdminUsername") ) )
	{
		settingsObject.insert( Classroom::SettingKey::exitAdminUsername(),
							   user.value( QStringLiteral("ExitAdminUsername") ).toString() );
	}
	QSettings machine( QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\ikeback\\Student"),
					   QSettings::Registry64Format );
	const auto storedHash = machine.value( QStringLiteral("AdminKeyHash") ).toString();
	if( ClassroomSecrets::adminKeyHashConfigured( storedHash ) )
	{
		settingsObject.insert( Classroom::SettingKey::adminKeyHash(), storedHash );
	}
#else
	Q_UNUSED(settingsObject)
#endif
	return settingsObject;
}


//! Mirror the super-admin key digest into HKLM so the configurator and the
//! cooperative uninstaller can verify it. Never erase a working digest merely
//! because a student-side settings snapshot does not contain the teacher key.
static void persistAdminKeyHash( const QJsonObject& settingsObject )
{
#ifdef Q_OS_WIN
	QSettings machine( QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\ikeback\\Student"),
					   QSettings::Registry64Format );
	auto hash = settingsObject.value( Classroom::SettingKey::adminKeyHash() ).toString();
	if( hash.isEmpty() )
	{
		hash = machine.value( QStringLiteral("AdminKeyHash") ).toString();
	}
	if( ClassroomSecrets::adminKeyHashConfigured( hash ) )
	{
		machine.setValue( QStringLiteral("AdminKeyHash"), hash );
	}
	else
	{
		machine.remove( QStringLiteral("AdminKeyHash") );
		machine.setValue( QStringLiteral("UninstallProtection"), 0 );
	}
	machine.sync();
#else
	Q_UNUSED(settingsObject)
#endif
}


static void persistWorkerProtection( const QJsonObject& settingsObject )
{
#ifdef Q_OS_WIN
	QSettings user( QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\ikeback\\Classroom"),
					QSettings::Registry64Format );
	user.setValue( QStringLiteral("ExitProtection"),
				   settingsObject.value( Classroom::SettingKey::exitProtection() ).toBool( true ) );
	user.setValue( QStringLiteral("ExitAdminUsername" ),
				   settingsObject.value( Classroom::SettingKey::exitAdminUsername() ).toString() );
	user.sync();

	QSettings machine( QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\ikeback\\Classroom"),
					   QSettings::Registry64Format );
	machine.setValue( QStringLiteral("ExitProtection"),
					  settingsObject.value( Classroom::SettingKey::exitProtection() ).toBool( true ) );
	machine.setValue( QStringLiteral("ExitAdminUsername" ),
					  settingsObject.value( Classroom::SettingKey::exitAdminUsername() ).toString() );
	machine.sync();
	persistAdminKeyHash( settingsObject );
#else
	Q_UNUSED(settingsObject)
#endif
}


void ClassroomPlugin::applyWorkerSettings( const QJsonObject& settingsObject )
{
	m_workerSettings = settingsObject;
	persistWorkerProtection( settingsObject );
	if( m_clientWidget ) { m_clientWidget->setSettings( settingsObject ); }
	if( m_monitor ) { m_monitor->setSettings( settingsObject ); }
}


void ClassroomPlugin::reportEvent( const QString& type, const QString& detail, const QString& app )
{
	ClassroomEvent event;
	event.timestamp = QDateTime::currentDateTime();
	event.sessionId = m_sessionId;
	event.className = m_className;
	event.seat = m_seat;
	event.student = m_student;
	event.host = m_hostName;
	event.ip = m_ipAddress;
	event.type = type;
	event.detail = detail;
	event.app = app;

	auto message = Classroom::message( Command::ReportEvent );
	message.addArgument( Argument::Timestamp, event.timestamp.toString( Qt::ISODateWithMs ) )
		   .addArgument( Argument::SessionId, event.sessionId )
		   .addArgument( Argument::ClassName, event.className )
		   .addArgument( Argument::Seat, event.seat )
		   .addArgument( Argument::Student, event.student )
		   .addArgument( Argument::Host, event.host )
		   .addArgument( Argument::Ip, event.ip )
		   .addArgument( Argument::EventType, event.type )
		   .addArgument( Argument::Detail, event.detail )
		   .addArgument( Argument::App, event.app );

	if( m_worker ) { m_worker->sendFeatureMessageReply( message ); }
}


void ClassroomPlugin::sendPackageResult( const QString& phase, bool ok, const QString& detail )
{
	if( m_worker == nullptr ) { return; }
	auto reply = Classroom::message( Command::DeployPackageResult );
	reply.addArgument( Argument::DeploymentId, m_packageId )
		 .addArgument( Argument::DeployPhase, phase )
		 .addArgument( Argument::Result, ok ? QStringLiteral("ok") : QStringLiteral("failed") )
		 .addArgument( Argument::Detail, detail.left( 1000 ) )
		 .addArgument( Argument::Host, m_hostName )
		 .addArgument( Argument::Ip, m_ipAddress );
	m_worker->sendFeatureMessageReply( reply );
}


bool ClassroomPlugin::verifyDeployedPackage( const QJsonObject& state, QString* error ) const
{
	const auto manifest = state.value( QStringLiteral("manifest") ).toObject();
	const auto folder = manifest.value( QStringLiteral("folder") ).toString();
	if( safePackageFolder( folder ) == false ) { if( error ) { *error = QStringLiteral("invalid folder name"); } return false; }
	const auto root = QDir( portableAppsRoot() ).filePath( folder );
	const auto files = manifest.value( QStringLiteral("files") ).toArray();
	for( const auto& value : files )
	{
		const auto entry = value.toObject();
		const auto relative = entry.value( QStringLiteral("path") ).toString();
		if( safeRelativePath( relative ) == false ) { if( error ) { *error = QStringLiteral("invalid file path"); } return false; }
		const auto path = QDir( root ).filePath( relative );
		if( QFileInfo( path ).size() != entry.value( QStringLiteral("size") ).toVariant().toLongLong() ||
			fileSha256( path ) != entry.value( QStringLiteral("sha256") ).toString().toLatin1() )
		{
			if( error ) { *error = QStringLiteral("file verification failed: %1" ).arg( relative ); }
			return false;
		}
	}
	const auto shortcut = state.value( QStringLiteral("shortcutPath") ).toString();
	if( shortcut.isEmpty() || QFileInfo::exists( shortcut ) == false )
	{
		if( error ) { *error = QStringLiteral("desktop shortcut is missing"); }
		return false;
	}
	return true;
}


void ClassroomPlugin::resumePackageDeployment()
{
	if( m_worker == nullptr || ensureDeploymentStateDirectory() == false ) { return; }
	const auto stateDirectory = deploymentStateDirectory();
	const auto states = QDir( stateDirectory ).entryList( { QStringLiteral(".ikeback-deploy-*.json") }, QDir::Files );
	for( const auto& fileName : states )
	{
		QFile file( QDir( stateDirectory ).filePath( fileName ) );
		if( file.open( QIODevice::ReadOnly ) == false ) { continue; }
		const auto state = QJsonDocument::fromJson( file.readAll() ).object();
		if( state.value( QStringLiteral("phase") ).toString() != QStringLiteral("verify") ) { continue; }
		m_packageId = state.value( QStringLiteral("id") ).toString();
		QString error;
		const auto ok = verifyDeployedPackage( state, &error );
		const auto statePath = file.fileName();
		file.close();
		sendPackageResult( QStringLiteral("verify"), ok, ok ? QStringLiteral("verified after reboot") : error );
		if( ok ) { QFile::remove( statePath ); }
	}
	m_packageId.clear();
}


void ClassroomPlugin::resetPackageReceive()
{
	if( m_packageFile )
	{
		m_packageFile->close();
		delete m_packageFile;
		m_packageFile = nullptr;
	}
	m_packageRelativePath.clear();
	m_packageOffset = 0;
}


bool ClassroomPlugin::handleFeatureMessage( IkebackWorkerInterface& worker, const FeatureMessage& message )
{
	if( message.featureUid() != clientFeatureUid() ) { return false; }

	m_worker = &worker;
	ensureClientUi( worker );

	// Teacher-role machines have no floating card (Q2). Only the deployment
	// commands - which never touch the card - still run there.
	if( m_clientWidget == nullptr )
	{
		const auto commandWithoutUi = message.command<Command>();
		if( commandWithoutUi != Command::DeployFileBegin &&
			commandWithoutUi != Command::DeployFileChunk &&
			commandWithoutUi != Command::UwfInstall &&
			commandWithoutUi != Command::DeployPackageBegin &&
			commandWithoutUi != Command::DeployPackageChunk &&
			commandWithoutUi != Command::DeployPackageCommit )
		{
			return true;
		}
	}

	const auto settingsFromJson = [&message]() {
		return QJsonDocument::fromJson( message.argument( Argument::SettingsJson ).toString().toUtf8() ).object();
	};

	switch( message.command<Command>() )
	{
	case Command::StartClient:
	{
		applyWorkerSettings( settingsFromJson() );
		m_keyConfigured = message.argument( Argument::KeyConfigured ).toBool();
		m_sessionId = message.argument( Argument::SessionId ).toString();
		m_clientWidget->setKeyConfigured( m_keyConfigured );
		m_clientWidget->reactivate();
		IkebackCore::platform().coreFunctions().raiseWindow( m_clientWidget, false );
		resumePackageDeployment();
		auto protectionReply = Classroom::message( Command::ProtectionStateReport );
		protectionReply.addArgument( Argument::Enabled,
			m_workerSettings.value( SettingKey::exitProtection() ).toBool( true ) )
			.addArgument( Argument::Ip, m_ipAddress ).addArgument( Argument::Host, m_hostName );
		if( m_worker ) { m_worker->sendFeatureMessageReply( protectionReply ); }
		reportEvent( EventType::connection(), QStringLiteral("classroom client connected"), {} );
		break;
	}

	case Command::ApplySession:
		// Combined console-open state. Keeping it to one message avoids
		// repeatedly restarting the unmanaged session worker.
		applyWorkerSettings( settingsFromJson() );
		m_clientWidget->setRoster(
			QJsonDocument::fromJson( message.argument( Argument::RosterJson ).toString().toUtf8() ).object() );
		if( m_className.isEmpty() ) { m_className = m_clientWidget->className(); }
		m_keyConfigured = message.argument( Argument::KeyConfigured ).toBool();
		m_sessionId = message.argument( Argument::SessionId ).toString();
		m_clientWidget->setKeyConfigured( m_keyConfigured );
		m_clientWidget->reactivate();
		updateTrayTooltip();
		break;

	case Command::SetSettings:
		applyWorkerSettings( settingsFromJson() );
		break;

	case Command::SetRoster:
		m_clientWidget->setRoster(
			QJsonDocument::fromJson( message.argument( Argument::RosterJson ).toString().toUtf8() ).object() );
		if( m_className.isEmpty() ) { m_className = m_clientWidget->className(); }
		break;

	case Command::SetSessionKey:
		// Legacy command name, new payload: a heartbeat carrying only a boolean.
		m_keyConfigured = message.argument( Argument::KeyConfigured ).toBool();
		m_sessionId = message.argument( Argument::SessionId ).toString();
		m_clientWidget->setKeyConfigured( m_keyConfigured );
		break;

	case Command::SetIdentity:
		m_className = message.argument( Argument::ClassName ).toString();
		m_seat = message.argument( Argument::Seat ).toString();
		m_student = message.argument( Argument::Student ).toString();
		m_sessionId = message.argument( Argument::SessionId ).toString();
		m_clientWidget->setIdentity( m_className, m_seat, m_student );
		m_clientWidget->setLocked( true );
		reportEvent( EventType::assign(), QStringLiteral("signed in by teacher"), {} );
		break;

	case Command::SwitchStudent:
		m_seat = message.argument( Argument::NewSeat ).toString();
		m_student = message.argument( Argument::NewStudent ).toString();
		m_sessionId = message.argument( Argument::SessionId ).toString();
		m_clientWidget->setIdentity( m_className, m_seat, m_student );
		reportEvent( EventType::switchUser(), QStringLiteral("student switched by teacher"), {} );
		break;

	case Command::ClearIdentity:
	case Command::ForceLogout:
		m_clientWidget->clearIdentity();
		m_clientWidget->setLocked( false );
		m_clientWidget->setInputLocked( false );
		reportEvent( EventType::logout(), message.argument( Argument::Reason ).toString(), {} );
		m_seat.clear();
		m_student.clear();
		break;

	case Command::LockClient:
	case Command::UnlockClient:
	{
		const auto locked = message.command<Command>() == Command::LockClient;
		m_clientWidget->setLocked( locked );
		m_clientWidget->setInputLocked( locked );
		reportEvent( locked ? EventType::lock() : EventType::unlock(),
					 QStringLiteral("client lock changed by teacher"), {} );
		break;
	}

	case Command::ToggleAi:
	{
		auto object = m_workerSettings;
		object.insert( SettingKey::aiEnabled(), message.argument( Argument::Enabled ).toBool() );
		applyWorkerSettings( object );
		break;
	}

	case Command::StopClient:
		m_clientWidget->allowHide();
		if( m_monitor ) { m_monitor->stop(); }
		break;

	case Command::ScoreReply:
		m_clientWidget->setScoreHtml( message.argument( Argument::RichText ).toString() );
		break;

	case Command::AiReply:
		m_clientWidget->deliverAiReply( message.argument( Argument::AiSuccess ).toBool(),
										message.argument( Argument::AiAnswer ).toString() );
		break;

	// --------------------------------------------------- 5.3.0 shop & auction
	case Command::ShopState:
	{
		// 教师广播的目录属于教师当前所选班级；与本班不符时忽略。
		// 学生打开商场时的单独查询（ShopQuery）按学生自己的班级应答，不受影响。
		const auto stateClassName = message.argument( Argument::ClassName ).toString().trimmed();
		if( m_className.isEmpty() == false && stateClassName.isEmpty() == false &&
			stateClassName != m_className )
		{
			break;
		}
		const auto balanceValue = message.argument( Argument::TotalScore );
		const auto balance = balanceValue.isValid() ? balanceValue.toInt() : -1;
		m_clientWidget->applyShopState(
			QJsonDocument::fromJson( message.argument( Argument::ShopJson ).toByteArray() ).object(),
			balance );
		break;
	}

	case Command::ShopReply:
	{
		const auto replyClassName = message.argument( Argument::ClassName ).toString().trimmed();
		if( replyClassName != m_className ) { break; }
		const auto balanceValue = message.argument( Argument::TotalScore );
		const auto balance = balanceValue.isValid() ? balanceValue.toInt() : -1;
		m_clientWidget->shopReply(
			message.argument( Argument::Result ).toString() == Classroom::ShopAction::ok(),
			message.argument( Argument::Message ).toString(),
			balance,
			QJsonDocument::fromJson( message.argument( Argument::ShopJson ).toString().toUtf8() ).object() );
		break;
	}

		case Command::ShopImageReply:
		{
			const auto stateClassName = message.argument( Argument::ClassName ).toString().trimmed();
			if( stateClassName != m_className ) { break; }
			m_clientWidget->applyShopImage(
				message.argument( Argument::ProductId ).toString(),
				message.argument( Argument::ShopImageId ).toString(),
				message.argument( Argument::ShopImageData ).toByteArray() );
			break;
		}

	case Command::StartCheckIn:
		m_clientWidget->showCheckIn();
		break;

	case Command::SetUninstallProtection:
	{
		// The floating card runs in the interactive user's session. HKLM writes
		// therefore only succeed when that account has administrative rights;
		// the result is read back and reported instead of silently succeeding.
		const bool protect = message.argument( Argument::Enabled ).toBool();
		bool confirmed = false;
#ifdef Q_OS_WIN
		QSettings settings( QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\ikeback\\Student"),
							QSettings::Registry64Format );
		const auto canProtect = protect == false ||
				ClassroomSecrets::adminKeyHashConfigured( settings.value( QStringLiteral("AdminKeyHash") ).toString() );
		settings.setValue( QStringLiteral("UninstallProtection"), protect && canProtect ? 1 : 0 );
		settings.sync();
		const auto status = settings.status();
		const auto readBack = settings.value( QStringLiteral("UninstallProtection"), -1 ).toInt();
		confirmed = canProtect && status == QSettings::NoError && readBack == ( protect ? 1 : 0 );
		if( confirmed == false )
		{
			qWarning( "ikeback: SetUninstallProtection failed (status=%d, readback=%d, lastError=%s)",
					  static_cast<int>( status ), readBack, qPrintable( ClassroomSystem::lastError() ) );
		}
#endif
		reportEvent( EventType::protection(),
					 confirmed ? ( protect ? QStringLiteral("uninstall protection enabled by teacher")
										   : QStringLiteral("uninstall protection cleared by teacher") )
							   : QStringLiteral("uninstall protection FAILED: HKLM write not confirmed"), {} );
		auto reply = Classroom::message( Command::ProtectionStateReport );
		reply.addArgument( Argument::Enabled, protect )
			 .addArgument( Argument::Result, confirmed ? QStringLiteral("ok") : QStringLiteral("failed") );
		if( m_worker ) { m_worker->sendFeatureMessageReply( reply ); }
		break;
	}

	case Command::ShowSilencePage:
		m_clientWidget->setSilence( true, message.argument( Argument::Message ).toString() );
		break;

	case Command::HideSilencePage:
		m_clientWidget->setSilence( false, {} );
		break;

	case Command::SetMute:
		if( ClassroomSystem::setMute( message.argument( Argument::Enabled ).toBool() ) == false )
		{
			qWarning( "ikeback: SetMute failed: %s", qPrintable( ClassroomSystem::lastError() ) );
		}
		reportEvent( EventType::mute(), QStringLiteral("audio mute changed by teacher"), {} );
		break;

	case Command::SetInternetBlock:
	{
		// A second line of defence: even a compromised teacher console or a
		// forged feature message cannot turn remoteip= into a CIDR range.
		const auto teacherIp = Classroom::sanitizeUnicastIp( message.argument( Argument::Ip ).toString() );
		if( ClassroomSystem::setInternetBlock( message.argument( Argument::Enabled ).toBool(), teacherIp ) == false )
		{
			qWarning( "ikeback: SetInternetBlock failed: %s", qPrintable( ClassroomSystem::lastError() ) );
		}
		reportEvent( EventType::internet(), QStringLiteral("InternetGuard changed by teacher"), {} );
		break;
	}

	case Command::UwfQuery:
	{
		const auto state = ClassroomSystem::uwfState();
		reportEvent( EventType::uwf(), state.raw, {} );
		break;
	}

	case Command::UwfSetFilter:
		if( ClassroomSystem::setUwfFilter( message.argument( Argument::Enabled ).toBool() ) == false )
		{
			qWarning( "ikeback: UwfSetFilter failed: %s", qPrintable( ClassroomSystem::lastError() ) );
		}
		reportEvent( EventType::uwf(), QStringLiteral("UWF filter change queued (reboot required)"), {} );
		break;

	case Command::UwfRestart:
		ClassroomSystem::restartSystem();
		break;

	case Command::DeployPackageBegin:
	{
		resetPackageReceive();
		const auto id = message.argument( Argument::DeploymentId ).toString();
		const auto manifestBytes = message.argument( Argument::ManifestJson ).toByteArray();
		const auto manifest = QJsonDocument::fromJson( manifestBytes ).object();
		const auto folder = manifest.value( QStringLiteral("folder") ).toString();
		const auto shortcut = manifest.value( QStringLiteral("shortcut") ).toObject();
		const auto target = shortcut.value( QStringLiteral("target") ).toString();
		if( QUuid( id ).isNull() || manifest.value( QStringLiteral("id") ).toString() != id ||
			safePackageFolder( folder ) == false || safeRelativePath( target ) == false ||
			target.endsWith( QStringLiteral(".exe"), Qt::CaseInsensitive ) == false ||
			manifestBytes.size() > 256 * 1024 || manifest.value( QStringLiteral("files") ).toArray().size() > 5000 )
		{
			sendPackageResult( QStringLiteral("transfer"), false, QStringLiteral("invalid deployment manifest") );
			break;
		}
		m_packageId = id;
		m_packageManifest = manifest;
		const auto apps = QDir( portableAppsRoot() );
		const auto stagingDirectory = apps.filePath( QStringLiteral(".ikeback-staging-%1").arg( id ) );
		m_packageStagingRoot = stagingDirectory;
		if( pathWithoutLinks( apps.absolutePath() ) == false || pathWithoutLinks( stagingDirectory ) == false ||
			QDir().mkdir( stagingDirectory ) == false )
		{
			sendPackageResult( QStringLiteral("transfer"), false, QStringLiteral("cannot create a fresh staging directory" ) );
			m_packageId.clear();
			break;
		}
#ifdef Q_OS_WIN
		// The directory is created by the interactive user. Remove inherited
		// read-only Users access before writing, so the protected DACL survives
		// the same-volume rename that activates the package.
		if( IkebackCore::platform().filesystemFunctions().setFileOwnerGroupPermissions(
					stagingDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) == false )
		{
			sendPackageResult( QStringLiteral("transfer"), false, QStringLiteral("cannot secure staging directory") );
			m_packageId.clear();
			break;
		}
#endif
		break;
	}

	case Command::DeployPackageChunk:
	{
		if( message.argument( Argument::DeploymentId ).toString() != m_packageId || m_packageId.isEmpty() ) { break; }
		const auto relative = message.argument( Argument::RelativePath ).toString();
		const auto offset = message.argument( Argument::FileOffset ).toLongLong();
		const auto data = message.argument( Argument::FileData ).toByteArray();
		qint64 expectedSize = -1;
		for( const auto& value : m_packageManifest.value( QStringLiteral("files") ).toArray() )
		{
			const auto entry = value.toObject();
			if( entry.value( QStringLiteral("path") ).toString() == relative )
			{
				expectedSize = entry.value( QStringLiteral("size") ).toVariant().toLongLong();
				break;
			}
		}
		if( safeRelativePath( relative ) == false || expectedSize < 0 || expectedSize > 512LL * 1024 * 1024 ||
			offset < 0 || offset + data.size() > expectedSize || data.size() > 256 * 1024 )
		{
			sendPackageResult( QStringLiteral("transfer"), false, QStringLiteral("invalid file chunk") );
			break;
		}
		if( relative != m_packageRelativePath )
		{
			resetPackageReceive();
			m_packageRelativePath = relative;
			const auto path = QDir( m_packageStagingRoot ).filePath( relative );
			if( pathWithoutLinks( path ) == false )
			{
				sendPackageResult( QStringLiteral("transfer"), false, QStringLiteral("unsafe package path" ) );
				break;
			}
			if( QDir().mkpath( QFileInfo( path ).absolutePath() ) == false )
			{
				sendPackageResult( QStringLiteral("transfer"), false, QStringLiteral("cannot create package subdirectory") );
				break;
			}
			m_packageFile = new QFile( path, this );
			if( m_packageFile->open( QIODevice::WriteOnly | QIODevice::Truncate ) == false )
			{
				sendPackageResult( QStringLiteral("transfer"), false, m_packageFile->errorString() );
				resetPackageReceive();
				break;
			}
		}
		if( m_packageFile == nullptr || offset != m_packageOffset ||
			m_packageFile->write( data ) != data.size() || m_packageFile->flush() == false )
		{
			sendPackageResult( QStringLiteral("transfer"), false, QStringLiteral("out-of-order or failed file write") );
			break;
		}
		m_packageOffset += data.size();
		break;
	}

	case Command::DeployPackageCommit:
	{
		if( message.argument( Argument::DeploymentId ).toString() != m_packageId || m_packageId.isEmpty() ) { break; }
		resetPackageReceive();
		QString error;
		for( const auto& value : m_packageManifest.value( QStringLiteral("files") ).toArray() )
		{
			const auto entry = value.toObject();
			const auto relative = entry.value( QStringLiteral("path") ).toString();
			if( safeRelativePath( relative ) == false || pathWithoutLinks( m_packageStagingRoot ) == false )
			{ error = QStringLiteral("invalid file path" ); break; }
			const auto stagedPath = QDir( m_packageStagingRoot ).filePath( relative );
			if( pathWithoutLinks( stagedPath ) == false )
			{ error = QStringLiteral("unsafe staged file path" ); break; }
			const auto expectedSize = entry.value( QStringLiteral("size") ).toVariant().toLongLong();
			if( expectedSize == 0 && QFileInfo::exists( stagedPath ) == false )
			{
				QDir().mkpath( QFileInfo( stagedPath ).absolutePath() );
				QFile emptyFile( stagedPath );
				if( emptyFile.open( QIODevice::WriteOnly ) == false ) { error = QStringLiteral("cannot create empty file: %1" ).arg( relative ); break; }
			}
			if( safeRelativePath( relative ) == false ||
				QFileInfo( stagedPath ).size() != expectedSize ||
				fileSha256( stagedPath ) != entry.value( QStringLiteral("sha256") ).toString().toLatin1() )
			{
				error = QStringLiteral("file verification failed: %1" ).arg( relative );
				break;
			}
		}
		const auto apps = QDir( portableAppsRoot() );
		const auto folder = m_packageManifest.value( QStringLiteral("folder") ).toString();
		const auto finalRoot = apps.filePath( folder );
		const auto shortcut = m_packageManifest.value( QStringLiteral("shortcut") ).toObject();
		const auto shortcutName = shortcut.value( QStringLiteral("name") ).toString();
		const auto shortcutTarget = QDir::fromNativeSeparators( shortcut.value( QStringLiteral("target") ).toString() );
		QString backup;
		QString shortcutBackup;
		const bool shortcutExisted = QFileInfo::exists( desktopShortcutPath( shortcutName ) );
		bool activated = false;
		const auto rollback = [&]() {
			bool restored = true;
			if( activated )
			{
				restored = QDir( finalRoot ).removeRecursively();
				activated = false;
			}
			if( backup.isEmpty() == false )
			{
				restored = QDir().rename( backup, finalRoot ) && restored;
			}
			if( shortcutBackup.isEmpty() == false )
			{
				QFile::remove( desktopShortcutPath( shortcutName ) );
				restored = QFile::rename( shortcutBackup, desktopShortcutPath( shortcutName ) ) && restored;
			}
			else if( shortcutExisted == false && QFileInfo::exists( desktopShortcutPath( shortcutName ) ) )
			{
				restored = QFile::remove( desktopShortcutPath( shortcutName ) ) && restored;
			}
			if( restored == false )
			{
				error += QStringLiteral("; rollback incomplete - manual recovery required" );
			}
		};
		if( error.isEmpty() )
		{
			if( pathWithoutLinks( apps.absolutePath() ) == false || pathWithoutLinks( finalRoot ) == false )
			{
				error = QStringLiteral("unsafe deployment directory");
			}
			else if( QDir().mkpath( apps.absolutePath() ) == false ) { error = QStringLiteral("cannot create C:\\IkebackApps"); }
			else if( QFileInfo::exists( finalRoot ) )
			{
				backup = apps.filePath( QStringLiteral(".ikeback-backup-%1-%2" ).arg( m_packageId ).arg( QDateTime::currentMSecsSinceEpoch() ) );
				if( QDir().rename( finalRoot, backup ) == false ) { error = QStringLiteral("cannot preserve the previous application folder"); }
			}
			if( error.isEmpty() && QDir().rename( m_packageStagingRoot, finalRoot ) == false )
			{
				error = QStringLiteral("cannot activate the deployed folder");
				if( backup.isEmpty() == false && QDir().rename( backup, finalRoot ) == false )
				{
					error += QStringLiteral("; previous folder rollback failed");
				}
				else { backup.clear(); }
			}
			else if( error.isEmpty() )
			{
				activated = true;
			}
		}
		if( error.isEmpty() )
		{
			const auto target = QDir( finalRoot ).filePath( shortcutTarget );
			if( pathWithoutLinks( target ) == false )
			{
				error = QStringLiteral("unsafe deployment target");
			}
			else
			{
				if( shortcutExisted )
				{
					shortcutBackup = QDir( QFileInfo( desktopShortcutPath( shortcutName ) ).absolutePath() ).
						filePath( QStringLiteral(".ikeback-shortcut-%1.bak" ).arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) ) );
					if( QFile::copy( desktopShortcutPath( shortcutName ), shortcutBackup ) == false )
					{
						error = QStringLiteral("cannot preserve the previous desktop shortcut");
					}
				}
				if( error.isEmpty() && ClassroomSystem::createDesktopShortcut( shortcutName, target,
					shortcut.value( QStringLiteral("arguments") ).toString(), QFileInfo( target ).absolutePath() ) == false )
				{
					error = ClassroomSystem::lastError();
				}
			}
		}
		if( error.isEmpty() )
		{
			const QJsonObject state{ { QStringLiteral("id"), m_packageId }, { QStringLiteral("phase"), QStringLiteral("verify") },
				{ QStringLiteral("manifest"), m_packageManifest }, { QStringLiteral("shortcutPath"), desktopShortcutPath( shortcutName ) } };
			const auto stateDirectory = deploymentStateDirectory();
			if( ensureDeploymentStateDirectory() == false )
			{
				error = QStringLiteral("cannot create private deployment state directory");
			}
			else
			{
				QSaveFile stateFile( QDir( stateDirectory ).filePath( QStringLiteral(".ikeback-deploy-%1.json" ).arg( m_packageId ) ) );
				if( stateFile.open( QIODevice::WriteOnly ) == false ||
					stateFile.write( QJsonDocument( state ).toJson( QJsonDocument::Compact ) ) < 0 || stateFile.commit() == false )
				{
					error = QStringLiteral("cannot save reboot verification state");
				}
			}
		}
		if( error.isEmpty() == false ) { rollback(); }
		else if( shortcutBackup.isEmpty() == false ) { QFile::remove( shortcutBackup ); }
		sendPackageResult( QStringLiteral("commit"), error.isEmpty(), error.isEmpty() ?
			QStringLiteral("files verified and desktop shortcut created") : error );
		if( error.isEmpty() == false ) { m_packageId.clear(); }
		break;
	}

	// ------------------------------------------- 5.2.2 remote software deployment
	case Command::DeployFileBegin:
	{
		if( m_deployFile )
		{
			m_deployFile->close();
			m_deployFile->deleteLater();
			m_deployFile = nullptr;
		}
		// Only the bare file name is ever used as a path component.
		const auto fileName = QFileInfo( message.argument( Argument::FileName ).toString() ).fileName();
		const auto stagingDir = QStandardPaths::writableLocation( QStandardPaths::TempLocation ) +
								QStringLiteral("/ikeback-deploy");
		QDir().mkpath( stagingDir );
		m_deployFile = new QFile( stagingDir + QLatin1Char('/') + fileName, this );
		if( pathWithoutLinks( m_deployFile->fileName() ) == false ||
			IkebackCore::platform().filesystemFunctions().openFileSafely(
				m_deployFile, QFile::WriteOnly | QFile::Truncate,
				QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner ) == false )
		{
			qWarning( "ikeback: could not stage installer %s", qPrintable( fileName ) );
			m_deployFile->deleteLater();
			m_deployFile = nullptr;
		}
		break;
	}

	case Command::DeployFileChunk:
		if( m_deployFile )
		{
			m_deployFile->write( message.argument( Argument::FileData ).toByteArray() );
		}
		break;

	case Command::UwfInstall:
	{
		QString stagedPath;
		if( m_deployFile )
		{
			stagedPath = m_deployFile->fileName();
			m_deployFile->close();
			m_deployFile->deleteLater();
			m_deployFile = nullptr;
		}
		if( stagedPath.isEmpty() || QFileInfo::exists( stagedPath ) == false )
		{
			reportEvent( EventType::install(), QStringLiteral("install failed: no staged installer"), {} );
			break;
		}

		QStringList extraArgs;
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
		extraArgs = QProcess::splitCommand( message.argument( Argument::SoftwareArgs ).toString() );
#else
		extraArgs = message.argument( Argument::SoftwareArgs ).toString().split(
			QLatin1Char(' '), QString::SkipEmptyParts );
#endif
		const auto fileName = QFileInfo( stagedPath ).fileName();
		const auto reportInstall = [this, fileName]( bool ok, const QString& detail ) {
			if( m_worker == nullptr ) { return; }
			auto reply = Classroom::message( Command::InstallResult );
			reply.addArgument( Argument::Result, ok ? QStringLiteral("ok") : QStringLiteral("failed") )
				 .addArgument( Argument::Detail, detail )
				 .addArgument( Argument::SoftwarePath, fileName )
				 .addArgument( Argument::Host, m_hostName )
				 .addArgument( Argument::Ip, m_ipAddress );
			m_worker->sendFeatureMessageReply( reply );
		};

		if( m_installProcess )
		{
			m_installProcess->deleteLater();
			m_installProcess = nullptr;
		}
		QString program;
		QStringList args;
		if( stagedPath.endsWith( QStringLiteral(".msi"), Qt::CaseInsensitive ) )
		{
			program = QStringLiteral("msiexec.exe");
			args = QStringList{ QStringLiteral("/i"), stagedPath, QStringLiteral("/quiet"),
								QStringLiteral("/norestart") };
		}
		else
		{
			program = stagedPath;
			args = QStringList{ QStringLiteral("/VERYSILENT"), QStringLiteral("/SUPPRESSMSGBOXES"),
								QStringLiteral("/NORESTART"), QStringLiteral("/S") };
		}
		args.append( extraArgs );

		auto* process = new QProcess( this );
		m_installProcess = process;
		connect( process, QOverload<int, QProcess::ExitStatus>::of( &QProcess::finished ),
				 this, [this, process, reportInstall]( int exitCode, QProcess::ExitStatus status ) {
					 reportInstall( status == QProcess::NormalExit && exitCode == 0,
									QStringLiteral("exit=%1").arg( exitCode ) );
					 if( m_installProcess == process )
					 {
						 m_installProcess = nullptr;
						 process->deleteLater();
					 }
				 } );
		process->start( program, args );
		const auto started = process->waitForStarted( 10000 );
		if( started == false )
		{
			reportInstall( false, process->errorString() );
			if( m_installProcess == process )
			{
				m_installProcess = nullptr;
				process->deleteLater();
			}
		}
		reportEvent( EventType::install(),
					 started ? QStringLiteral("installer launched: %1").arg( stagedPath )
							 : QStringLiteral("installer FAILED to launch: %1").arg( stagedPath ), {} );
		break;
	}

	// ---------------------------------------------------------- 5.2.0 quick tools
	case Command::SetExitProtection:
	{
		// "Protection" is CLIENT protection (the student cannot quit the worker).
		// It is deliberately independent from UWF / uninstall protection.
		const auto enabled = message.argument( Argument::Enabled ).toBool();
		auto object = m_workerSettings;
		object.insert( SettingKey::exitProtection(), enabled );
		applyWorkerSettings( object );
		reportEvent( EventType::protection(),
					 enabled ? QStringLiteral("client protection enabled by teacher")
							 : QStringLiteral("client protection disabled by teacher"), {} );
		auto reply = Classroom::message( Command::ProtectionStateReport );
		reply.addArgument( Argument::Enabled, enabled );
		if( m_worker ) { m_worker->sendFeatureMessageReply( reply ); }
		break;
	}

	case Command::OpenSoftware:
	{
		const auto path = message.argument( Argument::SoftwarePath ).toString().trimmed();
		const QFileInfo info( path );
		// Only an existing .exe may be launched; scripts and arbitrary paths
		// would turn this command into remote command execution.
		if( path.isEmpty() || info.isFile() == false ||
			info.suffix().compare( QStringLiteral("exe"), Qt::CaseInsensitive ) != 0 )
		{
			qWarning( "ikeback: OpenSoftware rejected a non-.exe or missing path" );
			break;
		}
		const auto args = message.argument( Argument::SoftwareArgs ).toString();
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
		const auto arguments = QProcess::splitCommand( args );
#else
		const auto arguments = args.split( QLatin1Char(' '), QString::SkipEmptyParts );
#endif
		QProcess::startDetached( info.absoluteFilePath(), arguments );
		reportEvent( EventType::software(), path, {} );
		break;
	}

	case Command::OpenWebsite:
	{
		const auto url = message.argument( Argument::Message ).toString().trimmed();
		if( url.isEmpty() ) { break; }
		QDesktopServices::openUrl( QUrl( url ) );
		reportEvent( EventType::website(), url, {} );
		break;
	}

	case Command::SetAppBlockRules:
	case Command::SetWebBlockRules:
	{
		const auto isApp = message.command<Command>() == Command::SetAppBlockRules;
		auto object = m_workerSettings;
		object.insert( isApp ? SettingKey::blockedApps() : SettingKey::blockedWebsites(),
					   QJsonDocument::fromJson(
						   message.argument( Argument::RulesJson ).toString().toUtf8() ).array() );
		applyWorkerSettings( object );
		reportEvent( isApp ? EventType::appBlock() : EventType::webBlock(),
					 QStringLiteral("fuzzy block rules updated by teacher"), {} );
		break;
	}

	default:
		return false;
	}

	return true;
}
