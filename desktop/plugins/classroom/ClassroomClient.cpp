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
#include <QImage>
#include <QJsonArray>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCursor>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVariantMap>
#include <QVBoxLayout>

#include "ClassroomAiClient.h"
#include "ClassroomClient.h"
#include "ClassroomProtocol.h"
#include "ClassroomSecrets.h"
#include "ClassroomShop.h"
#include "ClassroomTheme.h"
#include "CryptoCore.h"
#include "PlatformCoreFunctions.h"
#include "PlatformInputDeviceFunctions.h"
#include "PlatformPluginInterface.h"
#include "PlatformUserFunctions.h"
#include "IkebackCore.h"


static void keepOnTop( QWidget* widget )
{
	widget->setWindowFlags( Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
							Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint );
	IkebackCore::platform().coreFunctions().raiseWindow( widget, true );
}


static QScreen* screenForWidget( QWidget* widget )
{
	auto* screen = QGuiApplication::screenAt( QCursor::pos() );
	if( screen == nullptr && widget ) { screen = widget->screen(); }
	return screen ? screen : QGuiApplication::primaryScreen();
}


static void centerOnScreen( QWidget* widget )
{
	if( widget == nullptr ) { return; }
	widget->adjustSize();
	auto* screen = screenForWidget( widget );
	if( screen == nullptr ) { return; }
	const auto area = screen->availableGeometry();
	const auto frameSize = widget->frameGeometry().size();
	const auto clientSize = widget->size();
	const auto frameAdjust = frameSize - clientSize;
	const auto maximumClientSize = QSize( qMax( 1, area.width() - frameAdjust.width() ),
										 qMax( 1, area.height() - frameAdjust.height() ) );
	widget->resize( clientSize.boundedTo( maximumClientSize ) );
	const auto centeredFrameSize = widget->frameGeometry().size();
	widget->move( area.center().x() - centeredFrameSize.width() / 2,
				  area.center().y() - centeredFrameSize.height() / 2 );
}


static void showCentered( QWidget* widget )
{
	centerOnScreen( widget );
	widget->show();
	widget->raise();
	IkebackCore::platform().coreFunctions().raiseWindow( widget, true );
}


static QPoint clampedOnScreen( QWidget* widget, QPoint position )
{
	auto* screen = screenForWidget( widget );
	if( screen == nullptr ) { return position; }
	const auto area = screen->availableGeometry();
	const int right = qMax( area.left(), area.right() - widget->width() + 1 );
	const int bottom = qMax( area.top(), area.bottom() - widget->height() + 1 );
	return { qBound( area.left(), position.x(), right ), qBound( area.top(), position.y(), bottom ) };
}


//! The student floating card is deliberately NOT always-on-top: it must not
//! cover the student's work. Only the popups opened from it stay on top.
static void keepFloating( QWidget* widget )
{
	widget->setWindowFlags( Qt::Tool | Qt::CustomizeWindowHint | Qt::FramelessWindowHint );
	IkebackCore::platform().coreFunctions().raiseWindow( widget, false );
}


#ifdef Q_OS_WIN
//! 网址命中时只关掉当前浏览器标签页，而不是 taskkill 整个浏览器。
static void closeForegroundBrowserTab()
{
	keybd_event( VK_CONTROL, 0, 0, 0 );
	keybd_event( static_cast<BYTE>( 'W' ), 0, 0, 0 );
	keybd_event( static_cast<BYTE>( 'W' ), 0, KEYEVENTF_KEYUP, 0 );
	keybd_event( VK_CONTROL, 0, KEYEVENTF_KEYUP, 0 );
}
#endif


static QPoint mouseGlobalPoint( QMouseEvent* event )
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return event->globalPosition().toPoint();
#else
	return event->globalPos();
#endif
}


//! 锁定输入采用引用计数：肃静页与「锁定全部」可能同时生效，
//! 任意一个解除时不能把另一个的输入锁也放开。
static int s_inputLockCount = 0;

static void lockStudentInput()
{
	if( s_inputLockCount++ == 0 )
	{
		IkebackCore::platform().coreFunctions().setSystemUiState( false );
		IkebackCore::platform().inputDeviceFunctions().disableInputDevices();
		QGuiApplication::setOverrideCursor( Qt::BlankCursor );
	}
}


static void unlockStudentInput()
{
	if( s_inputLockCount == 0 ) { return; }
	if( --s_inputLockCount == 0 )
	{
		IkebackCore::platform().inputDeviceFunctions().enableInputDevices();
		IkebackCore::platform().coreFunctions().setSystemUiState( true );
		QGuiApplication::restoreOverrideCursor();
	}
}


//! 肃静页必须显示中文；旧配置里残留的英文文案一律回退为「肃静」。
static QString chineseSilenceText( const QString& text )
{
	const auto trimmed = text.trimmed();
	for( const auto ch : trimmed )
	{
		const auto code = ch.unicode();
		if( code >= 0x4E00 && code <= 0x9FFF ) { return trimmed; }
	}
	return QStringLiteral("肃静");
}


