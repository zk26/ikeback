/*
 * ClassroomSeatGrid.cpp - live seat grid with thumbnails and one-click scoring
 *
 * This file is part of the Ikeback classroom modification.
 */

#include "ClassroomSeatGrid.h"

#include <QtGlobal>

#include <QAbstractItemView>
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include "ClassroomData.h"
#include "ClassroomSeatOrder.h"
#include "ClassroomTheme.h"

namespace
{
constexpr int CardSpacing = 10;
constexpr int MinimumCardWidth = 150;
}


ClassroomSeatGrid::ClassroomSeatGrid( ClassroomRoster& roster, QWidget* parent ) :
	QWidget( parent ),
	m_roster( roster )
{
	setObjectName( QStringLiteral("classroomSeatGrid") );
	installEventFilter( this );
	Classroom::Theme::apply( this );
	auto* root = new QVBoxLayout( this );

	auto* bar = new QHBoxLayout;
	m_viewButton = new QPushButton( tr( "列表视图" ), this );
	m_viewButton->setProperty( "secondary", true );
	m_viewButton->setToolTip( tr( "列表视图显示全部详情（包括 UWF 状态），不显示学生屏幕画面" ) );
	m_zoomCombo = new QComboBox( this );
	m_zoomCombo->addItem( tr( "紧凑 50%" ), 50 );
	m_zoomCombo->addItem( tr( "舒适 75%" ), 75 );
	m_zoomCombo->addItem( tr( "大图 100%" ), 100 );
	QSettings settings( QStringLiteral("ikeback"), QStringLiteral("classroom") );
	const auto storedZoom = settings.value( QStringLiteral("seatZoom") ).toInt();
	const auto savedZoom = qBound( 50, storedZoom > 0 ? storedZoom : 75, 100 );
	const auto savedIndex = m_zoomCombo->findData( savedZoom );
	m_zoomCombo->setCurrentIndex( savedIndex >= 0 ? savedIndex : 1 );
	m_zoomCombo->setToolTip( tr( "座位卡片缩放" ) );
	bar->addWidget( m_viewButton );
	bar->addWidget( m_zoomCombo );
	bar->addStretch( 1 );
	root->addLayout( bar );

	m_summary = new QLabel( this );
	m_summary->setWordWrap( true );
	root->addWidget( m_summary );

	// Grid view: draggable cards with live thumbnails.
	m_gridPage = new QWidget( this );
	auto* gridWrapper = new QVBoxLayout( m_gridPage );
	m_grid = new QGridLayout;
	m_grid->setSpacing( CardSpacing );
	gridWrapper->addLayout( m_grid );
	gridWrapper->addStretch( 1 );
	root->addWidget( m_gridPage, 1 );

	// List view: full information, deliberately NO student screen capture.
	m_table = new QTableWidget( 0, 10, this );
	m_table->setHorizontalHeaderLabels( QStringList{
		tr( "座位" ), tr( "名册姓名" ), tr( "当前签到" ), tr( "IP 地址" ),
		tr( "在线状态" ), tr( "退出保护" ), tr( "UWF 还原" ),
		tr( "积分" ), tr( "等级" ), tr( "最近活动" ) } );
	m_table->horizontalHeader()->setSectionResizeMode( QHeaderView::ResizeToContents );
	m_table->setEditTriggers( QAbstractItemView::NoEditTriggers );
	m_table->setSelectionBehavior( QAbstractItemView::SelectRows );
	m_table->setAlternatingRowColors( true );
	m_table->setVisible( false );
	root->addWidget( m_table, 1 );

	connect( m_viewButton, &QPushButton::clicked, this, [this]() {
		setViewMode( m_viewMode == ViewMode::Grid ? ViewMode::List : ViewMode::Grid );
	} );
	connect( m_zoomCombo, QOverload<int>::of( &QComboBox::currentIndexChanged ),
			 this, &ClassroomSeatGrid::updateZoom );
	updateZoom( m_zoomCombo->currentIndex() );

	m_timer = new QTimer( this );
	m_timer->setInterval( 2000 );
	connect( m_timer, &QTimer::timeout, this, &ClassroomSeatGrid::refresh );
	m_timer->start();
}


