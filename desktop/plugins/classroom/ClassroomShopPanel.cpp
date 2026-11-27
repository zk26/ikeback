/*
 * ClassroomShopPanel.cpp - teacher UI for the class shop and auctions
 *
 * Pure UI on top of ClassroomShopBook: every rule (stock, balance, highest
 * bid, refund) lives in the book so the student-request path in the plugin
 * and this panel cannot drift apart. This file is part of the Ikeback
 * classroom modification.
 */

#include <QBrush>
#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "ClassroomData.h"
#include "ClassroomProtocol.h"
#include "ClassroomShopPanel.h"
#include "ClassroomTheme.h"


ClassroomShopPanel::ClassroomShopPanel( const QString& dataDirectory, ClassroomRoster& roster,
										ClassroomLogStore& store, QWidget* parent ) :
	QWidget( parent ),
	m_dataDirectory( dataDirectory ),
	m_roster( roster ),
	m_store( store ),
	m_book( dataDirectory )
{
	setObjectName( QStringLiteral("classroomShopPanel") );
	Classroom::Theme::apply( this );
	auto* layout = new QVBoxLayout( this );

	m_statusLabel = new QLabel( tr( "积分就是货币：兑换与竞拍成交都会记入积分册；学生只能使用超过 80 分的部分。" ), this );
	m_statusLabel->setWordWrap( true );
	layout->addWidget( m_statusLabel );

	setupProductsCard();
	setupAuctionCard();
	setupRedemptionCard();
}


void ClassroomShopPanel::setClassName( const QString& className )
{
	if( className == m_className ) { return; }
	m_className = className;
	refresh();
}


