/*
 * ClassroomConfigurationPage.cpp - Ikeback Configurator page for the classroom extension
 *
 * The standard view shows only the settings a teacher actually needs; niche
 * rows carry the Advanced flag and appear when the configurator is switched
 * to the advanced view. Settings that never had any effect (base score, UWF /
 * internet / USB presets, exit admin requirement) and the mobile bridge
 * (owned by the teacher console) were removed. This file is part of the
 * Ikeback classroom modification.
 */

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>

#include "ClassroomConfiguration.h"
#include "ClassroomData.h"
#include "ClassroomConfigurationPage.h"
#include "ClassroomTheme.h"
#include "Filesystem.h"
#include "IkebackCore.h"
#include "Configuration/UiMapping.h"


ClassroomConfigurationPage::ClassroomConfigurationPage( ClassroomConfiguration& configuration, QWidget* parent ) :
	ConfigurationPage( parent ),
	m_configuration( configuration )
{
	setWindowTitle( Classroom::productName() );
	setObjectName( QStringLiteral("classroomConfiguration") );
	Classroom::Theme::apply( this );
	setWindowIcon( QIcon( QStringLiteral(":/master/app-icon.png") ) );

	auto* form = new QFormLayout( this );
	const auto addWidget = [form]( QWidget* widget ) {
		Configuration::UiMapping::setFlags( widget, Configuration::Property::Flag::Standard );
		form->addWidget( widget );
		return widget;
	};
	// Standard rows need an explicit label widget so that switching to the
	// advanced view (or back) can hide label and field together.
	const auto addRow = [form, this]( const QString& label, QWidget* widget ) {
		auto* labelWidget = new QLabel( label, this );
		Configuration::UiMapping::setFlags( labelWidget, Configuration::Property::Flag::Standard );
		Configuration::UiMapping::setFlags( widget, Configuration::Property::Flag::Standard );
		form->addRow( labelWidget, widget );
		return widget;
	};
	const auto addAdvancedRow = [form, this]( const QString& label, QWidget* widget ) {
		auto* labelWidget = new QLabel( label + tr( "（高级）" ), this );
		Configuration::UiMapping::setFlags( labelWidget, Configuration::Property::Flag::Advanced );
		Configuration::UiMapping::setFlags( widget, Configuration::Property::Flag::Advanced );
		form->addRow( labelWidget, widget );
		return widget;
	};

	m_enabled = qobject_cast<QCheckBox*>( addWidget( new QCheckBox( tr( "启用班级管理功能" ), this ) ) );
	m_exitProtection = qobject_cast<QCheckBox*>( addWidget( new QCheckBox( tr( "保护学生端不被关闭" ), this ) ) );
	m_allowSelfLogin = qobject_cast<QCheckBox*>( addWidget( new QCheckBox( tr( "允许学生自行签到登录" ), this ) ) );

	auto* importRosterButton = new QPushButton( tr( "批量导入名单（座位号 + 姓名）" ), this );
	addWidget( importRosterButton );
	connect( importRosterButton, &QPushButton::clicked, this, [this]() {
		const auto path = QFileDialog::getOpenFileName(
			this, tr( "批量导入学生名单" ), {},
			tr( "表格文件 (*.csv *.txt *.tsv);;所有文件 (*)" ) );
		if( path.isEmpty() ) { return; }
		const auto dataDirectory = IkebackCore::filesystem().expandPath( m_configuration.dataDirectory() );
		QDir().mkpath( dataDirectory );
		ClassroomRoster roster( dataDirectory );
		ClassroomImportReport report;
		QString error;
		if( roster.importTableFile( path, {}, &report, &error ) == false )
		{
			QMessageBox::warning( this, windowTitle(),
								  error.isEmpty() ? tr( "未能从该文件导入任何学生。" ) : error );
			return;
		}
		if( roster.save() == false )
		{
			QMessageBox::warning( this, windowTitle(),
								  tr( "名单未能写入 %1，本次导入未生效。" ).arg( roster.filePath() ) );
			return;
		}
		QString summary = tr( "导入完成：%1 个班级，%2 名学生。" )
							  .arg( report.classes ).arg( report.students );
		if( report.overwritten > 0 )
		{
			summary += QLatin1Char('\n') + tr( "更新了 %1 个已有座位。" ).arg( report.overwritten );
		}
		if( report.ipConflicts > 0 )
		{
			summary += QLatin1Char('\n') + tr( "跳过 %1 行：同一班级中该 IP 已被其他座位使用。" ).arg( report.ipConflicts );
		}
		if( report.problems.isEmpty() )
		{
			QMessageBox::information( this, windowTitle(), summary );
		}
		else
		{
			QMessageBox::warning( this, windowTitle(),
								  summary + QLatin1Char('\n') + QLatin1Char('\n') + report.problems.join( QLatin1Char('\n') ) );
		}
	} );

	form->addRow( new QLabel( tr( "AI 助手" ), this ) );
	m_aiEnabled = qobject_cast<QCheckBox*>( addWidget( new QCheckBox( tr( "启用 AI 助手" ), this ) ) );
	m_aiEndpoint = qobject_cast<QLineEdit*>( addRow( tr( "API 地址：" ), new QLineEdit( this ) ) );
	m_aiApiKey = qobject_cast<QLineEdit*>( addRow( tr( "API 密钥（可选）：" ), new QLineEdit( this ) ) );
	m_aiApiKey->setEchoMode( QLineEdit::Password );
	m_aiModel = qobject_cast<QLineEdit*>( addRow( tr( "模型：" ), new QLineEdit( this ) ) );
	m_aiSystemPrompt = qobject_cast<QLineEdit*>( addRow( tr( "系统提示词：" ), new QLineEdit( this ) ) );
	m_aiTimeout = qobject_cast<QSpinBox*>( addAdvancedRow( tr( "超时（秒）：" ), new QSpinBox( this ) ) );
	m_aiTimeout->setRange( 5, 600 );

	form->addRow( new QLabel( tr( "行为监控" ), this ) );
	m_monitoringEnabled = qobject_cast<QCheckBox*>( addWidget( new QCheckBox( tr( "记录活动窗口和应用程序" ), this ) ) );
	m_captureInterval = qobject_cast<QSpinBox*>( addAdvancedRow( tr( "采样间隔（秒）：" ), new QSpinBox( this ) ) );
	m_captureInterval->setRange( 1, 300 );
	m_retentionDays = qobject_cast<QSpinBox*>( addAdvancedRow( tr( "日志保留天数：" ), new QSpinBox( this ) ) );
	m_retentionDays->setRange( 1, 3650 );

	form->addRow( new QLabel( tr( "课堂与同步" ), this ) );
	m_silencePageText = qobject_cast<QLineEdit*>( addRow( tr( "肃静页文字：" ), new QLineEdit( this ) ) );
	m_syncEnabled = qobject_cast<QCheckBox*>( addWidget( new QCheckBox( tr( "启用局域网自动同步（手机端）" ), this ) ) );
	m_dataDirectory = qobject_cast<QLineEdit*>( addAdvancedRow( tr( "数据目录：" ), new QLineEdit( this ) ) );
	m_exitAdminUsername = qobject_cast<QLineEdit*>( addAdvancedRow( tr( "管理员账户：" ), new QLineEdit( this ) ) );
	m_wordCloudStopWords = qobject_cast<QLineEdit*>( addAdvancedRow( tr( "词云额外停用词：" ), new QLineEdit( this ) ) );

	Configuration::UiMapping::setFlags( this, Configuration::Property::Flag::Standard );
}