void ClassroomSeatGrid::setViewMode( ViewMode mode )
{
	if( m_viewMode == mode ) { return; }
	m_viewMode = mode;
	const auto grid = mode == ViewMode::Grid;
	m_gridPage->setVisible( grid );
	m_table->setVisible( grid == false );
	m_viewButton->setText( grid ? tr( "列表视图" ) : tr( "表格视图" ) );
	refresh();
}


void ClassroomSeatGrid::setClassName( const QString& className )
{
	if( m_className == className ) { return; }
	m_className = className;
	m_order = Classroom::loadSeatOrder( className );
	rebuild();
}


void ClassroomSeatGrid::setOnlineIps( const QStringList& ips )
{
	if( m_onlineIps == ips ) { return; }
	m_onlineIps = ips;
	refresh();
}


void ClassroomSeatGrid::rebuild()
{
	while( m_grid->count() > 0 )
	{
		auto* item = m_grid->takeAt( 0 );
		if( item->widget() ) { item->widget()->hide(); item->widget()->deleteLater(); }
		delete item;
	}
	m_cards.clear();
	m_activeScoreSeat.clear();
	m_scoreFocusButton.clear();
	m_keyboardSwapSeat.clear();

	const auto* classItem = m_className.isEmpty() ? nullptr : m_roster.findClass( m_className );
	if( classItem == nullptr )
	{
		m_summary->setText( tr( "No class selected." ) );
		m_table->setRowCount( 0 );
		return;
	}

	// Replay the persisted drag order, then append newly added students.
	QVector<ClassroomStudent> ordered;
	for( const auto& seat : m_order )
	{
		for( const auto& student : classItem->students )
		{
			if( student.seat == seat ) { ordered.append( student ); break; }
		}
	}
	for( const auto& student : classItem->students )
	{
		bool placed = false;
		for( const auto& item : ordered )
		{
			if( item.seat == student.seat ) { placed = true; break; }
		}
		if( placed == false ) { ordered.append( student ); }
	}
	m_order.clear();
	for( const auto& student : ordered ) { m_order.append( student.seat ); }
	Classroom::saveSeatOrder( m_className, m_order );

	int index = 0;
	const auto columns = columnCount();
	for( const auto& student : ordered )
	{
		auto* card = new QFrame( this );
		card->setFrameShape( QFrame::StyledPanel );
		card->setProperty( "seatCard", true );
		card->setFixedWidth( m_thumbWidth + 16 );
		card->setFocusPolicy( Qt::StrongFocus );
		card->setToolTip( tr( "Drag onto another card, or focus a card and press Enter/Space, then choose the destination and press Enter/Space. Esc cancels." ) );
		card->setProperty( "ikebackSeat", student.seat );
		card->setAccessibleName( tr( "Seat %1, %2" ).arg( student.seat, student.name ) );
		card->setAccessibleDescription( card->toolTip() );
		card->setAcceptDrops( true );
		card->installEventFilter( this );

		auto* layout = new QVBoxLayout( card );
		layout->setContentsMargins( 8, 8, 8, 8 );
		layout->setSpacing( 6 );

		auto* thumbnail = new QLabel( card );
		thumbnail->setFixedSize( m_thumbWidth, m_thumbHeight );
		thumbnail->setAlignment( Qt::AlignCenter );
		thumbnail->setProperty( "seatThumbnail", true );
		thumbnail->setProperty( "ikebackSeat", student.seat );
		thumbnail->setAcceptDrops( true );
		thumbnail->installEventFilter( this );
		layout->addWidget( thumbnail );

		auto* caption = new QLabel( card );
		caption->setWordWrap( true );
		caption->setProperty( "seatCaption", true );
		layout->addWidget( caption );

		auto* state = new QLabel( card );
		state->setWordWrap( true );
		state->setProperty( "role", QStringLiteral("hint") );
		layout->addWidget( state );

		auto* score = new QPushButton( tr( "积分" ), card );
		score->setProperty( "secondary", true );
		score->setToolTip( tr( "Add or deduct points (-5..+5)" ) );
		layout->addWidget( score );

		auto* editor = new QWidget( card );
		auto* editorLayout = new QVBoxLayout( editor );
		editorLayout->setContentsMargins( 0, 0, 0, 0 );
		editorLayout->setSpacing( 4 );
		auto* deltaRow = new QHBoxLayout;
		auto* delta = new QSpinBox( editor );
		delta->setRange( -5, 5 );
		delta->setValue( 1 );
		delta->setSuffix( tr( " pts" ) );
		deltaRow->addWidget( delta );
		auto* confirm = new QPushButton( tr( "应用" ), editor );
		auto* cancel = new QPushButton( tr( "Cancel" ), editor );
		cancel->setProperty( "secondary", true );
		deltaRow->addWidget( confirm );
		deltaRow->addWidget( cancel );
		editorLayout->addLayout( deltaRow );
		auto* reason = new QLineEdit( editor );
		reason->setPlaceholderText( tr( "Reason (optional)" ) );
		editorLayout->addWidget( reason );
		editor->hide();
		layout->addWidget( editor );

		const auto seat = student.seat;
		connect( score, &QPushButton::clicked, this, [this, seat]() { chooseDelta( seat ); } );
		connect( confirm, &QPushButton::clicked, this, [this, seat]() { commitScore( seat ); } );
		connect( cancel, &QPushButton::clicked, this, [this, seat]() { cancelScore( seat ); } );
		connect( reason, &QLineEdit::returnPressed, this, [this, seat]() { commitScore( seat ); } );

		m_grid->addWidget( card, index / columns, index % columns );
		m_cards.insert( seat, SeatCard{ card, thumbnail, score, confirm, cancel, delta, reason,
										editor, caption, state, student.ip, student.name, seat } );
		++index;
	}

	m_summary->setText( tr( "%1 students in %2. Drag cards or focus a card and press Enter/Space, then choose a destination and press Enter/Space; Esc cancels. Click a score for -5..+5." )
						.arg( classItem->students.size() ).arg( m_className ) );
	rebuildList();
	refresh();
}


