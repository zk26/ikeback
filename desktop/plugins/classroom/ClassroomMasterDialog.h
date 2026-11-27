/*
 * ClassroomMasterDialog.h - teacher console for classes, logs, analysis and AI
 *
 * This file is part of the Ikeback classroom modification.
 */

#pragma once

#include <QWidget>
#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ClassroomData.h"
#include "ComputerControlInterface.h"

#include <functional>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDialog;
class QFile;
class QLabel;
class QProgressBar;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTimer;
class ClassroomAiClient;
class ClassroomBridge;
class ClassroomScorePanel;
class ClassroomShopPanel;
class ClassroomSeatGrid;
class ComputerControlInterface;
class ClassroomConfiguration;
class ClassroomLogStore;
class ClassroomRoster;
class ClassroomSettings;
class FeatureMessage;
class IkebackMasterInterface;

class ClassroomMasterDialog : public QWidget
{
	Q_OBJECT
public:
	ClassroomMasterDialog( IkebackMasterInterface& master,
						   ClassroomConfiguration& configuration,
						   ClassroomRoster& roster,
						   ClassroomLogStore& store,
						   ClassroomSettings& settings,
						   QWidget* parent = nullptr );
	~ClassroomMasterDialog() override;

	//! One entry of the "Quick tools" drop-down on the Ikeback Master tool bar.
	//! `id` is a stable, untranslated identifier: the plug-in uses it to expose
	//! the same action both as a tool-bar button and as a menu entry, and the
	//! label can be translated without breaking that link.
	struct QuickToolAction
	{
		QString id;
		QString label;
		std::function<void()> run;
	};
	struct QuickToolGroup
	{
		QString label;
		QString icon;
		QVector<QuickToolAction> actions;
	};
	//! Grouped actions collected by setupToolsTab(); the plug-in turns them into
	//! a QMenu so the same code is not duplicated as tool-bar buttons.
	QVector<QuickToolGroup> quickToolGroups() const { return m_quickToolGroups; }
	//! Runs one quick-tool action by its stable id (tool-bar button click).
	bool runQuickTool( const QString& id, const ComputerControlInterfaceList& targets );
	void startCheckInFromToolBar();
	void beginLesson();
	void endLesson();
	bool lessonActive() const { return m_lessonActive; }
	Q_INVOKABLE void showPage( const QString& pageName );

	void showCall( const QString& ip, const QString& seat, const QString& student );
	//! 名册文件被配置器更新后重新加载并同步到所有界面。
	void reloadRoster();
	//! 学生自助签到后刷新教师端座位表/状态。
	void recordStudentIdentity( const QString& className, const QString& seat,
								const QString& student, bool loggedIn );
	void syncNow();
	//! Probes the local ikeback server port and the mobile bridge so "the
	//! computer cannot be reached" turns into an actionable message instead of
	//! a red icon with no explanation.
	void runConnectivityCheck();
	//! Restarts the Windows service with the bundled CLI (needs elevation).
	void restartService();
	//! Asks for a portable folder and deploys it to C:\IkebackApps.
	void deployInstaller();
	void recordPackageResult( const QString& ip, const QString& phase, bool success, const QString& detail );
	void recordProtectionState( const QString& ip, bool enabled );
	void restoreProtectionStates( const QMap<QString, bool>& states );
	//! 学生机真实安装回执，用于替代定时器误报完成。
	void recordInstallResult( const QString& ip, bool success, const QString& detail );
	//! 5.2.2: compact dialog with teacher operations and their results
	//! ("did my protection command actually succeed?").
	void openOperationLog();
	//! Switches to the Quick tools tab ("常用工具" button).
	void showQuickToolsTab();
	//! Sends the catalog of `className` (default: the selected class) to every
	//! online student.
	void broadcastShopState( const QString& className = QString{} );
	//! Refreshes the shop tables after a student redeem/bid changed the book.
	void refreshShopPanel();
	//! A redemption moved a student's score: republish it to the seat grid.
	void refreshScoresAfterShop() { refreshSeatGrid(); }

	// Toggle state kept by the tool-bar buttons, so one click flips the
	// behaviour instead of offering a separate "on" and "off" button.
	bool m_protectionOn = false;
	bool m_uwfOn = false;
	bool m_gatewayBlocked = false;
	bool m_silenceShown = false;
	bool m_appBlockOn = false;
	bool m_webBlockOn = false;
	bool m_muted = false;
	//! Called by the plugin when a student submits the attendance prompt.
	void recordCheckIn( const QString& className, const QString& seat, const QString& typedName );

Q_SIGNALS:
	//! Emitted by the Close button so the host QDockWidget can hide itself.
	void hideRequested();

private:
	void setupClassTab();
	void addPage( QWidget* page, const QString& label, int index = -1, const QString& key = QString{} );
	void setupLogTab();
	void setupAnalysisTab();
	void setupAiTab();
	void setupMonitorTab();
	void setupSystemTab();
	void setupSeatTab();
	void replayIdentityEvents();
	void refreshSeatGrid();
	void refreshUwfStatus();
	//! 5.2.0 teacher-side parity with the phone: timetable + per-lesson scoring.
	void setupTimetableTab();
	void refreshLessonRecords();
	QString timetableFilePath() const;