void ClassroomShopPanel::refresh()
{
	if( m_className.isEmpty() )
	{
		m_productTable->setRowCount( 0 );
		m_auctionTable->setRowCount( 0 );
		m_redemptionTable->setRowCount( 0 );
		m_openLabel->clear();
		return;
	}
	if( m_book.load( m_className ) == false )
	{
		m_productTable->setRowCount( 0 );
		m_auctionTable->setRowCount( 0 );
		m_redemptionTable->setRowCount( 0 );
		m_statusLabel->setText( tr( "商城数据加载失败，请检查数据目录中的商城文件。" ) );
		return;
	}
	m_statusLabel->setText( tr( "积分就是货币：兑换与竞拍成交都会记入积分册；学生只能使用超过 80 分的部分。" ) );

	m_openLabel->setText( m_book.isOpen() ? tr( "营业中" ) : tr( "已关门（学生不能兑换或出价）" ) );

	const QSignalBlocker productBlocker( m_productTable );
	m_productTable->setRowCount( m_book.products().size() );
	for( int row = 0; row < m_book.products().size(); ++row )
	{
		const auto& product = m_book.products().at( row );
		m_productTable->setItem( row, 0, new QTableWidgetItem( product.name ) );
		m_productTable->setItem( row, 1, new QTableWidgetItem( QString::number( product.price ) ) );
		auto* stockItem = new QTableWidgetItem( QString::number( product.stock ) );
		if( product.stock <= 0 ) { stockItem->setForeground( QBrush( Classroom::Theme::color( Classroom::Theme::ColorRole::Danger ) ) ); }
		m_productTable->setItem( row, 2, stockItem );
		m_productTable->setItem( row, 3, new QTableWidgetItem( product.description ) );
		auto* listedItem = new QTableWidgetItem( product.listed ? tr( "已上架" ) : tr( "已下架" ) );
		listedItem->setForeground( QBrush( Classroom::Theme::color(
			product.listed ? Classroom::Theme::ColorRole::Success : Classroom::Theme::ColorRole::Muted ) ) );
		m_productTable->setItem( row, 4, listedItem );
		m_productTable->item( row, 0 )->setData( Qt::UserRole, product.id );
	}

	const QSignalBlocker auctionBlocker( m_auctionTable );
	m_auctionTable->setRowCount( m_book.auctions().size() );
	for( int row = 0; row < m_book.auctions().size(); ++row )
	{
		const auto& auction = m_book.auctions().at( row );
		QString state;
		if( auction.open ) { state = tr( "进行中" ); }
		else if( auction.currentBid() > 0 ) { state = tr( "已成交" ); }
		else { state = tr( "已流拍" ); }
		const auto leader = auction.currentBid() > 0 ?
			QStringLiteral("%1（座位 %2）").arg( auction.bidderStudent(), auction.bidderSeat() ) :
			tr( "暂无出价" );
		m_auctionTable->setItem( row, 0, new QTableWidgetItem( auction.name ) );
		m_auctionTable->setItem( row, 1, new QTableWidgetItem( QString::number( auction.startPrice ) ) );
		m_auctionTable->setItem( row, 2, new QTableWidgetItem( QString::number( auction.currentBid() ) ) );
		m_auctionTable->setItem( row, 3, new QTableWidgetItem( leader ) );
		m_auctionTable->setItem( row, 4, new QTableWidgetItem( QString::number( auction.bids.size() ) ) );
		m_auctionTable->setItem( row, 5, new QTableWidgetItem( state ) );
		m_auctionTable->item( row, 0 )->setData( Qt::UserRole, auction.id );
	}

	const QSignalBlocker redemptionBlocker( m_redemptionTable );
	m_redemptionTable->setRowCount( m_book.redemptions().size() );
	for( int row = 0; row < m_book.redemptions().size(); ++row )
	{
		// newest first
		const auto& redemption = m_book.redemptions().at( m_book.redemptions().size() - 1 - row );
		QString status;
		QColor statusColor;
		if( redemption.status == ClassroomShopRedemption::Pending )
		{
			status = tr( "待发货" );
			statusColor = Classroom::Theme::color( Classroom::Theme::ColorRole::Warning );
		}
		else if( redemption.status == ClassroomShopRedemption::Delivered )
		{
			status = tr( "已发货" );
			statusColor = Classroom::Theme::color( Classroom::Theme::ColorRole::Success );
		}
		else
		{
			status = tr( "已拒绝（已退款）" );
			statusColor = Classroom::Theme::color( Classroom::Theme::ColorRole::Muted );
		}
		m_redemptionTable->setItem( row, 0, new QTableWidgetItem(
			redemption.timestamp.toString( QStringLiteral("MM-dd HH:mm") ) ) );
		m_redemptionTable->setItem( row, 1, new QTableWidgetItem( redemption.productName ) );
		m_redemptionTable->setItem( row, 2, new QTableWidgetItem(
			QStringLiteral("%1（座位 %2）").arg( redemption.student, redemption.seat ) ) );
		m_redemptionTable->setItem( row, 3, new QTableWidgetItem( QString::number( redemption.price ) ) );
		auto* statusItem = new QTableWidgetItem( status );
		statusItem->setForeground( QBrush( statusColor ) );
		m_redemptionTable->setItem( row, 4, statusItem );
		m_redemptionTable->item( row, 1 )->setData( Qt::UserRole, redemption.id );
	}
}