void ClassroomSeatGrid::rebuildList()
{
	m_table->setRowCount( int( m_order.size() ) );
	int row = 0;
	for( const auto& seat : m_order )
	{
		const auto it = m_cards.constFind( seat );
		if( it == m_cards.constEnd() ) { continue; }
		const auto& card = it.value();
		m_table->setItem( row, 0, new QTableWidgetItem( seat ) );
		m_table->setItem( row, 1, new QTableWidgetItem( card.expectedName ) );
		m_table->setItem( row, 2, new QTableWidgetItem( QString{} ) );
		m_table->setItem( row, 3, new QTableWidgetItem( card.ip ) );
		m_table->setItem( row, 4, new QTableWidgetItem( QString{} ) );
		m_table->setItem( row, 5, new QTableWidgetItem( QString{} ) );
		m_table->setItem( row, 6, new QTableWidgetItem( QString{} ) );
		m_table->setItem( row, 7, new QTableWidgetItem( QString{} ) );
		m_table->setItem( row, 8, new QTableWidgetItem( QString{} ) );
		m_table->setItem( row, 9, new QTableWidgetItem( QString{} ) );
		++row;
	}
	m_table->setRowCount( row );
	refreshList();
}


QString ClassroomSeatGrid::protectionText( const QString& seat ) const
{
	const auto enabled = m_protectionProvider ? m_protectionProvider( seat ) : false;
	return enabled ? tr( "已保护" ) : tr( "未保护" );
}


