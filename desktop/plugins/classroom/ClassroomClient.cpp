/*
 * ClassroomClient.cpp - non-topmost student card, popups and activity sampler
 *
 * This file is part of the Ikeback classroom modification.
 */

#include <QtGlobal>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#endif

#include <algorithm>

#include <QApplication>
#include <QBoxLayout>
#include <QJsonArray>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>
#include <QVariantMap>
#include <QVBoxLayout>

#include "ClassroomAiClient.h"
#include "ClassroomClient.h"
#include "ClassroomProtocol.h"
#include "ClassroomSecrets.h"
#include "CryptoCore.h"
#include "PlatformCoreFunctions.h"
#include "PlatformPluginInterface.h"
#include "PlatformUserFunctions.h"
#include "IkebackCore.h"


static void keepOnTop( QWidget* widget )
{
	widget->setWindowFlags( Qt::Tool | Qt::CustomizeWindowHint | Qt::FramelessWindowHint |
							Qt::WindowStaysOnTopHint );
	IkebackCore::platform().coreFunctions().raiseWindow( widget, true );
}


//! The student floating card is deliberately NOT always-on-top: it must not
//! cover the student's work. Only the popups opened from it stay on top.
static void keepFloating( QWidget* widget )
{
	widget->setWindowFlags( Qt::Tool | Qt::CustomizeWindowHint | Qt::FramelessWindowHint );
	IkebackCore::platform().coreFunctions().raiseWindow( widget, false );
}


static QPoint mouseGlobalPoint( QMouseEvent* event )
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return event->globalPosition().toPoint();
#else
	return event->globalPos();
#endif
}


ClassroomClientWidget::ClassroomClientWidget( QWidget* parent ) :
	QWidget( parent ),
	m_aiClient( new ClassroomAiClient( this ) )
{
	setObjectName( QStringLiteral("classroomClient") );
	setWindowTitle( Classroom::productName() );
	setAttribute( Qt::WA_DeleteOnClose, false );
	keepFloating( this );

	const auto dark = IkebackCore::useDarkMode();
	setStyleSheet( QStringLiteral(
		"QWidget#classroomClient { background: %1; border: 1px solid %2; border-radius: 6px; }"
		"QLabel { color: %3; }"
		"QLineEdit { padding: 3px; }"
		"QPushButton { padding: 4px 8px; }" ).
		arg( dark ? QStringLiteral("#2b2b2b") : QStringLiteral("#f5f7f9"),
			 dark ? QStringLiteral("#555555") : QStringLiteral("#9fc4d0"),
			 dark ? QStringLiteral("#f0f0f0") : QStringLiteral("#202020") ) );

	m_identityLabel = new QLabel( this );
	m_identityLabel->setStyleSheet( QStringLiteral("font-size: 15px; font-weight: bold;") );
	m_statusLabel = new QLabel( this );
	m_statusLabel->setWordWrap( true );
	m_statusLabel->setStyleSheet( QStringLiteral("font-size: 10px;") );
	m_lockLabel = new QLabel( this );
	m_lockLabel->setAlignment( Qt::AlignRight | Qt::AlignVCenter );

	auto* header = new QHBoxLayout;
	header->addWidget( m_identityLabel, 1 );
	header->addWidget( m_lockLabel );

	m_seatEdit = new QLineEdit( this );
	m_seatEdit->setMaxLength( 16 );
	m_nameEdit = new QLineEdit( this );
	m_nameEdit->setMaxLength( 32 );

	// Persistent labels above the fields instead of vanishing placeholders
	// (IKEBACK-UI.md: labels always visible).
	auto* seatLabel = new QLabel( tr( "座位号" ), this );
	auto* nameLabel = new QLabel( tr( "姓名" ), this );
	const auto labelStyle = QStringLiteral("font-size: 9px; opacity: 0.8;");
	seatLabel->setStyleSheet( labelStyle );
	nameLabel->setStyleSheet( labelStyle );

	// No key field on the card: the bridge key stays on the teacher machine
	// and the teacher's phone. Students sign in with seat + name only.
	auto* seatColumn = new QVBoxLayout;
	seatColumn->setSpacing( 2 );
	seatColumn->addWidget( seatLabel );
	seatColumn->addWidget( m_seatEdit );
	auto* nameColumn = new QVBoxLayout;
	nameColumn->setSpacing( 2 );
	nameColumn->addWidget( nameLabel );
	nameColumn->addWidget( m_nameEdit );
	auto* inputLayout = new QHBoxLayout;
	inputLayout->addLayout( seatColumn );
	inputLayout->addLayout( nameColumn );

	m_loginButton = new QPushButton( tr( "签到登录" ), this );
	m_aiButton = new QPushButton( tr( "AI 助手" ), this );
	m_aiButton->setEnabled( false );
	m_scoreButton = new QPushButton( tr( "我的成绩" ), this );
	m_callButton = new QPushButton( tr( "呼叫老师" ), this );

	// The card exposes exactly TWO action buttons: score query and AI. The
	// sign-in and call-teacher controls stay available but are only shown while
	// nobody is signed in (call-teacher also lives in the context menu).
	auto* buttonLayout = new QHBoxLayout;
	buttonLayout->addWidget( m_loginButton );
	buttonLayout->addWidget( m_callButton );
	buttonLayout->addWidget( m_scoreButton );
	buttonLayout->addWidget( m_aiButton );

	auto* hint = new QLabel( tr( "右键菜单：呼叫老师 / 受保护退出" ), this );
	hint->setStyleSheet( QStringLiteral("font-size: 9px; opacity: 0.7;") );

	auto* mainLayout = new QVBoxLayout( this );
	mainLayout->setContentsMargins( 10, 8, 10, 8 );
	mainLayout->setSpacing( 6 );
	mainLayout->addLayout( header );
	mainLayout->addWidget( m_statusLabel );
	mainLayout->addLayout( inputLayout );
	mainLayout->addLayout( buttonLayout );
	mainLayout->addWidget( hint );

	setFixedWidth( 360 );
	adjustSize();
	placeOnScreen();
	updateVisibility();
	updateStatus();

	// "server 不退出就关闭不了": close requests are ignored and a watchdog
	// restores the card if anything else hides it (taskkill of the worker is
	// handled by the service watchdog, so this card lives as long as it does).
	m_guardTimer = new QTimer( this );
	m_guardTimer->setInterval( 1500 );
	connect( m_guardTimer, &QTimer::timeout, this, [this]() {
		if( m_hideAllowed == false && m_exitProtection && isVisible() == false ) { show(); }
	} );
	m_guardTimer->start();

	connect( m_loginButton, &QPushButton::clicked, this, &ClassroomClientWidget::handleLogin );
	connect( m_aiButton, &QPushButton::clicked, this, &ClassroomClientWidget::showAiChat );
	connect( m_nameEdit, &QLineEdit::returnPressed, this, &ClassroomClientWidget::handleLogin );
	connect( m_aiClient, &ClassroomAiClient::promptRequested,
			 this, &ClassroomClientWidget::aiPromptRequested );
	connect( m_scoreButton, &QPushButton::clicked, this, [this]() { Q_EMIT scoreRequested(); } );
	connect( m_callButton, &QPushButton::clicked, this, [this]() { Q_EMIT callTeacherRequested(); } );
}