void ClassroomShopPanel::setupProductsCard()
{
	auto* card = new QWidget( this );
	auto* layout = new QVBoxLayout( card );

	m_openLabel = new QLabel( card );
	m_openLabel->setProperty( "role", QStringLiteral("heading") );
	auto* toggleOpenButton = new QPushButton( tr( "开/关商场" ), card );
	toggleOpenButton->setToolTip( tr( "关门后学生能看到商品但不能兑换或出价" ) );
	auto* topRow = new QHBoxLayout;
	topRow->addWidget( new QLabel( tr( "商品管理" ), card ) );
	topRow->addWidget( m_openLabel );
	topRow->addStretch( 1 );
	topRow->addWidget( toggleOpenButton );
	layout->addLayout( topRow );
	connect( toggleOpenButton, &QPushButton::clicked, this, &ClassroomShopPanel::toggleShopOpen );

	m_productTable = new QTableWidget( 0, 5, card );
	m_productTable->setHorizontalHeaderLabels( { tr( "商品" ), tr( "价格" ), tr( "库存" ), tr( "说明" ), tr( "状态" ) } );
	m_productTable->horizontalHeader()->setSectionResizeMode( QHeaderView::Stretch );
	m_productTable->setSelectionBehavior( QAbstractItemView::SelectRows );
	m_productTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
	m_productTable->setContextMenuPolicy( Qt::CustomContextMenu );
	m_productTable->setMaximumHeight( 220 );
	connect( m_productTable, &QTableWidget::customContextMenuRequested,
			 this, &ClassroomShopPanel::productContextMenu );
	layout->addWidget( m_productTable );

	auto* buttonRow = new QHBoxLayout;
	auto* addButton = new QPushButton( tr( "上架商品" ), card );
	auto* editButton = new QPushButton( tr( "编辑商品" ), card );
	auto* removeButton = new QPushButton( tr( "删除商品" ), card );
	auto* stockPlusButton = new QPushButton( tr( "库存 +1" ), card );
	auto* stockMinusButton = new QPushButton( tr( "库存 -1" ), card );
	for( auto* button : { addButton, editButton, removeButton, stockPlusButton, stockMinusButton } )
	{
		buttonRow->addWidget( button );
	}
	buttonRow->addStretch( 1 );
	layout->addLayout( buttonRow );

	connect( addButton, &QPushButton::clicked, this, &ClassroomShopPanel::addProduct );
	connect( editButton, &QPushButton::clicked, this, &ClassroomShopPanel::editProduct );
	connect( removeButton, &QPushButton::clicked, this, &ClassroomShopPanel::removeProduct );
	connect( stockPlusButton, &QPushButton::clicked, this, [this]() { adjustStock( 1 ); } );
	connect( stockMinusButton, &QPushButton::clicked, this, [this]() { adjustStock( -1 ); } );

	qobject_cast<QVBoxLayout*>( this->layout() )->addWidget( card );
}


void ClassroomShopPanel::setupAuctionCard()
{
	auto* card = new QWidget( this );
	auto* layout = new QVBoxLayout( card );
	layout->addWidget( new QLabel( tr( "竞拍区（价高者得：结束时给最高出价者按成交价扣积分）" ), card ) );

	m_auctionTable = new QTableWidget( 0, 6, card );
	m_auctionTable->setHorizontalHeaderLabels( { tr( "拍品" ), tr( "起拍价" ), tr( "当前价" ),
												 tr( "领先者" ), tr( "出价数" ), tr( "状态" ) } );
	m_auctionTable->horizontalHeader()->setSectionResizeMode( QHeaderView::Stretch );
	m_auctionTable->setSelectionBehavior( QAbstractItemView::SelectRows );
	m_auctionTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
	m_auctionTable->setMaximumHeight( 200 );
	layout->addWidget( m_auctionTable );

	auto* buttonRow = new QHBoxLayout;
	auto* startButton = new QPushButton( tr( "发起竞拍" ), card );
	auto* closeButton = new QPushButton( tr( "结束竞拍" ), card );
	closeButton->setToolTip( tr( "价高者得：成交后自动扣积分并生成发货任务；无人出价则流拍" ) );
	auto* removeButton = new QPushButton( tr( "删除记录" ), card );
	removeButton->setToolTip( tr( "删除进行中的竞拍等于取消：出价不扣分" ) );
	buttonRow->addWidget( startButton );
	buttonRow->addWidget( closeButton );
	buttonRow->addWidget( removeButton );
	buttonRow->addStretch( 1 );
	layout->addLayout( buttonRow );

	connect( startButton, &QPushButton::clicked, this, [this]() { startAuction(); } );
	connect( closeButton, &QPushButton::clicked, this, &ClassroomShopPanel::closeSelectedAuction );
	connect( removeButton, &QPushButton::clicked, this, &ClassroomShopPanel::removeSelectedAuction );

	qobject_cast<QVBoxLayout*>( this->layout() )->addWidget( card );
}