void ClassroomConfigurationPage::resetWidgets()
{
	m_enabled->setChecked( m_configuration.enabled() );
	m_dataDirectory->setText( m_configuration.dataDirectory() );
	m_exitProtection->setChecked( m_configuration.exitProtection() );
	m_exitAdminUsername->setText( m_configuration.exitAdminUsername() );
	m_allowSelfLogin->setChecked( m_configuration.allowSelfLogin() );
	m_aiEnabled->setChecked( m_configuration.aiEnabled() );
	m_aiEndpoint->setText( m_configuration.aiEndpoint() );
	m_aiApiKey->setText( m_configuration.aiApiKey() );
	m_aiModel->setText( m_configuration.aiModel() );
	m_aiSystemPrompt->setText( m_configuration.aiSystemPrompt() );
	m_aiTimeout->setValue( m_configuration.aiTimeout() );
	m_monitoringEnabled->setChecked( m_configuration.monitoringEnabled() );
	m_captureInterval->setValue( m_configuration.captureInterval() );
	m_retentionDays->setValue( m_configuration.retentionDays() );
	m_syncEnabled->setChecked( m_configuration.syncEnabled() );
	m_silencePageText->setText( m_configuration.silencePageText() );
	m_wordCloudStopWords->setText( m_configuration.wordCloudStopWords() );
}