void ClassroomClientWidget::updateVisibility()
{
	const auto signedIn = m_loggedIn;
	m_seatEdit->setVisible( signedIn == false );
	m_nameEdit->setVisible( signedIn == false );
	m_loginButton->setVisible( signedIn == false && m_allowSelfLogin );
	m_callButton->setVisible( signedIn == false );
	m_scoreButton->setVisible( true );
	m_aiButton->setVisible( true );
	adjustSize();
}


void ClassroomClientWidget::placeOnScreen()
{
	if( auto* screen = QGuiApplication::primaryScreen() )
	{
		const auto area = screen->availableGeometry();
		if( m_loggedIn )
		{
			// After sign-in the card moves to the top-right corner so it stays
			// visible without covering the student's work.
			move( area.right() - width() - 16, area.top() + 16 );
		}
		else
		{
			// Before sign-in the card is centered so every student sees it.
			move( area.center().x() - width() / 2, area.center().y() - height() / 2 );
		}
	}
}


void ClassroomClientWidget::setSettings( const QJsonObject& settings )
{
	m_allowSelfLogin = settings.value( Classroom::SettingKey::allowSelfLogin() ).toBool( true );
	m_aiEnabled = settings.value( Classroom::SettingKey::aiEnabled() ).toBool( true );
	m_exitProtection = settings.value( Classroom::SettingKey::exitProtection() ).toBool( true );
	m_exitRequiresAdmin = settings.value( Classroom::SettingKey::exitRequiresAdmin() ).toBool( true );
	m_exitAdminUsername = settings.value( Classroom::SettingKey::exitAdminUsername() ).toString();
	m_adminKeyHash = settings.value( Classroom::SettingKey::adminKeyHash() ).toString();

	// The API key, endpoint, model and system prompt stay on the teacher
	// machine. The student worker only submits a prompt through the bridge of
	// Ikeback feature messages (see ClassroomPlugin::handleFeatureMessage).
	ClassroomAiClient::Settings aiSettings;
	aiSettings.enabled = m_aiEnabled;
	m_aiClient->setProxyMode( true );
	m_aiClient->setSettings( aiSettings );

	m_aiButton->setEnabled( m_aiEnabled );
	m_loginButton->setEnabled( m_allowSelfLogin );
	updateVisibility();
	updateStatus();
}