void ClassroomShopPanel::setupRedemptionCard()
{
	auto* card = new QWidget( this );
	auto* layout = new QVBoxLayout( card );
	layout->addWidget( new QLabel( tr( "发货队列（兑换与竞拍成交都会出现在这里）" ), card ) );

	m_redemptionTable = new QTableWidget( 0, 5, card );
	m_redemptionTable->setHorizontalHeaderLabels( { tr( "时间" ), tr( "商品" ), tr( "学生" ),
													tr( "花费" ), tr( "状态" ) } );
	m_redemptionTable->horizontalHeader()->setSectionResizeMode( QHeaderView::Stretch );
	m_redemptionTable->setSelectionBehavior( QAbstractItemView::SelectRows );
	m_redemptionTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
	m_redemptionTable->setMaximumHeight( 200 );
	layout->addWidget( m_redemptionTable );

	auto* buttonRow = new QHBoxLayout;
	auto* deliverButton = new QPushButton( tr( "标记已发货" ), card );
	auto* rejectButton = new QPushButton( tr( "拒绝并退款" ), card );
	rejectButton->setToolTip( tr( "退款会以正分记录写回积分册，普通兑换还会恢复库存" ) );
	buttonRow->addWidget( deliverButton );
	buttonRow->addWidget( rejectButton );
	buttonRow->addStretch( 1 );
	layout->addLayout( buttonRow );

	connect( deliverButton, &QPushButton::clicked, this, &ClassroomShopPanel::deliverRedemption );
	connect( rejectButton, &QPushButton::clicked, this, &ClassroomShopPanel::rejectRedemption );

	qobject_cast<QVBoxLayout*>( this->layout() )->addWidget( card );
}


QString ClassroomShopPanel::currentProductId() const
{
	const auto row = m_productTable->currentRow();
	if( row < 0 ) { return {}; }
	const auto* item = m_productTable->item( row, 0 );
	return item ? item->data( Qt::UserRole ).toString() : QString{};
}


QString ClassroomShopPanel::currentAuctionId() const
{
	const auto row = m_auctionTable->currentRow();
	if( row < 0 ) { return {}; }
	const auto* item = m_auctionTable->item( row, 0 );
	return item ? item->data( Qt::UserRole ).toString() : QString{};
}


QString ClassroomShopPanel::currentRedemptionId() const
{
	const auto row = m_redemptionTable->currentRow();
	if( row < 0 ) { return {}; }
	const auto* item = m_redemptionTable->item( row, 1 );
	return item ? item->data( Qt::UserRole ).toString() : QString{};
}


void ClassroomShopPanel::appendShopEvent( const QString& detail, const QString& seat, const QString& student )
{
	ClassroomEvent event;
	event.timestamp = QDateTime::currentDateTime();
	event.className = m_className;
	event.seat = seat;
	event.student = student;
	event.host = tr( "teacher" );
	event.type = Classroom::EventType::shop();
	event.detail = detail;
	m_store.append( event );
}


void ClassroomShopPanel::toggleShopOpen()
{
	if( m_className.isEmpty() ) { return; }
	if( m_book.setOpen( m_book.isOpen() == false ) == false )
	{
		QMessageBox::warning( this, windowTitle(), tr( "保存失败，请检查教师机数据目录。" ) );
		return;
	}
	appendShopEvent( m_book.isOpen() ? tr( "商场已开放" ) : tr( "商场已关门" ), {}, {} );
	Q_EMIT shopChanged();
	refresh();
}