void ClassroomSeatGrid::refreshList()
{
	for( int row = 0; row < m_table->rowCount(); ++row )
	{
		auto* seatItem = m_table->item( row, 0 );
		if( seatItem == nullptr ) { continue; }
		const auto seat = seatItem->text();
		const auto it = m_cards.constFind( seat );
		if( it == m_cards.constEnd() ) { continue; }
		const auto& card = it.value();

		const auto online = m_onlineIps.contains( card.ip );
		const auto identity = m_identityProvider ? m_identityProvider( seat ) : QString{};
		const auto uwf = m_uwfProvider ? m_uwfProvider( seat ) : QString{};
		int total = 100;
		if( m_scoreProvider ) { total = m_scoreProvider( seat ); }

		const auto set = [this, row]( int column, const QString& text ) {
			if( auto* item = m_table->item( row, column ) ) { item->setText( text ); }
		};
		set( 2, identity.isEmpty() ? tr( "未签到" ) : identity );
		set( 4, online ? tr( "在线" ) : tr( "离线" ) );
		const auto protectedNow = m_protectionProvider && m_protectionProvider( seat );
		set( 5, protectedNow ? tr( "已保护（不能退出）" ) : tr( "未保护" ) );
		if( auto* protectionItem = m_table->item( row, 5 ) )
		{
			protectionItem->setIcon( QIcon( protectedNow ? QStringLiteral(":/classroom/icons/shield-on.png" ) :
				QStringLiteral(":/classroom/icons/shield-off.png" ) ) );
		}
		set( 6, uwf.isEmpty() ? tr( "未知，请查询 UWF" ) : uwf );
		set( 7, QString::number( total ) );
		set( 8, m_levelProvider ? m_levelProvider( seat ) : QString{} );
		set( 9, online ? tr( "活跃" ) : tr( "-" ) );
	}
}


void ClassroomSeatGrid::refresh()
{
	int online = 0;
	for( auto it = m_cards.begin(); it != m_cards.end(); ++it )
	{
		auto& card = it.value();
		const auto onlineNow = m_onlineIps.contains( card.ip );

		if( m_thumbnailProvider && onlineNow )
		{
			const auto image = m_thumbnailProvider( card.ip );
			if( image.isNull() == false )
			{
				// Rescale only when a new frame arrived: QImage::cacheKey()
				// changes with every framebuffer update, so idle screens skip
				// the SmoothTransformation rescale entirely.
				if( image.cacheKey() != card.frameKey )
				{
					card.frameKey = image.cacheKey();
					card.thumbnail->setPixmap( QPixmap::fromImage( image ).scaled(
						m_thumbWidth, m_thumbHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation ) );
				}
			}
		}
		else
		{
			card.frameKey = 0;
			card.thumbnail->setPixmap( QPixmap() );
			card.thumbnail->setText( tr( "offline" ) );
		}

		int total = 100;
		if( m_scoreProvider ) { total = m_scoreProvider( card.seat ); }
		card.score->setText( tr( "积分" ) );
		card.score->setEnabled( m_scoreSink != nullptr );

		const auto state = onlineNow ? tr( "在线" ) : tr( "离线" );
		const auto level = m_levelProvider ? m_levelProvider( card.seat ) : QString{};
		card.caption->setText( level.isEmpty() ? QStringLiteral("%1 · %2").arg( card.expectedName ).arg( total ) :
			QStringLiteral("%1 · %2 · %3").arg( card.expectedName ).arg( total ).arg( level ) );
		card.state->setProperty( "online", onlineNow );
		card.state->setText( QStringLiteral("%1 · %2").arg( state, protectionText( card.seat ) ) );
		const auto uwf = m_uwfProvider ? m_uwfProvider( card.seat ) : QString{};
		card.root->setToolTip( tr( "Drag this card onto another one to swap positions\n%1\nUWF: %2" )
							   .arg( state, uwf.isEmpty() ? tr( "unknown" ) : uwf ) );
		if( onlineNow ) { ++online; }
	}
	if( m_keyboardSwapSeat.isEmpty() == false )
	{
		m_summary->setText( tr( "已选择座位 %1。请焦点移到目标座位后按 Enter/空格交换，Esc 取消。" )
							.arg( m_keyboardSwapSeat ) );
	}
	else if( m_cards.isEmpty() == false )
	{
		m_summary->setText( tr( "%1：%2/%3 名学生在线。拖动卡片或按 Enter/空格交换座位，Esc 取消；可点击“积分”直接加减分。" )
							.arg( m_className, QString::number( online ), QString::number( m_cards.size() ) ) );
	}
	refreshList();
}