void ClassroomClientWidget::setRoster( const QJsonObject& roster )
{
	m_roster = roster;
	const auto classes = roster.value( QStringLiteral("classes") ).toArray();
	if( m_className.isEmpty() && classes.isEmpty() == false )
	{
		m_className = classes.first().toObject().value( QStringLiteral("name") ).toString();
	}
	updateStatus();
}


void ClassroomClientWidget::setKeyConfigured( bool configured )
{
	m_keyConfigured = configured;
	updateStatus();
}


void ClassroomClientWidget::deliverAiReply( bool success, const QString& reply )
{
	if( m_aiClient ) { m_aiClient->deliverReply( success, reply ); }
}


void ClassroomClientWidget::setIdentity( const QString& className, const QString& seat, const QString& student )
{
	m_className = className;
	m_seat = seat;
	m_student = student;
	m_loggedIn = true;

	m_seatEdit->setText( seat );
	m_nameEdit->setText( student );
	m_loginButton->setText( tr( "签到登录" ) );
	updateVisibility();
	updateStatus();
	placeOnScreen();
	Q_EMIT identityChanged( m_seat, m_student, true );
}


void ClassroomClientWidget::clearIdentity()
{
	m_seat.clear();
	m_student.clear();
	m_loggedIn = false;
	m_seatEdit->clear();
	m_nameEdit->clear();
	m_loginButton->setText( tr( "签到登录" ) );
	updateVisibility();
	updateStatus();
	placeOnScreen();
	Q_EMIT identityChanged( {}, {}, false );
}


void ClassroomClientWidget::setLocked( bool locked )
{
	m_locked = locked;
	updateStatus();
}


void ClassroomClientWidget::updateStatus()
{
	m_statusLabel->setText( tr( "班级：%1  |  课堂：%2" ).
							arg( m_className.isEmpty() ? tr( "未选择" ) : m_className,
								 m_keyConfigured ? tr( "已开始" ) : tr( "未开始" ) ) );
	m_lockLabel->setText( m_locked ? tr( "已锁定" ) : tr( "未锁定" ) );

	// Prominent line: shield (client protection) + signed-in name + seat number.
	const auto shield = QString::fromUtf8( "\xF0\x9F\x9B\xA1" );
	m_identityLabel->setText( m_loggedIn ?
		QStringLiteral("%1 %2  |  %3 %4").arg( shield ).arg( m_student ).arg( tr( "座位" ) ).arg( m_seat ) :
		QStringLiteral("%1 %2").arg( shield ).arg( tr( "未签到" ) ) );
}


bool ClassroomClientWidget::rosterAccepts( const QString& seat, const QString& name ) const
{
	const auto classes = m_roster.value( QStringLiteral("classes") ).toArray();
	if( classes.isEmpty() ) { return true; }

	QJsonArray acceptedClasses;
	if( m_className.isEmpty() ) { acceptedClasses = classes; }
	else
	{
		for( const auto& value : classes )
		{
			if( value.toObject().value( QStringLiteral("name") ).toString() == m_className )
			{
				acceptedClasses.append( value );
			}
		}
	}
	if( acceptedClasses.isEmpty() ) { return true; }

	for( const auto& value : acceptedClasses )
	{
		for( const auto& studentValue : value.toObject().value( QStringLiteral("students") ).toArray() )
		{
			const auto student = studentValue.toObject();
			if( student.value( QStringLiteral("seat") ).toString() == seat &&
				student.value( QStringLiteral("name") ).toString() == name )
			{
				return true;
			}
		}
	}
	return false;
}