void ClassroomShopPanel::addProduct()
{
	if( m_className.isEmpty() ) { return; }
	if( m_book.products().size() >= ClassroomShopBook::MaxProducts )
	{
		QMessageBox::information( this, windowTitle(), tr( "商品数量已达上限（%1 个）。" )
													  .arg( ClassroomShopBook::MaxProducts ) );
		return;
	}

	QDialog dialog( this );
	dialog.setWindowTitle( tr( "上架商品" ) );
	auto* form = new QFormLayout( &dialog );
	auto* nameEdit = new QLineEdit( &dialog );
	auto* priceSpin = new QSpinBox( &dialog );
	priceSpin->setRange( 0, static_cast<int>( ClassroomShopBook::MaxPrice ) );
	priceSpin->setValue( 5 );
	auto* stockSpin = new QSpinBox( &dialog );
	stockSpin->setRange( 0, static_cast<int>( ClassroomShopBook::MaxStock ) );
	stockSpin->setValue( 1 );
	auto* descriptionEdit = new QLineEdit( &dialog );
	descriptionEdit->setPlaceholderText( tr( "可选：一句话说明" ) );
	auto* imageLabel = new QLabel( tr( "未选择图片" ), &dialog );
	imageLabel->setAlignment( Qt::AlignCenter );
	imageLabel->setMinimumSize( 120, 120 );
	imageLabel->setProperty( "role", QStringLiteral("hint") );
	QImage selectedImage;
	auto* pickButton = new QPushButton( tr( "选择图片…" ), &dialog );
	form->addRow( tr( "商品名称：" ), nameEdit );
	form->addRow( tr( "价格（积分）：" ), priceSpin );
	form->addRow( tr( "库存：" ), stockSpin );
	form->addRow( tr( "说明：" ), descriptionEdit );
	form->addRow( tr( "商品图片（可选）：" ), imageLabel );
	form->addRow( {}, pickButton );
	auto* buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
	form->addRow( buttons );
	connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );

	connect( pickButton, &QPushButton::clicked, this, [&]() {
		const auto fileName = QFileDialog::getOpenFileName( &dialog, tr( "选择商品图片" ), {},
			tr( "图片 (*.png *.jpg *.jpeg *.bmp *.gif);;所有文件 (*)" ) );
		if( fileName.isEmpty() ) { return; }
		QImageReader reader( fileName );
		reader.setAutoTransform( true );
		const auto image = reader.read();
		if( image.isNull() )
		{
			QMessageBox::warning( &dialog, dialog.windowTitle(), reader.errorString() );
			return;
		}
		selectedImage = image;
		imageLabel->setPixmap( QPixmap::fromImage( image.scaled( 120, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation ) ) );
	} );

	if( dialog.exec() != QDialog::Accepted || nameEdit->text().trimmed().isEmpty() ) { return; }

	ClassroomShopProduct product;
	product.name = nameEdit->text();
	product.description = descriptionEdit->text();
	product.price = priceSpin->value();
	product.stock = stockSpin->value();
	if( selectedImage.isNull() == false )
	{
		product.image = ClassroomShopBook::catalogImage( selectedImage );
		if( product.image.isEmpty() )
		{
			QMessageBox::warning( this, windowTitle(), tr( "图片无法压缩到目录大小限制内，已忽略图片。" ) );
		}
	}
	if( m_book.addProduct( &product ) )
	{
		appendShopEvent( tr( "上架商品：%1（%2 分，库存 %3）" )
							 .arg( product.name ).arg( product.price ).arg( product.stock ), {}, {} );
		Q_EMIT shopChanged();
		refresh();
	}
	else
	{
		QMessageBox::warning( this, windowTitle(), tr( "上架失败，请检查商品名称后重试。" ) );
	}
}