ClassroomClientWidget::ClassroomClientWidget( QWidget* parent ) :
	QWidget( parent ),
	m_aiClient( new ClassroomAiClient( this ) )
{
	setObjectName( QStringLiteral("classroomClient") );
	setWindowTitle( Classroom::productName() );
	setAttribute( Qt::WA_DeleteOnClose, false );
	keepFloating( this );
	Classroom::Theme::apply( this );

	m_identityLabel = new QLabel( this );
	m_identityLabel->setProperty( "role", QStringLiteral("title") );
	m_statusLabel = new QLabel( this );
	m_statusLabel->setWordWrap( true );
	m_statusLabel->setProperty( "role", QStringLiteral("status") );
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
	m_seatLabel = seatLabel;
	m_nameLabel = nameLabel;
	seatLabel->setProperty( "role", QStringLiteral("hint") );
	nameLabel->setProperty( "role", QStringLiteral("hint") );

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
	m_shopButton = new QPushButton( tr( "班级商城" ), this );
	m_callButton = new QPushButton( tr( "呼叫老师" ), this );
	m_quitButton = new QPushButton( tr( "退出学生端" ), this );
	m_quitButton->setProperty( "quiet", true );

	auto* buttonLayout = new QGridLayout;
	buttonLayout->setHorizontalSpacing( 6 );
	buttonLayout->setVerticalSpacing( 6 );
	buttonLayout->addWidget( m_loginButton, 0, 0 );
	buttonLayout->addWidget( m_callButton, 0, 1 );
	buttonLayout->addWidget( m_scoreButton, 0, 2 );
	buttonLayout->addWidget( m_shopButton, 1, 0 );
	buttonLayout->addWidget( m_aiButton, 1, 1 );
	buttonLayout->addWidget( m_quitButton, 1, 2 );
	for( int column = 0; column < 3; ++column ) { buttonLayout->setColumnStretch( column, 1 ); }

	auto* hint = new QLabel( tr( "退出与卸载是否受保护，由教师端统一管理。" ), this );
	hint->setProperty( "role", QStringLiteral("hint") );

	auto* mainLayout = new QVBoxLayout( this );
	mainLayout->setContentsMargins( 10, 8, 10, 8 );
	mainLayout->setSpacing( 6 );
	mainLayout->addLayout( header );
	mainLayout->addWidget( m_statusLabel );
	mainLayout->addLayout( inputLayout );
	mainLayout->addLayout( buttonLayout );
	mainLayout->addWidget( hint );

	setFixedWidth( 380 );
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
	connect( m_shopButton, &QPushButton::clicked, this, &ClassroomClientWidget::openShop );
	connect( m_quitButton, &QPushButton::clicked, this, &ClassroomClientWidget::requestQuit );
}


void ClassroomClientWidget::updateVisibility()
{
	const auto signedIn = m_loggedIn;
	m_seatEdit->setVisible( signedIn == false );
	m_nameEdit->setVisible( signedIn == false );
	m_seatLabel->setVisible( signedIn == false );
	m_nameLabel->setVisible( signedIn == false );
	m_loginButton->setVisible( signedIn == false && m_allowSelfLogin );
	// 呼叫老师必须在签到前后都能点：老师端靠这条消息定位学生座位。
	m_callButton->setVisible( true );
	m_scoreButton->setVisible( true );
	m_shopButton->setVisible( true );
	m_aiButton->setVisible( true );
	m_quitButton->setVisible( true );
	adjustSize();
}


void ClassroomClientWidget::placeOnScreen()
{
	auto* screen = screenForWidget( this );
	if( screen == nullptr ) { return; }
	const auto area = screen->availableGeometry();
	QPoint position;
	if( m_loggedIn )
	{
		position = { area.right() - width() - 16, area.top() + 16 };
	}
	else
	{
		position = { area.center().x() - width() / 2, area.center().y() - height() / 2 };
	}
	move( clampedOnScreen( this, position ) );
}


void ClassroomClientWidget::setSettings( const QJsonObject& settings )
{
	m_allowSelfLogin = settings.value( Classroom::SettingKey::allowSelfLogin() ).toBool( true );
	m_aiEnabled = settings.value( Classroom::SettingKey::aiEnabled() ).toBool( true );
	m_exitProtection = settings.value( Classroom::SettingKey::exitProtection() ).toBool( true );
	m_exitAdminUsername = settings.value( Classroom::SettingKey::exitAdminUsername() ).toString();
	m_adminKeyHash = settings.value( Classroom::SettingKey::adminKeyHash() ).toString();
	if( m_adminKeyHash.isEmpty() ) { m_adminKeyHash = ClassroomSecrets::defaultAdminKeyHash(); }

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
	if( m_shopView )
	{
		m_shopView->setIdentity( seat, student );
		m_shopView->setSignedIn( true );
	}
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
	if( m_shopView ) { m_shopView->setSignedIn( false ); }
	Q_EMIT identityChanged( {}, {}, false );
}


void ClassroomClientWidget::setLocked( bool locked )
{
	m_locked = locked;
	updateStatus();
}


void ClassroomClientWidget::setInputLocked( bool locked )
{
	if( locked )
	{
		if( m_lockOverlay == nullptr )
		{
			m_lockOverlay = new ClassroomSilencePage( this );
			m_lockOverlay->setText( tr( "已锁定" ) );
		}
		m_lockOverlay->setVisible( true );
		m_lockOverlay->raise();
		m_lockOverlay->activateWindow();
		m_lockOverlay->setInputLocked( true );
	}
	else if( m_lockOverlay )
	{
		m_lockOverlay->setInputLocked( false );
		m_lockOverlay->setVisible( false );
	}
}