void ClassroomSeatGrid::chooseDelta( const QString& seat )
{
	if( m_scoreSink == nullptr ) { return; }
	const auto it = m_cards.find( seat );
	if( it == m_cards.end() ) { return; }

	if( m_activeScoreSeat == seat )
	{
		cancelScore( seat );
		return;
	}
	if( m_activeScoreSeat.isEmpty() == false ) { cancelScore( m_activeScoreSeat ); }

	auto& card = it.value();
	card.scoreDelta->setValue( 1 );
	card.scoreReason->clear();
	m_scoreFocusButton = card.score;
	card.scoreEditor->show();
	card.scoreDelta->setFocus();
	m_activeScoreSeat = seat;
}


void ClassroomSeatGrid::commitScore( const QString& seat )
{
	const auto it = m_cards.find( seat );
	if( it == m_cards.end() || m_scoreSink == nullptr ) { return; }
	auto& card = it.value();
	const auto delta = card.scoreDelta->value();
	if( delta == 0 )
	{
		QMessageBox::information( this, windowTitle(), tr( "Choose a non-zero score change." ) );
		return;
	}
	const auto reason = card.scoreReason->text().trimmed();
	if( m_scoreSink( seat, delta, reason ) == false )
	{
		QMessageBox::warning( this, windowTitle(), tr( "Could not save the score change. Nothing was changed." ) );
		return;
	}
	cancelScore( seat );
	refresh();
}


void ClassroomSeatGrid::cancelScore( const QString& seat )
{
	const auto it = m_cards.find( seat );
	if( it != m_cards.end() ) { it.value().scoreEditor->hide(); }
	if( m_activeScoreSeat == seat )
	{
		m_activeScoreSeat.clear();
		if( m_scoreFocusButton ) { m_scoreFocusButton->setFocus(); }
		m_scoreFocusButton.clear();
	}
}


int ClassroomSeatGrid::columnCount() const
{
	const auto available = qMax( MinimumCardWidth, m_gridPage->width() );
	const auto cardWidth = m_thumbWidth + 16;
	return qMax( 1, ( available + CardSpacing ) / ( cardWidth + CardSpacing ) );
}


void ClassroomSeatGrid::reflow()
{
	if( m_cards.isEmpty() ) { return; }
	const auto columns = columnCount();
	int index = 0;
	for( auto it = m_cards.begin(); it != m_cards.end(); ++it )
	{
		auto* card = it.value().root;
		m_grid->removeWidget( card );
		m_grid->addWidget( card, index / columns, index % columns );
		++index;
	}
}


void ClassroomSeatGrid::updateZoom( int index )
{
	if( m_zoomCombo == nullptr ) { return; }
	const auto zoom = m_zoomCombo->itemData( index ).toInt();
	if( zoom <= 0 ) { return; }
	m_thumbWidth = qRound( 168.0 * zoom / 100.0 );
	m_thumbHeight = qRound( 105.0 * zoom / 100.0 );
	QSettings settings( QStringLiteral("ikeback"), QStringLiteral("classroom") );
	settings.setValue( QStringLiteral("seatZoom"), zoom );
	rebuild();
}


void ClassroomSeatGrid::resizeEvent( QResizeEvent* event )
{
	QWidget::resizeEvent( event );
	reflow();
}