void ClassroomClientWidget::handleLogin()
{
	const auto seat = m_seatEdit->text().trimmed();
	const auto name = m_nameEdit->text().trimmed();

	if( seat.isEmpty() || name.isEmpty() )
	{
		QMessageBox::warning( this, windowTitle(), tr( "请同时输入座位号和姓名。" ) );
		return;
	}
	// The bridge key never reaches this process. Self sign-in is gated by the
	// teacher having started a lesson plus the roster check below.
	if( m_keyConfigured == false )
	{
		Q_EMIT eventOccurred( Classroom::EventType::keyFailed(), tr( "教师尚未开始上课" ), {} );
		QMessageBox::warning( this, windowTitle(), tr( "老师还没有开始上课，请稍候。" ) );
		return;
	}
	if( m_allowSelfLogin == false )
	{
		QMessageBox::information( this, windowTitle(), tr( "自助签到已关闭，请等待老师为您签到。" ) );
		return;
	}
	if( rosterAccepts( seat, name ) == false )
	{
		Q_EMIT eventOccurred( Classroom::EventType::keyFailed(), tr( "seat/name mismatch for %1" ).arg( seat ), {} );
		QMessageBox::warning( this, windowTitle(), tr( "座位号与姓名和班级名册不匹配。" ) );
		return;
	}
	if( m_loggedIn )
	{
		m_loggedIn = false;
		updateVisibility();
	}

	setIdentity( m_className, seat, name );
	setLocked( true );
	Q_EMIT eventOccurred( Classroom::EventType::login(), tr( "student signed in at seat %1" ).arg( seat ), {} );
}


void ClassroomClientWidget::openAiAssistant()
{
	showAiChat();
}


void ClassroomClientWidget::requestProtectedQuit()
{
	requestQuit();
}


void ClassroomClientWidget::requestScore()
{
	Q_EMIT scoreRequested();
}


void ClassroomClientWidget::showAiChat()
{
	if( m_aiEnabled == false )
	{
		QMessageBox::information( this, windowTitle(), tr( "老师已关闭 AI 助手。" ) );
		return;
	}

	if( m_aiChat == nullptr )
	{
		m_aiChat = new ClassroomAiChatWidget( m_aiClient, this );
		connect( m_aiChat, &ClassroomAiChatWidget::eventOccurred,
				 this, &ClassroomClientWidget::eventOccurred );
	}
	m_aiChat->setIdentityLabel( m_loggedIn ? QStringLiteral("%1 (%2)").arg( m_student, m_seat ) : tr( "未签到" ) );
	m_aiChat->show();
	m_aiChat->raise();
	IkebackCore::platform().coreFunctions().raiseWindow( m_aiChat, true );
}


void ClassroomClientWidget::requestQuit()
{
	if( m_exitProtection == false )
	{
		Q_EMIT eventOccurred( Classroom::EventType::quitAllowed(), tr( "exit protection disabled" ), {} );
		Q_EMIT quitAuthorized();
		return;
	}

	// 5.2.2: the super-admin key is the primary exit credential. The teacher
	// configures it once; students only ever see its hash. Windows
	// administrator credentials remain as a fallback for installs without a
	// configured key.
	QDialog dialog( this );
	dialog.setWindowTitle( tr( "受保护退出" ) );
	auto* form = new QFormLayout( &dialog );

	QLineEdit* keyEdit = nullptr;
	QLineEdit* username = nullptr;
	QLineEdit* password = nullptr;
	const auto useAdminKey = m_adminKeyHash.isEmpty() == false;
	if( useAdminKey )
	{
		keyEdit = new QLineEdit( &dialog );
		keyEdit->setEchoMode( QLineEdit::Password );
		keyEdit->setPlaceholderText( tr( "超级管理员密钥" ) );
		form->addRow( tr( "超级管理员密钥：" ), keyEdit );
	}
	else
	{
		username = new QLineEdit( &dialog );
		password = new QLineEdit( &dialog );
		username->setText( IkebackCore::platform().userFunctions().
						   queryCurrentUserProperty( PlatformUserFunctions::UserProperty::LoginName ) );
		password->setEchoMode( QLineEdit::Password );
		form->addRow( tr( "管理员账户：" ), username );
		form->addRow( tr( "管理员密码：" ), password );
	}
	auto* buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
	form->addRow( buttons );
	connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );

	if( dialog.exec() != QDialog::Accepted ) { return; }

	bool authorized = false;
	if( useAdminKey )
	{
		authorized = ClassroomSecrets::verifyAdminKey( keyEdit->text(), m_adminKeyHash );
	}
	else
	{
		// Fallback: Windows administrator credentials. exitRequiresAdmin is
		// kept in the configuration for backwards compatibility but no longer
		// weakens this.
		Q_UNUSED( m_exitRequiresAdmin )
		const auto expectedUser = m_exitAdminUsername.trimmed();
		const auto userMatches = expectedUser.isEmpty() ||
								 username->text().trimmed().compare( expectedUser, Qt::CaseInsensitive ) == 0;
		if( userMatches && password->text().isEmpty() == false )
		{
			const CryptoCore::PlaintextPassword plainPassword = password->text().toUtf8();
			authorized = IkebackCore::platform().userFunctions().authenticate( username->text().trimmed(), plainPassword );
		}
	}

	if( authorized )
	{
		Q_EMIT eventOccurred( Classroom::EventType::quitAllowed(), tr( "authorized client exit" ), {} );
		Q_EMIT quitAuthorized();
	}
	else
	{
		Q_EMIT eventOccurred( Classroom::EventType::quitAttempt(), tr( "rejected client exit" ), {} );
		QMessageBox::warning( this, windowTitle(),
							  useAdminKey ? tr( "超级管理员密钥不正确，本次尝试已记录。" )
										  : tr( "管理员凭据无效，本次尝试已记录。" ) );
	}
}