void ClassroomClientWidget::updateStatus()
{
	m_statusLabel->setText( tr( "班级：%1  |  课堂：%2" ).
							arg( m_className.isEmpty() ? tr( "未选择" ) : m_className,
								 m_keyConfigured ? tr( "已开始" ) : tr( "未开始" ) ) );
	m_lockLabel->setText( m_exitProtection ? tr( "已保护" ) : tr( "未保护" ) );

	const auto protection = m_exitProtection ? tr( "已保护" ) : tr( "未保护" );
	m_identityLabel->setText( m_loggedIn ?
		QStringLiteral("%1 · %2 · 座位 %3").arg( protection ).arg( m_student ).arg( m_seat ) :
		QStringLiteral("%1 · 未签到").arg( protection ) );
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
	// 学生只需姓名 + 座位号即可自助签到；名单匹配在下方完成，不再要求老师先下发课堂密钥。
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


ClassroomShopView* ClassroomClientWidget::ensureShopView()
{
	if( m_shopView == nullptr )
	{
		m_shopView = new ClassroomShopView( this );
		connect( m_shopView, &ClassroomShopView::refreshRequested,
				 this, &ClassroomClientWidget::shopRequested );
		connect( m_shopView, &ClassroomShopView::redeemRequested,
				 this, &ClassroomClientWidget::shopRedeemRequested );
		connect( m_shopView, &ClassroomShopView::bidRequested,
				 this, &ClassroomClientWidget::shopBidRequested );
		connect( m_shopView, &ClassroomShopView::imageRequested,
				 this, &ClassroomClientWidget::shopImageRequested );
		connect( m_shopView, &ClassroomShopView::noticeRaised, this, [this]( const QString& text ) {
			QMessageBox::information( this, m_shopView->windowTitle(), text );
		} );
	}
	m_shopView->setIdentity( m_seat, m_student );
	m_shopView->setSignedIn( m_loggedIn );
	return m_shopView;
}


void ClassroomClientWidget::openShop()
{
	auto* view = ensureShopView();
	showCentered( view );
	Q_EMIT shopRequested();
}


void ClassroomClientWidget::applyShopState( const QJsonObject& catalog, int balance )
{
	if( m_loggedIn == false ) { return; }
	ensureShopView()->applyCatalog( catalog, balance );
}


void ClassroomClientWidget::applyShopImage( const QString& productId, const QString& imageId,
											const QByteArray& image )
{
	if( m_shopView ) { m_shopView->applyImage( productId, imageId, image ); }
}


void ClassroomClientWidget::shopReply( bool ok, const QString& message, int balance,
									   const QJsonObject& catalog )
{
	if( m_shopView )
	{
		m_shopView->applyReply( ok, message, balance, catalog );
		if( m_shopView->isVisible() == false )
		{
			QMessageBox::information( this, m_shopView->windowTitle(), message );
		}
	}
	else
	{
		QMessageBox::information( this, windowTitle(), message );
	}
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
	showCentered( m_aiChat );
}


void ClassroomClientWidget::requestQuit()
{
	if( m_quitDialogOpen )
	{
		return;
	}
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
	m_quitDialogOpen = true;
	struct QuitDialogGuard
	{
		bool& flag;
		~QuitDialogGuard() { flag = false; }
	} guard{ m_quitDialogOpen };

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
		keyEdit->setPlaceholderText( tr( "管理员密码" ) );
		form->addRow( tr( "管理员密码：" ), keyEdit );
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
	Classroom::Theme::apply( &dialog );
	centerOnScreen( &dialog );

	if( dialog.exec() != QDialog::Accepted ) { return; }

	bool authorized = false;
	if( useAdminKey )
	{
		authorized = ClassroomSecrets::verifyAdminKey( keyEdit->text(), m_adminKeyHash );
	}
	else
	{
		// Fallback: Windows administrator credentials. The configured admin
		// account name is honoured when it is set; the password is always
		// verified against Windows itself.
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
							  useAdminKey ? tr( "管理员密码不正确，本次尝试已记录。" )
										  : tr( "管理员凭据无效，本次尝试已记录。" ) );
	}
}


void ClassroomClientWidget::allowHide()
{
	m_hideAllowed = true;
	setInputLocked( false );
	if( m_silencePage )
	{
		m_silencePage->setInputLocked( false );
		m_silencePage->hide();
	}
	if( m_guardTimer ) { m_guardTimer->stop(); }
	hide();
}


void ClassroomClientWidget::reactivate()
{
	m_hideAllowed = false;
	if( m_guardTimer && m_guardTimer->isActive() == false ) { m_guardTimer->start(); }
	placeOnScreen();
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
	// 受保护时关闭请求只拦截，退出必须使用卡片上的“退出学生端”按钮。
	event->ignore();
	show();
	raise();
	m_statusLabel->setText( tr( "卡片受保护，不能直接关闭。请使用“退出学生端”并输入管理员密码。" ) );
	QTimer::singleShot( 2500, this, &ClassroomClientWidget::updateStatus );
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
		move( clampedOnScreen( this, mouseGlobalPoint( event ) - m_dragPosition ) );
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
	showCentered( m_scoreView );
}


void ClassroomClientWidget::setSilence( bool visible, const QString& text )
{
	if( m_silencePage == nullptr )
	{
		m_silencePage = new ClassroomSilencePage( this );
	}
	m_silencePage->setText( text );
	if( visible == false ) { m_silencePage->setInputLocked( false ); }
	m_silencePage->setVisible( visible );
	if( visible )
	{
		m_silencePage->raise();
		m_silencePage->activateWindow();
		m_silencePage->setInputLocked( true );
	}
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
	showCentered( m_checkIn );
}