bool ClassroomSeatGrid::eventFilter( QObject* watched, QEvent* event )
{
	const auto seat = watched->property( "ikebackSeat" ).toString();
	if( seat.isEmpty() )
	{
		if( watched == this && event->type() == QEvent::KeyPress &&
			static_cast<QKeyEvent*>( event )->key() == Qt::Key_Escape &&
			m_keyboardSwapSeat.isEmpty() == false )
		{
			cancelKeyboardSwap();
			return true;
		}
		return false;
	}

	if( event->type() == QEvent::KeyPress )
	{
		auto* key = static_cast<QKeyEvent*>( event );
		if( key->key() == Qt::Key_Escape && m_keyboardSwapSeat.isEmpty() == false )
		{
			cancelKeyboardSwap();
			return true;
		}
		if( key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter &&
			key->key() != Qt::Key_Space ) { return false; }
		if( m_keyboardSwapSeat.isEmpty() )
		{
			m_keyboardSwapSeat = seat;
			m_summary->setText( tr( "已选择座位 %1。请焦点移到目标座位后按 Enter/空格交换，Esc 取消。" ).arg( seat ) );
			return true;
		}
		if( m_keyboardSwapSeat == seat ) { cancelKeyboardSwap(); return true; }
		const auto from = m_keyboardSwapSeat;
		swapSeats( from, seat );
		m_keyboardSwapSeat.clear();
		return true;
	}

	if( event->type() == QEvent::MouseButtonPress )
	{
		auto* mouse = static_cast<QMouseEvent*>( event );
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		m_dragStart = mouse->position().toPoint();
#else
		m_dragStart = mouse->pos();
#endif
		m_dragSeat = seat;
		return false;
	}

	if( event->type() == QEvent::MouseMove && m_dragSeat.isEmpty() == false )
	{
		auto* mouse = static_cast<QMouseEvent*>( event );
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		const QPoint current = mouse->position().toPoint();
#else
		const QPoint current = mouse->pos();
#endif
		if( ( mouse->buttons() & Qt::LeftButton ) &&
			( current - m_dragStart ).manhattanLength() > 12 )
		{
			auto* drag = new QDrag( watched );
			auto* mime = new QMimeData;
			mime->setText( m_dragSeat );
			drag->setMimeData( mime );
			drag->exec( Qt::MoveAction );
			m_dragSeat.clear();
			return true;
		}
		return false;
	}

	if( event->type() == QEvent::DragEnter )
	{
		auto* enter = static_cast<QDragEnterEvent*>( event );
		if( enter->mimeData()->hasText() )
		{
			enter->acceptProposedAction();
			return true;
		}
	}

	if( event->type() == QEvent::Drop )
	{
		auto* drop = static_cast<QDropEvent*>( event );
		const auto from = drop->mimeData()->text();
		if( from.isEmpty() == false && from != seat )
		{
			swapSeats( from, seat );
			drop->acceptProposedAction();
			return true;
		}
	}

	return QWidget::eventFilter( watched, event );
}


void ClassroomSeatGrid::cancelKeyboardSwap()
{
	const auto selected = m_keyboardSwapSeat;
	m_keyboardSwapSeat.clear();
	const auto it = m_cards.find( selected );
	if( it != m_cards.end() ) { it.value().root->setFocus(); }
	refresh();
}


void ClassroomSeatGrid::swapSeats( const QString& from, const QString& to )
{
	const auto left = m_order.indexOf( from );
	const auto right = m_order.indexOf( to );
	if( left < 0 || right < 0 || left == right ) { return; }

	const auto previous = m_order.at( left );
	m_order[ left ] = m_order.at( right );
	m_order[ right ] = previous;

	// "下一次开机不变": persist immediately, not on exit.
	Classroom::saveSeatOrder( m_className, m_order );
	rebuild();
	const auto destination = m_cards.find( to );
	if( destination != m_cards.end() ) { destination.value().root->setFocus(); }
}