void ClassroomShopPanel::editProduct()
{
	if( m_className.isEmpty() ) { return; }
	const auto productId = currentProductId();
	if( productId.isEmpty() )
	{
		QMessageBox::information( this, windowTitle(), tr( "请先在商品列表中选择一行。" ) );
		return;
	}
	const auto* existing = m_book.product( productId );
	if( existing == nullptr ) { return; }

	QDialog dialog( this );
	dialog.setWindowTitle( tr( "编辑商品" ) );
	auto* form = new QFormLayout( &dialog );
	auto* nameEdit = new QLineEdit( existing->name, &dialog );
	auto* priceSpin = new QSpinBox( &dialog );
	priceSpin->setRange( 0, static_cast<int>( ClassroomShopBook::MaxPrice ) );
	priceSpin->setValue( existing->price );
	auto* stockSpin = new QSpinBox( &dialog );
	stockSpin->setRange( 0, static_cast<int>( ClassroomShopBook::MaxStock ) );
	stockSpin->setValue( existing->stock );
	auto* descriptionEdit = new QLineEdit( existing->description, &dialog );
	form->addRow( tr( "商品名称：" ), nameEdit );
	form->addRow( tr( "价格（积分）：" ), priceSpin );
	form->addRow( tr( "库存：" ), stockSpin );
	form->addRow( tr( "说明：" ), descriptionEdit );
	auto* buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
	form->addRow( buttons );
	connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
	if( dialog.exec() != QDialog::Accepted ) { return; }

	auto product = *existing;
	product.name = nameEdit->text();
	product.price = priceSpin->value();
	product.stock = stockSpin->value();
	product.description = descriptionEdit->text();
	if( m_book.updateProduct( product ) )
	{
		Q_EMIT shopChanged();
		refresh();
	}
}


void ClassroomShopPanel::removeProduct()
{
	if( m_className.isEmpty() ) { return; }
	const auto productId = currentProductId();
	if( productId.isEmpty() )
	{
		QMessageBox::information( this, windowTitle(), tr( "请先在商品列表中选择一行。" ) );
		return;
	}
	const auto* existing = m_book.product( productId );
	if( existing == nullptr ) { return; }
	const auto productName = existing->name;
	if( QMessageBox::question( this, windowTitle(),
							   tr( "确定永久删除「%1」？历史竞拍和发货记录会保留名称；临时停售请使用右键“下架”。" ).arg( productName ) ) !=
		QMessageBox::Yes ) { return; }
	if( m_book.removeProduct( productId ) )
	{
		appendShopEvent( tr( "删除商品：%1" ).arg( productName ), {}, {} );
		Q_EMIT shopChanged();
		refresh();
	}
}


void ClassroomShopPanel::toggleProductListed()
{
	const auto productId = currentProductId();
	const auto* product = m_book.product( productId );
	if( product == nullptr ) { return; }
	const auto wasListed = product->listed;
	if( m_book.setProductListed( productId, wasListed == false ) == false )
	{
		QMessageBox::warning( this, windowTitle(), tr( "保存失败，请重试。" ) );
		return;
	}
	appendShopEvent( wasListed ? tr( "下架商品：%1" ).arg( product->name )
							   : tr( "上架商品：%1" ).arg( product->name ), {}, {} );
	Q_EMIT shopChanged();
	refresh();
}


void ClassroomShopPanel::setProductStock()
{
	const auto productId = currentProductId();
	const auto* product = m_book.product( productId );
	if( product == nullptr ) { return; }
	const auto stock = QInputDialog::getInt( this, tr( "设置库存" ),
		tr( "「%1」库存：" ).arg( product->name ), product->stock, 0,
		static_cast<int>( ClassroomShopBook::MaxStock ), 1 );
	if( m_book.adjustStock( productId, stock - product->stock ) == false )
	{
		QMessageBox::warning( this, windowTitle(), tr( "保存失败，请重试。" ) );
		return;
	}
	appendShopEvent( tr( "调整库存：%1 → %2" ).arg( product->name ).arg( stock ), {}, {} );
	Q_EMIT shopChanged();
	refresh();
}