ClassroomCheckInDialog::ClassroomCheckInDialog( QWidget* parent ) :
	QWidget( parent )
{
	setWindowTitle( tr( "课堂签到" ) );
	setObjectName( QStringLiteral("classroomCheckIn") );
	setWindowFlags( Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowSystemMenuHint |
					Qt::WindowCloseButtonHint );
	resize( 460, 210 );
	Classroom::Theme::apply( this );

	m_info = new QLabel( this );
	m_info->setWordWrap( true );
	m_info->setProperty( "role", QStringLiteral("heading") );

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
	setObjectName( QStringLiteral("classroomScore") );
	setWindowFlags( Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowSystemMenuHint |
					Qt::WindowCloseButtonHint );
	resize( 620, 620 );
	Classroom::Theme::apply( this );
	m_view = new QTextBrowser( this );
	m_view->setOpenExternalLinks( false );
	auto* layout = new QVBoxLayout( this );
	layout->addWidget( m_view );
}


void ClassroomScoreView::setHtml( const QString& html )
{
	m_view->setHtml( html );
}


ClassroomSilencePage::ClassroomSilencePage( QWidget* parent ) :
	QWidget( parent )
{
	// 覆盖所有显示器，且不再透传输入。
	setWindowFlags( Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint );
	setObjectName( QStringLiteral("classroomSilence") );
	QRect allScreens;
	for( auto* screen : QGuiApplication::screens() )
	{
		if( screen ) { allScreens = allScreens.united( screen->geometry() ); }
	}
	if( allScreens.isEmpty() == false ) { setGeometry( allScreens ); }
	setWindowState( Qt::WindowFullScreen );
	setFocusPolicy( Qt::StrongFocus );
	Classroom::Theme::apply( this );
	m_label = new QLabel( this );
	m_label->setAlignment( Qt::AlignCenter );
	m_label->setWordWrap( true );
	m_label->setText( QStringLiteral("Silence") );
	m_label->setProperty( "role", QStringLiteral("title") );
	auto* layout = new QVBoxLayout( this );
	layout->addWidget( m_label );
}


ClassroomSilencePage::~ClassroomSilencePage()
{
	setInputLocked( false );
}


void ClassroomSilencePage::setText( const QString& text )
{
	const auto value = chineseSilenceText( text );
	m_label->setText( value.isEmpty() ? tr( "Silence" ) : value );
}


void ClassroomSilencePage::setInputLocked( bool locked )
{
	if( locked == m_inputLocked ) { return; }
	if( locked )
	{
		lockStudentInput();
		setFocus();
		grabMouse();
		grabKeyboard();
	}
	else
	{
		releaseMouse();
		releaseKeyboard();
		unlockStudentInput();
	}
	m_inputLocked = locked;
}


ClassroomAiChatWidget::ClassroomAiChatWidget( ClassroomAiClient* client, QWidget* parent ) :
	QWidget( parent ),
	m_aiClient( client )
{
	setWindowTitle( tr( "AI 助手" ) );
	setAttribute( Qt::WA_DeleteOnClose, false );
	keepOnTop( this );
	setFixedSize( 420, 460 );
	setObjectName( QStringLiteral("classroomAi") );
	Classroom::Theme::apply( this );

	auto* title = new QLabel( tr( "AI 助手（由老师配置）" ), this );
	title->setProperty( "role", QStringLiteral("title") );

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
	mainLayout->addWidget( title );
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
		move( clampedOnScreen( this, mouseGlobalPoint( event ) - m_dragPosition ) );
	}
	QWidget::mouseMoveEvent( event );
}


namespace {

//! Removes and deletes every widget of a dynamic list layout.
static void clearWidgetList( QLayout* layout )
{
	while( layout->count() > 0 )
	{
		auto* item = layout->takeAt( 0 );
		if( item->widget() ) { item->widget()->hide(); item->widget()->deleteLater(); }
		delete item;
	}
}

} // namespace