void ClassroomClientWidget::allowHide()
{
	m_hideAllowed = true;
	if( m_guardTimer ) { m_guardTimer->stop(); }
	hide();
}


void ClassroomClientWidget::reactivate()
{
	m_hideAllowed = false;
	if( m_guardTimer && m_guardTimer->isActive() == false ) { m_guardTimer->start(); }
	show();
	raise();
}


void ClassroomClientWidget::closeEvent( QCloseEvent* event )
{
	// 5.2.2: closing the card requires the same authentication as exiting the
	// program - a student must not be able to dismiss it. Only the teacher's
	// StopClient command may hide it without credentials.
	if( m_hideAllowed || m_exitProtection == false )
	{
		event->accept();
		hide();
		return;
	}
	event->ignore();
	show();
	raise();
	requestQuit();
}


void ClassroomClientWidget::contextMenuEvent( QContextMenuEvent* event )
{
	QMenu menu( this );
	menu.addAction( tr( "打开 AI 助手" ), this, &ClassroomClientWidget::showAiChat );
	menu.addAction( tr( "查看我的成绩" ), this, [this]() { Q_EMIT scoreRequested(); } );
	menu.addAction( tr( "呼叫老师" ), this, [this]() { Q_EMIT callTeacherRequested(); } );
	menu.addSeparator();
	menu.addAction( tr( "退出客户端（受保护）..." ), this, &ClassroomClientWidget::requestQuit );
	menu.exec( event->globalPos() );
}


void ClassroomClientWidget::mousePressEvent( QMouseEvent* event )
{
	if( event->button() == Qt::LeftButton )
	{
		m_dragPosition = mouseGlobalPoint( event ) - frameGeometry().topLeft();
	}
	QWidget::mousePressEvent( event );
}


void ClassroomClientWidget::mouseMoveEvent( QMouseEvent* event )
{
	if( event->buttons() & Qt::LeftButton )
	{
		move( mouseGlobalPoint( event ) - m_dragPosition );
	}
	QWidget::mouseMoveEvent( event );
}


void ClassroomClientWidget::setScoreHtml( const QString& html )
{
	if( m_scoreView == nullptr )
	{
		m_scoreView = new ClassroomScoreView( this );
	}
	m_scoreView->setHtml( html );
	m_scoreView->show();
	m_scoreView->raise();
}


void ClassroomClientWidget::setSilence( bool visible, const QString& text )
{
	if( m_silencePage == nullptr )
	{
		m_silencePage = new ClassroomSilencePage( this );
	}
	m_silencePage->setText( text );
	m_silencePage->setVisible( visible );
	if( visible ) { m_silencePage->raise(); }
}


void ClassroomClientWidget::showCheckIn()
{
	if( m_checkIn == nullptr )
	{
		m_checkIn = new ClassroomCheckInDialog( this );
		connect( m_checkIn, &ClassroomCheckInDialog::submitted,
				 this, &ClassroomClientWidget::checkInSubmitted );
	}
	m_checkIn->prepare( m_className, m_seat );
	m_checkIn->show();
	m_checkIn->raise();
}


ClassroomCheckInDialog::ClassroomCheckInDialog( QWidget* parent ) :
	QWidget( parent )
{
	setWindowTitle( tr( "课堂签到" ) );
	setWindowFlags( Qt::Tool | Qt::WindowStaysOnTopHint );
	resize( 460, 210 );
	setStyleSheet( QStringLiteral("background:#ffffff;") );

	m_info = new QLabel( this );
	m_info->setWordWrap( true );
	m_info->setStyleSheet( QStringLiteral("color:#1f2d3d; font-size:15px; font-weight:bold;") );

	m_nameEdit = new QLineEdit( this );
	connect( m_nameEdit, &QLineEdit::returnPressed, this, &ClassroomCheckInDialog::submit );

	auto* submitButton = new QPushButton( tr( "签到" ), this );
	connect( submitButton, &QPushButton::clicked, this, &ClassroomCheckInDialog::submit );

	auto* form = new QFormLayout;
	form->addRow( tr( "姓名：" ), m_nameEdit );

	auto* layout = new QVBoxLayout( this );
	layout->addWidget( m_info );
	layout->addLayout( form );
	layout->addWidget( submitButton );
}