void ClassroomConfigurationPage::connectWidgetsToProperties()
{
	const auto changed = [this]() { Q_EMIT widgetsChanged(); };

	connect( m_enabled, &QCheckBox::toggled, this, [this, changed]( bool value ) {
		m_configuration.setEnabled( value ); changed(); } );
	connect( m_dataDirectory, &QLineEdit::textChanged, this, [this, changed]( const QString& value ) {
		m_configuration.setDataDirectory( value ); changed(); } );
	connect( m_exitProtection, &QCheckBox::toggled, this, [this, changed]( bool value ) {
		m_configuration.setExitProtection( value ); changed(); } );
	connect( m_exitAdminUsername, &QLineEdit::textChanged, this, [this, changed]( const QString& value ) {
		m_configuration.setExitAdminUsername( value ); changed(); } );
	connect( m_allowSelfLogin, &QCheckBox::toggled, this, [this, changed]( bool value ) {
		m_configuration.setAllowSelfLogin( value ); changed(); } );
	connect( m_aiEnabled, &QCheckBox::toggled, this, [this, changed]( bool value ) {
		m_configuration.setAiEnabled( value ); changed(); } );
	connect( m_aiEndpoint, &QLineEdit::textChanged, this, [this, changed]( const QString& value ) {
		m_configuration.setAiEndpoint( value ); changed(); } );
	connect( m_aiApiKey, &QLineEdit::textChanged, this, [this, changed]( const QString& value ) {
		m_configuration.setAiApiKey( value ); changed(); } );
	connect( m_aiModel, &QLineEdit::textChanged, this, [this, changed]( const QString& value ) {
		m_configuration.setAiModel( value ); changed(); } );
	connect( m_aiSystemPrompt, &QLineEdit::textChanged, this, [this, changed]( const QString& value ) {
		m_configuration.setAiSystemPrompt( value ); changed(); } );
	connect( m_aiTimeout, QOverload<int>::of( &QSpinBox::valueChanged ), this, [this, changed]( int value ) {
		m_configuration.setAiTimeout( value ); changed(); } );
	connect( m_monitoringEnabled, &QCheckBox::toggled, this, [this, changed]( bool value ) {
		m_configuration.setMonitoringEnabled( value ); changed(); } );
	connect( m_captureInterval, QOverload<int>::of( &QSpinBox::valueChanged ), this, [this, changed]( int value ) {
		m_configuration.setCaptureInterval( value ); changed(); } );
	connect( m_retentionDays, QOverload<int>::of( &QSpinBox::valueChanged ), this, [this, changed]( int value ) {
		m_configuration.setRetentionDays( value ); changed(); } );
	connect( m_syncEnabled, &QCheckBox::toggled, this, [this, changed]( bool value ) {
		m_configuration.setSyncEnabled( value ); changed(); } );
	connect( m_silencePageText, &QLineEdit::textChanged, this, [this, changed]( const QString& value ) {
		m_configuration.setSilencePageText( value ); changed(); } );
	connect( m_wordCloudStopWords, &QLineEdit::textChanged, this, [this, changed]( const QString& value ) {
		m_configuration.setWordCloudStopWords( value ); changed(); } );
}