ClassroomShopView::ClassroomShopView( QWidget* parent ) :
	QWidget( parent )
{
	setWindowTitle( Classroom::productName() + QLatin1Char(' ') + tr( "班级商城" ) );
	setAttribute( Qt::WA_DeleteOnClose, false );
	keepOnTop( this );
	setFixedSize( 620, 660 );

	setObjectName( QStringLiteral("classroomShop") );
	Classroom::Theme::apply( this );

	auto* title = new QLabel( tr( "班级商城（价高者得 · 积分兑换）" ), this );
	title->setProperty( "role", QStringLiteral("title") );

	m_balanceLabel = new QLabel( tr( "积分余额：..." ), this );
	m_balanceLabel->setProperty( "role", QStringLiteral("balance") );
	m_statusLabel = new QLabel( this );
	m_statusLabel->setWordWrap( true );
	m_statusLabel->setProperty( "role", QStringLiteral("status") );
	auto* refreshButton = new QPushButton( tr( "刷新" ), this );
	connect( refreshButton, &QPushButton::clicked, this, [this]() {
		m_actionTimer->stop();
		m_requestedImages.clear();
		m_failedImages.clear();
		Q_EMIT refreshRequested();
	} );

	m_actionTimer = new QTimer( this );
	m_actionTimer->setSingleShot( true );
	connect( m_actionTimer, &QTimer::timeout, this, [this]() {
		m_statusLabel->setText( tr( "请求超时，请先刷新确认状态，再决定是否重试。" ) );
		m_statusLabel->setProperty( "state", QStringLiteral("error") );
	} );

	auto* statusRow = new QHBoxLayout;
	statusRow->addWidget( m_balanceLabel );
	statusRow->addWidget( m_statusLabel, 1 );
	statusRow->addWidget( refreshButton );

	m_tabs = new QTabWidget( this );

	auto* productsScroller = new QScrollArea( this );
	productsScroller->setWidgetResizable( true );
	productsScroller->setFrameShape( QFrame::NoFrame );
	m_productsArea = new QWidget( productsScroller );
	auto* productsLayout = new QVBoxLayout( m_productsArea );
	productsLayout->setContentsMargins( 6, 6, 6, 6 );
	productsLayout->setSpacing( 8 );
	productsScroller->setWidget( m_productsArea );

	auto* auctionsScroller = new QScrollArea( this );
	auctionsScroller->setWidgetResizable( true );
	auctionsScroller->setFrameShape( QFrame::NoFrame );
	m_auctionsArea = new QWidget( auctionsScroller );
	auto* auctionsLayout = new QVBoxLayout( m_auctionsArea );
	auctionsLayout->setContentsMargins( 6, 6, 6, 6 );
	auctionsLayout->setSpacing( 8 );
	auctionsScroller->setWidget( m_auctionsArea );

	m_tabs->addTab( productsScroller, tr( "商品" ) );
	m_tabs->addTab( auctionsScroller, tr( "竞拍" ) );

	auto* mainLayout = new QVBoxLayout( this );
	mainLayout->setContentsMargins( 8, 8, 8, 8 );
	mainLayout->addWidget( title );
	mainLayout->addLayout( statusRow );
	mainLayout->addWidget( m_tabs, 1 );

	rebuild();
}


void ClassroomShopView::applyCatalog( const QJsonObject& catalog, int balance )
{
	// Detect what happened to the auctions the student took part in, so a
	// student who never opened the window still learns about it through the
	// card notice.
	const auto auctions = catalog.value( QStringLiteral("auctions") ).toArray();
	for( const auto& value : auctions )
	{
		const auto auction = value.toObject();
		const auto id = auction.value( QStringLiteral("id") ).toString();
		const auto open = auction.value( QStringLiteral("open") ).toBool( true );
		const auto leader = auction.value( QStringLiteral("bidderSeat") ).toString();
		const auto previous = m_auctionStates.value( id );
		const auto name = auction.value( QStringLiteral("name") ).toString();
		if( previous.first && open == false )
		{
			// 成交/流拍：只有实际拍到的人会收到提示。
			if( m_signedIn && leader == m_seat && leader.isEmpty() == false )
			{
				Q_EMIT noticeRaised( tr( "你拍得了「%1」，请等待老师发货！" ).arg( name ) );
			}
		}
		else if( previous.first && open && previous.second.isEmpty() == false &&
				 previous.second == m_seat && leader != m_seat )
		{
			Q_EMIT noticeRaised( tr( "「%1」被 %2 出价 %3 分超越了，去看看吗？" )
									 .arg( name, auction.value( QStringLiteral("bidderStudent") ).toString() )
									 .arg( auction.value( QStringLiteral("currentBid") ).toInt() ) );
		}
		m_auctionStates.insert( id, { open, leader } );
	}
	while( m_auctionStates.size() > 64 ) { m_auctionStates.erase( m_auctionStates.begin() ); }

	m_catalog = catalog;
	if( balance >= 0 )
	{
		m_balance = balance;
		if( m_actionPending )
		{
			m_actionTimer->stop();
			m_actionPending = false;
		}
	}
	else if( catalog.isEmpty() ) { m_balance = -1; }
	QStringList productIds;
	const auto products = catalog.value( QStringLiteral("products") ).toArray();
	for( const auto& value : products ) { productIds.append( value.toObject().value( QStringLiteral("id") ).toString() ); }
	for( auto it = m_requestedImages.begin(); it != m_requestedImages.end(); )
	{
		if( productIds.contains( it.key() ) == false ) { it = m_requestedImages.erase( it ); }
		else { ++it; }
	}
	for( auto it = m_failedImages.begin(); it != m_failedImages.end(); )
	{
		if( productIds.contains( it.key() ) == false ) { it = m_failedImages.erase( it ); }
		else { ++it; }
	}
	for( auto it = m_images.begin(); it != m_images.end(); )
	{
		if( productIds.contains( it.key() ) == false ) { it = m_images.erase( it ); }
		else { ++it; }
	}
	rebuild();
	requestMissingImages();
}


void ClassroomShopView::applyReply( bool ok, const QString& message, int balance,
									const QJsonObject& catalog )
{
	m_actionTimer->stop();
	m_actionPending = false;
	m_balance = balance;
	m_balanceLabel->setText( m_signedIn && m_balance >= 0 ?
		tr( "积分余额：%1 · 可用：%2（保留 %3 分）" ).
			arg( m_balance ).arg( ClassroomShopBook::spendableScore( m_balance ) ).
			arg( ClassroomShopBook::ProtectedScore )
															  : tr( "请先签到后再使用商场" ) );
	m_statusLabel->setText( message );
	m_statusLabel->setProperty( "state", ok ? QStringLiteral("success") : QStringLiteral("error") );
	if( catalog.isEmpty() == false ) { applyCatalog( catalog, m_balance ); }
	else { rebuild(); }
}