void ClassroomCheckInDialog::prepare( const QString& className, const QString& seat )
{
	m_info->setText( tr( "%1 - 座位 %2：老师发起了签到，请输入姓名确认。" )
					 .arg( className, seat.isEmpty() ? tr( "未知" ) : seat ) );
	m_nameEdit->clear();
	m_nameEdit->setFocus();
}


void ClassroomCheckInDialog::submit()
{
	const auto name = m_nameEdit->text().trimmed();
	if( name.isEmpty() )
	{
		QMessageBox::information( this, windowTitle(), tr( "请先输入姓名。" ) );
		return;
	}
	Q_EMIT submitted( name );
	hide();
}


ClassroomScoreView::ClassroomScoreView( QWidget* parent ) :
	QWidget( parent )
{
	setWindowTitle( tr( "我的成绩" ) );
	setWindowFlags( Qt::Tool | Qt::WindowStaysOnTopHint );
	resize( 620, 620 );
	m_view = new QTextBrowser( this );
	m_view->setOpenExternalLinks( false );
	auto* layout = new QVBoxLayout( this );
	layout->addWidget( m_view );
	auto* closeButton = new QPushButton( tr( "关闭" ), this );
	layout->addWidget( closeButton );
	connect( closeButton, &QPushButton::clicked, this, &QWidget::hide );
}


void ClassroomScoreView::setHtml( const QString& html )
{
	m_view->setHtml( html );
}


ClassroomSilencePage::ClassroomSilencePage( QWidget* parent ) :
	QWidget( parent )
{
	setWindowFlags( Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
					Qt::WindowTransparentForInput );
	setWindowState( Qt::WindowFullScreen );
	setStyleSheet( QStringLiteral("background:#0d1b2a;") );
	m_label = new QLabel( this );
	m_label->setAlignment( Qt::AlignCenter );
	m_label->setWordWrap( true );
	m_label->setStyleSheet( QStringLiteral("color:#f0f0f0; font-size:48px; font-weight:bold;") );
	auto* layout = new QVBoxLayout( this );
	layout->addWidget( m_label );
}


void ClassroomSilencePage::setText( const QString& text )
{
	m_label->setText( text );
}


ClassroomAiChatWidget::ClassroomAiChatWidget( ClassroomAiClient* client, QWidget* parent ) :
	QWidget( parent ),
	m_aiClient( client )
{
	setWindowTitle( tr( "AI 助手" ) );
	setAttribute( Qt::WA_DeleteOnClose, false );
	keepOnTop( this );
	setFixedSize( 420, 460 );

	const auto dark = IkebackCore::useDarkMode();
	setStyleSheet( QStringLiteral(
		"QWidget#classroomAi { background: %1; border: 1px solid %2; border-radius: 6px; }"
		"QLabel, QTextBrowser { color: %3; }" ).
		arg( dark ? QStringLiteral("#2b2b2b") : QStringLiteral("#f5f7f9"),
			 dark ? QStringLiteral("#555555") : QStringLiteral("#9fc4d0"),
			 dark ? QStringLiteral("#f0f0f0") : QStringLiteral("#202020") ) );
	setObjectName( QStringLiteral("classroomAi") );

	auto* title = new QLabel( tr( "AI 助手（由老师配置）" ), this );
	title->setStyleSheet( QStringLiteral("font-weight: bold; padding: 4px;") );

	// The window is frameless, so it needs an explicit close button.
	auto* closeButton = new QPushButton( QStringLiteral("✕"), this );
	// 32 px gives the frameless window's only close affordance a usable
	// click target (24 px was easy to miss).
	closeButton->setFixedSize( 32, 32 );
	closeButton->setStyleSheet( QStringLiteral("QPushButton { border: none; font-size: 14px; }"
											   "QPushButton:hover { background: #e81123; color: white; }") );
	closeButton->setToolTip( tr( "关闭" ) );
	connect( closeButton, &QPushButton::clicked, this, &QWidget::hide );

	auto* titleRow = new QHBoxLayout;
	titleRow->addWidget( title, 1 );
	titleRow->addWidget( closeButton );

	m_view = new QTextBrowser( this );
	m_view->setReadOnly( true );
	m_view->setOpenExternalLinks( true );
	m_input = new QLineEdit( this );
	m_input->setPlaceholderText( tr( "输入你的问题..." ) );
	m_sendButton = new QPushButton( tr( "发送" ), this );

	auto* inputLayout = new QHBoxLayout;
	inputLayout->addWidget( m_input, 1 );
	inputLayout->addWidget( m_sendButton );

	auto* mainLayout = new QVBoxLayout( this );
	mainLayout->setContentsMargins( 8, 8, 8, 8 );
	mainLayout->addLayout( titleRow );
	mainLayout->addWidget( m_view, 1 );
	mainLayout->addLayout( inputLayout );

	appendLine( tr( "欢迎使用！你的提问和本次会话会记录在课堂日志中。" ) );

	connect( m_sendButton, &QPushButton::clicked, this, &ClassroomAiChatWidget::sendMessage );
	connect( m_input, &QLineEdit::returnPressed, this, &ClassroomAiChatWidget::sendMessage );
	connect( m_aiClient, &ClassroomAiClient::finished, this, [this]( bool success, const QString& reply ) {
		m_sendButton->setEnabled( true );
		if( success )
		{
			m_history.append( QVariantMap{ { QStringLiteral("role"), QStringLiteral("user") },
										   { QStringLiteral("content"), m_lastPrompt } } );
			m_history.append( QVariantMap{ { QStringLiteral("role"), QStringLiteral("assistant") },
										   { QStringLiteral("content"), reply } } );
			while( m_history.size() > 20 ) { m_history.removeFirst(); }
			appendLine( QStringLiteral("<b>AI:</b> %1").arg( reply.toHtmlEscaped() ) );
		}
		else
		{
			appendLine( QStringLiteral("<i>%1</i>").arg( reply.toHtmlEscaped() ) );
		}
	} );
}


