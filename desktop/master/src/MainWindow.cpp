/*
 * MainWindow.cpp - implementation of MainWindow class
 *
 * Copyright (c) 2004-2026 Tobias Junghans <tobydox@veyon.io>
 *
 * This file is part of Ikeback - https://github.com/zk26/ikeback
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include <QAction>
#include <QCloseEvent>
#include <QFileDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QHostAddress>
#include <QMenu>
#include <QMessageBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QVector>
#include <QWidgetAction>

#include "AboutDialog.h"
#include "AccessControlProvider.h"
#include "MainWindow.h"
#include "BuiltinFeatures.h"
#include "AuthenticationCredentials.h"
#include "ComputerControlListModel.h"
#include "ComputerManager.h"
#include "ComputerSelectPanel.h"
#include "ScreenshotManagementPanel.h"
#include "FeatureManager.h"
#include "MonitoringMode.h"
#include "NetworkObjectDirectory.h"
#include "NetworkObjectDirectoryManager.h"
#include "PlatformUserFunctions.h"
#include "SlideshowPanel.h"
#include "SpotlightPanel.h"
#include "ToolButton.h"
#include "IkebackConfiguration.h"
#include "IkebackMaster.h"
#include "UserConfig.h"

#include "ui_MainWindow.h"


MainWindow::MainWindow( IkebackMaster &masterCore, QWidget* parent ) :
	QMainWindow( parent ),
	ui( new Ui::MainWindow ),
	m_master( masterCore ),
	m_modeGroup( new QButtonGroup( this ) )
{
	ui->setupUi( this );
	setWindowIcon( QIcon( QStringLiteral(":/master/app-icon.png") ) );
	setStyleSheet( QStringLiteral(
		"QMainWindow { background: #FFF9EA; color: #4C566A; }"
		"QToolBar { background: #FFF3D6; border: 0; spacing: 3px; padding: 4px; }"
		"QToolBar::separator { background: #E8CF9D; width: 6px; margin: 5px 2px; }"
		"QToolButton { color: #4C566A; border: 1px solid transparent; border-radius: 9px; padding: 5px 8px; }"
		"QToolButton:hover { background: #FFE7AD; border-color: #F2C96D; }"
		"QToolButton:checked { background: #DDF4EE; border-color: #65C9B8; color: #247F71; }"
		"QPushButton { background: #FFDF8E; color: #584A32; border: 1px solid #E9BE5C; border-radius: 8px; padding: 6px 12px; }"
		"QPushButton:hover { background: #FFD16D; }"
		"QLineEdit, QComboBox, QSpinBox { background: #FFFFFF; color: #4C566A; border: 1px solid #E5CFA5; border-radius: 7px; padding: 4px; }"
		"QTableView { background: #FFFDF7; alternate-background-color: #FFF5DE; gridline-color: #EEDDBB; selection-background-color: #DDF4EE; selection-color: #247F71; }"
		"QHeaderView::section { background: #FFE9B5; color: #584A32; border: 0; padding: 6px; }"
		"QStatusBar { background: #FFF3D6; color: #6B6255; }"
		"QLabel[role=\"heading\"] { color: #D8892B; font-weight: bold; }"
		"QLabel[role=\"hint\"] { color: #8C8272; }" ) );

	restoreState( QByteArray::fromBase64( m_master.userConfig().windowState().toUtf8() ) );
	restoreGeometry( QByteArray::fromBase64( m_master.userConfig().windowGeometry().toUtf8() ) );

	// add widgets to status bar
	ui->statusBar->addWidget( ui->panelButtons );
	ui->statusBar->addWidget( ui->spacerLabel1 );
	ui->statusBar->addWidget( ui->filterLineEdit, 2 );
	ui->statusBar->addWidget( ui->filterPoweredOnComputersButton );
	ui->statusBar->addWidget( ui->filterComputersWithLoggedOnUsersButton );
	ui->statusBar->addWidget( ui->spacerLabel2, 1 );
	ui->statusBar->addWidget( ui->gridSizeSlider, 2 );
	ui->statusBar->addWidget( ui->autoAdjustComputerIconSizeButton );
	ui->statusBar->addWidget( ui->spacerLabel3 );
	ui->statusBar->addWidget( ui->useCustomComputerPositionsButton );
	ui->statusBar->addWidget( ui->alignComputersButton );
	ui->statusBar->addWidget( ui->spacerLabel4 );
	ui->statusBar->addWidget( ui->aboutButton );

	// create all views
	auto mainSplitter = new QSplitter( Qt::Horizontal, ui->centralWidget );
	mainSplitter->setChildrenCollapsible( false );
	mainSplitter->setObjectName( QStringLiteral("MainSplitter") );

	auto monitoringSplitter = new QSplitter( Qt::Vertical, mainSplitter );
	monitoringSplitter->setChildrenCollapsible( false );
	monitoringSplitter->setObjectName( QStringLiteral("MonitoringSplitter") );

	auto slideshowSpotlightSplitter = new QSplitter( Qt::Horizontal, monitoringSplitter );
	slideshowSpotlightSplitter->setChildrenCollapsible( false );
	slideshowSpotlightSplitter->setObjectName( QStringLiteral("SlideshowSpotlightSplitter") );

	auto computerSelectPanel = new ComputerSelectPanel( m_master.computerManager() );
	auto screenshotManagementPanel = new ScreenshotManagementPanel();
	auto slideshowPanel = new SlideshowPanel( m_master.userConfig(), ui->computerMonitoringWidget );
	auto spotlightPanel = new SpotlightPanel( m_master.userConfig(), ui->computerMonitoringWidget );

	slideshowSpotlightSplitter->addWidget( slideshowPanel );
	slideshowSpotlightSplitter->addWidget( spotlightPanel );
	slideshowSpotlightSplitter->setStretchFactor( slideshowSpotlightSplitter->indexOf(slideshowPanel), 1 );
	slideshowSpotlightSplitter->setStretchFactor( slideshowSpotlightSplitter->indexOf(spotlightPanel), 1 );

	monitoringSplitter->addWidget( slideshowSpotlightSplitter );
	monitoringSplitter->addWidget( ui->computerMonitoringWidget );
	monitoringSplitter->setStretchFactor( monitoringSplitter->indexOf(slideshowSpotlightSplitter), 1 );
	monitoringSplitter->setStretchFactor( monitoringSplitter->indexOf(ui->computerMonitoringWidget), 1 );

	mainSplitter->addWidget( computerSelectPanel );
	mainSplitter->addWidget( screenshotManagementPanel );
	mainSplitter->addWidget( monitoringSplitter );

	mainSplitter->setStretchFactor( mainSplitter->indexOf(monitoringSplitter), 1 );


	static const QHash<QWidget *, QAbstractButton *> panelButtons{
		{ computerSelectPanel, ui->computerSelectPanelButton },
		{ screenshotManagementPanel, ui->screenshotManagementPanelButton },
		{ slideshowPanel, ui->slideshowPanelButton },
		{ spotlightPanel, ui->spotlightPanelButton }
	};

	for( auto it = panelButtons.constBegin(), end = panelButtons.constEnd(); it != end; ++it )
	{
		it.key()->hide();
		it.key()->installEventFilter( this );
		connect( *it, &QAbstractButton::toggled, it.key(), &QWidget::setVisible );
	}

	QList<int> splitterSizes;
	for( auto* splitter : { slideshowSpotlightSplitter, monitoringSplitter, mainSplitter } )
	{
		splitter->setHandleWidth( 7 );
		splitter->setStyleSheet( QStringLiteral("QSplitter::handle:hover{background-color:#66a0b3;}") );

		splitter->installEventFilter( this );

		int index = 0;

		const auto splitterStates = m_master.userConfig().splitterStates()[splitter->objectName()].toArray();
		splitterSizes.clear();
		splitterSizes.reserve( splitterStates.size() );

		for( const auto& sizeObject : splitterStates )
		{
			auto size = sizeObject.toInt();
			const auto widget = splitter->widget( index );
			const auto button = panelButtons.value( widget );

			if( widget )
			{
				if( button )
				{
					widget->setVisible( size > 0 );
					button->setChecked( size > 0 );
				}
				size = qAbs( size );
				if( splitter->orientation() == Qt::Horizontal )
				{
					widget->resize( size, widget->height() );
				}
				else
				{
					widget->resize( widget->width(), size );
				}

				widget->setProperty( originalSizePropertyName(), widget->size() );
			}
			splitterSizes.append( size );
			++index;
		}
		splitter->setSizes( splitterSizes );
	}

	const auto SplitterContentBaseSize = 500;

	if( spotlightPanel->property( originalSizePropertyName() ).isNull() ||
		slideshowPanel->property( originalSizePropertyName() ).isNull() )
	{
		slideshowSpotlightSplitter->setSizes( { SplitterContentBaseSize, SplitterContentBaseSize } );
	}

	if( slideshowSpotlightSplitter->property( originalSizePropertyName() ).isNull() ||
		ui->computerMonitoringWidget->property( originalSizePropertyName() ).isNull() )
	{
		monitoringSplitter->setSizes( { SplitterContentBaseSize, SplitterContentBaseSize } );
	}

	ui->centralLayout->addWidget( mainSplitter );

	m_centralStack = new QStackedWidget( this );
	m_centralStack->setObjectName( QStringLiteral("ikebackCentralStack") );
	m_centralStack->addWidget( ui->centralWidget );
	setCentralWidget( m_centralStack );

	if( IkebackCore::config().autoOpenComputerSelectPanel() )
	{
		ui->computerSelectPanelButton->setChecked( true );
	}

	// initialize filter controls
	connect( ui->filterLineEdit, &QLineEdit::textChanged,
			 this, [this]( const QString& filter ) { ui->computerMonitoringWidget->setSearchFilter( filter ); } );
	connect( ui->filterPoweredOnComputersButton, &QToolButton::toggled,
			 this, [this]( bool enabled ) { ui->computerMonitoringWidget->setFilterPoweredOnComputers( enabled ); } );
	connect( ui->filterComputersWithLoggedOnUsersButton, &QToolButton::toggled,
			 this, [this]( bool enabled ) { ui->computerMonitoringWidget->setFilterComputersWithLoggedOnUsers( enabled ); } );
	ui->filterPoweredOnComputersButton->setChecked(m_master.userConfig().filterPoweredOnComputers());
	ui->filterComputersWithLoggedOnUsersButton->setChecked(m_master.userConfig().filterComputersWithLoggedOnUsers());

	// initialize monitoring screen size slider
	ui->gridSizeSlider->setMinimum( ComputerMonitoringWidget::MinimumComputerScreenSize );
	ui->gridSizeSlider->setMaximum( ComputerMonitoringWidget::MaximumComputerScreenSize );

	ui->autoAdjustComputerIconSizeButton->setChecked( ui->computerMonitoringWidget->autoAdjustIconSize() );

	connect( ui->gridSizeSlider, &QSlider::valueChanged,
			 this, [this]( int size ) { ui->computerMonitoringWidget->setComputerScreenSize( size ); } );
	connect( ui->computerMonitoringWidget, &ComputerMonitoringWidget::computerScreenSizeAdjusted,
			 ui->gridSizeSlider, &QSlider::setValue );
	connect( ui->autoAdjustComputerIconSizeButton, &QToolButton::toggled,
			 this, [this]( bool enabled ) {
				 ui->computerMonitoringWidget->setAutoAdjustIconSize( enabled );
				 m_master.userConfig().setAutoAdjustMonitoringIconSize( enabled );
			 } );

	int size = ComputerMonitoringWidget::DefaultComputerScreenSize;
	if( m_master.userConfig().monitoringScreenSize() >= ComputerMonitoringWidget::MinimumComputerScreenSize )
	{
		size = m_master.userConfig().monitoringScreenSize();
	}

	ui->gridSizeSlider->setValue( size );
	ui->computerMonitoringWidget->setComputerScreenSize( size );

	// initialize computer placement controls
	auto customComputerPositionsControlMenu = new QMenu(this);
	const auto darkSuffix = IkebackCore::useDarkMode() ? QStringLiteral("-dark") : QString();
	customComputerPositionsControlMenu->addAction(QIcon(QStringLiteral(":/core/document-open%1.png").arg(darkSuffix)),
													tr("Load computer positions"),
													this, &MainWindow::loadComputerPositions);
	customComputerPositionsControlMenu->addAction(QIcon(QStringLiteral(":/core/document-save%1.png").arg(darkSuffix)),
													tr("Save computer positions"),
													this, &MainWindow::saveComputerPositions);
	ui->useCustomComputerPositionsButton->setMenu(customComputerPositionsControlMenu);

	ui->useCustomComputerPositionsButton->setChecked(m_master.userConfig().useCustomComputerPositions());
	connect(ui->useCustomComputerPositionsButton, &QToolButton::toggled,
			ui->computerMonitoringWidget, &ComputerMonitoringWidget::setUseCustomComputerPositions);
	connect(ui->alignComputersButton, &QToolButton::clicked,
			ui->computerMonitoringWidget, &ComputerMonitoringWidget::alignComputers);


	const auto toolButtons = findChildren<QToolButton*>();
	for(auto* btn : toolButtons)
	{
		btn->setIconSize(QSize(20, 20));
	}

	if (IkebackCore::useDarkMode())
	{
		ui->aboutButton->setIcon(QIcon(QStringLiteral(":/core/help-about-dark.png")));
		ui->filterComputersWithLoggedOnUsersButton->setIcon(QIcon(QStringLiteral(":/core/user-group-new-dark.png")));
		ui->autoAdjustComputerIconSizeButton->setIcon(QIcon(QStringLiteral(":/master/zoom-fit-best-dark.png")));
		ui->alignComputersButton->setIcon(QIcon(QStringLiteral(":/master/align-grid-dark.png")));
		ui->useCustomComputerPositionsButton->setIcon(QIcon(QStringLiteral(":/master/exchange-positions-zorder-dark.png")));
		ui->filterPoweredOnComputersButton->setIcon(QIcon(QStringLiteral(":/master/powered-on-dark.png")));
	}

	// create the main toolbar
	ui->toolBar->layout()->setSpacing( 2 );
	ui->toolBar->toggleViewAction()->setEnabled( false );

	addToolBar( Qt::TopToolBarArea, ui->toolBar );

	m_quickToolBar = new MainToolBar( this );
	m_quickToolBar->setObjectName( QStringLiteral("ikebackQuickToolBar") );
	m_quickToolBar->setWindowTitle( tr( "Classroom tools" ) );
	m_quickToolBar->toggleViewAction()->setEnabled( false );
	m_quickToolBar->layout()->setSpacing( 2 );
	m_quickToolBar->hide();
	addToolBar( Qt::TopToolBarArea, m_quickToolBar );

	addFeaturesToToolBar();
	reloadSubFeatures();

	const auto monitoringModeButton = m_modeGroup->button(int(qHash(IkebackCore::builtinFeatures().monitoringMode().feature().uid()))); // clazy:exclude=qt6-qhash-signature
	if (monitoringModeButton)
	{
		monitoringModeButton->setChecked(true);
	}
}



MainWindow::~MainWindow()
{
	ui->computerMonitoringWidget->saveConfiguration();

	delete ui;
}



bool MainWindow::initAuthentication()
{
	if( IkebackCore::instance()->initAuthentication() )
	{
		return true;
	}

	if( IkebackCore::config().authenticationMethod() == IkebackCore::AuthenticationMethod::KeyFileAuthentication )
	{
		QMessageBox::information(nullptr,
								 tr("Authentication impossible"),
								 tr("No authentication key files were found or your current ones "
									"are outdated. Please create new key files using Ikeback "
									"Configurator. Alternatively set up logon authentication "
									"using Ikeback Configurator. Otherwise you won't be "
									"able to access computers using Ikeback."));
	}

	return false;
}



bool MainWindow::initAccessControl()
{
	if (IkebackCore::config().accessControlForMasterEnabled())
	{
		const auto accessingUser = IkebackCore::authenticationCredentials().hasCredentials(AuthenticationCredentials::Type::UserLogon) ?
									   IkebackCore::authenticationCredentials().logonUsername() :
									   IkebackCore::platform().userFunctions().queryCurrentUserProperty(PlatformUserFunctions::UserProperty::LoginName);
		const auto accessControlResult = IkebackCore::builtinFeatures().accessControlProvider()
										 .checkAccess(accessingUser,
													  QHostAddress(QHostAddress::LocalHost).toString(),
													  {});
		if( accessControlResult.access == AccessControlProvider::Access::Deny )
		{
			vWarning() << "user" << IkebackCore::authenticationCredentials().logonUsername()
					   << "is not allowed to access computers";
			QMessageBox::critical( nullptr, tr( "Access denied" ),
								   tr( "According to the local configuration you're not allowed "
									   "to access computers in the network. Please log in with a different "
									   "account or let your system administrator check the local configuration." ) );
			return false;
		}
	}

	return true;
}



bool MainWindow::isFeatureSuppressed( const Feature& feature )
{
	static const QStringList names{
		QStringLiteral("UserLogin"), QStringLiteral("UserLogoff"),
		QStringLiteral("StartApp"), QStringLiteral("OpenWebsite"),
		QStringLiteral("ClassroomSignOut"), QStringLiteral("ClassroomQuickTools")
	};
	return feature.testFlag( Feature::Flag::Meta ) || names.contains( feature.name(), Qt::CaseInsensitive );
}


bool MainWindow::isClassroomQuickAction( const Feature& feature )
{
	return feature.uid().toString().startsWith( QStringLiteral("d3f1c0de-7a41-4b8e-9c14-"), Qt::CaseInsensitive );
}


void MainWindow::reloadSubFeatures()
{
	for( const auto& feature : m_master.features() )
	{
		auto button = ui->toolBar->findChild<QToolButton *>( feature.name() );
		if( button )
		{
			addSubFeaturesToToolButton( button, feature );
		}
	}
}


void MainWindow::setClassroomPage( QWidget* page )
{
	if( m_centralStack == nullptr || page == nullptr ) { return; }
	if( m_classroomPage && m_classroomPage != page )
	{
		m_centralStack->removeWidget( m_classroomPage );
	}
	if( m_centralStack->indexOf( page ) < 0 ) { m_centralStack->addWidget( page ); }
	m_classroomPage = page;
	if( m_classroomMode ) { m_centralStack->setCurrentWidget( page ); }
}


void MainWindow::setClassroomMode( bool active )
{
	if( m_classroomMode == active ) { return; }
	m_classroomMode = active;
	if( m_centralStack )
	{
		m_centralStack->setCurrentWidget( active && m_classroomPage ? m_classroomPage : ui->centralWidget );
	}
	rebuildToolBar();
}



ComputerControlInterfaceList MainWindow::selectedComputerControlInterfaces() const
{
	return ui->computerMonitoringWidget->selectedComputerControlInterfaces();
}



void MainWindow::closeEvent( QCloseEvent* event )
{
	if( m_master.currentMode() != IkebackCore::builtinFeatures().monitoringMode().feature().uid() )
	{
		const Feature& activeFeature = IkebackCore::featureManager().feature( m_master.currentMode() );

		QMessageBox::information(this, tr("Feature active"),
								 tr("The feature \"%1\" is still active. Please stop it before closing Ikeback.")
								 .arg(activeFeature.displayName()));
		event->ignore();
		return;
	}

	QJsonObject splitterStates;
	const auto splitters = findChildren<QSplitter *>();
	for( const auto* splitter : splitters )
	{
		const auto sizes = splitter->sizes();
		QJsonArray splitterSizes;
		int i = 0;
		int hiddenSize = 0;
		for( auto size : sizes )
		{
			auto widget = splitter->widget(i);
			const auto originalSize = widget->property( originalSizePropertyName() ).toSize();
			if( widget->size().isEmpty() && originalSize.isEmpty() == false )
			{
				size = splitter->orientation() == Qt::Horizontal ? -originalSize.width() : -originalSize.height();
				hiddenSize += qAbs(size);
			}
			else
			{
				size -= hiddenSize;
			}

			splitterSizes.append( size );
			++i;
		}
		splitterStates[splitter->objectName()] = splitterSizes;
	}

	m_master.userConfig().setSplitterStates( splitterStates );

	m_master.userConfig().setWindowState( QString::fromLatin1( saveState().toBase64() ) );
	m_master.userConfig().setWindowGeometry( QString::fromLatin1( saveGeometry().toBase64() ) );

	QMainWindow::closeEvent( event );
}



bool MainWindow::eventFilter( QObject* object, QEvent* event )
{
	if( event->type() == QEvent::Resize )
	{
		const auto widget = qobject_cast<QWidget *>( object );
		const auto resizeEvent = static_cast<QResizeEvent *>( event );

		if( resizeEvent->oldSize().isEmpty() == false )
		{
			widget->setProperty( originalSizePropertyName(), resizeEvent->oldSize() );
		}
	}

	return QMainWindow::eventFilter( object, event );
}



void MainWindow::keyPressEvent( QKeyEvent* event )
{
	switch( event->key() )
	{
	case Qt::Key_F5:
		IkebackCore::networkObjectDirectoryManager().configuredDirectory()->update();
		m_master.computerControlListModel().reload();
		event->accept();
		break;
	case Qt::Key_F11:
		QWidget::setWindowState( QWidget::windowState() ^ Qt::WindowFullScreen );
		event->accept();
		break;
	default:
		QMainWindow::keyPressEvent( event );
		break;
	}
}



void MainWindow::showAboutDialog()
{
	AboutDialog( this ).exec();
}



void MainWindow::rebuildToolBar()
{
	if( ui->toolBar == nullptr ) { return; }
	const auto clearToolBar = []( QToolBar* toolBar ) {
		if( toolBar == nullptr ) { return; }
		const auto actions = toolBar->actions();
		for( auto* action : actions )
		{
			toolBar->removeAction( action );
			delete action;
		}
		qDeleteAll( toolBar->findChildren<ToolButton*>( Qt::FindDirectChildrenOnly ) );
	};
	clearToolBar( ui->toolBar );
	clearToolBar( m_quickToolBar );
	delete m_modeGroup;
	m_modeGroup = new QButtonGroup( this );
	addFeaturesToToolBar();
	reloadSubFeatures();
}


void MainWindow::addFeaturesToToolBar()
{
	if( m_classroomMode && m_classroomPage )
	{
		struct ClassroomPageButton
		{
			const char* label;
			const char* page;
			const char* icon;
		};
		static const ClassroomPageButton pageButtons[] = {
			{ "座位与签到", "seats", ":/classroom/icons/check-in.png" },
			{ "班级管理", "class", ":/classroom/icons/class.png" },
			{ "积分", "score", ":/classroom/icons/plus.png" },
			{ "课程记录", "course", ":/core/document-edit.png" },
			{ "商城", "shop", ":/classroom/icons/shop.png" },
			{ "等级规则", "levels", ":/core/help-about.png" }
		};
		for( const auto& entry : pageButtons )
		{
			auto* button = new ToolButton( QIcon( QString::fromLatin1( entry.icon ) ),
										   tr( entry.label ), {}, tr( entry.label ) );
			button->setObjectName( QStringLiteral("classroomPage_%1").arg( QString::fromLatin1( entry.page ) ) );
			connect( button, &QToolButton::clicked, this, [this, page = QString::fromLatin1( entry.page )]() {
				if( m_classroomPage )
				{
					QMetaObject::invokeMethod( m_classroomPage, "showPage", Q_ARG( QString, page ) );
				}
			} );
			button->addTo( ui->toolBar );
		}
		ui->toolBar->addSeparator();
	}

	// Everyday actions stay visible. Mode buttons are always primary so their
	// checked state remains in the existing QButtonGroup; everything else is
	// available from one overflow menu instead of a second toolbar row.
	static const QStringList primaryNames{
		QStringLiteral("monitoring"), QStringLiteral("demo"), QStringLiteral("lock"),
		QStringLiteral("remoteaccess"), QStringLiteral("textmessage"),
		QStringLiteral("ClassroomConsole"), QStringLiteral("ClassroomSignIn"),
		QStringLiteral("ClassroomCheckIn")
	};

	QVector<Feature> primary;
	QVector<Feature> quick;
	QVector<Feature> secondary;
	for( const auto& feature : m_master.features() )
	{
		if( isFeatureSuppressed( feature ) )
		{
			continue;
		}

		if( isClassroomQuickAction( feature ) )
		{
			quick.append( feature );
		}
		else if( primaryNames.contains( feature.name(), Qt::CaseInsensitive ) ||
				 feature.testFlag( Feature::Flag::Mode ) )
		{
			primary.append( feature );
		}
		else
		{
			secondary.append( feature );
		}
	}

	const auto addFeature = [this]( const Feature& feature, QToolBar* toolBar )
	{
		auto btn = new ToolButton( QIcon( feature.iconUrl() ),
									  feature.displayName(),
									  feature.displayNameActive(),
									  feature.description(),
									  feature.shortcut() );
		connect(btn, &QToolButton::clicked, this, [=, this] () {
			m_master.runFeature( feature );
			updateModeButtonGroup();
			if( feature.testFlag( Feature::Flag::Mode ) )
			{
				reloadSubFeatures();
			}
		});
		btn->setObjectName( feature.name() );
		btn->addTo( toolBar );

		if( feature.testFlag( Feature::Flag::Mode ) )
		{
			btn->setCheckable( true );
			m_modeGroup->addButton( btn, buttonId( feature ) );
		}
	};

	for( const auto& feature : primary )
	{
		addFeature( feature, ui->toolBar );
	}

	if( m_classroomMode && m_quickToolBar && quick.isEmpty() == false )
	{
		m_quickToolBar->setVisible( true );
		auto* heading = new QLabel( tr( "课堂快捷工具" ), m_quickToolBar );
		heading->setProperty( "role", QStringLiteral("heading") );
		auto* headingAction = new QWidgetAction( m_quickToolBar );
		headingAction->setDefaultWidget( heading );
		m_quickToolBar->addAction( headingAction );
		int group = -1;
		for( int index = 0; index < quick.size(); ++index )
		{
			const auto nextGroup = index < 2 ? 0 : index < 5 ? 1 : index < 8 ? 2 : index < 11 ? 3 : 4;
			if( nextGroup != group )
			{
				if( group >= 0 ) { m_quickToolBar->addSeparator(); }
				const auto labels = QStringList{ tr( "保护与管控" ), tr( "网络与静音" ),
					tr( "课堂秩序" ), tr( "积分与部署" ), tr( "批量工具" ) };
				auto* label = new QLabel( labels.at( nextGroup ), m_quickToolBar );
				label->setProperty( "role", QStringLiteral("hint") );
				auto* action = new QWidgetAction( m_quickToolBar );
				action->setDefaultWidget( label );
				m_quickToolBar->addAction( action );
				group = nextGroup;
			}
			addFeature( quick.at( index ), m_quickToolBar );
		}
	}
	else if( m_quickToolBar )
	{
		m_quickToolBar->hide();
	}
	if( secondary.isEmpty() == false )
	{
		auto* more = new ToolButton( QIcon( QStringLiteral(":/core/icon22.png") ),
									 tr( "More" ), {}, tr( "More classroom actions" ) );
		more->setObjectName( QStringLiteral("ikebackMoreActions") );
		more->setPopupMode( QToolButton::InstantPopup );
		auto* menu = new QMenu( more );
		more->setMenu( menu );
		for( const auto& feature : secondary )
		{
			const auto subFeatures = m_master.subFeatures( feature.uid() );
			if( subFeatures.isEmpty() == false )
			{
				auto* submenu = menu->addMenu( QIcon( feature.iconUrl() ), feature.displayName() );
				submenu->setToolTipsVisible( true );
				for( const auto& subFeature : subFeatures )
				{
					auto* subAction = submenu->addAction( QIcon( subFeature.iconUrl() ), subFeature.displayName() );
					subAction->setShortcut( subFeature.shortcut() );
					subAction->setToolTip( subFeature.description() );
					if( subFeature.testFlag( Feature::Flag::Option ) )
					{
						subAction->setCheckable( true );
						subAction->setChecked( subFeature.testFlag( Feature::Flag::Checked ) );
					}
					connect( subAction, &QAction::triggered, this, [this, subFeature]() {
						m_master.runFeature( subFeature );
						updateModeButtonGroup();
					} );
				}
				continue;
			}
			auto* action = menu->addAction( QIcon( feature.iconUrl() ), feature.displayName() );
			action->setShortcut( feature.shortcut() );
			action->setToolTip( feature.description() );
			connect( action, &QAction::triggered, this, [this, feature]() {
				m_master.runFeature( feature );
				updateModeButtonGroup();
				if( feature.testFlag( Feature::Flag::Mode ) ) { reloadSubFeatures(); }
			} );
		}
		more->addTo( ui->toolBar );
	}
}



void MainWindow::addSubFeaturesToToolButton( QToolButton* button, const Feature& parentFeature )
{
	if( button->menu() )
	{
		button->menu()->close();
		button->menu()->deleteLater();
		button->setMenu( nullptr );
	}

	const auto parentFeatureIsMode = parentFeature.testFlag( Feature::Flag::Mode );
	const auto subFeatures = m_master.subFeatures( parentFeature.uid() );

	if( subFeatures.isEmpty() ||
		( parentFeatureIsMode && button->isChecked() ) )
	{
		return;
	}

	auto menu = new QMenu( button );
	menu->setObjectName( parentFeature.name() );
	menu->setToolTipsVisible( true );

	for( const auto& subFeature : subFeatures )
	{
		auto action = menu->addAction(QIcon(subFeature.iconUrl()), subFeature.displayName(),
							   #if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
									  subFeature.shortcut(),
							   #endif
									  this, [=, this]()
		{
			m_master.runFeature( subFeature );
			if( parentFeatureIsMode )
			{
				if( subFeature.testFlag( Feature::Flag::Option ) == false )
				{
					button->setChecked( true );
				}
				reloadSubFeatures();
			}
		}
#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
		,subFeature.shortcut()
#endif
		);
		action->setToolTip( subFeature.description() );
		action->setObjectName( subFeature.uid().toString() );

		if( subFeature.testFlag( Feature::Flag::Option ) )
		{
			action->setCheckable( true );
			action->setChecked( subFeature.testFlag( Feature::Flag::Checked ) );
		}
	}

	button->setMenu( menu );
	button->setPopupMode( ToolButton::InstantPopup );
}



void MainWindow::updateModeButtonGroup()
{
	const auto& monitoringMode = IkebackCore::builtinFeatures().monitoringMode().feature();

	if( m_master.currentMode() == monitoringMode.uid() )
	{
		m_modeGroup->button( buttonId( monitoringMode ) )->setChecked( true );
	}
}



void MainWindow::loadComputerPositions()
{
	if (const auto fileName = QFileDialog::getOpenFileName(this, tr("Load computer positions"),
														   QDir::homePath(), tr("JSON files (*.json)"));
		!fileName.isEmpty())
	{
		if (QFile file(fileName); file.open(QFile::ReadOnly))
		{
			const auto& computerPositionsProperty = m_master.userConfig().computerPositionsProperty();
			const auto config = QJsonDocument::fromJson(file.readAll()).object();
			const auto uiConfig = config[computerPositionsProperty.parentKey()].toObject();
			const auto computerPositions = uiConfig[computerPositionsProperty.key()].toObject()[QStringLiteral("JsonStoreArray")].toArray();

			ui->computerMonitoringWidget->loadPositions(computerPositions);
			ui->computerMonitoringWidget->setUseCustomComputerPositions(true);
			ui->computerMonitoringWidget->doItemsLayout();
		}
	}
}



void MainWindow::saveComputerPositions()
{
	if (const auto fileName = QFileDialog::getSaveFileName(this, tr("Save computer positions"),
														   QDir::homePath(), tr("JSON files (*.json)"));
		!fileName.isEmpty())
	{
		if (QFile file(fileName); file.open(QFile::WriteOnly | QFile::Truncate))
		{
			const auto& computerPositionsProperty = m_master.userConfig().computerPositionsProperty();

			// create structure identical to UserConfig so file with positions can be used as template for UserConfig
			QJsonObject computerPositions;
			computerPositions[QStringLiteral("JsonStoreArray")] = ui->computerMonitoringWidget->savePositions();
			QJsonObject uiConfig;
			uiConfig[computerPositionsProperty.key()] = computerPositions;
			QJsonObject config;
			config[computerPositionsProperty.parentKey()] = uiConfig;
			file.write(QJsonDocument(config).toJson());
		}
	}
}