	//! 5.3.0 class shop and auctions: management tab plus catalog broadcast.
	void setupShopTab();

	//! 5.2.0 quick tools. "Client protection" and "UWF" are separate switches.
	void setupToolsTab();
	QStringList toolTargets() const;
	void sendToTargets( const FeatureMessage& message );
	void setToolStatus( const QString& text );
	void setClientProtection( bool enabled );
	void setUwfFilter( bool enabled );
	void applyFuzzyRules( const QString& key, const QString& text );
	void openOnTargets( bool website, const QString& target );
	//! 弹出关键字编辑框：命中关键字即拦截应用/网址。
	void showKeywordDialog( bool websites );
	//! 多行批量下发程序/网址。
	void showBatchOpenDialog( bool websites );
	//! 全部加分/减分：先选分数并填写原因，再下发。
	void showWholeClassScoreDialog( bool add );
	void scoreWholeClass( int delta, const QString& reason = QString{} );
	void setDeployProgress( const QString& stage, int percent );
	void startCheckIn();
	void refreshCheckIn();

	//! Remote deployment state machine (UWFTool-style flow):
	//! disable UWF -> reboot -> wait online -> transfer -> install -> re-enable.
	enum class DeployPhase
	{
		Idle,
		DisableUwf,
		RebootAfterDisable,
		WaitOnline,
		Transfer,
		WaitInstall,
		EnableUwf,
		RebootAfterEnable,
		WaitVerify
	};
	void advanceDeploy();
	QString installSummaryText() const;
	int deployOnlineCount() const;
	void finishDeploy( const QString& result );

	QString dataDirectory() const;
	QString currentClassName() const;
	QString currentSessionId();
	QJsonObject settingsJson() const;
	//! Student-bound copy of settingsJson() with every teacher-side secret removed.
	QJsonObject settingsForStudents() const;
	QString sessionKey() const;

	void addClass();
	void importRoster();
	//! Bulk import straight from a pasted spreadsheet selection (Excel/WPS).
	void importRosterPaste();
	void saveImportTemplate();
	void exportRoster();
	void saveRoster();
	//! Insert or update one student in `className` and keep the table in sync.
	//! Returns false when the IP already belongs to a different seat.
	bool upsertStudent( const QString& className, const QString& seat, const QString& name,
						const QString& ip );
	void reportImport( const ClassroomImportReport& report );
	void registerComputers( const QVector<ClassroomStudent>& students );
	void refreshRosterTable();
	void refreshStatus();
	void refreshMachines();

	void startNewSession();
	void generateKey();
	void assignByIp();
	void quickLoginByIp();
	void batchLogin();
	void batchLogout();
	void forceLogout();
	void switchStudent();
	void lockAll( bool locked );
	void toggleAi( bool enabled );
	void pushSettings();
	void pushRoster();
	void pushKey();
	void pushIdentity( const QString& ip, const QString& seat, const QString& name );
	void pushLogout( const QString& ip );
	void sendToAll( const FeatureMessage& message );
	void sendToIp( const QString& ip, const FeatureMessage& message );
	void queueDirectMessage( const QString& ip, const FeatureMessage& message, int retries );
	QStringList selectedIps() const;
	QStringList onlineIps() const;
	static QString ipOfInterface( const class ComputerControlInterface* controlInterface );
	void appendTeacherEvent( const QString& type, const QString& detail, const QString& ip = QString{},
							 const QString& seat = QString{}, const QString& student = QString{} );

	void refreshLogs();
	void openLogFolder();
	void exportLogs();
	void purgeLogs();
	QVector<ClassroomEvent> filteredEvents() const;

	void refreshAnalysis();
	void refreshSettingsWidgets();
	void saveSettingsFromWidgets();
	void editCategories();
	void exportAnalysis();
	void testAiConnection();

	IkebackMasterInterface& m_master;
	ClassroomConfiguration& m_configuration;
	ClassroomRoster& m_roster;
	ClassroomLogStore& m_store;
	ClassroomSettings& m_settings;
	ClassroomAiClient* m_testAiClient = nullptr;
	ClassroomScorePanel* m_scorePanel = nullptr;
	ClassroomShopPanel* m_shopPanel = nullptr;
	ClassroomBridge* m_bridge = nullptr;
	QTimer* m_refreshTimer = nullptr;

	QString m_sessionId;
	QString m_currentLessonId;
	bool m_lessonActive = false;
	ComputerControlInterfaceList m_actionTargets;
	bool m_actionTargetsActive = false;
	int m_refreshTicks = 0;
	QMap<QString, QString> m_status;
	QMap<QString, QSharedPointer<ComputerControlInterface>> m_directInterfaces;
	QVector<ClassroomEvent> m_logEvents;
	QVector<ClassroomInterestProfile> m_profiles;