void ClassroomShopView::setIdentity( const QString& seat, const QString& student )
{
	if( seat != m_seat || student != m_student )
	{
		if( m_actionTimer ) { m_actionTimer->stop(); }
		m_actionPending = false;
		m_auctionStates.clear();
		m_images.clear();
		m_requestedImages.clear();
		m_failedImages.clear();
		m_catalog = QJsonObject{};
		m_balance = -1;
	}
	m_seat = seat;
	m_student = student;
}


void ClassroomShopView::setSignedIn( bool signedIn )
{
	if( signedIn != m_signedIn && signedIn == false )
	{
		if( m_actionTimer ) { m_actionTimer->stop(); }
		m_actionPending = false;
		m_balance = -1;
		m_auctionStates.clear();
		m_images.clear();
		m_requestedImages.clear();
		m_failedImages.clear();
		m_catalog = QJsonObject{};
	}
	m_signedIn = signedIn;
	rebuild();
}


QImage ClassroomShopView::decodeImage( const QByteArray& image )
{
	if( image.isEmpty() ) { return {}; }
	return QImage::fromData( image );
}


void ClassroomShopView::applyImage( const QString& productId, const QString& imageId,
									const QByteArray& image )
{
	const auto products = m_catalog.value( QStringLiteral("products") ).toArray();
	for( const auto& value : products )
	{
		const auto product = value.toObject();
		if( product.value( QStringLiteral("id") ).toString() != productId ) { continue; }
		if( product.value( QStringLiteral("imageId") ).toString() != imageId ||
			product.value( QStringLiteral("hasImage") ).toBool() == false )
		{
			m_requestedImages.remove( productId );
			requestMissingImages();
			return;
		}
		const auto decoded = decodeImage( image );
		m_images.insert( productId, { imageId, decoded } );
		m_requestedImages.remove( productId );
		if( decoded.isNull() )
		{
			m_failedImages.insert( productId, imageId );
			m_statusLabel->setText( tr( "商品图片加载失败，可点击刷新重试。" ) );
			requestMissingImages();
			return;
		}
		m_failedImages.remove( productId );
		rebuild();
		requestMissingImages();
		return;
	}
	m_requestedImages.remove( productId );
}


void ClassroomShopView::requestMissingImages()
{
	if( isVisible() == false || m_signedIn == false ||
		static_cast<int>( m_requestedImages.size() ) >= 6 )
	{
		return;
	}
	const auto products = m_catalog.value( QStringLiteral("products") ).toArray();
	for( const auto& value : products )
	{
		if( static_cast<int>( m_requestedImages.size() ) >= 6 ) { break; }
		const auto product = value.toObject();
		if( product.value( QStringLiteral("hasImage") ).toBool() == false ) { continue; }
		const auto productId = product.value( QStringLiteral("id") ).toString();
		const auto imageId = product.value( QStringLiteral("imageId") ).toString();
		const auto cached = m_images.value( productId );
		if( cached.first == imageId && cached.second.isNull() == false ) { continue; }
		if( m_failedImages.value( productId ) == imageId ) { continue; }
		if( m_requestedImages.value( productId ) == imageId ) { continue; }
		m_requestedImages.insert( productId, imageId );
		Q_EMIT imageRequested( productId, imageId );
	}
}


void ClassroomShopView::showEvent( QShowEvent* event )
{
	QWidget::showEvent( event );
	centerOnScreen( this );
	requestMissingImages();
}


QWidget* ClassroomShopView::buildProductCard( const QJsonObject& product )
{
	const auto name = product.value( QStringLiteral("name") ).toString();
	const auto description = product.value( QStringLiteral("description") ).toString();
	const auto price = product.value( QStringLiteral("price") ).toInt();
	const auto stock = product.value( QStringLiteral("stock") ).toInt();
	const auto open = m_catalog.value( QStringLiteral("open") ).toBool( true );
	const auto productId = product.value( QStringLiteral("id") ).toString();
	const auto imageId = product.value( QStringLiteral("imageId") ).toString();

	auto* card = new QWidget;
	card->setProperty( "shopCard", true );
	auto* row = new QHBoxLayout( card );
	row->setContentsMargins( 8, 8, 8, 8 );

	const auto cached = m_images.value( productId );
	const auto image = cached.first == imageId ? cached.second : QImage{};
	if( image.isNull() == false )
	{
		auto* imageLabel = new QLabel( card );
		imageLabel->setPixmap( QPixmap::fromImage( image.scaled( 96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation ) ) );
		imageLabel->setFixedSize( 96, 96 );
		imageLabel->setAlignment( Qt::AlignCenter );
		row->addWidget( imageLabel );
	}
	else if( product.value( QStringLiteral("hasImage") ).toBool() )
	{
		auto* imageLabel = new QLabel( card );
		imageLabel->setFixedSize( 96, 96 );
		imageLabel->setAlignment( Qt::AlignCenter );
		imageLabel->setText( tr( "图片加载中…" ) );
		row->addWidget( imageLabel );
	}

	auto* column = new QVBoxLayout;
	auto* nameLabel = new QLabel( name, card );
	nameLabel->setProperty( "role", QStringLiteral("heading") );
	nameLabel->setWordWrap( true );
	column->addWidget( nameLabel );
	if( description.isEmpty() == false )
	{
		auto* descriptionLabel = new QLabel( description, card );
		descriptionLabel->setWordWrap( true );
		descriptionLabel->setProperty( "role", QStringLiteral("hint") );
		column->addWidget( descriptionLabel );
	}
	column->addWidget( new QLabel( stock > 0 ? tr( "价格 %1 分 · 库存 %2" ).arg( price ).arg( stock )
											 : tr( "价格 %1 分 · 已兑完" ).arg( price ), card ) );

	auto* redeemButton = new QPushButton( tr( "兑换" ), card );
	redeemButton->setEnabled( m_actionPending == false && m_signedIn && m_balance >= 0 && open && stock > 0 );
	if( m_signedIn == false ) { redeemButton->setToolTip( tr( "请先签到" ) ); }
	else if( open == false ) { redeemButton->setToolTip( tr( "老师暂时关闭了商场" ) ); }
	else if( stock <= 0 ) { redeemButton->setToolTip( tr( "已兑完" ) ); }
	else if( ClassroomShopBook::spendableScore( m_balance ) < price )
	{
		redeemButton->setToolTip( tr( "可用积分不足（需保留 %1 分）" ).arg( ClassroomShopBook::ProtectedScore ) );
	}
	connect( redeemButton, &QPushButton::clicked, this, [this, productId, name, price]() {
		// 确认框防误触，也挡住双击造成的重复兑换。
		if( QMessageBox::question( this, windowTitle(),
								   tr( "花费 %1 积分兑换「%2」？" ).arg( price ).arg( name ),
								   QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes ) != QMessageBox::Yes )
		{
			return;
		}
		m_statusLabel->setText( tr( "已发送兑换请求…" ) );
		m_actionPending = true;
		m_actionTimer->start( 15000 );
		rebuild();
		Q_EMIT redeemRequested( productId );
	} );

	auto* bottomRow = new QHBoxLayout;
	bottomRow->addWidget( redeemButton );
	bottomRow->addStretch( 1 );
	column->addLayout( bottomRow );

	row->addLayout( column, 1 );
	return card;
}