void ClassroomShopPanel::productContextMenu( const QPoint& position )
{
	const auto index = m_productTable->indexAt( position );
	if( index.isValid() == false ) { return; }
	m_productTable->selectRow( index.row() );
	const auto* product = m_book.product( currentProductId() );
	if( product == nullptr ) { return; }

	QMenu menu( this );
	Classroom::Theme::apply( &menu );
	auto* auctionAction = menu.addAction( tr( "将此商品发起竞拍" ) );
	auto* stockAction = menu.addAction( tr( "设置库存…" ) );
	auto* listingAction = menu.addAction( product->listed ? tr( "下架商品" ) : tr( "上架商品" ) );
	menu.addSeparator();
	auto* editAction = menu.addAction( tr( "编辑商品…" ) );
	auto* removeAction = menu.addAction( tr( "永久删除商品…" ) );
	const auto* action = menu.exec( m_productTable->viewport()->mapToGlobal( position ) );
	if( action == auctionAction ) { startAuction( product->id ); }
	else if( action == stockAction ) { setProductStock(); }
	else if( action == listingAction ) { toggleProductListed(); }
	else if( action == editAction ) { editProduct(); }
	else if( action == removeAction ) { removeProduct(); }
}


void ClassroomShopPanel::adjustStock( int delta )
{
	if( m_className.isEmpty() ) { return; }
	const auto productId = currentProductId();
	if( productId.isEmpty() )
	{
		QMessageBox::information( this, windowTitle(), tr( "请先在商品列表中选择一行。" ) );
		return;
	}
	if( m_book.adjustStock( productId, delta ) == false )
	{
		QMessageBox::warning( this, windowTitle(), tr( "保存失败，请重试。" ) );
		return;
	}
	Q_EMIT shopChanged();
	refresh();
}


void ClassroomShopPanel::startAuction( const QString& productId )
{
	if( m_className.isEmpty() ) { return; }
	const auto* product = m_book.product( productId );

	QDialog dialog( this );
	dialog.setWindowTitle( tr( "发起竞拍" ) );
	auto* form = new QFormLayout( &dialog );
	auto* nameEdit = new QLineEdit( product ? product->name : QString(), &dialog );
	nameEdit->setPlaceholderText( tr( "例如：课外读物一本" ) );
	auto* startSpin = new QSpinBox( &dialog );
	startSpin->setRange( 1, static_cast<int>( ClassroomShopBook::MaxPrice ) );
	startSpin->setValue( product ? qMax( 1, product->price ) : 10 );
	auto* incrementSpin = new QSpinBox( &dialog );
	incrementSpin->setRange( 1, static_cast<int>( ClassroomShopBook::MaxIncrement ) );
	incrementSpin->setValue( 1 );
	form->addRow( tr( "拍品名称：" ), nameEdit );
	form->addRow( tr( "起拍价（积分）：" ), startSpin );
	form->addRow( tr( "每次至少加价：" ), incrementSpin );
	auto* hint = new QLabel( tr( "学生可以出价，价高者得；结束时给最高出价者按成交价扣积分。" ), &dialog );
	hint->setWordWrap( true );
	form->addRow( hint );
	auto* buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog );
	form->addRow( buttons );
	connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
	connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
	if( dialog.exec() != QDialog::Accepted || nameEdit->text().trimmed().isEmpty() ) { return; }

	const auto auctionId = m_book.startAuction( nameEdit->text(), startSpin->value(), incrementSpin->value() );
	if( auctionId.isEmpty() )
	{
		QMessageBox::warning( this, windowTitle(),
							  tr( "竞拍没有发起：进行中的竞拍已达上限（%1 个）或保存失败。" )
								  .arg( ClassroomShopBook::MaxOpenAuctions ) );
		return;
	}
	appendShopEvent( tr( "发起竞拍：%1（起拍价 %2 分，每次至少加 %3 分）" )
						 .arg( nameEdit->text().trimmed() ).arg( startSpin->value() )
						 .arg( incrementSpin->value() ), {}, {} );
	Q_EMIT shopChanged();
	refresh();
}