void ClassroomAiChatWidget::setIdentityLabel( const QString& label )
{
	setWindowTitle( tr( "AI 助手 - %1" ).arg( label ) );
}


void ClassroomAiChatWidget::appendLine( const QString& html )
{
	m_view->append( html );
	m_view->verticalScrollBar()->setValue( m_view->verticalScrollBar()->maximum() );
}


void ClassroomAiChatWidget::sendMessage()
{
	const auto prompt = m_input->text().trimmed();
	if( prompt.isEmpty() || m_sendButton->isEnabled() == false ) { return; }

	m_lastPrompt = prompt;
	m_input->clear();
	m_sendButton->setEnabled( false );
	appendLine( QStringLiteral("<b>%1</b> %2").arg( tr( "我：" ), prompt.toHtmlEscaped() ) );
	Q_EMIT eventOccurred( Classroom::EventType::ai(), prompt.left( 200 ), QStringLiteral("AI") );
	m_aiClient->chat( m_history, prompt );
}


void ClassroomAiChatWidget::closeEvent( QCloseEvent* event )
{
	event->ignore();
	hide();
}


void ClassroomAiChatWidget::mousePressEvent( QMouseEvent* event )
{
	if( event->button() == Qt::LeftButton )
	{
		m_dragPosition = mouseGlobalPoint( event ) - frameGeometry().topLeft();
	}
	QWidget::mousePressEvent( event );
}


void ClassroomAiChatWidget::mouseMoveEvent( QMouseEvent* event )
{
	if( event->buttons() & Qt::LeftButton )
	{
		move( mouseGlobalPoint( event ) - m_dragPosition );
	}
	QWidget::mouseMoveEvent( event );
}


QPair<QString, QString> ClassroomMonitor::foregroundWindowInfo()
{
#ifdef Q_OS_WIN
	const auto window = GetForegroundWindow();
	if( window == nullptr ) { return {}; }

	wchar_t titleBuffer[512]{};
	const auto titleLength = GetWindowTextW( window, titleBuffer, 512 );
	const auto title = QString::fromWCharArray( titleBuffer, titleLength );

	DWORD processId = 0;
	GetWindowThreadProcessId( window, &processId );
	QString application;
	const auto process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId );
	if( process != nullptr )
	{
		wchar_t pathBuffer[MAX_PATH]{};
		DWORD pathLength = MAX_PATH;
		if( QueryFullProcessImageNameW( process, 0, pathBuffer, &pathLength ) )
		{
			application = QFileInfo( QString::fromWCharArray( pathBuffer ) ).fileName();
		}
		CloseHandle( process );
	}
	return { application, title };