	QListWidget* m_navigation = nullptr;
	QStackedWidget* m_tabs = nullptr;
	QWidget* m_toolsPage = nullptr;

	QComboBox* m_classCombo = nullptr;
	QTableWidget* m_rosterTable = nullptr;
	QLabel* m_keyLabel = nullptr;
	QLabel* m_sessionLabel = nullptr;
	QLineEdit* m_quickIp = nullptr;
	QLineEdit* m_quickSeat = nullptr;
	QLineEdit* m_quickName = nullptr;

	QDateEdit* m_logFrom = nullptr;
	QDateEdit* m_logTo = nullptr;
	QComboBox* m_logScope = nullptr;
	QComboBox* m_logMachine = nullptr;
	QComboBox* m_logType = nullptr;
	QLineEdit* m_logKeyword = nullptr;
	QTableWidget* m_logTable = nullptr;

	QSpinBox* m_halfLife = nullptr;
	QTableWidget* m_analysisTable = nullptr;
	QLabel* m_analysisHint = nullptr;

	QCheckBox* m_aiEnabled = nullptr;
	QLineEdit* m_aiEndpoint = nullptr;
	QLineEdit* m_aiApiKey = nullptr;
	QLineEdit* m_aiModel = nullptr;
	QPlainTextEdit* m_aiSystemPrompt = nullptr;
	QSpinBox* m_aiTimeout = nullptr;
	QLabel* m_aiTestResult = nullptr;

	QCheckBox* m_monitoringEnabled = nullptr;
	QCheckBox* m_captureWindowTitle = nullptr;
	QCheckBox* m_captureProcess = nullptr;
	QCheckBox* m_exitProtection = nullptr;
	QCheckBox* m_allowSelfLogin = nullptr;
	QLineEdit* m_exitAdminUsername = nullptr;
	//! Super-admin key (超级管理员密钥) gating protected exit on students.
	QLineEdit* m_adminKey = nullptr;
	QSpinBox* m_captureInterval = nullptr;
	QSpinBox* m_retentionDays = nullptr;

	QLabel* m_systemStatus = nullptr;
	QLabel* m_connectivityLabel = nullptr;
	QComboBox* m_teacherIpPicker = nullptr;
	QLineEdit* m_teacherIp = nullptr;
	QLineEdit* m_packagePath = nullptr;
	QLineEdit* m_silenceText = nullptr;
	QCheckBox* m_bridgeEnabled = nullptr;
	QSpinBox* m_bridgePort = nullptr;

	ClassroomSeatGrid* m_seatGrid = nullptr;
	QTableWidget* m_checkInTable = nullptr;

	// Quick tools tab (inserted as the first tab). Buttons no longer live here:
	// the grouped actions are rendered by the Ikeback Master tool-bar menu.
	QVector<QuickToolGroup> m_quickToolGroups;
	QComboBox* m_toolScope = nullptr;
	QLabel* m_toolsStatus = nullptr;
	QLineEdit* m_toolSilence = nullptr;
	QLineEdit* m_toolAppRules = nullptr;
	QLineEdit* m_toolWebRules = nullptr;
	QLineEdit* m_toolSoftware = nullptr;
	QLineEdit* m_toolWebsite = nullptr;
	QMap<QString, QString> m_uwfStatus; //!< seat -> latest UWF report from that machine
	QMap<QString, bool> m_protectionStatus;
	QLineEdit* m_lessonTopic = nullptr;
	QPlainTextEdit* m_lessonRecords = nullptr;
	QLabel* m_timetableSummary = nullptr;
	QLabel* m_checkInSummary = nullptr;
	QMap<QString, QString> m_checkInNames;
	QMap<QString, QDateTime> m_checkInTimes;
	QDateTime m_checkInStarted;

	//! Remote deployment state (see DeployPhase).
	QTimer* m_deployTimer = nullptr;
	ComputerControlInterfaceList m_deployTargets;
	QJsonObject m_deployManifest;
	QJsonArray m_deployFiles;
	QString m_deployFolder;
	QString m_deploySourceFolder;
	QString m_deployId;
	QString m_deployArgs;
	QFile* m_deploySource = nullptr;
	int m_deployFileIndex = 0;
	qint64 m_deployFileOffset = 0;
	qint64 m_deployTotalBytes = 0;
	qint64 m_deploySentBytes = 0;
	bool m_deployUwfFlow = true;
	DeployPhase m_deployPhase = DeployPhase::Idle;
	int m_deployWaitTicks = 0;
	int m_deployExpected = 0;
	int m_deploySuccess = 0;
	int m_deployFailure = 0;
	QSet<QString> m_deployReported;
	QSet<QString> m_deployVerified;
	int m_deployVerifySuccess = 0;
	int m_deployVerifyFailure = 0;
	QString m_deployInstallSummary;
	//! 安装期间显示的进度弹窗：窗口模态，禁止教师误操作其他设置。
	QDialog* m_deployProgressDialog = nullptr;
	QLabel* m_deployProgressLabel = nullptr;
	QProgressBar* m_deployProgressBar = nullptr;
};