QWidget* ClassroomShopView::buildAuctionCard( const QJsonObject& auction )
{
	const auto name = auction.value( QStringLiteral("name") ).toString();
	const auto open = auction.value( QStringLiteral("open") ).toBool( false );
	const auto startPrice = auction.value( QStringLiteral("startPrice") ).toInt();
	const auto currentBid = auction.value( QStringLiteral("currentBid") ).toInt();
	const auto minNextBid = auction.value( QStringLiteral("minNextBid") ).toInt( startPrice );
	const auto bidder = auction.value( QStringLiteral("bidderStudent") ).toString();
	const auto bidderSeat = auction.value( QStringLiteral("bidderSeat") ).toString();

	auto* card = new QWidget;
	card->setProperty( "auctionCard", true );
	auto* column = new QVBoxLayout( card );
	column->setContentsMargins( 8, 8, 8, 8 );

	auto* nameLabel = new QLabel( tr( "拍品：%1" ).arg( name ), card );
	nameLabel->setProperty( "role", QStringLiteral("heading") );
	nameLabel->setWordWrap( true );
	column->addWidget( nameLabel );

	if( open )
	{
		QString stateText;
		if( currentBid > 0 )
		{
			stateText = tr( "当前出价 %1 分 · 领先：%2（座位 %3）" ).arg( currentBid ).arg( bidder, bidderSeat );
		}
		else
		{
			stateText = tr( "起拍价 %1 分 · 暂无出价 · 价高者得" ).arg( startPrice );
		}
		column->addWidget( new QLabel( stateText, card ) );

		// 出价不扣分，成交时才扣；保留基础分，只能使用超出 80 分的部分。
		auto* bidRow = new QHBoxLayout;
		auto* bidSpin = new QSpinBox( card );
		const auto available = ClassroomShopBook::spendableScore( m_balance );
		const auto maximum = m_balance >= 0 ? qMin( available, static_cast<int>( ClassroomShopBook::MaxBid ) )
											: static_cast<int>( ClassroomShopBook::MaxBid );
		bidSpin->setRange( minNextBid, qMax( minNextBid, maximum ) );
		bidSpin->setValue( minNextBid );
		bidSpin->setToolTip( tr( "至少 %1 分" ).arg( minNextBid ) );
		auto* bidButton = new QPushButton( tr( "出价" ), card );
		bidButton->setEnabled( m_actionPending == false );
		const auto auctionId = auction.value( QStringLiteral("id") ).toString();
		const auto auctionName = name;
		connect( bidButton, &QPushButton::clicked, this, [this, auctionId, auctionName, bidSpin]() {
			if( bidSpin == nullptr ) { return; }
			// 出价确认：防误触，也让学生意识到成交是要扣积分的。
			if( QMessageBox::question( this, windowTitle(),
									   tr( "为「%1」出价 %2 分？（结束时价高者得，成交才扣分）" )
										   .arg( auctionName ).arg( bidSpin->value() ),
									   QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes ) != QMessageBox::Yes )
			{
				return;
			}
			const auto amount = bidSpin->value();
			m_statusLabel->setText( tr( "已发送出价 %1 分…" ).arg( amount ) );
			m_actionPending = true;
			m_actionTimer->start( 15000 );
			rebuild();
			Q_EMIT bidRequested( auctionId, amount );
		} );
		if( m_signedIn == false || available < minNextBid || minNextBid > ClassroomShopBook::MaxBid )
		{
			bidSpin->setEnabled( false );
			bidButton->setEnabled( false );
			bidButton->setToolTip( m_signedIn == false ? tr( "请先签到" ) :
				( minNextBid > ClassroomShopBook::MaxBid ? tr( "出价超出范围" ) :
					tr( "可用积分不足（当前可用 %1 分，需保留 %2 分）" ).
						arg( available ).arg( ClassroomShopBook::ProtectedScore ) ) );
		}
		bidRow->addWidget( new QLabel( tr( "我的出价：" ), card ) );
		bidRow->addWidget( bidSpin );
		bidRow->addWidget( bidButton );
		bidRow->addStretch( 1 );
		column->addLayout( bidRow );

		// 最近出价，让全班看到争夺过程。
		const auto bids = auction.value( QStringLiteral("bids") ).toArray();
		if( bids.isEmpty() == false )
		{
			QStringList lines;
			for( int index = bids.size() - 1; index >= 0 && lines.size() < 5; --index )
			{
				const auto bid = bids.at( index ).toObject();
				lines.append( QStringLiteral("%1  %2（座位 %3）出 %4 分")
								  .arg( bid.value( QStringLiteral("time") ).toString(),
										bid.value( QStringLiteral("student") ).toString(),
										bid.value( QStringLiteral("seat") ).toString() )
								  .arg( bid.value( QStringLiteral("amount") ).toInt() ) );
			}
			auto* bidsLabel = new QLabel( lines.join( QLatin1Char('\n') ), card );
			bidsLabel->setProperty( "role", QStringLiteral("hint") );
			column->addWidget( bidsLabel );
		}
	}
	else if( currentBid > 0 )
	{
		column->addWidget( new QLabel( tr( "已结束 · %2（座位 %3）以 %1 分拍得" )
										  .arg( currentBid ).arg( bidder, bidderSeat ), card ) );
	}
	else
	{
		column->addWidget( new QLabel( tr( "已结束 · 流拍（无人出价）" ), card ) );
	}

	return card;
}