#else
	static const auto xprop = QStandardPaths::findExecutable( QStringLiteral("xprop") );
	if( xprop.isEmpty() ) { return {}; }

	QProcess process;
	process.start( xprop, { QStringLiteral("-root"), QStringLiteral("_NET_ACTIVE_WINDOW") } );
	process.waitForFinished( 300 );
	const auto output = QString::fromUtf8( process.readAllStandardOutput() );
	const auto id = output.section( QLatin1Char('#'), 1 ).trimmed();
	if( id.isEmpty() || id == QStringLiteral("0x0") ) { return {}; }

	process.start( xprop, { QStringLiteral("-id"), id, QStringLiteral("WM_NAME"), QStringLiteral("WM_CLASS") } );
	process.waitForFinished( 300 );
	const auto details = QString::fromUtf8( process.readAllStandardOutput() );
	QString title;
	QString application;
	for( const auto& line : details.split( QLatin1Char('\n') ) )
	{
		if( line.contains( QStringLiteral("WM_NAME") ) ) { title = line.section( QLatin1Char('='), 1 ).trimmed(); }
		if( line.contains( QStringLiteral("WM_CLASS") ) )
		{
			const auto values = line.section( QLatin1Char('='), 1 ).split( QLatin1Char(',') );
			application = values.value( values.size() - 1 ).trimmed().remove( QLatin1Char('"') );
		}
	}
	return { application, title.remove( QLatin1Char('"') ) };
#endif
}


ClassroomMonitor::ClassroomMonitor( QObject* parent ) :
	QObject( parent ),
	m_timer( new QTimer( this ) )
{
	connect( m_timer, &QTimer::timeout, this, &ClassroomMonitor::sample );
}


void ClassroomMonitor::setSettings( const QJsonObject& settings )
{
	m_enabled = settings.value( Classroom::SettingKey::monitoringEnabled() ).toBool( true );
	m_captureWindowTitle = settings.value( Classroom::SettingKey::captureWindowTitle() ).toBool( true );
	m_captureProcess = settings.value( Classroom::SettingKey::captureProcess() ).toBool( true );

	const auto toStringList = [&settings]( const QString& key ) {
		QStringList rules;
		for( const auto& value : settings.value( key ).toArray() )
		{
			const auto rule = value.toString().trimmed();
			// Fuzzy substring matching plus taskkill /F /IM: a one-character
			// rule such as "e" would terminate dozens of unrelated processes.
			if( rule.size() >= 4 ) { rules.append( rule ); }
		}
		return rules;
	};
	m_blockedApps = toStringList( Classroom::SettingKey::blockedApps() );
	m_blockedWebsites = toStringList( Classroom::SettingKey::blockedWebsites() );

	const auto interval = settings.value( Classroom::SettingKey::captureInterval() ).toInt( 5 );
	m_timer->setInterval( std::max( 1, interval ) * 1000 );
	if( m_enabled ) { start(); }
	else { stop(); }
}


void ClassroomMonitor::start()
{
	if( m_enabled ) { m_timer->start(); }
}


void ClassroomMonitor::stop()
{
	m_timer->stop();
}


void ClassroomMonitor::sample()
{
	const auto info = foregroundWindowInfo();
	const auto title = m_captureWindowTitle ? info.second : QString{};
	const auto application = m_captureProcess ? info.first : QString{};
	if( title.isEmpty() && application.isEmpty() ) { return; }
	if( title.contains( Classroom::productName() ) || title.contains( tr( "AI 助手" ) ) ||
		title.contains( tr( "我的成绩" ) ) || title.contains( tr( "课堂签到" ) ) ||
		title.contains( QStringLiteral("iKeback") ) ) { return; }

	const QString key = application + QLatin1Char('|') + title;
	if( key == m_lastSample ) { return; }
	m_lastSample = key;

	static const QStringList browsers{ QStringLiteral("chrome"), QStringLiteral("msedge"), QStringLiteral("firefox"),
									  QStringLiteral("iexplore"), QStringLiteral("opera"), QStringLiteral("brave"),
									  QStringLiteral("360"), QStringLiteral("qqbrowser"), QStringLiteral("safari") };
	bool browser = false;
	for( const auto& candidate : browsers )
	{
		if( application.toLower().contains( candidate ) ) { browser = true; break; }
	}

	// Fuzzy blocking: every rule is a case-insensitive substring of either the
	// foreground process name (apps) or the browser window title (websites).
	const auto matchRule = []( const QString& value, const QStringList& rules ) {
		for( const auto& rule : rules )
		{
			if( rule.isEmpty() == false && value.contains( rule, Qt::CaseInsensitive ) ) { return rule; }
		}
		return QString{};
	};
	const auto appRule = matchRule( application, m_blockedApps );
	const auto webRule = browser ?
		matchRule( title + QLatin1Char(' ') + application, m_blockedWebsites ) : QString{};
	if( appRule.isEmpty() == false || webRule.isEmpty() == false )
	{
		Q_EMIT blockedActivity( application, title,
								appRule.isEmpty() ? webRule : appRule,
								appRule.isEmpty() );
	}

	Q_EMIT activity( application, title, browser );
}