void ClassroomShopPanel::closeSelectedAuction()
{
	if( m_className.isEmpty() ) { return; }
	const auto auctionId = currentAuctionId();
	if( auctionId.isEmpty() )
	{
		QMessageBox::information( this, windowTitle(), tr( "请先在竞拍列表中选择一行。" ) );
		return;
	}
	const auto* existing = m_book.auction( auctionId );
	if( existing == nullptr || existing->open == false ) { return; }

	const auto result = m_book.closeAuction( auctionId );
	if( result.error.isEmpty() == false )
	{
		QMessageBox::warning( this, windowTitle(), result.error );
		return;
	}
	if( result.sold )
	{
		const auto message = tr( "竞拍已结束：%1 由 %2（座位 %3）以 %4 分拍得，已扣积分并加入发货队列。" )
								 .arg( result.name, result.winnerStudent, result.winnerSeat )
								 .arg( result.price );
		appendShopEvent( message, result.winnerSeat, result.winnerStudent );
		QMessageBox::information( this, windowTitle(), message );
	}
	else
	{
		appendShopEvent( tr( "竞拍结束（流拍）：%1" ).arg( result.name ), {}, {} );
		QMessageBox::information( this, windowTitle(), tr( "竞拍已结束：无人出价，流拍。" ) );
	}
	Q_EMIT shopChanged();
	refresh();
}


void ClassroomShopPanel::removeSelectedAuction()
{
	if( m_className.isEmpty() ) { return; }
	const auto auctionId = currentAuctionId();
	if( auctionId.isEmpty() )
	{
		QMessageBox::information( this, windowTitle(), tr( "请先在竞拍列表中选择一行。" ) );
		return;
	}
	const auto* existing = m_book.auction( auctionId );
	if( existing == nullptr ) { return; }
	if( existing->open &&
		QMessageBox::question( this, windowTitle(),
							   tr( "「%1」还在进行中，删除等于取消竞拍（出价不扣分）。确定？" )
								   .arg( existing->name ) ) != QMessageBox::Yes )
	{
		return;
	}
	if( m_book.removeAuction( auctionId ) )
	{
		Q_EMIT shopChanged();
		refresh();
	}
}


void ClassroomShopPanel::deliverRedemption()
{
	if( m_className.isEmpty() ) { return; }
	const auto redemptionId = currentRedemptionId();
	if( redemptionId.isEmpty() )
	{
		QMessageBox::information( this, windowTitle(), tr( "请先在发货队列中选择一行。" ) );
		return;
	}
	const auto* existing = m_book.redemption( redemptionId );
	if( existing == nullptr || existing->status != ClassroomShopRedemption::Pending )
	{
		QMessageBox::information( this, windowTitle(), tr( "只有待发货记录可以操作。" ) );
		return;
	}
	if( m_book.setRedemptionStatus( redemptionId, ClassroomShopRedemption::Delivered ) )
	{
		appendShopEvent( tr( "发货完成" ), {}, {} );
		Q_EMIT shopChanged();
		refresh();
	}
}


void ClassroomShopPanel::rejectRedemption()
{
	if( m_className.isEmpty() ) { return; }
	const auto redemptionId = currentRedemptionId();
	if( redemptionId.isEmpty() )
	{
		QMessageBox::information( this, windowTitle(), tr( "请先在发货队列中选择一行。" ) );
		return;
	}
	const auto* existing = m_book.redemption( redemptionId );
	if( existing == nullptr || existing->status != ClassroomShopRedemption::Pending ) { return; }

	if( QMessageBox::question( this, windowTitle(),
			tr( "拒绝 %1 的「%2」并退还 %3 积分？普通兑换还会恢复一件库存。" )
				.arg( existing->student, existing->productName ).arg( existing->price ) ) !=
		QMessageBox::Yes ) { return; }

	if( m_book.setRedemptionStatus( redemptionId, ClassroomShopRedemption::Rejected ) == false )
	{
		QMessageBox::warning( this, windowTitle(), tr( "退款失败，请稍后重试。" ) );
		return;
	}
	appendShopEvent( tr( "拒绝兑换并退款：%1 - %2" ).arg( existing->student, existing->productName ),
					 existing->seat, existing->student );
	Q_EMIT shopChanged();
	refresh();
}