void ClassroomShopView::rebuild()
{
	m_balanceLabel->setText( m_signedIn == false ? tr( "请先签到后再使用商城" ) :
		( m_balance >= 0 ? tr( "积分余额：%1 · 可用：%2（保留 %3 分）" ).
			arg( m_balance ).arg( ClassroomShopBook::spendableScore( m_balance ) ).
			arg( ClassroomShopBook::ProtectedScore ) : tr( "积分余额：未知" ) ) );

	// products tab
	{
		auto* layout = qobject_cast<QVBoxLayout*>( m_productsArea->layout() );
		if( layout == nullptr ) { return; }
		clearWidgetList( layout );
		const auto open = m_catalog.value( QStringLiteral("open") ).toBool( true );
		const auto products = m_catalog.value( QStringLiteral("products") ).toArray();
		if( products.isEmpty() )
		{
			layout->addWidget( new QLabel( tr( "老师还没有上架商品。" ), m_productsArea ) );
		}
		else if( open == false )
		{
			layout->addWidget( new QLabel( tr( "老师暂时关闭了商场，稍后再来看看。" ), m_productsArea ) );
		}
		for( const auto& value : products )
		{
			layout->addWidget( buildProductCard( value.toObject() ) );
		}
		layout->addStretch( 1 );
	}

	// auctions tab
	{
		auto* layout = qobject_cast<QVBoxLayout*>( m_auctionsArea->layout() );
		if( layout == nullptr ) { return; }
		clearWidgetList( layout );
		const auto auctions = m_catalog.value( QStringLiteral("auctions") ).toArray();
		if( auctions.isEmpty() )
		{
			layout->addWidget( new QLabel( tr( "当前没有竞拍，老师发起后会出现在这里。" ), m_auctionsArea ) );
		}
		for( const auto& value : auctions )
		{
			layout->addWidget( buildAuctionCard( value.toObject() ) );
		}
		layout->addStretch( 1 );
	}
}


void ClassroomShopView::closeEvent( QCloseEvent* event )
{
	event->ignore();
	hide();
}


void ClassroomShopView::mousePressEvent( QMouseEvent* event )
{
	if( event->button() == Qt::LeftButton )
	{
		m_dragPosition = mouseGlobalPoint( event ) - frameGeometry().topLeft();
	}
	QWidget::mousePressEvent( event );
}


void ClassroomShopView::mouseMoveEvent( QMouseEvent* event )
{
	if( event->buttons() & Qt::LeftButton )
	{
		move( clampedOnScreen( this, mouseGlobalPoint( event ) - m_dragPosition ) );
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
			// 允许「微信」这类两字关键字；单字符仍会误杀大量进程。
			if( rule.size() >= 2 ) { rules.append( rule ); }
		}
		return rules;
	};
	m_blockedApps = toStringList( Classroom::SettingKey::blockedApps() );
	m_blockedWebsites = toStringList( Classroom::SettingKey::blockedWebsites() );

	const auto interval = settings.value( Classroom::SettingKey::captureInterval() ).toInt( 5 );
	const auto hasRules = m_blockedApps.isEmpty() == false || m_blockedWebsites.isEmpty() == false;
	// 有拦截规则时最快 2 秒复查一次，避免规则形同虚设。
	m_timer->setInterval( std::max( 1, hasRules ? std::min( interval, 2 ) : interval ) * 1000 );
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
	const auto unchanged = key == m_lastSample;
	// 即使窗口没变化也要复查拦截规则：上次 taskkill 失败时必须在下一次重试。
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
#ifdef Q_OS_WIN
		if( webRule.isEmpty() == false ) { closeForegroundBrowserTab(); }
#endif
		Q_EMIT blockedActivity( application, title,
								appRule.isEmpty() ? webRule : appRule,
								appRule.isEmpty() );
	}

	if( unchanged == false ) { Q_EMIT activity( application, title, browser ); }
}
